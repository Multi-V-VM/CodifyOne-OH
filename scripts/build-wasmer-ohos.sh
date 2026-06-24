#!/usr/bin/env bash
set -euo pipefail

REPO_URL="${WASMER_REPO_URL:-https://github.com/Multi-V-VM/wasmer.git}"
REPO_REF="${WASMER_REPO_REF:-affc5cc6e3532b0dc482e3d1b982b8443cd3aed7}"
TARGET="${WASMER_OHOS_TARGET:-aarch64-unknown-linux-ohos}"
FEATURES="${WASMER_FEATURES:-wat,wamr-default,wasi}"

if [[ "${TARGET}" != "aarch64-unknown-linux-ohos" ]]; then
  echo "Only aarch64-unknown-linux-ohos is supported right now; got ${TARGET}" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_DIR="${WASMER_WORK_DIR:-${PROJECT_ROOT}/.wasmer-ohos}"

# Check for submodule first, then fall back to work directory
SUBMODULE_DIR="${PROJECT_ROOT}/third_party/wasmer"
if [[ -d "${SUBMODULE_DIR}/.git" ]]; then
  echo "Using wasmer submodule: ${SUBMODULE_DIR}"
  SRC_DIR="${WASMER_SRC_DIR:-${SUBMODULE_DIR}}"
else
  SRC_DIR="${WASMER_SRC_DIR:-${WORK_DIR}/wasmer}"
fi

ABI="${OHCODE_ABI:-${WASMER_OHOS_ABI:-arm64-v8a}}"
OUT_DIR="${PROJECT_ROOT}/electron/libs/${ABI}"

# Auto-detect OHOS_NDK_HOME from DevEco Studio if not set
if [[ -z "${OHOS_NDK_HOME:-}" ]]; then
  # Try DevEco Studio default location
  DEV_ECO_SDK="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native"
  if [[ -d "${DEV_ECO_SDK}" ]]; then
    export OHOS_NDK_HOME="${DEV_ECO_SDK}"
    echo "Auto-detected OHOS_NDK_HOME: ${OHOS_NDK_HOME}"
  else
    cat >&2 <<'EOF'
OHOS_NDK_HOME is not set.

Set it to the HarmonyOS/OpenHarmony native SDK directory, for example:
  export OHOS_NDK_HOME="$HOME/Library/Huawei/Sdk/openharmony/10/native"

Or from DevEco Studio:
  export OHOS_NDK_HOME="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native"
EOF
    exit 2
  fi
fi

CLANG="${OHOS_NDK_HOME}/llvm/bin/${TARGET}-clang"
CLANGXX="${OHOS_NDK_HOME}/llvm/bin/${TARGET}-clang++"
AR="${OHOS_NDK_HOME}/llvm/bin/llvm-ar"
OBJCOPY="${OHOS_NDK_HOME}/llvm/bin/llvm-objcopy"

if [[ ! -x "${CLANG}" ]]; then
  echo "OHOS clang not found: ${CLANG}" >&2
  exit 2
fi

if [[ ! -x "${OBJCOPY}" ]]; then
  echo "OHOS llvm-objcopy not found: ${OBJCOPY}" >&2
  exit 2
fi

export PATH="${OHOS_NDK_HOME}/llvm/bin:${PATH}"

mkdir -p "${WORK_DIR}" "${OUT_DIR}"

if [[ ! -d "${SRC_DIR}/.git" ]]; then
  git clone "${REPO_URL}" "${SRC_DIR}"
fi

if git -C "${SRC_DIR}" rev-parse --verify --quiet "${REPO_REF}^{commit}" >/dev/null; then
  git -C "${SRC_DIR}" checkout --detach "${REPO_REF}"
else
  git -C "${SRC_DIR}" fetch --depth 1 origin "${REPO_REF}"
  git -C "${SRC_DIR}" checkout --detach FETCH_HEAD
fi

PATCH_FILE="${PROJECT_ROOT}/scripts/patches/multiv-wasmer-ohos.patch"
if [[ -f "${PATCH_FILE}" ]]; then
  if git -C "${SRC_DIR}" apply --check "${PATCH_FILE}" >/dev/null 2>&1; then
    git -C "${SRC_DIR}" apply "${PATCH_FILE}"
  elif git -C "${SRC_DIR}" apply --reverse --check "${PATCH_FILE}" >/dev/null 2>&1; then
    echo "Wasmer OHOS patch already applied: ${PATCH_FILE}"
  else
    echo "Unable to apply Wasmer OHOS patch: ${PATCH_FILE}" >&2
    git -C "${SRC_DIR}" status --short >&2
    exit 1
  fi
fi

rustup target add "${TARGET}"

TARGET_ENV_UPPER="$(printf '%s' "${TARGET}" | tr '[:lower:]-' '[:upper:]_')"
TARGET_ENV_LOWER="$(printf '%s' "${TARGET}" | tr '-' '_')"
SYSROOT="${OHOS_NDK_HOME}/sysroot"

export "CC_${TARGET_ENV_LOWER}=${CLANG}"
export "CXX_${TARGET_ENV_LOWER}=${CLANGXX}"
export "AR_${TARGET_ENV_LOWER}=${AR}"
export "CFLAGS_${TARGET_ENV_LOWER}=--sysroot=${SYSROOT} ${CFLAGS:-}"
export "CXXFLAGS_${TARGET_ENV_LOWER}=--sysroot=${SYSROOT} ${CXXFLAGS:-}"
export "CARGO_TARGET_${TARGET_ENV_UPPER}_LINKER=${CLANG}"
export "BINDGEN_EXTRA_CLANG_ARGS_${TARGET_ENV_LOWER}=--sysroot=${SYSROOT} ${BINDGEN_EXTRA_CLANG_ARGS:-}"
export BINDGEN_EXTRA_CLANG_ARGS="--sysroot=${SYSROOT} ${BINDGEN_EXTRA_CLANG_ARGS:-}"

cargo build \
  --manifest-path "${SRC_DIR}/lib/c-api/Cargo.toml" \
  --release \
  --target "${TARGET}" \
  --no-default-features \
  --features "${FEATURES}" \
  --locked

LIB_PATH="${SRC_DIR}/target/${TARGET}/release/libwasmer.so"
if [[ ! -f "${LIB_PATH}" ]]; then
  echo "libwasmer.so was not produced at: ${LIB_PATH}" >&2
  exit 1
fi

cp "${LIB_PATH}" "${OUT_DIR}/libwasmer.so"
echo "Installed ${OUT_DIR}/libwasmer.so"
