#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/deploy-to-hdc.sh [--hap PATH] [--device DEVICE_ID] [--bundle BUNDLE_NAME] [--ability ABILITY]

Examples:
  scripts/deploy-to-hdc.sh
  scripts/deploy-to-hdc.sh --bundle com.mikannqaq.ohcode --ability EntryAbility
  scripts/deploy-to-hdc.sh --hap electron/build/default/outputs/default/electron-debug-signed.hap

Environment:
  OHCODE_TARGET_NAME   (default: default)
  OHCODE_BUILD_MODE    (default: release)
  OHCODE_BUNDLE_NAME   (default: com.mikannqaq.ohcode)
  OHCODE_ENTRY_ABILITY (default: EntryAbility)
EOF
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_DIR="$ROOT_DIR/electron"

HAP_PATH=""
DEVICE_ID=""
BUNDLE_NAME="${OHCODE_BUNDLE_NAME:-com.mikannqaq.ohcode}"
ENTRY_ABILITY="${OHCODE_ENTRY_ABILITY:-EntryAbility}"
TARGET_NAME="${OHCODE_TARGET_NAME:-default}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hap)
      HAP_PATH="$2"
      shift 2
      ;;
    --device)
      DEVICE_ID="$2"
      shift 2
      ;;
    --bundle)
      BUNDLE_NAME="$2"
      shift 2
      ;;
    --ability)
      ENTRY_ABILITY="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1"
      usage
      exit 1
      ;;
  esac
done

if ! command -v hdc >/dev/null 2>&1; then
  echo "hdc not found in PATH"
  exit 1
fi

if [[ -z "$HAP_PATH" ]]; then
  # Prefer the expected default output location; prefer a signed HAP because
  # plain name sorting picks "-unsigned.hap" over "-signed.hap".
  HAP_PATH="$(find "$MODULE_DIR/build/$TARGET_NAME/outputs/$TARGET_NAME" -maxdepth 1 -name '*-signed.hap' -type f 2>/dev/null | sort | tail -n 1 || true)"
  if [[ -z "$HAP_PATH" ]]; then
    HAP_PATH="$(find "$MODULE_DIR/build/$TARGET_NAME/outputs/$TARGET_NAME" -maxdepth 1 -name '*.hap' -type f 2>/dev/null | sort | tail -n 1 || true)"
  fi

  # Fallback to any matching target output.
  if [[ -z "$HAP_PATH" ]]; then
    HAP_PATH="$(find "$MODULE_DIR/build" -path "*/outputs/$TARGET_NAME/*-signed.hap" -type f 2>/dev/null | sort | tail -n 1 || true)"
    if [[ -z "$HAP_PATH" ]]; then
      HAP_PATH="$(find "$MODULE_DIR/build" -path "*/outputs/$TARGET_NAME/*.hap" -type f 2>/dev/null | sort | tail -n 1 || true)"
    fi
  fi
fi

if [[ -z "$HAP_PATH" ]]; then
  echo "No HAP found. Build first or pass --hap /path/to/file.hap"
  exit 1
fi

if [[ ! -f "$HAP_PATH" ]]; then
  echo "HAP not found: $HAP_PATH"
  exit 1
fi

HDCTARGET=()
if [[ -n "$DEVICE_ID" ]]; then
  HDCTARGET=(-t "$DEVICE_ID")
fi
# set -u + empty array: bash 3.2 (macOS /bin/bash) treats "${arr[@]}" as unbound,
# so every expansion of a possibly-empty array needs the ${arr[@]+"..."} guard.
echo "[OHcode] Installing: $HAP_PATH"
hdc ${HDCTARGET[@]+"${HDCTARGET[@]}"} install -r "$HAP_PATH"

echo "[OHcode] Launching: $ENTRY_ABILITY"
hdc ${HDCTARGET[@]+"${HDCTARGET[@]}"} shell aa start -a "$ENTRY_ABILITY" -b "$BUNDLE_NAME"

echo "[OHcode] Done."
