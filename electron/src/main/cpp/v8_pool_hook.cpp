// Copyright (c) 2024 V8 Pool Hook for HarmonyOS Electron
// NDK module to manage V8 isolate lifecycle from TypeScript

#include "napi/native_api.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <utility>
#include <vector>
#include <unistd.h>

#if __has_include(<hilog/log.h>)
#include <hilog/log.h>
#define OHCODE_HAS_HILOG 1
#else
#define OHCODE_HAS_HILOG 0
#endif

extern "C" uint32_t
_ZN4node17InitializeContextEN2v85LocalINS0_7ContextEEE(void* context);
extern "C" void*
_ZN4node10NewContextEPN2v87IsolateENS0_5LocalINS0_14ObjectTemplateEEE(
    void* isolate, void* objectTemplate);
extern "C" void*
_ZN4node17CreateEnvironmentEPNS_11IsolateDataEN2v85LocalINS2_7ContextEEERKNSt4__n16vectorINS6_12basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEENSB_ISD_EEEESH_NS_16EnvironmentFlags5FlagsENS_8ThreadIdENS6_10unique_ptrINS_21InspectorParentHandleENS6_14default_deleteISM_EEEE(
    void* isolateData, void* context, void* args, void* execArgs, void* flags,
    void* threadId, void* inspectorParentHandle);
extern "C" void*
_ZN4node15LoadEnvironmentEPNS_11EnvironmentEPKc(void* environment,
                                                const char* source);
extern "C" void*
_ZN4node15LoadEnvironmentEPNS_11EnvironmentENSt4__n18functionIFN2v810MaybeLocalINS4_5ValueEEERKNS_26StartExecutionCallbackInfoEEEE(
    void* environment, void* callback, void* preload);

#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif

#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif

#if defined(__linux__) || defined(__OHOS__)
#include <sys/epoll.h>
#else
struct epoll_event;
#endif

namespace {

// Isolate tracking
struct IsolateInfo {
    std::string windowId;
    void* isolatePtr;  // Store as opaque pointer
    uint64_t lastUsed;
    bool isActive;
    int refCount;
};

static std::unordered_map<std::string, IsolateInfo> g_isolateMap;
static std::unordered_map<void*, std::string> g_ptrToWindowId;
static std::mutex g_mutex;
static bool g_enabled = true;
static size_t g_maxPoolSize = 5;

// Disassembly anchor from libelectron.so BuildID
// 54fa48401561befd00ed0771eb990af1372f4374:
//
//   4b27ca4: 29430262  ldp w2, w0, [x19, #0x18]
//   4b27ca8: 1a89b103  csel w3, w8, w9, lt
//   4b27cac: 94f6f529  bl 0x88e5150 <epoll_wait@plt>
//   4b27cb0: 3100041f  cmn w0, #0x1
//
// The stack reports the call site (0x4b27cac), while __builtin_return_address
// gives the next instruction (0x4b27cb0).
constexpr uintptr_t kChromeIoThreadEpollCallOffset = 0x4b27cac;
constexpr uintptr_t kChromeIoThreadEpollReturnOffset =
    kChromeIoThreadEpollCallOffset + 4;
constexpr int kDefaultChromeIoThreadMaxWaitMs = 1000;
constexpr int kMinChromeIoThreadMaxWaitMs = 1;
constexpr int kMaxChromeIoThreadMaxWaitMs = 60000;

// Disassembly anchors for the V8 OOM stack in libelectron.so BuildID
// 54fa48401561befd00ed0771eb990af1372f4374:
//
//   6056394: f9400680  ldr x0, [x20, #0x8]
//   6056398: f94002a1  ldr x1, [x21]
//   605639c: 94a2497d  bl 0x88e8990
//              <v8::Isolate::Initialize(...)@plt>
//   60563a0: 94001526  bl 0x605b838
//
// The crash then reaches:
//
//   244f7b4: bl SnapshotCompression::Decompress(...)@plt
//   1f0d96c: tbz w0, #0, 0x1f0da70
//   1f0da70: brk #0
//
// Hooking Isolate::Initialize keeps this source-free and catches the PLT call
// before snapshot decompression allocates its temporary buffer.
constexpr uintptr_t kElectronV8InitializeCallOffset = 0x605639c;
constexpr uintptr_t kElectronV8InitializeReturnOffset =
    kElectronV8InitializeCallOffset + 4;
constexpr const char* kDefaultV8StartupFlags =
    "--max-old-space-size=512 --max-semi-space-size=16";
constexpr const char* kNodePostLoadTraceScript = R"OHCODE_JS(
;(() => {
  const STAMP = "diag-20260626-tracefanout1";
  const TRACE_PATHS = [
    "/data/storage/el2/base/files/ohcode-main-trace.log",
    "/data/storage/el1/base/files/ohcode-main-trace.log",
    "/tmp/ohcode-main-trace.log"
  ];
  function stringify(value) {
    try {
      return JSON.stringify(value);
    } catch (err) {
      return String(value);
    }
  }
  function errorText(err) {
    return String((err && (err.stack || err.message)) || err);
  }
  function trace(phase, detail) {
    const line =
      `${Date.now()} [${STAMP}] [loadenv-post] ${phase}${detail === undefined ? "" : " " + detail}`;
    try {
      if (process && typeof process._rawDebug === "function") {
        process._rawDebug(`[OHcode]${line}`);
      }
    } catch (_) {}
    try {
      if (process && process.stderr && typeof process.stderr.write === "function") {
        process.stderr.write(`[OHcode]${line}\n`);
      }
    } catch (_) {}
    try {
      console.error(`[OHcode]${line}`);
    } catch (_) {}
    try {
      const fs = require("fs");
      for (const tracePath of TRACE_PATHS) {
        try {
          fs.appendFileSync(tracePath, `${line}\n`);
        } catch (err) {
          try {
            if (process && typeof process._rawDebug === "function") {
              process._rawDebug(`[OHcode][loadenv-post] write failed ${tracePath} ${errorText(err)}`);
            }
          } catch (_) {}
        }
      }
    } catch (err) {
      try {
        if (process && typeof process._rawDebug === "function") {
          process._rawDebug(`[OHcode][loadenv-post] fs unavailable ${phase} ${errorText(err)}`);
        }
      } catch (_) {}
    }
  }
  try {
    const fs = require("fs");
    const path = require("path");
    const enableRescue =
      !!(process.env && process.env.OHCODE_POSTLOAD_RESCUE === "1");
    let hidden = {};
    try {
      const v8Util = process._linkedBinding("electron_common_v8_util");
      hidden = {
        appSearchPaths: v8Util.getHiddenValue(global, "appSearchPaths"),
        appSearchPathsOnlyLoadASAR: v8Util.getHiddenValue(
          global,
          "appSearchPathsOnlyLoadASAR"
        )
      };
    } catch (err) {
      hidden = { error: errorText(err) };
    }

    const roots = [
      process.resourcesPath || "",
      "/data/storage/el1/bundle/electron/resources/resfile/resources",
      "/data/storage/el1/bundle/electron/resources/resfile"
    ].filter(Boolean);
    const rels = [
      "app.asar/package.json",
      "app/package.json",
      "resources/app/package.json",
      "app/ohcode-entry-probe.js",
      "resources/app/ohcode-entry-probe.js"
    ];
    const exists = [];
    for (const root of roots) {
      for (const rel of rels) {
        const candidate = path.join(root, rel);
        exists.push([candidate, fs.existsSync(candidate)]);
      }
    }

    trace(
      "state",
      stringify({
        resourcesPath: process.resourcesPath || "",
        argv: process.argv,
        execPath: process.execPath || "",
        cwd: typeof process.cwd === "function" ? process.cwd() : "",
        modulePaths:
          typeof module !== "undefined" && module && module.paths
            ? module.paths
            : [],
        appCodeLoadedType: typeof process.appCodeLoaded,
        electronVersion: (process.versions && process.versions.electron) || "",
        postLoadRescueEnabled: enableRescue,
        hidden,
        exists
      })
    );

    const appDirs = [
      process.resourcesPath
        ? path.join(process.resourcesPath, "app")
        : "",
      process.resourcesPath
        ? path.join(process.resourcesPath, "resources", "app")
        : "",
      "/data/storage/el1/bundle/electron/resources/resfile/resources/app",
      "/data/storage/el1/bundle/electron/resources/resfile/app"
    ].filter(Boolean);
    const candidates = appDirs.map((appDir) => {
      const entry = path.join(appDir, "ohcode-entry-probe.js");
      const pkg = path.join(appDir, "package.json");
      return {
        appDir,
        entry,
        pkg,
        entryExists: fs.existsSync(entry),
        packageExists: fs.existsSync(pkg)
      };
    });
    for (const candidate of candidates) {
      trace(
        "rescue candidate",
        `${candidate.entry} exists=${candidate.entryExists} pkg=${candidate.packageExists}`
      );
    }
    if (!enableRescue) {
      trace("rescue disabled", stringify(candidates));
    } else {
      const candidate = candidates.find((item) => item.entryExists);
      if (!candidate) {
        trace("rescue missing", stringify(candidates));
      } else {
        const schedule =
          typeof setImmediate === "function" ? setImmediate : setTimeout;
        trace("rescue scheduled", candidate.entry);
        schedule(() => {
          try {
            const electron = require("electron");
            if (
              electron &&
              electron.app &&
              typeof electron.app.setAppPath === "function"
            ) {
              electron.app.setAppPath(candidate.appDir);
              trace("rescue setAppPath", candidate.appDir);
            }
          } catch (err) {
            trace("rescue setAppPath failed", errorText(err));
          }
          try {
            require(candidate.entry);
            trace("rescue loaded", candidate.entry);
          } catch (err) {
            trace("rescue load failed", errorText(err));
          }
        }, 0);
      }
    }
  } catch (err) {
    trace("error", errorText(err));
  }
})();
)OHCODE_JS";
constexpr size_t kV8CreateParamsSlotDumpCount = 32;
constexpr int kContextSnapshotReplacementMinBytes = 100000;
constexpr const char* kDefaultElectronResourceDir =
    "/data/storage/el1/bundle/electron/resources/resfile";
constexpr const char* kStartupSnapshotBlobFileName = "snapshot_blob.bin";
constexpr uintptr_t kSnapshotVectorAllocReturnOffset = 0x244f3e8;
constexpr uintptr_t kSnapshotVectorRetryAllocReturnOffset = 0x244f420;
constexpr uintptr_t kSnapshotDataAllocReturnOffset = 0x244f520;
constexpr int kDefaultSnapshotMmapMinBytes = 1024 * 1024;
constexpr int kDefaultSnapshotMmapMaxBytes = 768 * 1024 * 1024;
constexpr size_t kMaxTrackedMmapAllocations = 128;
constexpr uintptr_t kNodeInitializeContextOffset = 0x857edec;
// node::SetIsolateUpForNode calls platform->RegisterIsolate(isolate, loop)
// through the NodePlatform/MultiIsolatePlatform vtable at byte offset 0xe0.
// Calling libelectron's internal map-insert helper directly is not ABI-safe.
constexpr size_t kNodePlatformRegisterIsolateVtableOffset = 0xe0;
// node::NodePlatform::ForIsolate(v8::Isolate*) aborts if the isolate is not in
// the platform map. Hook it too because the renderer can reach this lookup
// outside node::InitializeContext.
constexpr uintptr_t kNodePlatformForIsolateOffset = 0x868bb68;
constexpr uintptr_t kNodePlatformMapLookupOffset = 0x2cf09c4;
constexpr uintptr_t kNodePlatformMapLookupAuxOffset = 0x176e178;
constexpr size_t kNodePlatformMutexOffset = 0x8;
constexpr size_t kNodePlatformMapOffset = 0x30;
constexpr size_t kNodePlatformLookupDataOffset = 0x18;
constexpr size_t kNodePlatformLookupControlOffset = 0x28;
constexpr size_t kMaxNodePlatformHookIsolates = 256;
constexpr size_t kAarch64InlineBranchBytes = 16;
constexpr uint32_t kAarch64LdrX16Literal8 = 0x58000050u;
constexpr uint32_t kAarch64BrX16 = 0xd61f0200u;

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

using EpollWaitFn = int (*)(int, struct epoll_event*, int, int);
using V8IsolateInitializeFn = bool (*)(void*, const void*);
using V8SetFlagsFromStringFn = void (*)(const char*);
using V8SetSnapshotDataBlobFn = void (*)(void*);
using V8InitializeExternalStartupDataFn = void (*)(const char*);
using V8InternalInitWithSnapshotFn =
    bool (*)(void*, void*, void*, void*, bool);
using NothrowNewFn = void* (*)(size_t, const void*) noexcept;
using DeleteFn = void (*)(void*) noexcept;
using NodeInitializeContextFn = uint32_t (*)(void*);
using V8ContextGetIsolateFn = void* (*)(void*);
using V8GetCurrentPlatformFn = void* (*)();
using UvDefaultLoopFn = void* (*)();
using UvMutexFn = void (*)(void*);
using NodePlatformRegisterIsolateFn = void (*)(void*, void*, void*);
using NodePlatformForIsolateFn = void* (*)(void*, void*);
using NodePlatformMapLookupFn =
    void* (*)(void*, void*, void*, void*, void*);
using AdapterArgVector = std::vector<std::string>;
using AdapterFdVector = std::vector<std::pair<int, int>>;
using AdapterStartChildProcess2Fn =
    int (*)(void*, const AdapterArgVector&, const AdapterFdVector&);
using AdapterStartChildProcess3Fn =
    int (*)(void*, const AdapterArgVector&, const AdapterFdVector&,
            const std::string&);

struct V8OwnedByteVector {
    void* vtable;
    uint8_t* data;
    uint32_t length;
    uint8_t ownsData;
    uint8_t padding[3];
};

static_assert(sizeof(V8OwnedByteVector) == 24,
              "V8OwnedByteVector layout must match the disassembly");

using SnapshotDecompressFn = V8OwnedByteVector (*)(const uint8_t*, size_t);
using NodeNewContextFn = void* (*)(void*, void*);
using NodeCreateEnvironmentFn =
    void* (*)(void*, void*, void*, void*, void*, void*, void*);
using NodeLoadEnvironmentStringFn = void* (*)(void*, const char*);
using NodeLoadEnvironmentCallbackFn = void* (*)(void*, void*, void*);

struct V8StartupDataView {
    const char* data;
    int raw_size;
};

struct V8SnapshotDataView {
    void* vtable;
    const uint8_t* data;
    uint32_t length;
    uint8_t ownsData;
    uint8_t padding[3];
};

static_assert(sizeof(V8SnapshotDataView) == 24,
              "V8SnapshotDataView layout must match the disassembly");

static std::once_flag g_epollConfigOnce;
static std::once_flag g_realEpollWaitOnce;
static EpollWaitFn g_realEpollWait = nullptr;
static std::atomic<bool> g_epollHookEnabled{true};
static std::atomic<bool> g_requireChromeIoThreadName{true};
static std::atomic<int> g_chromeIoThreadMaxWaitMs{
    kDefaultChromeIoThreadMaxWaitMs};
static std::atomic<uint64_t> g_epollTargetHits{0};
static std::atomic<uint64_t> g_epollClampHits{0};
static std::atomic<uint64_t> g_epollPassThroughHits{0};
static std::atomic<int> g_lastOriginalEpollTimeoutMs{0};
static std::atomic<int> g_lastEffectiveEpollTimeoutMs{0};
static std::atomic<uintptr_t> g_lastEpollCallerOffset{0};
static std::atomic<void*> g_gotRealEpollWait{nullptr};

static std::once_flag g_v8InitializeConfigOnce;
static std::once_flag g_realV8InitializeOnce;
static std::once_flag g_v8FlagsOnce;
static std::once_flag g_realV8SetSnapshotDataBlobOnce;
static std::once_flag g_realV8InitializeExternalStartupDataOnce;
static std::once_flag g_realV8InitializeExternalStartupDataFromFileOnce;
static std::once_flag g_realV8InternalInitWithSnapshotOnce;
static std::once_flag g_baseStartupBlobOnce;
static V8IsolateInitializeFn g_realV8IsolateInitialize = nullptr;
static V8SetSnapshotDataBlobFn g_realV8SetSnapshotDataBlob = nullptr;
static V8InitializeExternalStartupDataFn g_realV8InitializeExternalStartupData =
    nullptr;
static V8InitializeExternalStartupDataFn
    g_realV8InitializeExternalStartupDataFromFile = nullptr;
static V8InternalInitWithSnapshotFn g_realV8InternalInitWithSnapshot = nullptr;
static std::mutex g_v8InitializeMutex;
static std::mutex g_v8FlagsMutex;
static std::string g_v8StartupFlags;
static std::atomic<bool> g_v8InitializeHookEnabled{true};
static std::atomic<bool> g_serializeV8Initialize{true};
static std::atomic<bool> g_forceV8InitializeSuccess{false};
static std::atomic<bool> g_v8StartupFlagsApplied{false};
static std::atomic<bool> g_v8StartupFlagsResolveFailed{false};
static std::atomic<bool> g_v8StartupFlagsSymbolResolved{false};
static std::atomic<bool> g_v8StartupFlagsEnvPresent{false};
static std::atomic<bool> g_v8StartupFlagsEnvEmpty{false};
static std::atomic<bool> g_v8StartupFlagsUsingDefault{false};
static std::atomic<uint32_t> g_v8StartupFlagsLength{0};
static std::atomic<uint64_t> g_v8StartupFlagsApplyAttempts{0};
static std::atomic<uint64_t> g_v8StartupFlagsEmptySkips{0};
static std::atomic<uint64_t> g_v8SetSnapshotDataBlobCalls{0};
static std::atomic<uintptr_t> g_lastV8SnapshotDataBlobAddress{0};
static std::atomic<uintptr_t> g_lastV8SnapshotDataBlobDataAddress{0};
static std::atomic<int32_t> g_lastV8SnapshotDataBlobRawSize{0};
static std::atomic<uintptr_t> g_lastEffectiveV8SnapshotDataBlobAddress{0};
static std::atomic<uintptr_t> g_lastEffectiveV8SnapshotDataBlobDataAddress{0};
static std::atomic<int32_t> g_lastEffectiveV8SnapshotDataBlobRawSize{0};
static std::atomic<bool> g_replaceContextSnapshotWithStartup{false};
static std::atomic<bool> g_skipV8SnapshotDataBlob{false};
static std::atomic<uint64_t> g_snapshotBlobSkips{0};
static std::atomic<uint64_t> g_snapshotBlobReplacementAttempts{0};
static std::atomic<uint64_t> g_snapshotBlobReplacements{0};
static std::atomic<uint64_t> g_snapshotBlobReplacementFailures{0};
static std::atomic<int32_t> g_baseStartupBlobRawSize{0};
static std::mutex g_snapshotBlobReplacementPathMutex;
static std::string g_snapshotBlobReplacementPath;
static std::vector<char> g_baseStartupBlobBytes;
static V8StartupDataView g_baseStartupBlobData{nullptr, 0};
static std::atomic<uint64_t> g_v8InitializeExternalStartupDataCalls{0};
static std::atomic<uint64_t> g_v8InitializeExternalStartupDataFromFileCalls{0};
static std::mutex g_v8ExternalStartupDataPathMutex;
static std::string g_lastV8ExternalStartupDataPath;
static std::atomic<uint64_t> g_v8InitWithSnapshotCalls{0};
static std::atomic<uint64_t> g_v8InitWithSnapshotSuccesses{0};
static std::atomic<uint64_t> g_v8InitWithSnapshotFailures{0};
static std::atomic<uintptr_t> g_lastV8InitWithSnapshotCallerOffset{0};
static std::atomic<uintptr_t> g_lastV8InitWithSnapshotIsolate{0};
static std::atomic<uintptr_t> g_lastV8InitWithSnapshotReadOnlyAddress{0};
static std::atomic<uintptr_t> g_lastV8InitWithSnapshotReadOnlyDataAddress{0};
static std::atomic<uint32_t> g_lastV8InitWithSnapshotReadOnlyLength{0};
static std::atomic<uintptr_t> g_lastV8InitWithSnapshotSharedAddress{0};
static std::atomic<uintptr_t> g_lastV8InitWithSnapshotSharedDataAddress{0};
static std::atomic<uint32_t> g_lastV8InitWithSnapshotSharedLength{0};
static std::atomic<uintptr_t> g_lastV8InitWithSnapshotStartupAddress{0};
static std::atomic<uintptr_t> g_lastV8InitWithSnapshotStartupDataAddress{0};
static std::atomic<uint32_t> g_lastV8InitWithSnapshotStartupLength{0};
static std::atomic<bool> g_lastV8InitWithSnapshotCanRehash{false};
static std::atomic<bool> g_lastV8InitWithSnapshotResult{false};
static std::atomic<uint64_t> g_v8InitializeCalls{0};
static std::atomic<uint64_t> g_v8InitializeTargetHits{0};
static std::atomic<uint64_t> g_v8InitializeSerializedCalls{0};
static std::atomic<uint64_t> g_v8InitializePassThroughCalls{0};
static std::atomic<uint64_t> g_v8InitializeFailures{0};
static std::atomic<uint64_t> g_v8InitializeForcedSuccesses{0};
static std::atomic<bool> g_disableNodeStartupSnapshot{false};
static std::atomic<uint64_t> g_v8SnapshotBlobClears{0};
static std::atomic<uint64_t> g_v8SnapshotBlobNulls{0};
static std::atomic<uintptr_t> g_lastV8CreateParamsAddress{0};
static std::atomic<uintptr_t> g_lastV8CreateParamsSlot0{0};
static std::atomic<uintptr_t> g_lastV8CreateParamsSlot1{0};
static std::atomic<uintptr_t> g_lastV8CreateParamsSlot2{0};
static std::atomic<uintptr_t> g_lastV8CreateParamsSlot3{0};
static std::atomic<uintptr_t> g_lastV8CreateParamsSlot4{0};
static std::atomic<uintptr_t> g_lastV8CreateParamsSlot5{0};
static std::atomic<uintptr_t> g_lastV8SnapshotBlobAddress{0};
static std::mutex g_createParamsDumpMutex;
static std::string g_lastV8CreateParamsSlotsHex;
static std::atomic<uint32_t> g_activeV8Initializations{0};
static std::atomic<uint32_t> g_maxConcurrentV8Initializations{0};
static std::atomic<uintptr_t> g_lastV8InitializeCallerOffset{0};
static thread_local bool g_insideV8InitializeHook = false;

static std::once_flag g_snapshotAllocConfigOnce;
static std::once_flag g_nodePlatformHookConfigOnce;
static std::once_flag g_adapterChildHookConfigOnce;
static std::once_flag g_realArrayNothrowNewOnce;
static std::once_flag g_realScalarNothrowNewOnce;
static std::once_flag g_realArrayDeleteOnce;
static std::once_flag g_realScalarDeleteOnce;
static std::once_flag g_realSnapshotDecompressOnce;
static std::once_flag g_realNodeInitializeContextOnce;
static std::once_flag g_realNodeNewContextOnce;
static std::once_flag g_realNodeCreateEnvironmentOnce;
static std::once_flag g_realNodeLoadEnvironmentStringOnce;
static std::once_flag g_realNodeLoadEnvironmentCallbackOnce;
static std::once_flag g_realAdapterStartGpuProcessOnce;
static std::once_flag g_realAdapterStartLegacyChildProcessOnce;
static std::once_flag g_realAdapterStartNormalChildProcessOnce;
static std::once_flag g_realAdapterStartIsolateChildProcessOnce;
static std::once_flag g_nodePlatformSymbolsOnce;
static std::once_flag g_nodePlatformLookupSymbolsOnce;
static std::mutex g_nodeInitializeContextInlineHookMutex;
static NothrowNewFn g_realArrayNothrowNew = nullptr;
static NothrowNewFn g_realScalarNothrowNew = nullptr;
static DeleteFn g_realArrayDelete = nullptr;
static DeleteFn g_realScalarDelete = nullptr;
static SnapshotDecompressFn g_realSnapshotDecompress = nullptr;
static NodeInitializeContextFn g_realNodeInitializeContext = nullptr;
static NodeNewContextFn g_realNodeNewContext = nullptr;
static NodeCreateEnvironmentFn g_realNodeCreateEnvironment = nullptr;
static NodeLoadEnvironmentStringFn g_realNodeLoadEnvironmentString = nullptr;
static NodeLoadEnvironmentCallbackFn g_realNodeLoadEnvironmentCallback =
    nullptr;
static V8ContextGetIsolateFn g_v8ContextGetIsolate = nullptr;
static V8GetCurrentPlatformFn g_v8GetCurrentPlatform = nullptr;
static UvDefaultLoopFn g_uvDefaultLoop = nullptr;
static UvMutexFn g_uvMutexLock = nullptr;
static UvMutexFn g_uvMutexUnlock = nullptr;
static NodePlatformMapLookupFn g_nodePlatformMapLookup = nullptr;
static void* g_nodePlatformMapLookupAux = nullptr;
static NodePlatformForIsolateFn g_realNodePlatformForIsolate = nullptr;
static AdapterStartChildProcess2Fn g_realAdapterStartGpuProcess = nullptr;
static AdapterStartChildProcess2Fn g_realAdapterStartLegacyChildProcess =
    nullptr;
static AdapterStartChildProcess3Fn g_realAdapterStartNormalChildProcess =
    nullptr;
static AdapterStartChildProcess3Fn g_realAdapterStartIsolateChildProcess =
    nullptr;
static std::atomic<void*> g_gotRealV8IsolateInitialize{nullptr};
static std::atomic<void*> g_gotRealV8SetSnapshotDataBlob{nullptr};
static std::atomic<void*> g_gotRealV8InitializeExternalStartupData{nullptr};
static std::atomic<void*> g_gotRealV8InitializeExternalStartupDataFromFile{
    nullptr};
static std::atomic<void*> g_gotRealV8InternalInitWithSnapshot{nullptr};
static std::atomic<void*> g_gotRealSnapshotDecompress{nullptr};
static std::atomic<void*> g_gotRealArrayNothrowNew{nullptr};
static std::atomic<void*> g_gotRealScalarNothrowNew{nullptr};
static std::atomic<void*> g_gotRealArrayDelete{nullptr};
static std::atomic<void*> g_gotRealScalarDelete{nullptr};
static std::atomic<void*> g_gotRealNodeInitializeContext{nullptr};
static std::atomic<void*> g_gotRealNodeNewContext{nullptr};
static std::atomic<void*> g_gotRealNodeCreateEnvironment{nullptr};
static std::atomic<void*> g_gotRealNodeLoadEnvironmentString{nullptr};
static std::atomic<void*> g_gotRealNodeLoadEnvironmentCallback{nullptr};
static std::atomic<void*> g_nodePlatformForIsolateInlineTrampoline{nullptr};
static std::atomic<void*> g_gotRealAdapterStartGpuProcess{nullptr};
static std::atomic<void*> g_gotRealAdapterStartLegacyChildProcess{nullptr};
static std::atomic<void*> g_gotRealAdapterStartNormalChildProcess{nullptr};
static std::atomic<void*> g_gotRealAdapterStartIsolateChildProcess{nullptr};
static std::atomic<void*> g_nodeInitializeContextInlineTrampoline{nullptr};
constexpr uintptr_t kReservedMmapAllocationPtrValue = UINTPTR_MAX;
static std::atomic<void*> g_mmapAllocationPtrs[kMaxTrackedMmapAllocations];
static std::atomic<size_t> g_mmapAllocationSizes[kMaxTrackedMmapAllocations];
static std::atomic<void*>
    g_nodePlatformRegisteredIsolates[kMaxNodePlatformHookIsolates];
static std::atomic<uint32_t> g_activeMmapAllocationCount{0};
static std::atomic<bool> g_snapshotMmapFallbackEnabled{true};
static std::atomic<bool> g_nodePlatformHookEnabled{false};
static std::atomic<bool> g_nodePlatformRegisterOnLookupEnabled{true};
static std::atomic<bool> g_adapterCrashpadBlockEnabled{true};
static std::atomic<bool> g_nodePlatformResolveFailed{false};
static std::atomic<int> g_snapshotMmapMinBytes{kDefaultSnapshotMmapMinBytes};
static std::atomic<int> g_snapshotMmapMaxBytes{kDefaultSnapshotMmapMaxBytes};
static std::atomic<uint64_t> g_snapshotNothrowNewCalls{0};
static std::atomic<uint64_t> g_snapshotNothrowNewFailures{0};
static std::atomic<uint64_t> g_snapshotMmapFallbacks{0};
static std::atomic<uint64_t> g_snapshotMmapFallbackFailures{0};
static std::atomic<uint64_t> g_snapshotMmapDeletes{0};
static std::atomic<uint64_t> g_snapshotMmapBytes{0};
static std::atomic<uint64_t> g_snapshotDecompressCalls{0};
static std::atomic<uint64_t> g_snapshotDecompressBytesIn{0};
static std::atomic<uint64_t> g_nodeInitializeContextCalls{0};
static std::atomic<uint64_t> g_nodeNewContextCalls{0};
static std::atomic<uint64_t> g_nodeNewContextNulls{0};
static std::atomic<uint64_t> g_nodeCreateEnvironmentCalls{0};
static std::atomic<uint64_t> g_nodeCreateEnvironmentNulls{0};
static std::atomic<uint64_t> g_nodeLoadEnvironmentStringCalls{0};
static std::atomic<uint64_t> g_nodeLoadEnvironmentCallbackCalls{0};
static std::atomic<uint64_t> g_nodeLoadEnvironmentNulls{0};
static std::atomic<uint64_t> g_nodePostLoadTraceAttempts{0};
static std::atomic<uint64_t> g_nodePostLoadTraceSuccesses{0};
static std::atomic<uint64_t> g_nodePostLoadTraceFailures{0};
static std::atomic<uint64_t> g_nodeInitializeContextInlineFailures{0};
static std::atomic<uint64_t> g_nodePlatformForIsolateInlineFailures{0};
static std::atomic<uint64_t> g_nodePlatformRegisterAttempts{0};
static std::atomic<uint64_t> g_nodePlatformRegisterSuccesses{0};
static std::atomic<uint64_t> g_nodePlatformRegisterDuplicateSkips{0};
static std::atomic<uint64_t> g_nodePlatformRegisterMissingVtable{0};
static std::atomic<uint64_t> g_nodePlatformForIsolateCalls{0};
static std::atomic<uint64_t> g_nodePlatformLookupHits{0};
static std::atomic<uint64_t> g_nodePlatformLookupMisses{0};
static std::atomic<uint64_t> g_nodePlatformLookupFallbacks{0};
static std::atomic<uint64_t> g_nodePlatformLookupFakeFallbacks{0};
static std::atomic<uint64_t> g_adapterChildProcessCalls{0};
static std::atomic<uint64_t> g_adapterChildProcessPassThrough{0};
static std::atomic<uint64_t> g_adapterCrashpadBlocks{0};
static std::atomic<uint32_t> g_lastSnapshotCompressedSize{0};
static std::atomic<uint32_t> g_lastSnapshotDecompressedSize{0};
static std::atomic<uintptr_t> g_lastSnapshotAllocCallerOffset{0};
static std::atomic<uintptr_t> g_lastNodePlatformIsolate{0};
static std::atomic<uintptr_t> g_lastNodePlatformAddress{0};
static std::atomic<uintptr_t> g_lastNodePlatformRegisterAddress{0};
static std::atomic<uintptr_t> g_lastNodePlatformDataAddress{0};
static std::atomic<uintptr_t> g_lastNodeNewContextIsolate{0};
static std::atomic<uintptr_t> g_lastNodeNewContextTemplate{0};
static std::atomic<uintptr_t> g_lastNodeNewContextResult{0};
static std::atomic<uintptr_t> g_lastNodeCreateEnvironmentIsolateData{0};
static std::atomic<uintptr_t> g_lastNodeCreateEnvironmentContext{0};
static std::atomic<uintptr_t> g_lastNodeCreateEnvironmentResult{0};
static std::atomic<uintptr_t> g_lastNodeLoadEnvironmentEnv{0};
static std::atomic<uintptr_t> g_lastNodeLoadEnvironmentSource{0};
static std::atomic<uintptr_t> g_lastNodeLoadEnvironmentPreload{0};
static std::atomic<uintptr_t> g_lastNodeLoadEnvironmentResult{0};
static std::atomic<void*> g_lastNodePlatformData{nullptr};
static std::atomic<void*> g_lastNodePlatformDataPlatform{nullptr};
static std::atomic<uint64_t> g_electronPltPatchAttempts{0};
static std::atomic<uint64_t> g_electronPltPatchedSlots{0};
static std::atomic<uint64_t> g_electronPltPatchFailures{0};
static std::atomic<uint32_t> g_electronPltPatchRuns{0};
static std::atomic<bool> g_electronPltPatchInstalled{false};
static std::atomic<bool> g_electronPltPatchThreadStarted{false};
static std::atomic<bool> g_electronPreloadAttempted{false};
static std::atomic<bool> g_electronPreloadSucceeded{false};
static void* g_electronPreloadHandle = nullptr;
static std::atomic<bool> g_nodeInitializeContextInlineInstalled{false};
static std::atomic<bool> g_nodePlatformForIsolateInlineInstalled{false};
static std::atomic<uint32_t> g_patchedEpollWaitSlots{0};
static std::atomic<uint32_t> g_patchedV8InitializeSlots{0};
static std::atomic<uint32_t> g_patchedV8SetSnapshotDataBlobSlots{0};
static std::atomic<uint32_t> g_patchedV8InitializeExternalStartupDataSlots{0};
static std::atomic<uint32_t>
    g_patchedV8InitializeExternalStartupDataFromFileSlots{0};
static std::atomic<uint32_t> g_patchedV8InternalInitWithSnapshotSlots{0};
static std::atomic<uint32_t> g_patchedSnapshotDecompressSlots{0};
static std::atomic<uint32_t> g_patchedArrayNothrowNewSlots{0};
static std::atomic<uint32_t> g_patchedScalarNothrowNewSlots{0};
static std::atomic<uint32_t> g_patchedArrayDeleteSlots{0};
static std::atomic<uint32_t> g_patchedScalarDeleteSlots{0};
static std::atomic<uint32_t> g_patchedNodeInitializeContextSlots{0};
static std::atomic<uint32_t> g_patchedNodeNewContextSlots{0};
static std::atomic<uint32_t> g_patchedNodeCreateEnvironmentSlots{0};
static std::atomic<uint32_t> g_patchedNodeLoadEnvironmentStringSlots{0};
static std::atomic<uint32_t> g_patchedNodeLoadEnvironmentCallbackSlots{0};
static std::atomic<uint32_t> g_patchedNodeInitializeContextInlineEntrypoints{0};
static std::atomic<uint32_t> g_patchedNodePlatformForIsolateInlineEntrypoints{0};
static std::atomic<uint32_t> g_patchedAdapterStartGpuProcessSlots{0};
static std::atomic<uint32_t> g_patchedAdapterStartLegacyChildProcessSlots{0};
static std::atomic<uint32_t> g_patchedAdapterStartNormalChildProcessSlots{0};
static std::atomic<uint32_t> g_patchedAdapterStartIsolateChildProcessSlots{0};
static thread_local bool g_insideNodeInitializeContextHook = false;
static thread_local bool g_insideNodePlatformForIsolateHook = false;

// Logging
void Log(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    fprintf(stderr, "[V8PoolHook] %s", message);
    fprintf(stderr, "\n");
#if OHCODE_HAS_HILOG
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "V8PoolHook", "%{public}s",
                 message);
#endif
}

static bool ReadEnvBool(const char* name, bool default_value) {
    const char* value = getenv(name);
    if (!value || value[0] == '\0') {
        return default_value;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
        strcmp(value, "FALSE") == 0 || strcmp(value, "off") == 0 ||
        strcmp(value, "OFF") == 0) {
        return false;
    }
    return true;
}

static int ReadEnvInt(const char* name, int default_value, int min_value,
                      int max_value) {
    const char* value = getenv(name);
    if (!value || value[0] == '\0') {
        return default_value;
    }

    char* end = nullptr;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return default_value;
    }
    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return static_cast<int>(parsed);
}

static uint32_t StringLengthForStats(const std::string& value) {
    constexpr size_t kMaxUint32Size = 0xffffffffu;
    if (value.size() > kMaxUint32Size) {
        return 0xffffffffu;
    }
    return static_cast<uint32_t>(value.size());
}

static void InitEpollHookConfig() {
    g_epollHookEnabled.store(
        ReadEnvBool("V8_POOL_HOOK_EPOLL_ENABLE", true),
        std::memory_order_relaxed);
    g_requireChromeIoThreadName.store(
        ReadEnvBool("V8_POOL_HOOK_REQUIRE_IO_THREAD", true),
        std::memory_order_relaxed);
    g_chromeIoThreadMaxWaitMs.store(
        ReadEnvInt("V8_POOL_HOOK_IO_EPOLL_MAX_MS",
                   kDefaultChromeIoThreadMaxWaitMs,
                   kMinChromeIoThreadMaxWaitMs,
                   kMaxChromeIoThreadMaxWaitMs),
        std::memory_order_relaxed);
}

static void InitV8InitializeHookConfig() {
    g_v8InitializeHookEnabled.store(
        ReadEnvBool("V8_POOL_HOOK_ISOLATE_INIT_ENABLE", true),
        std::memory_order_relaxed);
    g_serializeV8Initialize.store(
        ReadEnvBool("V8_POOL_HOOK_SERIALIZE_ISOLATE_INIT", true),
        std::memory_order_relaxed);
    g_forceV8InitializeSuccess.store(
        ReadEnvBool("V8_POOL_HOOK_FORCE_ISOLATE_INIT_SUCCESS", true),
        std::memory_order_relaxed);
    g_disableNodeStartupSnapshot.store(
        ReadEnvBool("V8_POOL_HOOK_DISABLE_NODE_STARTUP_SNAPSHOT", false),
        std::memory_order_relaxed);
    g_replaceContextSnapshotWithStartup.store(
        ReadEnvBool("V8_POOL_HOOK_REPLACE_CONTEXT_SNAPSHOT_WITH_STARTUP",
                    false),
        std::memory_order_relaxed);
    g_skipV8SnapshotDataBlob.store(
        ReadEnvBool("V8_POOL_HOOK_SKIP_SNAPSHOT_BLOB", false),
        std::memory_order_relaxed);

    const char* flags = getenv("V8_POOL_HOOK_V8_FLAGS");
    const bool envPresent = flags != nullptr;
    const bool envEmpty = envPresent && flags[0] == '\0';
    g_v8StartupFlagsEnvPresent.store(envPresent, std::memory_order_relaxed);
    g_v8StartupFlagsEnvEmpty.store(envEmpty, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_v8FlagsMutex);
    g_v8StartupFlags =
        (!envPresent || envEmpty) ? kDefaultV8StartupFlags : flags;
    g_v8StartupFlagsUsingDefault.store(!envPresent || envEmpty,
                                       std::memory_order_relaxed);
    g_v8StartupFlagsLength.store(StringLengthForStats(g_v8StartupFlags),
                                 std::memory_order_relaxed);
}

static void InitSnapshotAllocHookConfig() {
    g_snapshotMmapFallbackEnabled.store(
        ReadEnvBool("V8_POOL_HOOK_SNAPSHOT_MMAP_ENABLE", true),
        std::memory_order_relaxed);
    g_snapshotMmapMinBytes.store(
        ReadEnvInt("V8_POOL_HOOK_SNAPSHOT_MMAP_MIN_BYTES",
                   kDefaultSnapshotMmapMinBytes, 0,
                   kDefaultSnapshotMmapMaxBytes),
        std::memory_order_relaxed);
    g_snapshotMmapMaxBytes.store(
        ReadEnvInt("V8_POOL_HOOK_SNAPSHOT_MMAP_MAX_BYTES",
                   kDefaultSnapshotMmapMaxBytes, kDefaultSnapshotMmapMinBytes,
                   2147483647),
        std::memory_order_relaxed);
}

static void InitNodePlatformHookConfig() {
    // HarmonyOS phone runs the renderer in-process and hits node::InitializeContext
    // before NodePlatform has foreground task data for that renderer isolate.
    // Direct pre-registration is kept behind an env switch. Calling the internal
    // RegisterIsolate path with the wrong platform subobject aborts during
    // v8::Isolate::Initialize, so the default path must not register.
    g_nodePlatformHookEnabled.store(
        ReadEnvBool("V8_POOL_HOOK_NODE_PLATFORM_REGISTER_ENABLE", false),
        std::memory_order_relaxed);
    g_nodePlatformRegisterOnLookupEnabled.store(
        ReadEnvBool("V8_POOL_HOOK_NODE_PLATFORM_REGISTER_ON_LOOKUP_ENABLE",
                    true),
        std::memory_order_relaxed);
}

static void InitAdapterChildHookConfig() {
    g_adapterCrashpadBlockEnabled.store(
        ReadEnvBool("V8_POOL_HOOK_BLOCK_CRASHPAD_CHILD", true),
        std::memory_order_relaxed);
}

static EpollWaitFn GetRealEpollWait() {
    if (void* gotReal = g_gotRealEpollWait.load(std::memory_order_acquire)) {
        return reinterpret_cast<EpollWaitFn>(gotReal);
    }
    std::call_once(g_realEpollWaitOnce, []() {
        g_realEpollWait = reinterpret_cast<EpollWaitFn>(
            dlsym(RTLD_NEXT, "epoll_wait"));
    });
    return g_realEpollWait;
}

static V8IsolateInitializeFn GetRealV8IsolateInitialize() {
    if (void* gotReal =
            g_gotRealV8IsolateInitialize.load(std::memory_order_acquire)) {
        return reinterpret_cast<V8IsolateInitializeFn>(gotReal);
    }
    std::call_once(g_realV8InitializeOnce, []() {
        g_realV8IsolateInitialize = reinterpret_cast<V8IsolateInitializeFn>(
            dlsym(RTLD_NEXT,
                  "_ZN2v87Isolate10InitializeEPS0_RKNS0_12CreateParamsE"));
        if (!g_realV8IsolateInitialize) {
            Log("WARNING: v8::Isolate::Initialize real symbol not found");
        }
    });
    return g_realV8IsolateInitialize;
}

static V8SetSnapshotDataBlobFn GetRealV8SetSnapshotDataBlob() {
    if (void* gotReal =
            g_gotRealV8SetSnapshotDataBlob.load(std::memory_order_acquire)) {
        return reinterpret_cast<V8SetSnapshotDataBlobFn>(gotReal);
    }
    std::call_once(g_realV8SetSnapshotDataBlobOnce, []() {
        g_realV8SetSnapshotDataBlob =
            reinterpret_cast<V8SetSnapshotDataBlobFn>(
                dlsym(RTLD_NEXT, "_ZN2v82V819SetSnapshotDataBlobEPNS_11StartupDataE"));
        if (!g_realV8SetSnapshotDataBlob) {
            Log("WARNING: v8::V8::SetSnapshotDataBlob real symbol not found");
        }
    });
    return g_realV8SetSnapshotDataBlob;
}

static V8InitializeExternalStartupDataFn
GetRealV8InitializeExternalStartupData() {
    if (void* gotReal = g_gotRealV8InitializeExternalStartupData.load(
            std::memory_order_acquire)) {
        return reinterpret_cast<V8InitializeExternalStartupDataFn>(gotReal);
    }
    std::call_once(g_realV8InitializeExternalStartupDataOnce, []() {
        g_realV8InitializeExternalStartupData =
            reinterpret_cast<V8InitializeExternalStartupDataFn>(
                dlsym(RTLD_NEXT, "_ZN2v82V829InitializeExternalStartupDataEPKc"));
        if (!g_realV8InitializeExternalStartupData) {
            Log("WARNING: v8::V8::InitializeExternalStartupData real symbol not found");
        }
    });
    return g_realV8InitializeExternalStartupData;
}

static V8InitializeExternalStartupDataFn
GetRealV8InitializeExternalStartupDataFromFile() {
    if (void* gotReal = g_gotRealV8InitializeExternalStartupDataFromFile.load(
            std::memory_order_acquire)) {
        return reinterpret_cast<V8InitializeExternalStartupDataFn>(gotReal);
    }
    std::call_once(g_realV8InitializeExternalStartupDataFromFileOnce, []() {
        g_realV8InitializeExternalStartupDataFromFile =
            reinterpret_cast<V8InitializeExternalStartupDataFn>(
                dlsym(RTLD_NEXT, "_ZN2v82V837InitializeExternalStartupDataFromFileEPKc"));
        if (!g_realV8InitializeExternalStartupDataFromFile) {
            Log("WARNING: v8::V8::InitializeExternalStartupDataFromFile real symbol not found");
        }
    });
    return g_realV8InitializeExternalStartupDataFromFile;
}

static V8InternalInitWithSnapshotFn GetRealV8InternalInitWithSnapshot() {
    if (void* gotReal =
            g_gotRealV8InternalInitWithSnapshot.load(
                std::memory_order_acquire)) {
        return reinterpret_cast<V8InternalInitWithSnapshotFn>(gotReal);
    }
    std::call_once(g_realV8InternalInitWithSnapshotOnce, []() {
        g_realV8InternalInitWithSnapshot =
            reinterpret_cast<V8InternalInitWithSnapshotFn>(dlsym(
                RTLD_NEXT,
                "_ZN2v88internal7Isolate16InitWithSnapshotEPNS0_12SnapshotDataES3_S3_b"));
        if (!g_realV8InternalInitWithSnapshot) {
            Log("WARNING: v8::internal::Isolate::InitWithSnapshot real symbol not found");
        }
    });
    return g_realV8InternalInitWithSnapshot;
}

static NothrowNewFn GetRealArrayNothrowNew() {
    if (void* gotReal =
            g_gotRealArrayNothrowNew.load(std::memory_order_acquire)) {
        return reinterpret_cast<NothrowNewFn>(gotReal);
    }
    std::call_once(g_realArrayNothrowNewOnce, []() {
        g_realArrayNothrowNew = reinterpret_cast<NothrowNewFn>(
            dlsym(RTLD_NEXT, "_ZnamRKSt9nothrow_t"));
        if (!g_realArrayNothrowNew) {
            Log("WARNING: operator new[](nothrow) real symbol not found");
        }
    });
    return g_realArrayNothrowNew;
}

static NothrowNewFn GetRealScalarNothrowNew() {
    if (void* gotReal =
            g_gotRealScalarNothrowNew.load(std::memory_order_acquire)) {
        return reinterpret_cast<NothrowNewFn>(gotReal);
    }
    std::call_once(g_realScalarNothrowNewOnce, []() {
        g_realScalarNothrowNew = reinterpret_cast<NothrowNewFn>(
            dlsym(RTLD_NEXT, "_ZnwmRKSt9nothrow_t"));
        if (!g_realScalarNothrowNew) {
            Log("WARNING: operator new(nothrow) real symbol not found");
        }
    });
    return g_realScalarNothrowNew;
}

static DeleteFn GetRealArrayDelete() {
    if (void* gotReal =
            g_gotRealArrayDelete.load(std::memory_order_acquire)) {
        return reinterpret_cast<DeleteFn>(gotReal);
    }
    std::call_once(g_realArrayDeleteOnce, []() {
        g_realArrayDelete =
            reinterpret_cast<DeleteFn>(dlsym(RTLD_NEXT, "_ZdaPv"));
        if (!g_realArrayDelete) {
            Log("WARNING: operator delete[] real symbol not found");
        }
    });
    return g_realArrayDelete;
}

static DeleteFn GetRealScalarDelete() {
    if (void* gotReal =
            g_gotRealScalarDelete.load(std::memory_order_acquire)) {
        return reinterpret_cast<DeleteFn>(gotReal);
    }
    std::call_once(g_realScalarDeleteOnce, []() {
        g_realScalarDelete =
            reinterpret_cast<DeleteFn>(dlsym(RTLD_NEXT, "_ZdlPv"));
        if (!g_realScalarDelete) {
            Log("WARNING: operator delete real symbol not found");
        }
    });
    return g_realScalarDelete;
}

static SnapshotDecompressFn GetRealSnapshotDecompress() {
    if (void* gotReal =
            g_gotRealSnapshotDecompress.load(std::memory_order_acquire)) {
        return reinterpret_cast<SnapshotDecompressFn>(gotReal);
    }
    std::call_once(g_realSnapshotDecompressOnce, []() {
        g_realSnapshotDecompress =
            reinterpret_cast<SnapshotDecompressFn>(dlsym(
                RTLD_NEXT,
                "_ZN2v88internal19SnapshotCompression10DecompressENS_4base6VectorIKhEE"));
        if (!g_realSnapshotDecompress) {
            Log("WARNING: SnapshotCompression::Decompress real symbol not found");
        }
    });
    return g_realSnapshotDecompress;
}

static NodeInitializeContextFn GetRealNodeInitializeContext() {
    if (void* trampoline =
            g_nodeInitializeContextInlineTrampoline.load(
                std::memory_order_acquire)) {
        return reinterpret_cast<NodeInitializeContextFn>(trampoline);
    }

    void* gotReal =
        g_gotRealNodeInitializeContext.load(std::memory_order_acquire);
    if (gotReal && gotReal !=
                       reinterpret_cast<void*>(
                           &_ZN4node17InitializeContextEN2v85LocalINS0_7ContextEEE)) {
        return reinterpret_cast<NodeInitializeContextFn>(gotReal);
    }
    std::call_once(g_realNodeInitializeContextOnce, []() {
        g_realNodeInitializeContext =
            reinterpret_cast<NodeInitializeContextFn>(dlsym(
                RTLD_NEXT,
                "_ZN4node17InitializeContextEN2v85LocalINS0_7ContextEEE"));
        if (!g_realNodeInitializeContext) {
            Log("WARNING: node::InitializeContext real symbol not found");
        }
    });
    return g_realNodeInitializeContext;
}

static NodeNewContextFn GetRealNodeNewContext() {
    if (void* gotReal =
            g_gotRealNodeNewContext.load(std::memory_order_acquire)) {
        return reinterpret_cast<NodeNewContextFn>(gotReal);
    }
    std::call_once(g_realNodeNewContextOnce, []() {
        g_realNodeNewContext = reinterpret_cast<NodeNewContextFn>(dlsym(
            RTLD_NEXT,
            "_ZN4node10NewContextEPN2v87IsolateENS0_5LocalINS0_14ObjectTemplateEEE"));
        if (!g_realNodeNewContext) {
            Log("WARNING: node::NewContext real symbol not found");
        }
    });
    return g_realNodeNewContext;
}

static NodeCreateEnvironmentFn GetRealNodeCreateEnvironment() {
    if (void* gotReal =
            g_gotRealNodeCreateEnvironment.load(std::memory_order_acquire)) {
        return reinterpret_cast<NodeCreateEnvironmentFn>(gotReal);
    }
    std::call_once(g_realNodeCreateEnvironmentOnce, []() {
        g_realNodeCreateEnvironment =
            reinterpret_cast<NodeCreateEnvironmentFn>(dlsym(
                RTLD_NEXT,
                "_ZN4node17CreateEnvironmentEPNS_11IsolateDataEN2v85LocalINS2_7ContextEEERKNSt4__n16vectorINS6_12basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEENSB_ISD_EEEESH_NS_16EnvironmentFlags5FlagsENS_8ThreadIdENS6_10unique_ptrINS_21InspectorParentHandleENS6_14default_deleteISM_EEEE"));
        if (!g_realNodeCreateEnvironment) {
            Log("WARNING: node::CreateEnvironment real symbol not found");
        }
    });
    return g_realNodeCreateEnvironment;
}

static void* ResolveElectronExport(const char* symbolName,
                                   const void* wrapperAddress) {
    void* symbol = nullptr;
    if (g_electronPreloadHandle) {
        symbol = dlsym(g_electronPreloadHandle, symbolName);
    }

    if (!symbol) {
        void* handle = dlopen("libelectron.so", RTLD_LAZY | RTLD_GLOBAL);
        if (handle) {
            g_electronPreloadHandle = handle;
            symbol = dlsym(handle, symbolName);
        }
    }

    if (!symbol) {
        symbol = dlsym(RTLD_DEFAULT, symbolName);
    }

    if (symbol && wrapperAddress && symbol == wrapperAddress) {
        Log("WARNING: resolved %s to wrapper address %p; ignoring",
            symbolName, symbol);
        return nullptr;
    }

    if (symbol) {
        Dl_info info;
        memset(&info, 0, sizeof(info));
        if (dladdr(symbol, &info) != 0 && info.dli_fname &&
            strstr(info.dli_fname, "libelectron.so")) {
            Log("resolved %s from %s at %p", symbolName, info.dli_fname,
                symbol);
            return symbol;
        }
        Log("WARNING: resolved %s from non-electron module %s at %p",
            symbolName, info.dli_fname ? info.dli_fname : "<unknown>",
            symbol);
    }

    return symbol;
}

static NodeLoadEnvironmentStringFn GetRealNodeLoadEnvironmentString() {
    if (void* gotReal =
            g_gotRealNodeLoadEnvironmentString.load(
                std::memory_order_acquire)) {
        return reinterpret_cast<NodeLoadEnvironmentStringFn>(gotReal);
    }
    std::call_once(g_realNodeLoadEnvironmentStringOnce, []() {
        g_realNodeLoadEnvironmentString =
            reinterpret_cast<NodeLoadEnvironmentStringFn>(ResolveElectronExport(
                "_ZN4node15LoadEnvironmentEPNS_11EnvironmentEPKc",
                reinterpret_cast<const void*>(
                    &_ZN4node15LoadEnvironmentEPNS_11EnvironmentEPKc)));
        if (!g_realNodeLoadEnvironmentString) {
            Log("WARNING: node::LoadEnvironment(string) real symbol not found");
        }
    });
    return g_realNodeLoadEnvironmentString;
}

static NodeLoadEnvironmentCallbackFn GetRealNodeLoadEnvironmentCallback() {
    if (void* gotReal =
            g_gotRealNodeLoadEnvironmentCallback.load(
                std::memory_order_acquire)) {
        return reinterpret_cast<NodeLoadEnvironmentCallbackFn>(gotReal);
    }
    std::call_once(g_realNodeLoadEnvironmentCallbackOnce, []() {
        g_realNodeLoadEnvironmentCallback =
            reinterpret_cast<NodeLoadEnvironmentCallbackFn>(dlsym(
                RTLD_NEXT,
                "_ZN4node15LoadEnvironmentEPNS_11EnvironmentENSt4__n18functionIFN2v810MaybeLocalINS4_5ValueEEERKNS_26StartExecutionCallbackInfoEEEE"));
        if (!g_realNodeLoadEnvironmentCallback) {
            Log("WARNING: node::LoadEnvironment(callback) real symbol not found");
        }
    });
    return g_realNodeLoadEnvironmentCallback;
}

static void MaybeRunNodePostLoadTrace(void* environment,
                                      uint64_t callbackCallIndex) {
    if (!environment || callbackCallIndex != 1) {
        return;
    }

    NodeLoadEnvironmentStringFn realLoadEnvironmentString =
        GetRealNodeLoadEnvironmentString();
    if (!realLoadEnvironmentString) {
        g_nodePostLoadTraceFailures.fetch_add(1, std::memory_order_relaxed);
        Log("node post-load trace skipped: LoadEnvironment(string) missing");
        return;
    }

    g_nodePostLoadTraceAttempts.fetch_add(1, std::memory_order_relaxed);
    void* value =
        realLoadEnvironmentString(environment, kNodePostLoadTraceScript);
    if (!value) {
        g_nodePostLoadTraceFailures.fetch_add(1, std::memory_order_relaxed);
        Log("node post-load trace returned empty env=%p", environment);
        return;
    }

    g_nodePostLoadTraceSuccesses.fetch_add(1, std::memory_order_relaxed);
    Log("node post-load trace returned value=%p env=%p", value, environment);
}

static NodePlatformForIsolateFn GetRealNodePlatformForIsolate() {
    if (void* trampoline = g_nodePlatformForIsolateInlineTrampoline.load(
            std::memory_order_acquire)) {
        return reinterpret_cast<NodePlatformForIsolateFn>(trampoline);
    }
    return g_realNodePlatformForIsolate;
}

static AdapterStartChildProcess2Fn GetRealAdapterStartGpuProcess() {
    if (void* gotReal =
            g_gotRealAdapterStartGpuProcess.load(std::memory_order_acquire)) {
        return reinterpret_cast<AdapterStartChildProcess2Fn>(gotReal);
    }
    std::call_once(g_realAdapterStartGpuProcessOnce, []() {
        g_realAdapterStartGpuProcess =
            reinterpret_cast<AdapterStartChildProcess2Fn>(dlsym(
                RTLD_NEXT,
                "_ZN4ohos7adapter12multiprocess19ChildProcessStarter15StartGpuProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEE"));
        if (!g_realAdapterStartGpuProcess) {
            Log("WARNING: adapter StartGpuProcess real symbol not found");
        }
    });
    return g_realAdapterStartGpuProcess;
}

static AdapterStartChildProcess2Fn GetRealAdapterStartLegacyChildProcess() {
    if (void* gotReal = g_gotRealAdapterStartLegacyChildProcess.load(
            std::memory_order_acquire)) {
        return reinterpret_cast<AdapterStartChildProcess2Fn>(gotReal);
    }
    std::call_once(g_realAdapterStartLegacyChildProcessOnce, []() {
        g_realAdapterStartLegacyChildProcess =
            reinterpret_cast<AdapterStartChildProcess2Fn>(dlsym(
                RTLD_NEXT,
                "_ZN4ohos7adapter12multiprocess19ChildProcessStarter23StartLegacyChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEE"));
        if (!g_realAdapterStartLegacyChildProcess) {
            Log("WARNING: adapter StartLegacyChildProcess real symbol not found");
        }
    });
    return g_realAdapterStartLegacyChildProcess;
}

static AdapterStartChildProcess3Fn GetRealAdapterStartNormalChildProcess() {
    if (void* gotReal = g_gotRealAdapterStartNormalChildProcess.load(
            std::memory_order_acquire)) {
        return reinterpret_cast<AdapterStartChildProcess3Fn>(gotReal);
    }
    std::call_once(g_realAdapterStartNormalChildProcessOnce, []() {
        g_realAdapterStartNormalChildProcess =
            reinterpret_cast<AdapterStartChildProcess3Fn>(dlsym(
                RTLD_NEXT,
                "_ZN4ohos7adapter12multiprocess19ChildProcessStarter23StartNormalChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEERKSA_"));
        if (!g_realAdapterStartNormalChildProcess) {
            Log("WARNING: adapter StartNormalChildProcess real symbol not found");
        }
    });
    return g_realAdapterStartNormalChildProcess;
}

static AdapterStartChildProcess3Fn GetRealAdapterStartIsolateChildProcess() {
    if (void* gotReal = g_gotRealAdapterStartIsolateChildProcess.load(
            std::memory_order_acquire)) {
        return reinterpret_cast<AdapterStartChildProcess3Fn>(gotReal);
    }
    std::call_once(g_realAdapterStartIsolateChildProcessOnce, []() {
        g_realAdapterStartIsolateChildProcess =
            reinterpret_cast<AdapterStartChildProcess3Fn>(dlsym(
                RTLD_NEXT,
                "_ZN4ohos7adapter12multiprocess19ChildProcessStarter24StartIsolateChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEERKSA_"));
        if (!g_realAdapterStartIsolateChildProcess) {
            Log("WARNING: adapter StartIsolateChildProcess real symbol not found");
        }
    });
    return g_realAdapterStartIsolateChildProcess;
}

static bool AdapterArgsContainCrashpadHandler(const AdapterArgVector& args) {
    for (const std::string& arg : args) {
        if (arg == "--type=crashpad-handler" ||
            arg.find("chrome_crashpad_handler") != std::string::npos ||
            arg.find("crashpad-handler") != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool ShouldBlockAdapterChildProcess(const AdapterArgVector& args) {
    std::call_once(g_adapterChildHookConfigOnce, InitAdapterChildHookConfig);
    return g_adapterCrashpadBlockEnabled.load(std::memory_order_relaxed) &&
           AdapterArgsContainCrashpadHandler(args);
}

static uint32_t GetPatchedAdapterChildProcessSlots() {
    return g_patchedAdapterStartGpuProcessSlots.load(
               std::memory_order_relaxed) +
           g_patchedAdapterStartLegacyChildProcessSlots.load(
               std::memory_order_relaxed) +
           g_patchedAdapterStartNormalChildProcessSlots.load(
               std::memory_order_relaxed) +
           g_patchedAdapterStartIsolateChildProcessSlots.load(
               std::memory_order_relaxed);
}

struct ElectronOffsetLookup {
    uintptr_t offset;
    uintptr_t address;
};

static int ResolveElectronOffsetCallback(dl_phdr_info* info, size_t,
                                         void* data) {
    if (!info || !info->dlpi_name || !strstr(info->dlpi_name, "libelectron.so")) {
        return 0;
    }
    auto* lookup = reinterpret_cast<ElectronOffsetLookup*>(data);
    lookup->address = static_cast<uintptr_t>(info->dlpi_addr) + lookup->offset;
    return 1;
}

static void* ResolveElectronOffset(uintptr_t offset) {
    ElectronOffsetLookup lookup = {offset, 0};
    dl_iterate_phdr(ResolveElectronOffsetCallback, &lookup);
    return reinterpret_cast<void*>(lookup.address);
}

static void ResolveNodePlatformHookSymbols() {
    g_v8ContextGetIsolate = reinterpret_cast<V8ContextGetIsolateFn>(
        dlsym(RTLD_DEFAULT, "_ZN2v87Context10GetIsolateEv"));
    g_v8GetCurrentPlatform = reinterpret_cast<V8GetCurrentPlatformFn>(
        dlsym(RTLD_DEFAULT, "_ZN2v88internal2V818GetCurrentPlatformEv"));
    g_uvDefaultLoop = reinterpret_cast<UvDefaultLoopFn>(
        dlsym(RTLD_DEFAULT, "uv_default_loop"));

    if (!g_v8GetCurrentPlatform || !g_uvDefaultLoop) {
        g_nodePlatformResolveFailed.store(true, std::memory_order_relaxed);
        Log("WARNING: NodePlatform hook symbols missing: contextGetIsolate=%p "
            "getCurrentPlatform=%p uvDefaultLoop=%p",
            reinterpret_cast<void*>(g_v8ContextGetIsolate),
            reinterpret_cast<void*>(g_v8GetCurrentPlatform),
            reinterpret_cast<void*>(g_uvDefaultLoop));
    }
}

static void ResolveNodePlatformLookupSymbols() {
    g_uvMutexLock =
        reinterpret_cast<UvMutexFn>(dlsym(RTLD_DEFAULT, "uv_mutex_lock"));
    g_uvMutexUnlock =
        reinterpret_cast<UvMutexFn>(dlsym(RTLD_DEFAULT, "uv_mutex_unlock"));
    g_nodePlatformMapLookup = reinterpret_cast<NodePlatformMapLookupFn>(
        ResolveElectronOffset(kNodePlatformMapLookupOffset));
    g_nodePlatformMapLookupAux =
        ResolveElectronOffset(kNodePlatformMapLookupAuxOffset);
    if (!g_uvMutexLock || !g_uvMutexUnlock || !g_nodePlatformMapLookup ||
        !g_nodePlatformMapLookupAux) {
        g_nodePlatformResolveFailed.store(true, std::memory_order_relaxed);
        Log("WARNING: NodePlatform lookup symbols missing: lock=%p unlock=%p "
            "lookup=%p aux=%p",
            reinterpret_cast<void*>(g_uvMutexLock),
            reinterpret_cast<void*>(g_uvMutexUnlock),
            reinterpret_cast<void*>(g_nodePlatformMapLookup),
            g_nodePlatformMapLookupAux);
    }
}

static NodePlatformRegisterIsolateFn
ResolveNodePlatformRegisterIsolate(void* platform) {
    if (!platform) {
        return nullptr;
    }

    void** const vtable = *reinterpret_cast<void***>(platform);
    if (!vtable) {
        return nullptr;
    }

    return reinterpret_cast<NodePlatformRegisterIsolateFn>(
        vtable[kNodePlatformRegisterIsolateVtableOffset / sizeof(void*)]);
}

static bool ReserveNodePlatformHookIsolate(void* isolate) {
    for (size_t i = 0; i < kMaxNodePlatformHookIsolates; ++i) {
        void* current =
            g_nodePlatformRegisteredIsolates[i].load(std::memory_order_acquire);
        if (current == isolate) {
            return false;
        }
    }

    for (size_t i = 0; i < kMaxNodePlatformHookIsolates; ++i) {
        void* expected = nullptr;
        if (g_nodePlatformRegisteredIsolates[i].compare_exchange_strong(
                expected, isolate, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
        if (expected == isolate) {
            return false;
        }
    }
    return false;
}

static bool ShouldRegisterNodePlatformForCurrentThread() {
    char name[64] = {0};
#if defined(__APPLE__)
    const int rc = pthread_getname_np(name, sizeof(name));
#else
    const int rc = pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
    if (rc != 0 || name[0] == '\0') {
        return false;
    }

    return strcmp(name, "Chrome_InProcRe") == 0 ||
           strncmp(name, "Chrome_InProcRe", 15) == 0;
}

#if defined(__aarch64__)
__attribute__((naked)) static void FakeNodePlatformGetForegroundTaskRunner() {
    __asm__ volatile(
        "bti c\n"
        "stp xzr, xzr, [x8]\n"
        "ret\n");
}

__attribute__((naked)) static void FakeNodePlatformReturnFalse() {
    __asm__ volatile(
        "bti c\n"
        "mov w0, wzr\n"
        "ret\n");
}
#else
static void FakeNodePlatformGetForegroundTaskRunner() {}
static void FakeNodePlatformReturnFalse() {}
#endif

static void* g_fakeNodePlatformDataVtable[] = {
    reinterpret_cast<void*>(&FakeNodePlatformGetForegroundTaskRunner),
    reinterpret_cast<void*>(&FakeNodePlatformReturnFalse),
};

struct FakeNodePlatformData {
    void** vtable;
};

static FakeNodePlatformData g_fakeNodePlatformData = {
    g_fakeNodePlatformDataVtable,
};

static void CacheNodePlatformData(void* platform, void* data) {
    if (!platform || !data || data == &g_fakeNodePlatformData) {
        return;
    }
    g_lastNodePlatformDataPlatform.store(platform, std::memory_order_release);
    g_lastNodePlatformData.store(data, std::memory_order_release);
    g_lastNodePlatformDataAddress.store(reinterpret_cast<uintptr_t>(data),
                                        std::memory_order_relaxed);
}

static void* GetCachedNodePlatformData(void* platform) {
    void* data = g_lastNodePlatformData.load(std::memory_order_acquire);
    if (!data) {
        return nullptr;
    }
    void* cachedPlatform =
        g_lastNodePlatformDataPlatform.load(std::memory_order_acquire);
    if (cachedPlatform && cachedPlatform != platform) {
        return nullptr;
    }
    return data;
}

static void* TryLookupNodePlatformDataNoAbort(void* platform, void* isolate) {
    if (!platform || !isolate) {
        return nullptr;
    }

    std::call_once(g_nodePlatformLookupSymbolsOnce,
                   ResolveNodePlatformLookupSymbols);
    if (!g_uvMutexLock || !g_uvMutexUnlock || !g_nodePlatformMapLookup ||
        !g_nodePlatformMapLookupAux) {
        return nullptr;
    }

    void* key = isolate;
    void* lookupScratch = &key;
    void* nodeScratch = nullptr;
    auto* base = reinterpret_cast<uint8_t*>(platform);
    void* mutex = base + kNodePlatformMutexOffset;
    void* map = base + kNodePlatformMapOffset;

    g_uvMutexLock(mutex);
    void* node = g_nodePlatformMapLookup(map, &key,
                                         g_nodePlatformMapLookupAux,
                                         &lookupScratch, &nodeScratch);
    void* data = nullptr;
    if (node) {
        data = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(node) +
            kNodePlatformLookupDataOffset);
        // Keep the control-block read in step with libelectron's lookup
        // layout. The raw data pointer is owned by the platform map.
        (void)*reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(node) +
            kNodePlatformLookupControlOffset);
    }
    g_uvMutexUnlock(mutex);

    if (data) {
        g_nodePlatformLookupHits.fetch_add(1, std::memory_order_relaxed);
        CacheNodePlatformData(platform, data);
    } else {
        g_nodePlatformLookupMisses.fetch_add(1, std::memory_order_relaxed);
    }
    return data;
}

static bool RegisterNodePlatformForLookup(void* platform, void* isolate) {
    if (!platform || !isolate) {
        return false;
    }

    std::call_once(g_nodePlatformSymbolsOnce, ResolveNodePlatformHookSymbols);
    if (!g_uvDefaultLoop) {
        return false;
    }

    NodePlatformRegisterIsolateFn registerIsolate =
        ResolveNodePlatformRegisterIsolate(platform);
    g_lastNodePlatformRegisterAddress.store(
        reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(registerIsolate)),
        std::memory_order_relaxed);
    if (!registerIsolate) {
        g_nodePlatformRegisterMissingVtable.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }

    void* loop = g_uvDefaultLoop();
    if (!loop) {
        return false;
    }

    if (!ReserveNodePlatformHookIsolate(isolate)) {
        g_nodePlatformRegisterDuplicateSkips.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }

    g_nodePlatformRegisterAttempts.fetch_add(1, std::memory_order_relaxed);
    registerIsolate(platform, isolate, loop);
    g_nodePlatformRegisterSuccesses.fetch_add(1, std::memory_order_relaxed);
    Log("NodePlatform registered missing isolate from lookup isolate=%p "
        "platform=%p register=%p loop=%p",
        isolate, platform, reinterpret_cast<void*>(registerIsolate), loop);
    return true;
}

static void EnsureNodePlatformRegisteredForIsolate(void* platform,
                                                   void* isolate,
                                                   bool allowAnyThread = false) {
    std::call_once(g_nodePlatformHookConfigOnce, InitNodePlatformHookConfig);
    if (!g_nodePlatformHookEnabled.load(std::memory_order_relaxed) ||
        !isolate) {
        return;
    }
    if (!allowAnyThread && !ShouldRegisterNodePlatformForCurrentThread()) {
        return;
    }

    std::call_once(g_nodePlatformSymbolsOnce, ResolveNodePlatformHookSymbols);
    if (!g_uvDefaultLoop) {
        return;
    }

    if (!platform && g_v8GetCurrentPlatform) {
        platform = g_v8GetCurrentPlatform();
    }
    void* loop = g_uvDefaultLoop();
    g_lastNodePlatformIsolate.store(reinterpret_cast<uintptr_t>(isolate),
                                    std::memory_order_relaxed);
    g_lastNodePlatformAddress.store(reinterpret_cast<uintptr_t>(platform),
                                    std::memory_order_relaxed);
    if (!isolate || !platform || !loop) {
        g_nodePlatformResolveFailed.store(true, std::memory_order_relaxed);
        return;
    }

    NodePlatformRegisterIsolateFn registerIsolate =
        ResolveNodePlatformRegisterIsolate(platform);
    g_lastNodePlatformRegisterAddress.store(
        reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(registerIsolate)),
        std::memory_order_relaxed);
    if (!registerIsolate) {
        g_nodePlatformRegisterMissingVtable.fetch_add(
            1, std::memory_order_relaxed);
        g_nodePlatformResolveFailed.store(true, std::memory_order_relaxed);
        return;
    }

    if (!ReserveNodePlatformHookIsolate(isolate)) {
        g_nodePlatformRegisterDuplicateSkips.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    g_nodePlatformRegisterAttempts.fetch_add(1, std::memory_order_relaxed);
    registerIsolate(platform, isolate, loop);
    g_nodePlatformRegisterSuccesses.fetch_add(1, std::memory_order_relaxed);
    Log("NodePlatform registered renderer isolate=%p platform=%p "
        "register=%p loop=%p",
        isolate, platform, reinterpret_cast<void*>(registerIsolate), loop);
}

static void EnsureNodePlatformRegisteredForContext(void* context) {
    if (!context) {
        return;
    }

    std::call_once(g_nodePlatformSymbolsOnce, ResolveNodePlatformHookSymbols);
    if (!g_v8ContextGetIsolate) {
        return;
    }

    void* isolate = g_v8ContextGetIsolate(context);
    void* platform = g_v8GetCurrentPlatform ? g_v8GetCurrentPlatform() : nullptr;
    EnsureNodePlatformRegisteredForIsolate(platform, isolate);
}

static void* NodePlatformForIsolateLookupHook(void* platform, void* isolate) {
    g_nodePlatformForIsolateCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastNodePlatformIsolate.store(reinterpret_cast<uintptr_t>(isolate),
                                    std::memory_order_relaxed);
    g_lastNodePlatformAddress.store(reinterpret_cast<uintptr_t>(platform),
                                    std::memory_order_relaxed);

    void* data = TryLookupNodePlatformDataNoAbort(platform, isolate);
    if (data) {
        return data;
    }

    std::call_once(g_nodePlatformHookConfigOnce, InitNodePlatformHookConfig);
    if (!g_insideNodePlatformForIsolateHook &&
        g_nodePlatformRegisterOnLookupEnabled.load(
            std::memory_order_relaxed)) {
        g_insideNodePlatformForIsolateHook = true;
        RegisterNodePlatformForLookup(platform, isolate);
        g_insideNodePlatformForIsolateHook = false;

        data = TryLookupNodePlatformDataNoAbort(platform, isolate);
        if (data) {
            return data;
        }
    }

    if (void* cached = GetCachedNodePlatformData(platform)) {
        g_nodePlatformLookupFallbacks.fetch_add(
            1, std::memory_order_relaxed);
        return cached;
    }

    g_nodePlatformLookupFakeFallbacks.fetch_add(
        1, std::memory_order_relaxed);
    return &g_fakeNodePlatformData;
}

static bool IsAarch64Bl(uint32_t instruction) {
    return (instruction & 0xfc000000u) == 0x94000000u;
}

static uint32_t ReadInstruction(uintptr_t address) {
    uint32_t instruction = 0;
    memcpy(&instruction, reinterpret_cast<const void*>(address),
           sizeof(instruction));
    return instruction;
}

static bool MatchesChromeIoThreadEpollDisassembly(uintptr_t return_address) {
    if (return_address < 12) {
        return false;
    }

    const uintptr_t call_address = return_address - 4;
    return ReadInstruction(call_address - 8) == 0x29430262u &&
           ReadInstruction(call_address - 4) == 0x1a89b103u &&
           IsAarch64Bl(ReadInstruction(call_address)) &&
           ReadInstruction(return_address) == 0x3100041fu;
}

static bool IsChromeIoThread() {
    if (!g_requireChromeIoThreadName.load(std::memory_order_relaxed)) {
        return true;
    }

    char name[64] = {0};
#if defined(__APPLE__)
    const int rc = pthread_getname_np(name, sizeof(name));
#else
    const int rc = pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
    if (rc != 0 || name[0] == '\0') {
        // Some HarmonyOS builds do not expose thread names through pthread.
        // The caller-PC disassembly match is still specific enough to use.
        return true;
    }
    return strcmp(name, "Chrome_IOThread") == 0;
}

static bool IsTargetElectronEpollWaitCaller(void* raw_return_address) {
    if (!raw_return_address) {
        return false;
    }

    void* extracted = __builtin_extract_return_addr(raw_return_address);
    const uintptr_t return_address = reinterpret_cast<uintptr_t>(extracted);

    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(extracted, &info) == 0 || !info.dli_fname || !info.dli_fbase) {
        return false;
    }
    if (!strstr(info.dli_fname, "libelectron.so")) {
        return false;
    }

    const uintptr_t caller_offset =
        return_address - reinterpret_cast<uintptr_t>(info.dli_fbase);
    g_lastEpollCallerOffset.store(caller_offset, std::memory_order_relaxed);

    if (!MatchesChromeIoThreadEpollDisassembly(return_address)) {
        return false;
    }

    return IsChromeIoThread();
}

static bool MatchesElectronV8InitializeDisassembly(uintptr_t return_address) {
    if (return_address < 12) {
        return false;
    }

    const uintptr_t call_address = return_address - 4;
    return ReadInstruction(call_address - 8) == 0xf9400680u &&
           ReadInstruction(call_address - 4) == 0xf94002a1u &&
           IsAarch64Bl(ReadInstruction(call_address)) &&
           ReadInstruction(return_address) == 0x94001526u;
}

static bool IsTargetElectronV8InitializeCaller(void* raw_return_address) {
    if (!raw_return_address) {
        return false;
    }

    void* extracted = __builtin_extract_return_addr(raw_return_address);
    const uintptr_t return_address = reinterpret_cast<uintptr_t>(extracted);

    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(extracted, &info) == 0 || !info.dli_fname || !info.dli_fbase) {
        return false;
    }
    if (!strstr(info.dli_fname, "libelectron.so")) {
        return false;
    }

    const uintptr_t caller_offset =
        return_address - reinterpret_cast<uintptr_t>(info.dli_fbase);
    g_lastV8InitializeCallerOffset.store(caller_offset,
                                         std::memory_order_relaxed);

    return caller_offset == kElectronV8InitializeReturnOffset &&
           MatchesElectronV8InitializeDisassembly(return_address);
}

static bool IsTargetSnapshotAllocationCaller(void* raw_return_address) {
    if (!raw_return_address) {
        return false;
    }

    void* extracted = __builtin_extract_return_addr(raw_return_address);
    const uintptr_t return_address = reinterpret_cast<uintptr_t>(extracted);

    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(extracted, &info) == 0 || !info.dli_fname || !info.dli_fbase) {
        return false;
    }
    if (!strstr(info.dli_fname, "libelectron.so")) {
        return false;
    }

    const uintptr_t caller_offset =
        return_address - reinterpret_cast<uintptr_t>(info.dli_fbase);
    g_lastSnapshotAllocCallerOffset.store(caller_offset,
                                          std::memory_order_relaxed);

    return caller_offset == kSnapshotVectorAllocReturnOffset ||
           caller_offset == kSnapshotVectorRetryAllocReturnOffset ||
           caller_offset == kSnapshotDataAllocReturnOffset;
}

static bool TrackMmapAllocation(void* ptr, size_t size) {
    void* const reserved = reinterpret_cast<void*>(
        static_cast<uintptr_t>(kReservedMmapAllocationPtrValue));
    for (size_t i = 0; i < kMaxTrackedMmapAllocations; ++i) {
        void* expected = nullptr;
        if (g_mmapAllocationPtrs[i].compare_exchange_strong(
                expected, reserved, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            g_mmapAllocationSizes[i].store(size, std::memory_order_release);
            g_activeMmapAllocationCount.fetch_add(1,
                                                  std::memory_order_release);
            g_mmapAllocationPtrs[i].store(ptr, std::memory_order_release);
            return true;
        }
    }
    return false;
}

static bool TakeMmapAllocation(void* ptr, size_t* size) {
    if (g_activeMmapAllocationCount.load(std::memory_order_acquire) == 0) {
        return false;
    }

    for (size_t i = 0; i < kMaxTrackedMmapAllocations; ++i) {
        void* current = g_mmapAllocationPtrs[i].load(std::memory_order_acquire);
        if (current == ptr &&
            g_mmapAllocationPtrs[i].compare_exchange_strong(
                current, nullptr, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            *size = g_mmapAllocationSizes[i].load(std::memory_order_acquire);
            g_mmapAllocationSizes[i].store(0, std::memory_order_release);
            g_activeMmapAllocationCount.fetch_sub(1,
                                                  std::memory_order_acq_rel);
            return true;
        }
    }
    return false;
}

static void* TrySnapshotMmapFallback(size_t size) {
    std::call_once(g_snapshotAllocConfigOnce, InitSnapshotAllocHookConfig);

    if (!g_snapshotMmapFallbackEnabled.load(std::memory_order_relaxed)) {
        return nullptr;
    }
    const size_t minBytes = static_cast<size_t>(
        g_snapshotMmapMinBytes.load(std::memory_order_relaxed));
    const size_t maxBytes = static_cast<size_t>(
        g_snapshotMmapMaxBytes.load(std::memory_order_relaxed));
    if (size < minBytes || size > maxBytes) {
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        g_snapshotMmapFallbackFailures.fetch_add(1,
                                                 std::memory_order_relaxed);
        return nullptr;
    }

    if (!TrackMmapAllocation(ptr, size)) {
        munmap(ptr, size);
        g_snapshotMmapFallbackFailures.fetch_add(1,
                                                 std::memory_order_relaxed);
        return nullptr;
    }

    g_snapshotMmapFallbacks.fetch_add(1, std::memory_order_relaxed);
    g_snapshotMmapBytes.fetch_add(size, std::memory_order_relaxed);
    Log("snapshot allocation mmap fallback: ptr=%p size=%zu caller=0x%zx",
        ptr, size,
        static_cast<size_t>(
            g_lastSnapshotAllocCallerOffset.load(std::memory_order_relaxed)));
    return ptr;
}

static void* NothrowNewWithSnapshotFallback(NothrowNewFn realNew,
                                            size_t size,
                                            const void* nothrowTag,
                                            void* returnAddress) noexcept {
    void* ptr = realNew ? realNew(size, nothrowTag) : nullptr;
    if (ptr) {
        return ptr;
    }

    if (!IsTargetSnapshotAllocationCaller(returnAddress)) {
        return nullptr;
    }

    g_snapshotNothrowNewFailures.fetch_add(1, std::memory_order_relaxed);
    return TrySnapshotMmapFallback(size);
}

static std::string GetConfiguredV8StartupFlags() {
    std::call_once(g_v8InitializeConfigOnce, InitV8InitializeHookConfig);
    std::lock_guard<std::mutex> lock(g_v8FlagsMutex);
    if (g_v8StartupFlags.empty()) {
        g_v8StartupFlags = kDefaultV8StartupFlags;
        g_v8StartupFlagsUsingDefault.store(true, std::memory_order_relaxed);
        g_v8StartupFlagsLength.store(StringLengthForStats(g_v8StartupFlags),
                                     std::memory_order_relaxed);
    }
    return g_v8StartupFlags;
}

static void ApplyV8StartupFlagsOnce() {
    g_v8StartupFlagsApplyAttempts.fetch_add(1, std::memory_order_relaxed);
    if (g_v8StartupFlagsApplied.load(std::memory_order_relaxed)) {
        return;
    }
    const std::string flags = GetConfiguredV8StartupFlags();
    g_v8StartupFlagsLength.store(StringLengthForStats(flags),
                                 std::memory_order_relaxed);
    if (flags.empty()) {
        g_v8StartupFlagsEmptySkips.fetch_add(1, std::memory_order_relaxed);
        Log("V8 startup flags disabled");
        return;
    }

    auto setFlags = reinterpret_cast<V8SetFlagsFromStringFn>(
        dlsym(RTLD_NEXT, "_ZN2v82V818SetFlagsFromStringEPKc"));
    if (!setFlags) {
        setFlags = reinterpret_cast<V8SetFlagsFromStringFn>(
            dlsym(RTLD_DEFAULT, "_ZN2v82V818SetFlagsFromStringEPKc"));
    }
    if (!setFlags) {
        g_v8StartupFlagsResolveFailed.store(true, std::memory_order_relaxed);
        Log("WARNING: v8::V8::SetFlagsFromString real symbol not found");
        return;
    }

    g_v8StartupFlagsSymbolResolved.store(true, std::memory_order_relaxed);
    setFlags(flags.c_str());
    g_v8StartupFlagsApplied.store(true, std::memory_order_relaxed);
    Log("V8 startup flags applied: %s", flags.c_str());
}

static void UpdateMaxConcurrentV8Initializations(uint32_t active) {
    uint32_t current =
        g_maxConcurrentV8Initializations.load(std::memory_order_relaxed);
    while (active > current &&
           !g_maxConcurrentV8Initializations.compare_exchange_weak(
               current, active, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

class ScopedV8InitializationCounter {
public:
    ScopedV8InitializationCounter() {
        const uint32_t active =
            g_activeV8Initializations.fetch_add(1,
                                                std::memory_order_relaxed) +
            1;
        UpdateMaxConcurrentV8Initializations(active);
    }

    ~ScopedV8InitializationCounter() {
        g_activeV8Initializations.fetch_sub(1, std::memory_order_relaxed);
    }
};

static bool CallRealV8IsolateInitialize(V8IsolateInitializeFn realInitialize,
                                        void* isolate,
                                        const void* params) {
    ScopedV8InitializationCounter counter;
    const bool ok = realInitialize(isolate, params);
    if (!ok) {
        g_v8InitializeFailures.fetch_add(1, std::memory_order_relaxed);
        const bool sameSnapshotIsolate =
            g_lastV8InitWithSnapshotIsolate.load(
                std::memory_order_relaxed) ==
            reinterpret_cast<uintptr_t>(isolate);
        const bool snapshotInitSucceeded =
            sameSnapshotIsolate &&
            g_lastV8InitWithSnapshotResult.load(
                std::memory_order_relaxed);
        if (g_forceV8InitializeSuccess.load(std::memory_order_relaxed) &&
            snapshotInitSucceeded) {
            g_v8InitializeForcedSuccesses.fetch_add(
                1, std::memory_order_relaxed);
            Log("v8::Isolate::Initialize returned false; forcing success "
                "after successful InitWithSnapshot isolate=%p params=%p",
                isolate, params);
            return true;
        }
        if (g_forceV8InitializeSuccess.load(std::memory_order_relaxed)) {
            Log("v8::Isolate::Initialize returned false; not forcing because "
                "InitWithSnapshot did not succeed for this isolate "
                "isolate=%p params=%p lastSnapshotIsolate=%p "
                "lastSnapshotResult=%d",
                isolate, params,
                reinterpret_cast<void*>(
                    g_lastV8InitWithSnapshotIsolate.load(
                        std::memory_order_relaxed)),
                g_lastV8InitWithSnapshotResult.load(
                    std::memory_order_relaxed) ? 1 : 0);
        }
        Log("v8::Isolate::Initialize returned false; returning failure "
            "isolate=%p params=%p",
            isolate, params);
    }
    return ok;
}

static void StoreV8CreateParamsSlotsHex(void** slots) {
    std::string dump;
    dump.reserve(kV8CreateParamsSlotDumpCount * 24);
    for (size_t i = 0; i < kV8CreateParamsSlotDumpCount; ++i) {
        char item[40];
        const uintptr_t value = reinterpret_cast<uintptr_t>(slots[i]);
        snprintf(item, sizeof(item), "%s%zu=0x%016llx",
                 i == 0 ? "" : ",", i,
                 static_cast<unsigned long long>(value));
        dump += item;
    }

    std::lock_guard<std::mutex> lock(g_createParamsDumpMutex);
    g_lastV8CreateParamsSlotsHex = dump;
}

static std::string GetV8CreateParamsSlotsHex() {
    std::lock_guard<std::mutex> lock(g_createParamsDumpMutex);
    return g_lastV8CreateParamsSlotsHex;
}

static void StoreV8ExternalStartupDataPath(const char* path) {
    std::lock_guard<std::mutex> lock(g_v8ExternalStartupDataPathMutex);
    g_lastV8ExternalStartupDataPath = path ? path : "";
}

static std::string GetV8ExternalStartupDataPath() {
    std::lock_guard<std::mutex> lock(g_v8ExternalStartupDataPathMutex);
    return g_lastV8ExternalStartupDataPath;
}

static void StoreSnapshotBlobReplacementPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_snapshotBlobReplacementPathMutex);
    g_snapshotBlobReplacementPath = path;
}

static std::string GetSnapshotBlobReplacementPath() {
    std::lock_guard<std::mutex> lock(g_snapshotBlobReplacementPathMutex);
    return g_snapshotBlobReplacementPath;
}

static bool ReadFileToVector(const std::string& path,
                             std::vector<char>* output) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        Log("failed to open startup snapshot blob path=%s errno=%d",
            path.c_str(), errno);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        Log("failed to seek startup snapshot blob path=%s errno=%d",
            path.c_str(), errno);
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size <= 0) {
        Log("invalid startup snapshot blob size path=%s size=%ld",
            path.c_str(), size);
        fclose(file);
        return false;
    }
    rewind(file);

    std::vector<char> buffer(static_cast<size_t>(size));
    const size_t read =
        fread(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    if (read != buffer.size()) {
        Log("failed to read startup snapshot blob path=%s read=%zu size=%zu",
            path.c_str(), read, buffer.size());
        return false;
    }

    *output = std::move(buffer);
    return true;
}

static std::string GetBundleResourceDirFromCommandLine() {
    constexpr const char* kPrefix = "--bundle-installation-dir=";
    constexpr size_t kMaxCmdlineBytes = 16384;
    FILE* file = fopen("/proc/self/cmdline", "rb");
    if (!file) {
        return kDefaultElectronResourceDir;
    }

    std::vector<char> buffer(kMaxCmdlineBytes);
    const size_t bytes = fread(buffer.data(), 1, buffer.size() - 1, file);
    fclose(file);
    buffer[bytes] = '\0';

    size_t offset = 0;
    while (offset < bytes) {
        const char* arg = buffer.data() + offset;
        const size_t length = strnlen(arg, bytes - offset);
        if (strncmp(arg, kPrefix, strlen(kPrefix)) == 0) {
            const char* value = arg + strlen(kPrefix);
            if (value[0] != '\0') {
                return value;
            }
        }
        offset += length + 1;
    }

    return kDefaultElectronResourceDir;
}

static std::string GetStartupSnapshotBlobPath() {
    std::string dir = GetBundleResourceDirFromCommandLine();
    if (!dir.empty() && dir.back() != '/') {
        dir += '/';
    }
    dir += kStartupSnapshotBlobFileName;
    return dir;
}

static void LoadBaseStartupBlobOnce() {
    const std::string path = GetStartupSnapshotBlobPath();
    StoreSnapshotBlobReplacementPath(path);
    if (!ReadFileToVector(path, &g_baseStartupBlobBytes)) {
        g_snapshotBlobReplacementFailures.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    g_baseStartupBlobData.data = g_baseStartupBlobBytes.data();
    g_baseStartupBlobData.raw_size =
        static_cast<int>(g_baseStartupBlobBytes.size());
    g_baseStartupBlobRawSize.store(g_baseStartupBlobData.raw_size,
                                   std::memory_order_relaxed);
    Log("loaded replacement startup snapshot blob path=%s raw_size=%d",
        path.c_str(), g_baseStartupBlobData.raw_size);
}

static V8StartupDataView* GetBaseStartupBlob() {
    std::call_once(g_baseStartupBlobOnce, LoadBaseStartupBlobOnce);
    if (!g_baseStartupBlobData.data || g_baseStartupBlobData.raw_size <= 0) {
        return nullptr;
    }
    return &g_baseStartupBlobData;
}

static void RecordV8SnapshotDataBlob(void* blob) {
    g_lastV8SnapshotDataBlobAddress.store(reinterpret_cast<uintptr_t>(blob),
                                          std::memory_order_relaxed);
    if (!blob) {
        g_lastV8SnapshotDataBlobDataAddress.store(0,
                                                  std::memory_order_relaxed);
        g_lastV8SnapshotDataBlobRawSize.store(0, std::memory_order_relaxed);
        return;
    }

    auto* startupData = reinterpret_cast<V8StartupDataView*>(blob);
    g_lastV8SnapshotDataBlobDataAddress.store(
        reinterpret_cast<uintptr_t>(startupData->data),
        std::memory_order_relaxed);
    g_lastV8SnapshotDataBlobRawSize.store(startupData->raw_size,
                                          std::memory_order_relaxed);
}

static void RecordEffectiveV8SnapshotDataBlob(void* blob) {
    g_lastEffectiveV8SnapshotDataBlobAddress.store(
        reinterpret_cast<uintptr_t>(blob), std::memory_order_relaxed);
    if (!blob) {
        g_lastEffectiveV8SnapshotDataBlobDataAddress.store(
            0, std::memory_order_relaxed);
        g_lastEffectiveV8SnapshotDataBlobRawSize.store(
            0, std::memory_order_relaxed);
        return;
    }

    auto* startupData = reinterpret_cast<V8StartupDataView*>(blob);
    g_lastEffectiveV8SnapshotDataBlobDataAddress.store(
        reinterpret_cast<uintptr_t>(startupData->data),
        std::memory_order_relaxed);
    g_lastEffectiveV8SnapshotDataBlobRawSize.store(
        startupData->raw_size, std::memory_order_relaxed);
}

static void RecordV8InitWithSnapshotCaller(void* raw_return_address) {
    if (!raw_return_address) {
        g_lastV8InitWithSnapshotCallerOffset.store(
            0, std::memory_order_relaxed);
        return;
    }

    void* extracted = __builtin_extract_return_addr(raw_return_address);
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(extracted, &info) == 0 || !info.dli_fname || !info.dli_fbase ||
        !strstr(info.dli_fname, "libelectron.so")) {
        g_lastV8InitWithSnapshotCallerOffset.store(
            0, std::memory_order_relaxed);
        return;
    }

    g_lastV8InitWithSnapshotCallerOffset.store(
        reinterpret_cast<uintptr_t>(extracted) -
            reinterpret_cast<uintptr_t>(info.dli_fbase),
        std::memory_order_relaxed);
}

static void RecordSnapshotDataView(
    void* snapshotData,
    std::atomic<uintptr_t>* objectAddress,
    std::atomic<uintptr_t>* dataAddress,
    std::atomic<uint32_t>* length) {
    objectAddress->store(reinterpret_cast<uintptr_t>(snapshotData),
                         std::memory_order_relaxed);
    if (!snapshotData) {
        dataAddress->store(0, std::memory_order_relaxed);
        length->store(0, std::memory_order_relaxed);
        return;
    }

    auto* view = reinterpret_cast<V8SnapshotDataView*>(snapshotData);
    dataAddress->store(reinterpret_cast<uintptr_t>(view->data),
                       std::memory_order_relaxed);
    length->store(view->length, std::memory_order_relaxed);
}

static void RecordV8InitWithSnapshotArgs(void* isolate,
                                         void* readOnlySnapshot,
                                         void* sharedSnapshot,
                                         void* startupSnapshot,
                                         bool canRehash,
                                         void* returnAddress) {
    g_lastV8InitWithSnapshotIsolate.store(
        reinterpret_cast<uintptr_t>(isolate), std::memory_order_relaxed);
    RecordSnapshotDataView(readOnlySnapshot,
                           &g_lastV8InitWithSnapshotReadOnlyAddress,
                           &g_lastV8InitWithSnapshotReadOnlyDataAddress,
                           &g_lastV8InitWithSnapshotReadOnlyLength);
    RecordSnapshotDataView(sharedSnapshot,
                           &g_lastV8InitWithSnapshotSharedAddress,
                           &g_lastV8InitWithSnapshotSharedDataAddress,
                           &g_lastV8InitWithSnapshotSharedLength);
    RecordSnapshotDataView(startupSnapshot,
                           &g_lastV8InitWithSnapshotStartupAddress,
                           &g_lastV8InitWithSnapshotStartupDataAddress,
                           &g_lastV8InitWithSnapshotStartupLength);
    g_lastV8InitWithSnapshotCanRehash.store(canRehash,
                                            std::memory_order_relaxed);
    RecordV8InitWithSnapshotCaller(returnAddress);
}

static void* MaybeReplaceContextSnapshotBlob(void* blob) {
    if (!g_replaceContextSnapshotWithStartup.load(std::memory_order_relaxed) ||
        !blob) {
        return blob;
    }

    auto* startupData = reinterpret_cast<V8StartupDataView*>(blob);
    if (startupData->raw_size < kContextSnapshotReplacementMinBytes) {
        return blob;
    }

    g_snapshotBlobReplacementAttempts.fetch_add(
        1, std::memory_order_relaxed);
    V8StartupDataView* replacement = GetBaseStartupBlob();
    if (!replacement) {
        g_snapshotBlobReplacementFailures.fetch_add(
            1, std::memory_order_relaxed);
        Log("startup snapshot replacement failed original_raw_size=%d",
            startupData->raw_size);
        return blob;
    }

    g_snapshotBlobReplacements.fetch_add(1, std::memory_order_relaxed);
    Log("replacing V8 snapshot blob original_raw_size=%d replacement_raw_size=%d",
        startupData->raw_size, replacement->raw_size);
    return replacement;
}

static void InspectV8CreateParams(const void* params, bool targetCaller) {
    g_lastV8CreateParamsAddress.store(reinterpret_cast<uintptr_t>(params),
                                      std::memory_order_relaxed);
    if (!params) {
        g_lastV8SnapshotBlobAddress.store(0, std::memory_order_relaxed);
        g_v8SnapshotBlobNulls.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_createParamsDumpMutex);
        g_lastV8CreateParamsSlotsHex.clear();
        return;
    }

    auto** slots = reinterpret_cast<void**>(const_cast<void*>(params));
    StoreV8CreateParamsSlotsHex(slots);
    g_lastV8CreateParamsSlot0.store(reinterpret_cast<uintptr_t>(slots[0]),
                                    std::memory_order_relaxed);
    g_lastV8CreateParamsSlot1.store(reinterpret_cast<uintptr_t>(slots[1]),
                                    std::memory_order_relaxed);
    g_lastV8CreateParamsSlot2.store(reinterpret_cast<uintptr_t>(slots[2]),
                                    std::memory_order_relaxed);
    g_lastV8CreateParamsSlot3.store(reinterpret_cast<uintptr_t>(slots[3]),
                                    std::memory_order_relaxed);
    g_lastV8CreateParamsSlot4.store(reinterpret_cast<uintptr_t>(slots[4]),
                                    std::memory_order_relaxed);
    g_lastV8CreateParamsSlot5.store(reinterpret_cast<uintptr_t>(slots[5]),
                                    std::memory_order_relaxed);

    // This is retained only as the legacy probe field from nosnapshot1. The
    // slot is part of ResourceConstraints on this build, not snapshot_blob.
    void* snapshotBlob = nullptr;
    g_lastV8SnapshotBlobAddress.store(reinterpret_cast<uintptr_t>(snapshotBlob),
                                      std::memory_order_relaxed);
    if (!snapshotBlob) {
        g_v8SnapshotBlobNulls.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (targetCaller &&
        g_disableNodeStartupSnapshot.load(std::memory_order_relaxed)) {
        g_v8SnapshotBlobClears.fetch_add(1, std::memory_order_relaxed);
        Log("would clear V8 CreateParams snapshot_blob params=%p blob=%p",
            params, snapshotBlob);
    }
}

static void SetBool(napi_env env, napi_value object, const char* key,
                    bool value) {
    napi_value jsValue;
    napi_get_boolean(env, value, &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

static void SetInt(napi_env env, napi_value object, const char* key,
                   int32_t value) {
    napi_value jsValue;
    napi_create_int32(env, value, &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

static void SetUint32(napi_env env, napi_value object, const char* key,
                      uint32_t value) {
    napi_value jsValue;
    napi_create_uint32(env, value, &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

static void SetUint64(napi_env env, napi_value object, const char* key,
                      uint64_t value) {
    napi_value jsValue;
    napi_create_double(env, static_cast<double>(value), &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

static void SetString(napi_env env, napi_value object, const char* key,
                      const std::string& value) {
    napi_value jsValue;
    napi_create_string_utf8(env, value.c_str(), value.size(), &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

static void AppendV8StartupDataStats(napi_env env, napi_value result) {
    SetUint64(env, result, "v8SetSnapshotDataBlobCalls",
              g_v8SetSnapshotDataBlobCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastV8SnapshotDataBlobAddress",
              g_lastV8SnapshotDataBlobAddress.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastV8SnapshotDataBlobDataAddress",
              g_lastV8SnapshotDataBlobDataAddress.load(
                  std::memory_order_relaxed));
    SetInt(env, result, "lastV8SnapshotDataBlobRawSize",
           g_lastV8SnapshotDataBlobRawSize.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastEffectiveV8SnapshotDataBlobAddress",
              g_lastEffectiveV8SnapshotDataBlobAddress.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastEffectiveV8SnapshotDataBlobDataAddress",
              g_lastEffectiveV8SnapshotDataBlobDataAddress.load(
                  std::memory_order_relaxed));
    SetInt(env, result, "lastEffectiveV8SnapshotDataBlobRawSize",
           g_lastEffectiveV8SnapshotDataBlobRawSize.load(
               std::memory_order_relaxed));
    SetBool(env, result, "replaceContextSnapshotWithStartup",
            g_replaceContextSnapshotWithStartup.load(
                std::memory_order_relaxed));
    SetBool(env, result, "skipV8SnapshotDataBlob",
            g_skipV8SnapshotDataBlob.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobSkips",
              g_snapshotBlobSkips.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobReplacementAttempts",
              g_snapshotBlobReplacementAttempts.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobReplacements",
              g_snapshotBlobReplacements.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobReplacementFailures",
              g_snapshotBlobReplacementFailures.load(
                  std::memory_order_relaxed));
    SetInt(env, result, "baseStartupBlobRawSize",
           g_baseStartupBlobRawSize.load(std::memory_order_relaxed));
    SetString(env, result, "snapshotBlobReplacementPath",
              GetSnapshotBlobReplacementPath());
    SetUint64(env, result, "v8InitializeExternalStartupDataCalls",
              g_v8InitializeExternalStartupDataCalls.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeExternalStartupDataFromFileCalls",
              g_v8InitializeExternalStartupDataFromFileCalls.load(
                  std::memory_order_relaxed));
    SetString(env, result, "lastV8ExternalStartupDataPath",
              GetV8ExternalStartupDataPath());
    SetUint64(env, result, "v8InitWithSnapshotCalls",
              g_v8InitWithSnapshotCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitWithSnapshotSuccesses",
              g_v8InitWithSnapshotSuccesses.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "v8InitWithSnapshotFailures",
              g_v8InitWithSnapshotFailures.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastV8InitWithSnapshotCallerOffset",
              static_cast<uint32_t>(
                  g_lastV8InitWithSnapshotCallerOffset.load(
                      std::memory_order_relaxed)));
    SetUint64(env, result, "lastV8InitWithSnapshotIsolate",
              g_lastV8InitWithSnapshotIsolate.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastV8InitWithSnapshotReadOnlyAddress",
              g_lastV8InitWithSnapshotReadOnlyAddress.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastV8InitWithSnapshotReadOnlyDataAddress",
              g_lastV8InitWithSnapshotReadOnlyDataAddress.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "lastV8InitWithSnapshotReadOnlyLength",
              g_lastV8InitWithSnapshotReadOnlyLength.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastV8InitWithSnapshotSharedAddress",
              g_lastV8InitWithSnapshotSharedAddress.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastV8InitWithSnapshotSharedDataAddress",
              g_lastV8InitWithSnapshotSharedDataAddress.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "lastV8InitWithSnapshotSharedLength",
              g_lastV8InitWithSnapshotSharedLength.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastV8InitWithSnapshotStartupAddress",
              g_lastV8InitWithSnapshotStartupAddress.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastV8InitWithSnapshotStartupDataAddress",
              g_lastV8InitWithSnapshotStartupDataAddress.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "lastV8InitWithSnapshotStartupLength",
              g_lastV8InitWithSnapshotStartupLength.load(
                  std::memory_order_relaxed));
    SetBool(env, result, "lastV8InitWithSnapshotCanRehash",
            g_lastV8InitWithSnapshotCanRehash.load(
                std::memory_order_relaxed));
    SetBool(env, result, "lastV8InitWithSnapshotResult",
            g_lastV8InitWithSnapshotResult.load(std::memory_order_relaxed));
}

static void AppendNodeStartupStats(napi_env env, napi_value result) {
    SetUint32(env, result, "nodeNewContextSlots",
              g_patchedNodeNewContextSlots.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodeNewContextCalls",
              g_nodeNewContextCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodeNewContextNulls",
              g_nodeNewContextNulls.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeNewContextIsolate",
              g_lastNodeNewContextIsolate.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeNewContextTemplate",
              g_lastNodeNewContextTemplate.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeNewContextResult",
              g_lastNodeNewContextResult.load(std::memory_order_relaxed));
    SetUint32(env, result, "nodeCreateEnvironmentSlots",
              g_patchedNodeCreateEnvironmentSlots.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodeCreateEnvironmentCalls",
              g_nodeCreateEnvironmentCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodeCreateEnvironmentNulls",
              g_nodeCreateEnvironmentNulls.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeCreateEnvironmentIsolateData",
              g_lastNodeCreateEnvironmentIsolateData.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeCreateEnvironmentContext",
              g_lastNodeCreateEnvironmentContext.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeCreateEnvironmentResult",
              g_lastNodeCreateEnvironmentResult.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "nodeLoadEnvironmentStringSlots",
              g_patchedNodeLoadEnvironmentStringSlots.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "nodeLoadEnvironmentCallbackSlots",
              g_patchedNodeLoadEnvironmentCallbackSlots.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodeLoadEnvironmentStringCalls",
              g_nodeLoadEnvironmentStringCalls.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodeLoadEnvironmentCallbackCalls",
              g_nodeLoadEnvironmentCallbackCalls.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodeLoadEnvironmentNulls",
              g_nodeLoadEnvironmentNulls.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePostLoadTraceAttempts",
              g_nodePostLoadTraceAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePostLoadTraceSuccesses",
              g_nodePostLoadTraceSuccesses.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePostLoadTraceFailures",
              g_nodePostLoadTraceFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeLoadEnvironmentEnv",
              g_lastNodeLoadEnvironmentEnv.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeLoadEnvironmentSource",
              g_lastNodeLoadEnvironmentSource.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeLoadEnvironmentPreload",
              g_lastNodeLoadEnvironmentPreload.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastNodeLoadEnvironmentResult",
              g_lastNodeLoadEnvironmentResult.load(
                  std::memory_order_relaxed));
}

}  // namespace

extern "C" int epoll_wait(int epfd, struct epoll_event* events, int maxevents,
                          int timeout) {
    EpollWaitFn realEpollWait = GetRealEpollWait();
    if (!realEpollWait) {
        errno = ENOSYS;
        return -1;
    }

    std::call_once(g_epollConfigOnce, InitEpollHookConfig);

    const int maxWaitMs =
        g_chromeIoThreadMaxWaitMs.load(std::memory_order_relaxed);
    const bool shouldInspectCaller =
        g_epollHookEnabled.load(std::memory_order_relaxed) && timeout != 0 &&
        (timeout < 0 || timeout > maxWaitMs);

    int effectiveTimeout = timeout;
    if (shouldInspectCaller &&
        IsTargetElectronEpollWaitCaller(__builtin_return_address(0))) {
        g_epollTargetHits.fetch_add(1, std::memory_order_relaxed);
        effectiveTimeout = maxWaitMs;
        g_epollClampHits.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_epollPassThroughHits.fetch_add(1, std::memory_order_relaxed);
    }

    g_lastOriginalEpollTimeoutMs.store(timeout, std::memory_order_relaxed);
    g_lastEffectiveEpollTimeoutMs.store(effectiveTimeout,
                                        std::memory_order_relaxed);

    return realEpollWait(epfd, events, maxevents, effectiveTimeout);
}

extern "C" bool _ZN2v87Isolate10InitializeEPS0_RKNS0_12CreateParamsE(
    void* isolate, const void* params) {
    V8IsolateInitializeFn realInitialize = GetRealV8IsolateInitialize();
    if (!realInitialize) {
        return false;
    }

    std::call_once(g_v8InitializeConfigOnce, InitV8InitializeHookConfig);

    g_v8InitializeCalls.fetch_add(1, std::memory_order_relaxed);
    const bool targetCaller =
        IsTargetElectronV8InitializeCaller(__builtin_return_address(0));
    if (targetCaller) {
        g_v8InitializeTargetHits.fetch_add(1, std::memory_order_relaxed);
    }
    InspectV8CreateParams(params, targetCaller);

    if (!g_v8InitializeHookEnabled.load(std::memory_order_relaxed) ||
        g_insideV8InitializeHook) {
        g_v8InitializePassThroughCalls.fetch_add(1,
                                                 std::memory_order_relaxed);
        EnsureNodePlatformRegisteredForIsolate(nullptr, isolate, true);
        const bool ok =
            CallRealV8IsolateInitialize(realInitialize, isolate, params);
        EnsureNodePlatformRegisteredForIsolate(nullptr, isolate, true);
        return ok;
    }

    std::call_once(g_v8FlagsOnce, ApplyV8StartupFlagsOnce);

    if (!g_serializeV8Initialize.load(std::memory_order_relaxed)) {
        g_v8InitializePassThroughCalls.fetch_add(1,
                                                 std::memory_order_relaxed);
        g_insideV8InitializeHook = true;
        EnsureNodePlatformRegisteredForIsolate(nullptr, isolate, true);
        const bool ok =
            CallRealV8IsolateInitialize(realInitialize, isolate, params);
        g_insideV8InitializeHook = false;
        EnsureNodePlatformRegisteredForIsolate(nullptr, isolate, true);
        return ok;
    }

    std::lock_guard<std::mutex> lock(g_v8InitializeMutex);
    g_v8InitializeSerializedCalls.fetch_add(1, std::memory_order_relaxed);
    g_insideV8InitializeHook = true;
    EnsureNodePlatformRegisteredForIsolate(nullptr, isolate, true);
    const bool ok = CallRealV8IsolateInitialize(realInitialize, isolate, params);
    g_insideV8InitializeHook = false;
    EnsureNodePlatformRegisteredForIsolate(nullptr, isolate, true);
    return ok;
}

extern "C" uint32_t
_ZN4node17InitializeContextEN2v85LocalINS0_7ContextEEE(void* context) {
    NodeInitializeContextFn realInitializeContext =
        GetRealNodeInitializeContext();
    if (!realInitializeContext) {
        return 0;
    }

    g_nodeInitializeContextCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_insideNodeInitializeContextHook) {
        g_insideNodeInitializeContextHook = true;
        EnsureNodePlatformRegisteredForContext(context);
        g_insideNodeInitializeContextHook = false;
    }

    return realInitializeContext(context);
}

extern "C" void*
_ZN4node10NewContextEPN2v87IsolateENS0_5LocalINS0_14ObjectTemplateEEE(
    void* isolate, void* objectTemplate) {
    NodeNewContextFn realNewContext = GetRealNodeNewContext();
    if (!realNewContext) {
        g_nodeNewContextNulls.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    const uint64_t callIndex =
        g_nodeNewContextCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_lastNodeNewContextIsolate.store(reinterpret_cast<uintptr_t>(isolate),
                                      std::memory_order_relaxed);
    g_lastNodeNewContextTemplate.store(
        reinterpret_cast<uintptr_t>(objectTemplate), std::memory_order_relaxed);

    void* context = realNewContext(isolate, objectTemplate);
    g_lastNodeNewContextResult.store(reinterpret_cast<uintptr_t>(context),
                                     std::memory_order_relaxed);
    if (!context) {
        g_nodeNewContextNulls.fetch_add(1, std::memory_order_relaxed);
        Log("node::NewContext returned null isolate=%p template=%p",
            isolate, objectTemplate);
    } else if (callIndex <= 5) {
        Log("node::NewContext returned context=%p isolate=%p template=%p",
            context, isolate, objectTemplate);
    }
    return context;
}

extern "C" void*
_ZN4node17CreateEnvironmentEPNS_11IsolateDataEN2v85LocalINS2_7ContextEEERKNSt4__n16vectorINS6_12basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEENSB_ISD_EEEESH_NS_16EnvironmentFlags5FlagsENS_8ThreadIdENS6_10unique_ptrINS_21InspectorParentHandleENS6_14default_deleteISM_EEEE(
    void* isolateData, void* context, void* args, void* execArgs, void* flags,
    void* threadId, void* inspectorParentHandle) {
    NodeCreateEnvironmentFn realCreateEnvironment =
        GetRealNodeCreateEnvironment();
    if (!realCreateEnvironment) {
        g_nodeCreateEnvironmentNulls.fetch_add(1,
                                               std::memory_order_relaxed);
        return nullptr;
    }

    const uint64_t callIndex =
        g_nodeCreateEnvironmentCalls.fetch_add(1, std::memory_order_relaxed) +
        1;
    g_lastNodeCreateEnvironmentIsolateData.store(
        reinterpret_cast<uintptr_t>(isolateData), std::memory_order_relaxed);
    g_lastNodeCreateEnvironmentContext.store(
        reinterpret_cast<uintptr_t>(context), std::memory_order_relaxed);
    void* environment =
        realCreateEnvironment(isolateData, context, args, execArgs, flags,
                              threadId, inspectorParentHandle);
    g_lastNodeCreateEnvironmentResult.store(
        reinterpret_cast<uintptr_t>(environment), std::memory_order_relaxed);
    if (!environment) {
        g_nodeCreateEnvironmentNulls.fetch_add(1,
                                               std::memory_order_relaxed);
        Log("node::CreateEnvironment returned null isolateData=%p context=%p",
            isolateData, context);
    } else if (callIndex <= 5) {
        Log("node::CreateEnvironment returned env=%p isolateData=%p context=%p",
            environment, isolateData, context);
    }
    return environment;
}

extern "C" void*
_ZN4node15LoadEnvironmentEPNS_11EnvironmentEPKc(void* environment,
                                                const char* source) {
    NodeLoadEnvironmentStringFn realLoadEnvironment =
        GetRealNodeLoadEnvironmentString();
    if (!realLoadEnvironment) {
        g_nodeLoadEnvironmentNulls.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    const uint64_t callIndex =
        g_nodeLoadEnvironmentStringCalls.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    g_lastNodeLoadEnvironmentEnv.store(
        reinterpret_cast<uintptr_t>(environment), std::memory_order_relaxed);
    g_lastNodeLoadEnvironmentSource.store(
        reinterpret_cast<uintptr_t>(source), std::memory_order_relaxed);
    void* value = realLoadEnvironment(environment, source);
    g_lastNodeLoadEnvironmentResult.store(reinterpret_cast<uintptr_t>(value),
                                          std::memory_order_relaxed);
    if (!value) {
        g_nodeLoadEnvironmentNulls.fetch_add(1, std::memory_order_relaxed);
        Log("node::LoadEnvironment(string) returned empty env=%p source=%p",
            environment, source);
    } else if (callIndex <= 5) {
        Log("node::LoadEnvironment(string) returned value=%p env=%p source=%p",
            value, environment, source);
    }
    return value;
}

extern "C" void*
_ZN4node15LoadEnvironmentEPNS_11EnvironmentENSt4__n18functionIFN2v810MaybeLocalINS4_5ValueEEERKNS_26StartExecutionCallbackInfoEEEE(
    void* environment, void* callback, void* preload) {
    NodeLoadEnvironmentCallbackFn realLoadEnvironment =
        GetRealNodeLoadEnvironmentCallback();
    if (!realLoadEnvironment) {
        g_nodeLoadEnvironmentNulls.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    const uint64_t callIndex =
        g_nodeLoadEnvironmentCallbackCalls.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    g_lastNodeLoadEnvironmentEnv.store(
        reinterpret_cast<uintptr_t>(environment), std::memory_order_relaxed);
    g_lastNodeLoadEnvironmentSource.store(
        reinterpret_cast<uintptr_t>(callback), std::memory_order_relaxed);
    g_lastNodeLoadEnvironmentPreload.store(
        reinterpret_cast<uintptr_t>(preload), std::memory_order_relaxed);
    void* value = realLoadEnvironment(environment, callback, preload);
    g_lastNodeLoadEnvironmentResult.store(reinterpret_cast<uintptr_t>(value),
                                          std::memory_order_relaxed);
    MaybeRunNodePostLoadTrace(environment, callIndex);
    if (!value) {
        g_nodeLoadEnvironmentNulls.fetch_add(1, std::memory_order_relaxed);
        Log("node::LoadEnvironment(callback) returned empty env=%p callback=%p "
            "preload=%p",
            environment, callback, preload);
    } else if (callIndex <= 5) {
        Log("node::LoadEnvironment(callback) returned value=%p env=%p "
            "callback=%p preload=%p",
            value, environment, callback, preload);
    }
    return value;
}

extern "C" int
_ZN4ohos7adapter12multiprocess19ChildProcessStarter15StartGpuProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEE(
    void* self, const AdapterArgVector& args, const AdapterFdVector& fds) {
    g_adapterChildProcessCalls.fetch_add(1, std::memory_order_relaxed);
    if (ShouldBlockAdapterChildProcess(args)) {
        g_adapterCrashpadBlocks.fetch_add(1, std::memory_order_relaxed);
        Log("blocked adapter StartGpuProcess crashpad child launch");
        return -1;
    }

    AdapterStartChildProcess2Fn realStart = GetRealAdapterStartGpuProcess();
    if (!realStart) {
        return -1;
    }
    g_adapterChildProcessPassThrough.fetch_add(1, std::memory_order_relaxed);
    return realStart(self, args, fds);
}

extern "C" int
_ZN4ohos7adapter12multiprocess19ChildProcessStarter23StartLegacyChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEE(
    void* self, const AdapterArgVector& args, const AdapterFdVector& fds) {
    g_adapterChildProcessCalls.fetch_add(1, std::memory_order_relaxed);
    if (ShouldBlockAdapterChildProcess(args)) {
        g_adapterCrashpadBlocks.fetch_add(1, std::memory_order_relaxed);
        Log("blocked adapter StartLegacyChildProcess crashpad child launch");
        return -1;
    }

    AdapterStartChildProcess2Fn realStart =
        GetRealAdapterStartLegacyChildProcess();
    if (!realStart) {
        return -1;
    }
    g_adapterChildProcessPassThrough.fetch_add(1, std::memory_order_relaxed);
    return realStart(self, args, fds);
}

extern "C" int
_ZN4ohos7adapter12multiprocess19ChildProcessStarter23StartNormalChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEERKSA_(
    void* self, const AdapterArgVector& args, const AdapterFdVector& fds,
    const std::string& processName) {
    g_adapterChildProcessCalls.fetch_add(1, std::memory_order_relaxed);
    if (ShouldBlockAdapterChildProcess(args)) {
        g_adapterCrashpadBlocks.fetch_add(1, std::memory_order_relaxed);
        Log("blocked adapter StartNormalChildProcess crashpad child launch");
        return -1;
    }

    AdapterStartChildProcess3Fn realStart =
        GetRealAdapterStartNormalChildProcess();
    if (!realStart) {
        return -1;
    }
    g_adapterChildProcessPassThrough.fetch_add(1, std::memory_order_relaxed);
    return realStart(self, args, fds, processName);
}

extern "C" int
_ZN4ohos7adapter12multiprocess19ChildProcessStarter24StartIsolateChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEERKSA_(
    void* self, const AdapterArgVector& args, const AdapterFdVector& fds,
    const std::string& processName) {
    g_adapterChildProcessCalls.fetch_add(1, std::memory_order_relaxed);
    if (ShouldBlockAdapterChildProcess(args)) {
        g_adapterCrashpadBlocks.fetch_add(1, std::memory_order_relaxed);
        Log("blocked adapter StartIsolateChildProcess crashpad child launch");
        return -1;
    }

    AdapterStartChildProcess3Fn realStart =
        GetRealAdapterStartIsolateChildProcess();
    if (!realStart) {
        return -1;
    }
    g_adapterChildProcessPassThrough.fetch_add(1, std::memory_order_relaxed);
    return realStart(self, args, fds, processName);
}

extern "C" void _ZN2v82V819SetSnapshotDataBlobEPNS_11StartupDataE(void* blob) {
    g_v8SetSnapshotDataBlobCalls.fetch_add(1, std::memory_order_relaxed);
    RecordV8SnapshotDataBlob(blob);
    if (g_skipV8SnapshotDataBlob.load(std::memory_order_relaxed)) {
        g_snapshotBlobSkips.fetch_add(1, std::memory_order_relaxed);
        RecordEffectiveV8SnapshotDataBlob(nullptr);
        Log("skipping V8 snapshot blob original=%p", blob);
        return;
    }

    void* effectiveBlob = MaybeReplaceContextSnapshotBlob(blob);
    RecordEffectiveV8SnapshotDataBlob(effectiveBlob);
    V8SetSnapshotDataBlobFn realSetSnapshotDataBlob =
        GetRealV8SetSnapshotDataBlob();
    if (!realSetSnapshotDataBlob) {
        return;
    }
    realSetSnapshotDataBlob(effectiveBlob);
}

extern "C" void _ZN2v82V829InitializeExternalStartupDataEPKc(
    const char* directoryPath) {
    g_v8InitializeExternalStartupDataCalls.fetch_add(
        1, std::memory_order_relaxed);
    StoreV8ExternalStartupDataPath(directoryPath);
    V8InitializeExternalStartupDataFn realInitializeExternalStartupData =
        GetRealV8InitializeExternalStartupData();
    if (!realInitializeExternalStartupData) {
        return;
    }
    realInitializeExternalStartupData(directoryPath);
}

extern "C" void _ZN2v82V837InitializeExternalStartupDataFromFileEPKc(
    const char* snapshotBlobPath) {
    g_v8InitializeExternalStartupDataFromFileCalls.fetch_add(
        1, std::memory_order_relaxed);
    StoreV8ExternalStartupDataPath(snapshotBlobPath);
    V8InitializeExternalStartupDataFn realInitializeExternalStartupDataFromFile =
        GetRealV8InitializeExternalStartupDataFromFile();
    if (!realInitializeExternalStartupDataFromFile) {
        return;
    }
    realInitializeExternalStartupDataFromFile(snapshotBlobPath);
}

extern "C" bool
_ZN2v88internal7Isolate16InitWithSnapshotEPNS0_12SnapshotDataES3_S3_b(
    void* isolate,
    void* readOnlySnapshot,
    void* sharedSnapshot,
    void* startupSnapshot,
    bool canRehash) {
    g_v8InitWithSnapshotCalls.fetch_add(1, std::memory_order_relaxed);
    RecordV8InitWithSnapshotArgs(isolate, readOnlySnapshot, sharedSnapshot,
                                 startupSnapshot, canRehash,
                                 __builtin_return_address(0));

    V8InternalInitWithSnapshotFn realInitWithSnapshot =
        GetRealV8InternalInitWithSnapshot();
    if (!realInitWithSnapshot) {
        g_v8InitWithSnapshotFailures.fetch_add(1,
                                               std::memory_order_relaxed);
        g_lastV8InitWithSnapshotResult.store(false,
                                             std::memory_order_relaxed);
        return false;
    }

    const bool ok = realInitWithSnapshot(isolate, readOnlySnapshot,
                                         sharedSnapshot, startupSnapshot,
                                         canRehash);
    g_lastV8InitWithSnapshotResult.store(ok, std::memory_order_relaxed);
    if (ok) {
        g_v8InitWithSnapshotSuccesses.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        g_v8InitWithSnapshotFailures.fetch_add(
            1, std::memory_order_relaxed);
    }

    Log("InitWithSnapshot result=%d isolate=%p ro=%p/%u shared=%p/%u "
        "startup=%p/%u canRehash=%d caller=0x%zx",
        ok ? 1 : 0,
        isolate,
        reinterpret_cast<void*>(
            g_lastV8InitWithSnapshotReadOnlyDataAddress.load(
                std::memory_order_relaxed)),
        g_lastV8InitWithSnapshotReadOnlyLength.load(
            std::memory_order_relaxed),
        reinterpret_cast<void*>(
            g_lastV8InitWithSnapshotSharedDataAddress.load(
                std::memory_order_relaxed)),
        g_lastV8InitWithSnapshotSharedLength.load(
            std::memory_order_relaxed),
        reinterpret_cast<void*>(
            g_lastV8InitWithSnapshotStartupDataAddress.load(
                std::memory_order_relaxed)),
        g_lastV8InitWithSnapshotStartupLength.load(
            std::memory_order_relaxed),
        canRehash ? 1 : 0,
        static_cast<size_t>(
            g_lastV8InitWithSnapshotCallerOffset.load(
                std::memory_order_relaxed)));
    return ok;
}

extern "C" V8OwnedByteVector
_ZN2v88internal19SnapshotCompression10DecompressENS_4base6VectorIKhEE(
    const uint8_t* data, size_t size) {
    SnapshotDecompressFn realDecompress = GetRealSnapshotDecompress();
    if (!realDecompress) {
        return {};
    }

    g_snapshotDecompressCalls.fetch_add(1, std::memory_order_relaxed);
    g_snapshotDecompressBytesIn.fetch_add(size, std::memory_order_relaxed);
    g_lastSnapshotCompressedSize.store(
        size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size),
        std::memory_order_relaxed);
    if (data && size >= sizeof(uint32_t)) {
        uint32_t decompressedSize = 0;
        memcpy(&decompressedSize, data, sizeof(decompressedSize));
        g_lastSnapshotDecompressedSize.store(decompressedSize,
                                             std::memory_order_relaxed);
    }

    return realDecompress(data, size);
}

extern "C" void* _ZnamRKSt9nothrow_t(size_t size,
                                     const void* nothrowTag) noexcept {
    g_snapshotNothrowNewCalls.fetch_add(1, std::memory_order_relaxed);
    return NothrowNewWithSnapshotFallback(
        GetRealArrayNothrowNew(), size, nothrowTag,
        __builtin_return_address(0));
}

extern "C" void* _ZnwmRKSt9nothrow_t(size_t size,
                                     const void* nothrowTag) noexcept {
    g_snapshotNothrowNewCalls.fetch_add(1, std::memory_order_relaxed);
    return NothrowNewWithSnapshotFallback(
        GetRealScalarNothrowNew(), size, nothrowTag,
        __builtin_return_address(0));
}

extern "C" void _ZdaPv(void* ptr) noexcept {
    if (!ptr) {
        return;
    }

    size_t size = 0;
    if (TakeMmapAllocation(ptr, &size)) {
        munmap(ptr, size);
        g_snapshotMmapDeletes.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    DeleteFn realDelete = GetRealArrayDelete();
    if (realDelete) {
        realDelete(ptr);
    }
}

extern "C" void _ZdlPv(void* ptr) noexcept {
    if (!ptr) {
        return;
    }

    size_t size = 0;
    if (TakeMmapAllocation(ptr, &size)) {
        munmap(ptr, size);
        g_snapshotMmapDeletes.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    DeleteFn realDelete = GetRealScalarDelete();
    if (realDelete) {
        realDelete(ptr);
    }
}

extern "C" void _ZdaPvm(void* ptr, size_t) noexcept {
    _ZdaPv(ptr);
}

extern "C" void _ZdlPvm(void* ptr, size_t) noexcept {
    _ZdlPv(ptr);
}

namespace {

struct PltHookTarget {
    const char* symbol;
    void* replacement;
    std::atomic<void*>* original;
    std::atomic<uint32_t>* patchedSlots;
};

struct ModuleAddressRange {
    uintptr_t start;
    uintptr_t end;
};

static bool GetModuleAddressRange(dl_phdr_info* info,
                                  ModuleAddressRange* range) {
    uintptr_t start = UINTPTR_MAX;
    uintptr_t end = 0;
    const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        const uintptr_t segStart = base + static_cast<uintptr_t>(phdr.p_vaddr);
        const uintptr_t segEnd = segStart + static_cast<uintptr_t>(phdr.p_memsz);
        if (segStart < start) {
            start = segStart;
        }
        if (segEnd > end) {
            end = segEnd;
        }
    }
    if (start == UINTPTR_MAX || end <= start) {
        return false;
    }
    range->start = start;
    range->end = end;
    return true;
}

static uintptr_t ResolveModuleAddress(dl_phdr_info* info,
                                      const ModuleAddressRange& range,
                                      ElfW(Addr) value) {
    const uintptr_t raw = static_cast<uintptr_t>(value);
    if (raw >= range.start && raw < range.end) {
        return raw;
    }

    const uintptr_t withBase =
        static_cast<uintptr_t>(info->dlpi_addr) + raw;
    if (withBase >= range.start && withBase < range.end) {
        return withBase;
    }

    return raw;
}

static bool MakePageWritable(void* address, size_t* pageSizeOut,
                             void** pageStartOut) {
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    const uintptr_t pageMask = static_cast<uintptr_t>(pageSize - 1);
    void* pageStart = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(address) & ~pageMask);
    if (mprotect(pageStart, static_cast<size_t>(pageSize),
                 PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    *pageSizeOut = static_cast<size_t>(pageSize);
    *pageStartOut = pageStart;
    return true;
}

static bool MakeCodePageWritable(void* address, size_t* pageSizeOut,
                                 void** pageStartOut,
                                 bool* keptExecutableOut,
                                 bool allowRwFallback) {
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    const uintptr_t pageMask = static_cast<uintptr_t>(pageSize - 1);
    void* pageStart = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(address) & ~pageMask);

    if (mprotect(pageStart, static_cast<size_t>(pageSize),
                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        *pageSizeOut = static_cast<size_t>(pageSize);
        *pageStartOut = pageStart;
        *keptExecutableOut = true;
        return true;
    }

    const int rwxErrno = errno;
    if (!allowRwFallback) {
        errno = rwxErrno;
        return false;
    }
    if (mprotect(pageStart, static_cast<size_t>(pageSize),
                 PROT_READ | PROT_WRITE) == 0) {
        *pageSizeOut = static_cast<size_t>(pageSize);
        *pageStartOut = pageStart;
        *keptExecutableOut = false;
        Log("inline hook target page fell back to RW without EXEC "
            "target=%p rwx_errno=%d",
            address, rwxErrno);
        return true;
    }

    return false;
}

static void WriteAbsoluteBranch(void* address, void* target) {
    uint32_t instructions[2] = {
        kAarch64LdrX16Literal8,
        kAarch64BrX16,
    };
    memcpy(address, instructions, sizeof(instructions));
    const uintptr_t targetAddress = reinterpret_cast<uintptr_t>(target);
    memcpy(reinterpret_cast<uint8_t*>(address) + sizeof(instructions),
           &targetAddress, sizeof(targetAddress));
}

static void* CreateNodeInitializeContextTrampoline(void* target) {
    constexpr size_t trampolineSize =
        kAarch64InlineBranchBytes + kAarch64InlineBranchBytes;
    void* trampoline = mmap(nullptr, trampolineSize,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) {
        return nullptr;
    }

    memcpy(trampoline, target, kAarch64InlineBranchBytes);
    WriteAbsoluteBranch(
        reinterpret_cast<uint8_t*>(trampoline) + kAarch64InlineBranchBytes,
        reinterpret_cast<uint8_t*>(target) + kAarch64InlineBranchBytes);
    __builtin___clear_cache(
        reinterpret_cast<char*>(trampoline),
        reinterpret_cast<char*>(trampoline) + trampolineSize);
    if (mprotect(trampoline, trampolineSize, PROT_READ | PROT_EXEC) != 0) {
        const int savedErrno = errno;
        munmap(trampoline, trampolineSize);
        errno = savedErrno;
        return nullptr;
    }
    return trampoline;
}

static bool InstallNodeInitializeContextInlineHookAt(void* target,
                                                    bool allowRwFallback) {
    if (!ReadEnvBool("V8_POOL_HOOK_INIT_CONTEXT_INLINE_ENABLE", false)) {
        return false;
    }
    if (!target) {
        return false;
    }
    if (g_nodeInitializeContextInlineInstalled.load(
            std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_nodeInitializeContextInlineHookMutex);
    if (g_nodeInitializeContextInlineInstalled.load(
            std::memory_order_acquire)) {
        return true;
    }

    const uint32_t firstInstruction =
        ReadInstruction(reinterpret_cast<uintptr_t>(target));
    const uint32_t secondInstruction =
        ReadInstruction(reinterpret_cast<uintptr_t>(target) + 4);
    if (firstInstruction == kAarch64LdrX16Literal8 &&
        secondInstruction == kAarch64BrX16) {
        g_nodeInitializeContextInlineInstalled.store(
            true, std::memory_order_release);
        return true;
    }

    void* replacement = reinterpret_cast<void*>(
        &_ZN4node17InitializeContextEN2v85LocalINS0_7ContextEEE);
    void* trampoline = CreateNodeInitializeContextTrampoline(target);
    if (!trampoline) {
        g_nodeInitializeContextInlineFailures.fetch_add(
            1, std::memory_order_relaxed);
        Log("node::InitializeContext inline hook trampoline setup failed "
            "target=%p errno=%d",
            target, errno);
        return false;
    }

    g_nodeInitializeContextInlineTrampoline.store(
        trampoline, std::memory_order_release);

    size_t pageSize = 0;
    void* pageStart = nullptr;
    bool keptExecutable = false;
    if (!MakeCodePageWritable(target, &pageSize, &pageStart,
                              &keptExecutable, allowRwFallback)) {
        g_nodeInitializeContextInlineTrampoline.store(
            nullptr, std::memory_order_release);
        munmap(trampoline,
               kAarch64InlineBranchBytes + kAarch64InlineBranchBytes);
        g_nodeInitializeContextInlineFailures.fetch_add(
            1, std::memory_order_relaxed);
        Log("node::InitializeContext inline hook mprotect failed target=%p "
            "errno=%d",
            target, errno);
        return false;
    }

    WriteAbsoluteBranch(target, replacement);
    __builtin___clear_cache(
        reinterpret_cast<char*>(target),
        reinterpret_cast<char*>(target) + kAarch64InlineBranchBytes);
    if (pageStart && pageSize > 0) {
        if (mprotect(pageStart, pageSize, PROT_READ | PROT_EXEC) != 0) {
            g_nodeInitializeContextInlineFailures.fetch_add(
                1, std::memory_order_relaxed);
            Log("node::InitializeContext inline hook restore RX failed "
                "target=%p errno=%d",
                target, errno);
            return false;
        }
    }

    g_patchedNodeInitializeContextInlineEntrypoints.fetch_add(
        1, std::memory_order_relaxed);
    g_nodeInitializeContextInlineInstalled.store(true,
                                                 std::memory_order_release);
    g_electronPltPatchInstalled.store(true, std::memory_order_release);
    Log("node::InitializeContext inline hook installed target=%p "
        "trampoline=%p replacement=%p keptExec=%d",
        target, trampoline, replacement, keptExecutable ? 1 : 0);
    return true;
}

static bool InstallNodePlatformForIsolateInlineHookAt(void* target,
                                                     bool allowRwFallback) {
    if (!target) {
        return false;
    }
    if (g_nodePlatformForIsolateInlineInstalled.load(
            std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_nodeInitializeContextInlineHookMutex);
    if (g_nodePlatformForIsolateInlineInstalled.load(
            std::memory_order_acquire)) {
        return true;
    }

    const uint32_t firstInstruction =
        ReadInstruction(reinterpret_cast<uintptr_t>(target));
    const uint32_t secondInstruction =
        ReadInstruction(reinterpret_cast<uintptr_t>(target) + 4);
    if (firstInstruction == kAarch64LdrX16Literal8 &&
        secondInstruction == kAarch64BrX16) {
        g_nodePlatformForIsolateInlineInstalled.store(
            true, std::memory_order_release);
        return true;
    }

    void* replacement = reinterpret_cast<void*>(&NodePlatformForIsolateLookupHook);
    void* trampoline = CreateNodeInitializeContextTrampoline(target);
    if (!trampoline) {
        g_nodePlatformForIsolateInlineFailures.fetch_add(
            1, std::memory_order_relaxed);
        Log("NodePlatform::ForIsolate inline hook trampoline setup failed "
            "target=%p errno=%d",
            target, errno);
        return false;
    }

    g_nodePlatformForIsolateInlineTrampoline.store(
        trampoline, std::memory_order_release);
    g_realNodePlatformForIsolate =
        reinterpret_cast<NodePlatformForIsolateFn>(trampoline);

    size_t pageSize = 0;
    void* pageStart = nullptr;
    bool keptExecutable = false;
    if (!MakeCodePageWritable(target, &pageSize, &pageStart,
                              &keptExecutable, allowRwFallback)) {
        g_nodePlatformForIsolateInlineTrampoline.store(
            nullptr, std::memory_order_release);
        g_realNodePlatformForIsolate = nullptr;
        munmap(trampoline,
               kAarch64InlineBranchBytes + kAarch64InlineBranchBytes);
        g_nodePlatformForIsolateInlineFailures.fetch_add(
            1, std::memory_order_relaxed);
        Log("NodePlatform::ForIsolate inline hook mprotect failed target=%p "
            "errno=%d",
            target, errno);
        return false;
    }

    WriteAbsoluteBranch(target, replacement);
    __builtin___clear_cache(
        reinterpret_cast<char*>(target),
        reinterpret_cast<char*>(target) + kAarch64InlineBranchBytes);
    if (pageStart && pageSize > 0) {
        if (mprotect(pageStart, pageSize, PROT_READ | PROT_EXEC) != 0) {
            g_nodePlatformForIsolateInlineTrampoline.store(
                nullptr, std::memory_order_release);
            g_realNodePlatformForIsolate = nullptr;
            g_nodePlatformForIsolateInlineFailures.fetch_add(
                1, std::memory_order_relaxed);
            Log("NodePlatform::ForIsolate inline hook restore RX failed "
                "target=%p errno=%d",
                target, errno);
            return false;
        }
    }

    g_patchedNodePlatformForIsolateInlineEntrypoints.fetch_add(
        1, std::memory_order_relaxed);
    g_nodePlatformForIsolateInlineInstalled.store(
        true, std::memory_order_release);
    g_electronPltPatchInstalled.store(true, std::memory_order_release);
    Log("NodePlatform::ForIsolate inline hook installed target=%p "
        "trampoline=%p replacement=%p keptExec=%d",
        target, trampoline, replacement, keptExecutable ? 1 : 0);
    return true;
}

static bool PatchGotSlot(void** slot, const PltHookTarget& target) {
    void* current = *slot;
    if (current == target.replacement) {
        return false;
    }
    if (current == nullptr) {
        return false;
    }

    void* expected = nullptr;
    target.original->compare_exchange_strong(expected, current,
                                             std::memory_order_acq_rel);

    size_t pageSize = 0;
    void* pageStart = nullptr;
    if (!MakePageWritable(slot, &pageSize, &pageStart)) {
        g_electronPltPatchFailures.fetch_add(1, std::memory_order_relaxed);
        Log("PLT hook mprotect failed for %s slot=%p errno=%d",
            target.symbol, slot, errno);
        return false;
    }

    *slot = target.replacement;
    // Leave the GOT page writable. Some Harmony builds still lazily resolve
    // neighbouring PLT slots after this point, and restoring read-only here can
    // turn a later lazy bind into a crash.

    target.patchedSlots->fetch_add(1, std::memory_order_relaxed);
    g_electronPltPatchedSlots.fetch_add(1, std::memory_order_relaxed);
    Log("PLT hook patched %s slot=%p original=%p replacement=%p",
        target.symbol, slot, current, target.replacement);
    return true;
}

static const PltHookTarget* FindPltHookTarget(const char* symbol) {
    static PltHookTarget targets[] = {
        {"epoll_wait", reinterpret_cast<void*>(&epoll_wait),
         &g_gotRealEpollWait, &g_patchedEpollWaitSlots},
        {"_ZN2v87Isolate10InitializeEPS0_RKNS0_12CreateParamsE",
         reinterpret_cast<void*>(&_ZN2v87Isolate10InitializeEPS0_RKNS0_12CreateParamsE),
         &g_gotRealV8IsolateInitialize, &g_patchedV8InitializeSlots},
        {"_ZN2v82V819SetSnapshotDataBlobEPNS_11StartupDataE",
         reinterpret_cast<void*>(&_ZN2v82V819SetSnapshotDataBlobEPNS_11StartupDataE),
         &g_gotRealV8SetSnapshotDataBlob,
         &g_patchedV8SetSnapshotDataBlobSlots},
        {"_ZN2v82V829InitializeExternalStartupDataEPKc",
         reinterpret_cast<void*>(&_ZN2v82V829InitializeExternalStartupDataEPKc),
         &g_gotRealV8InitializeExternalStartupData,
         &g_patchedV8InitializeExternalStartupDataSlots},
        {"_ZN2v82V837InitializeExternalStartupDataFromFileEPKc",
         reinterpret_cast<void*>(&_ZN2v82V837InitializeExternalStartupDataFromFileEPKc),
         &g_gotRealV8InitializeExternalStartupDataFromFile,
         &g_patchedV8InitializeExternalStartupDataFromFileSlots},
        {"_ZN2v88internal7Isolate16InitWithSnapshotEPNS0_12SnapshotDataES3_S3_b",
         reinterpret_cast<void*>(
             &_ZN2v88internal7Isolate16InitWithSnapshotEPNS0_12SnapshotDataES3_S3_b),
         &g_gotRealV8InternalInitWithSnapshot,
         &g_patchedV8InternalInitWithSnapshotSlots},
        {"_ZN2v88internal19SnapshotCompression10DecompressENS_4base6VectorIKhEE",
         reinterpret_cast<void*>(&_ZN2v88internal19SnapshotCompression10DecompressENS_4base6VectorIKhEE),
         &g_gotRealSnapshotDecompress, &g_patchedSnapshotDecompressSlots},
        {"_ZN4node17InitializeContextEN2v85LocalINS0_7ContextEEE",
         reinterpret_cast<void*>(&_ZN4node17InitializeContextEN2v85LocalINS0_7ContextEEE),
         &g_gotRealNodeInitializeContext,
         &g_patchedNodeInitializeContextSlots},
        {"_ZN4node10NewContextEPN2v87IsolateENS0_5LocalINS0_14ObjectTemplateEEE",
         reinterpret_cast<void*>(
             &_ZN4node10NewContextEPN2v87IsolateENS0_5LocalINS0_14ObjectTemplateEEE),
         &g_gotRealNodeNewContext,
         &g_patchedNodeNewContextSlots},
        {"_ZN4node17CreateEnvironmentEPNS_11IsolateDataEN2v85LocalINS2_7ContextEEERKNSt4__n16vectorINS6_12basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEENSB_ISD_EEEESH_NS_16EnvironmentFlags5FlagsENS_8ThreadIdENS6_10unique_ptrINS_21InspectorParentHandleENS6_14default_deleteISM_EEEE",
         reinterpret_cast<void*>(
             &_ZN4node17CreateEnvironmentEPNS_11IsolateDataEN2v85LocalINS2_7ContextEEERKNSt4__n16vectorINS6_12basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEENSB_ISD_EEEESH_NS_16EnvironmentFlags5FlagsENS_8ThreadIdENS6_10unique_ptrINS_21InspectorParentHandleENS6_14default_deleteISM_EEEE),
         &g_gotRealNodeCreateEnvironment,
         &g_patchedNodeCreateEnvironmentSlots},
        {"_ZN4node15LoadEnvironmentEPNS_11EnvironmentEPKc",
         reinterpret_cast<void*>(
             &_ZN4node15LoadEnvironmentEPNS_11EnvironmentEPKc),
         &g_gotRealNodeLoadEnvironmentString,
         &g_patchedNodeLoadEnvironmentStringSlots},
        {"_ZN4node15LoadEnvironmentEPNS_11EnvironmentENSt4__n18functionIFN2v810MaybeLocalINS4_5ValueEEERKNS_26StartExecutionCallbackInfoEEEE",
         reinterpret_cast<void*>(
             &_ZN4node15LoadEnvironmentEPNS_11EnvironmentENSt4__n18functionIFN2v810MaybeLocalINS4_5ValueEEERKNS_26StartExecutionCallbackInfoEEEE),
         &g_gotRealNodeLoadEnvironmentCallback,
         &g_patchedNodeLoadEnvironmentCallbackSlots},
        {"_ZN4ohos7adapter12multiprocess19ChildProcessStarter15StartGpuProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEE",
         reinterpret_cast<void*>(
             &_ZN4ohos7adapter12multiprocess19ChildProcessStarter15StartGpuProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEE),
         &g_gotRealAdapterStartGpuProcess,
         &g_patchedAdapterStartGpuProcessSlots},
        {"_ZN4ohos7adapter12multiprocess19ChildProcessStarter23StartLegacyChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEE",
         reinterpret_cast<void*>(
             &_ZN4ohos7adapter12multiprocess19ChildProcessStarter23StartLegacyChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEE),
         &g_gotRealAdapterStartLegacyChildProcess,
         &g_patchedAdapterStartLegacyChildProcessSlots},
        {"_ZN4ohos7adapter12multiprocess19ChildProcessStarter23StartNormalChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEERKSA_",
         reinterpret_cast<void*>(
             &_ZN4ohos7adapter12multiprocess19ChildProcessStarter23StartNormalChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEERKSA_),
         &g_gotRealAdapterStartNormalChildProcess,
         &g_patchedAdapterStartNormalChildProcessSlots},
        {"_ZN4ohos7adapter12multiprocess19ChildProcessStarter24StartIsolateChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEERKSA_",
         reinterpret_cast<void*>(
             &_ZN4ohos7adapter12multiprocess19ChildProcessStarter24StartIsolateChildProcessERKNSt4__n16vectorINS3_12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS8_ISA_EEEERKNS4_INS3_4pairIiiEENS8_ISG_EEEERKSA_),
         &g_gotRealAdapterStartIsolateChildProcess,
         &g_patchedAdapterStartIsolateChildProcessSlots},
        {"_ZnamRKSt9nothrow_t", reinterpret_cast<void*>(&_ZnamRKSt9nothrow_t),
         &g_gotRealArrayNothrowNew, &g_patchedArrayNothrowNewSlots},
        {"_ZnwmRKSt9nothrow_t", reinterpret_cast<void*>(&_ZnwmRKSt9nothrow_t),
         &g_gotRealScalarNothrowNew, &g_patchedScalarNothrowNewSlots},
        {"_ZdaPv", reinterpret_cast<void*>(&_ZdaPv),
         &g_gotRealArrayDelete, &g_patchedArrayDeleteSlots},
        {"_ZdlPv", reinterpret_cast<void*>(&_ZdlPv),
         &g_gotRealScalarDelete, &g_patchedScalarDeleteSlots},
    };

    for (const PltHookTarget& target : targets) {
        if (strcmp(symbol, target.symbol) == 0) {
            return &target;
        }
    }
    return nullptr;
}

static size_t PatchRelaTable(dl_phdr_info* info,
                             const ModuleAddressRange& range,
                             ElfW(Sym)* symtab,
                             const char* strtab,
                             ElfW(Rela)* rela,
                             size_t relaBytes) {
    if (!symtab || !strtab || !rela || relaBytes == 0) {
        return 0;
    }

    size_t patched = 0;
    const size_t count = relaBytes / sizeof(ElfW(Rela));
    for (size_t i = 0; i < count; ++i) {
        const unsigned type = ELF64_R_TYPE(rela[i].r_info);
        if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) {
            continue;
        }

        const size_t symIndex = ELF64_R_SYM(rela[i].r_info);
        const char* symbol = strtab + symtab[symIndex].st_name;
        if (!symbol || symbol[0] == '\0') {
            continue;
        }

        const PltHookTarget* target = FindPltHookTarget(symbol);
        if (!target) {
            continue;
        }

        void** slot = reinterpret_cast<void**>(
            ResolveModuleAddress(info, range, rela[i].r_offset));
        if (reinterpret_cast<uintptr_t>(slot) < range.start ||
            reinterpret_cast<uintptr_t>(slot) + sizeof(void*) > range.end) {
            g_electronPltPatchFailures.fetch_add(1,
                                                 std::memory_order_relaxed);
            Log("PLT hook skipped %s invalid slot=%p", symbol, slot);
            continue;
        }

        if (PatchGotSlot(slot, *target)) {
            ++patched;
        }
    }
    return patched;
}

static bool PatchElectronModule(dl_phdr_info* info, size_t* patchedOut,
                                bool allowRwFallback) {
    ModuleAddressRange range = {};
    if (!GetModuleAddressRange(info, &range)) {
        return false;
    }

    InstallNodeInitializeContextInlineHookAt(reinterpret_cast<void*>(
        static_cast<uintptr_t>(info->dlpi_addr) + kNodeInitializeContextOffset),
        allowRwFallback);
    if (ReadEnvBool("V8_POOL_HOOK_FOR_ISOLATE_INLINE_ENABLE", false)) {
        InstallNodePlatformForIsolateInlineHookAt(reinterpret_cast<void*>(
            static_cast<uintptr_t>(info->dlpi_addr) +
            kNodePlatformForIsolateOffset),
            allowRwFallback);
    }

    ElfW(Dyn)* dynamic = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type == PT_DYNAMIC) {
            dynamic = reinterpret_cast<ElfW(Dyn)*>(
                static_cast<uintptr_t>(info->dlpi_addr) + phdr.p_vaddr);
            break;
        }
    }
    if (!dynamic) {
        return false;
    }

    ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    ElfW(Rela)* rela = nullptr;
    size_t relaBytes = 0;
    ElfW(Rela)* pltRela = nullptr;
    size_t pltRelaBytes = 0;
    bool pltUsesRela = true;

    for (ElfW(Dyn)* dyn = dynamic; dyn->d_tag != DT_NULL; ++dyn) {
        switch (dyn->d_tag) {
            case DT_SYMTAB:
                symtab = reinterpret_cast<ElfW(Sym)*>(
                    ResolveModuleAddress(info, range, dyn->d_un.d_ptr));
                break;
            case DT_STRTAB:
                strtab = reinterpret_cast<const char*>(
                    ResolveModuleAddress(info, range, dyn->d_un.d_ptr));
                break;
            case DT_RELA:
                rela = reinterpret_cast<ElfW(Rela)*>(
                    ResolveModuleAddress(info, range, dyn->d_un.d_ptr));
                break;
            case DT_RELASZ:
                relaBytes = static_cast<size_t>(dyn->d_un.d_val);
                break;
            case DT_JMPREL:
                pltRela = reinterpret_cast<ElfW(Rela)*>(
                    ResolveModuleAddress(info, range, dyn->d_un.d_ptr));
                break;
            case DT_PLTRELSZ:
                pltRelaBytes = static_cast<size_t>(dyn->d_un.d_val);
                break;
            case DT_PLTREL:
                pltUsesRela = dyn->d_un.d_val == DT_RELA;
                break;
            default:
                break;
        }
    }

    if (!pltUsesRela) {
        Log("PLT hook skipped libelectron.so: DT_PLTREL is not DT_RELA");
        return false;
    }

    size_t patched = 0;
    patched += PatchRelaTable(info, range, symtab, strtab, pltRela,
                              pltRelaBytes);
    patched += PatchRelaTable(info, range, symtab, strtab, rela, relaBytes);

    if (patched > 0) {
        *patchedOut += patched;
        g_electronPltPatchInstalled.store(true, std::memory_order_release);
    }
    return true;
}

static bool AreElectronHooksReady() {
    return g_electronPltPatchInstalled.load(std::memory_order_acquire) &&
           g_patchedV8InitializeSlots.load(std::memory_order_acquire) > 0;
}

struct ElectronPatchContext {
    size_t patched = 0;
    bool found = false;
    bool allowRwFallback = false;
};

static int PatchElectronCallback(dl_phdr_info* info, size_t, void* data) {
    if (!info || !info->dlpi_name || !strstr(info->dlpi_name, "libelectron.so")) {
        return 0;
    }

    auto* context = reinterpret_cast<ElectronPatchContext*>(data);
    context->found = true;
    PatchElectronModule(info, &context->patched, context->allowRwFallback);
    return 0;
}

static void TryPreloadElectronForEarlyPatch() {
    bool expected = false;
    if (!g_electronPreloadAttempted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }

    g_electronPreloadHandle =
        dlopen("libelectron.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_electronPreloadHandle) {
        Dl_info selfInfo;
        memset(&selfInfo, 0, sizeof(selfInfo));
        if (dladdr(reinterpret_cast<void*>(&TryPreloadElectronForEarlyPatch),
                   &selfInfo) != 0 &&
            selfInfo.dli_fname) {
            std::string path(selfInfo.dli_fname);
            const size_t slash = path.find_last_of('/');
            if (slash != std::string::npos) {
                path.resize(slash + 1);
                path += "libelectron.so";
                g_electronPreloadHandle =
                    dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
            }
        }
    }

    if (g_electronPreloadHandle) {
        g_electronPreloadSucceeded.store(true, std::memory_order_release);
        Log("preloaded libelectron.so for early patch handle=%p",
            g_electronPreloadHandle);
    } else {
        Log("preload libelectron.so for early patch failed: %s", dlerror());
    }
}

static bool InstallElectronPltHooksNow(bool allowPreload,
                                       bool allowRwFallback) {
    if (AreElectronHooksReady()) {
        return true;
    }

    const uint64_t attempt =
        g_electronPltPatchAttempts.fetch_add(1, std::memory_order_relaxed) + 1;

    ElectronPatchContext context;
    context.allowRwFallback = allowRwFallback;
    dl_iterate_phdr(PatchElectronCallback, &context);
    if (!context.found && allowPreload) {
        TryPreloadElectronForEarlyPatch();
        context = {};
        context.allowRwFallback = allowRwFallback;
        dl_iterate_phdr(PatchElectronCallback, &context);
    }
    if (context.patched > 0) {
        g_electronPltPatchRuns.fetch_add(1, std::memory_order_relaxed);
    }
    if (!context.found) {
        if (attempt == 1 || attempt % 1000 == 0) {
            Log("PLT hook: libelectron.so not loaded yet attempt=%llu",
                static_cast<unsigned long long>(attempt));
        }
    }
    return AreElectronHooksReady();
}

static void* ElectronPltHookMonitor(void*) {
    for (int i = 0; i < 5000; ++i) {
        if (InstallElectronPltHooksNow(false, false)) {
            return nullptr;
        }
        usleep(1000);
    }

    for (int i = 0; i < 200; ++i) {
        if (InstallElectronPltHooksNow(false, false)) {
            return nullptr;
        }
        usleep(50000);
    }
    return nullptr;
}

static void StartElectronPltHookMonitor() {
    bool expected = false;
    if (!g_electronPltPatchThreadStarted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }

    pthread_t thread;
    if (pthread_create(&thread, nullptr, ElectronPltHookMonitor, nullptr) == 0) {
        pthread_detach(thread);
    } else {
        g_electronPltPatchThreadStarted.store(false,
                                              std::memory_order_release);
    }
}

}  // namespace

// Register an isolate for tracking
extern "C" void RegisterIsolate(const char* window_id, void* isolate_ptr) {
    if (!window_id || !isolate_ptr) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    IsolateInfo info;
    info.windowId = window_id;
    info.isolatePtr = isolate_ptr;
    info.lastUsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    info.isActive = true;
    info.refCount = 1;

    g_isolateMap[window_id] = info;
    g_ptrToWindowId[isolate_ptr] = window_id;

    Log("Registered isolate %p for window %s", isolate_ptr, window_id);
}

// Mark isolate as available for pooling
extern "C" void PoolIsolate(const char* window_id) {
    if (!window_id) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = g_isolateMap.find(window_id);
    if (it != g_isolateMap.end()) {
        it->second.isActive = false;
        it->second.lastUsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        Log("Pooled isolate for window %s", window_id);
    }
}

// Mark window as destroyed
extern "C" void UnregisterIsolate(const char* window_id) {
    if (!window_id) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = g_isolateMap.find(window_id);
    if (it != g_isolateMap.end()) {
        void* ptr = it->second.isolatePtr;
        g_ptrToWindowId.erase(ptr);
        g_isolateMap.erase(it);
        Log("Unregistered isolate for window %s", window_id);
    }
}

// Get pooled isolate pointer for reuse
extern "C" void* GetPooledIsolate(const char* window_id) {
    if (!window_id) return nullptr;

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = g_isolateMap.find(window_id);
    if (it != g_isolateMap.end() && !it->second.isActive) {
        it->second.isActive = true;
        it->second.lastUsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        Log("Reusing pooled isolate %p for window %s", it->second.isolatePtr, window_id);
        return it->second.isolatePtr;
    }

    return nullptr;
}

// Check if isolate exists for window
extern "C" bool HasIsolate(const char* window_id) {
    if (!window_id) return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    return g_isolateMap.find(window_id) != g_isolateMap.end();
}

// N-API bindings
static napi_value RegisterIsolateWrapper(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_type_error(env, nullptr, "RegisterIsolate requires windowId and isolatePtr");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    // Get windowId
    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string windowId(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &windowId[0], len + 1, &len);

    // Get isolate pointer as BigInt (since pointers don't fit in number)
    void* isolatePtr = nullptr;
    bool isBigInt = napi_get_value_bigint_uint64(env, argv[1], (uint64_t*)&isolatePtr, nullptr) == napi_ok;

    if (isBigInt && isolatePtr) {
        RegisterIsolate(windowId.c_str(), isolatePtr);
    }

    napi_value ret;
    napi_get_undefined(env, &ret);
    return ret;
}

static napi_value PoolIsolateWrapper(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "PoolIsolate requires windowId");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string windowId(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &windowId[0], len + 1, &len);

    PoolIsolate(windowId.c_str());

    napi_value result;
    napi_create_object(env, &result);
    napi_value success;
    napi_get_boolean(env, true, &success);
    napi_set_named_property(env, result, "success", success);

    return result;
}

static napi_value UnregisterIsolateWrapper(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "UnregisterIsolate requires windowId");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string windowId(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &windowId[0], len + 1, &len);

    UnregisterIsolate(windowId.c_str());

    napi_value result;
    napi_create_object(env, &result);
    napi_value success;
    napi_get_boolean(env, true, &success);
    napi_set_named_property(env, result, "success", success);

    return result;
}

static napi_value GetPooledIsolateWrapper(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "GetPooledIsolate requires windowId");
        napi_value nullVal;
        napi_get_null(env, &nullVal);
        return nullVal;
    }

    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string windowId(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &windowId[0], len + 1, &len);

    void* isolatePtr = GetPooledIsolate(windowId.c_str());

    napi_value result;
    if (isolatePtr) {
        napi_create_bigint_uint64(env, (uint64_t)isolatePtr, &result);
    } else {
        napi_get_null(env, &result);
    }

    return result;
}

static napi_value HasIsolateWrapper(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
        napi_value fals;
        napi_get_boolean(env, false, &fals);
        return fals;
    }

    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string windowId(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &windowId[0], len + 1, &len);

    bool hasIsolate = HasIsolate(windowId.c_str());

    napi_value result;
    napi_get_boolean(env, hasIsolate, &result);
    return result;
}

static napi_value GetPoolStats(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_mutex);

    size_t activeCount = 0;
    size_t pooledCount = 0;

    for (const auto& pair : g_isolateMap) {
        if (pair.second.isActive) {
            activeCount++;
        } else {
            pooledCount++;
        }
    }

    napi_value result;
    napi_create_object(env, &result);

    napi_value active, pooled, total;
    napi_create_uint32(env, activeCount, &active);
    napi_create_uint32(env, pooledCount, &pooled);
    napi_create_uint32(env, g_isolateMap.size(), &total);

    napi_set_named_property(env, result, "active", active);
    napi_set_named_property(env, result, "pooled", pooled);
    napi_set_named_property(env, result, "total", total);

    return result;
}

static napi_value ConfigureEpollHook(napi_env env, napi_callback_info info) {
    std::call_once(g_epollConfigOnce, InitEpollHookConfig);

    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc >= 1 && argv[0] != nullptr) {
        bool enabled = true;
        if (napi_get_value_bool(env, argv[0], &enabled) == napi_ok) {
            g_epollHookEnabled.store(enabled, std::memory_order_relaxed);
        }
    }

    if (argc >= 2 && argv[1] != nullptr) {
        int32_t maxWaitMs = kDefaultChromeIoThreadMaxWaitMs;
        if (napi_get_value_int32(env, argv[1], &maxWaitMs) == napi_ok) {
            if (maxWaitMs < kMinChromeIoThreadMaxWaitMs) {
                maxWaitMs = kMinChromeIoThreadMaxWaitMs;
            }
            if (maxWaitMs > kMaxChromeIoThreadMaxWaitMs) {
                maxWaitMs = kMaxChromeIoThreadMaxWaitMs;
            }
            g_chromeIoThreadMaxWaitMs.store(maxWaitMs,
                                            std::memory_order_relaxed);
        }
    }

    if (argc >= 3 && argv[2] != nullptr) {
        bool requireThreadName = true;
        if (napi_get_value_bool(env, argv[2], &requireThreadName) == napi_ok) {
            g_requireChromeIoThreadName.store(requireThreadName,
                                              std::memory_order_relaxed);
        }
    }

    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "enabled",
            g_epollHookEnabled.load(std::memory_order_relaxed));
    SetInt(env, result, "maxWaitMs",
           g_chromeIoThreadMaxWaitMs.load(std::memory_order_relaxed));
    SetBool(env, result, "requireChromeIoThreadName",
            g_requireChromeIoThreadName.load(std::memory_order_relaxed));
    return result;
}

static napi_value GetEpollHookStats(napi_env env, napi_callback_info info) {
    std::call_once(g_epollConfigOnce, InitEpollHookConfig);

    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "enabled",
            g_epollHookEnabled.load(std::memory_order_relaxed));
    SetInt(env, result, "maxWaitMs",
           g_chromeIoThreadMaxWaitMs.load(std::memory_order_relaxed));
    SetBool(env, result, "requireChromeIoThreadName",
            g_requireChromeIoThreadName.load(std::memory_order_relaxed));
    SetUint32(env, result, "targetCallOffset",
              static_cast<uint32_t>(kChromeIoThreadEpollCallOffset));
    SetUint32(env, result, "targetReturnOffset",
              static_cast<uint32_t>(kChromeIoThreadEpollReturnOffset));
    SetUint32(env, result, "lastCallerOffset",
              static_cast<uint32_t>(
                  g_lastEpollCallerOffset.load(std::memory_order_relaxed)));
    SetInt(env, result, "lastOriginalTimeoutMs",
           g_lastOriginalEpollTimeoutMs.load(std::memory_order_relaxed));
    SetInt(env, result, "lastEffectiveTimeoutMs",
           g_lastEffectiveEpollTimeoutMs.load(std::memory_order_relaxed));
    SetUint64(env, result, "targetHits",
              g_epollTargetHits.load(std::memory_order_relaxed));
    SetUint64(env, result, "clampedHits",
              g_epollClampHits.load(std::memory_order_relaxed));
    SetUint64(env, result, "passThroughHits",
              g_epollPassThroughHits.load(std::memory_order_relaxed));
    return result;
}

static napi_value ResetEpollHookStats(napi_env env, napi_callback_info info) {
    g_epollTargetHits.store(0, std::memory_order_relaxed);
    g_epollClampHits.store(0, std::memory_order_relaxed);
    g_epollPassThroughHits.store(0, std::memory_order_relaxed);
    g_lastOriginalEpollTimeoutMs.store(0, std::memory_order_relaxed);
    g_lastEffectiveEpollTimeoutMs.store(0, std::memory_order_relaxed);
    g_lastEpollCallerOffset.store(0, std::memory_order_relaxed);

    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "success", true);
    return result;
}

static napi_value ConfigureV8InitializeHook(napi_env env,
                                            napi_callback_info info) {
    std::call_once(g_v8InitializeConfigOnce, InitV8InitializeHookConfig);

    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc >= 1 && argv[0] != nullptr) {
        bool enabled = true;
        if (napi_get_value_bool(env, argv[0], &enabled) == napi_ok) {
            g_v8InitializeHookEnabled.store(enabled,
                                            std::memory_order_relaxed);
        }
    }

    if (argc >= 2 && argv[1] != nullptr) {
        bool serialize = true;
        if (napi_get_value_bool(env, argv[1], &serialize) == napi_ok) {
            g_serializeV8Initialize.store(serialize,
                                          std::memory_order_relaxed);
        }
    }

    if (argc >= 3 && argv[2] != nullptr) {
        bool forceSuccess = false;
        if (napi_get_value_bool(env, argv[2], &forceSuccess) == napi_ok) {
            g_forceV8InitializeSuccess.store(forceSuccess,
                                             std::memory_order_relaxed);
        }
    }

    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "enabled",
            g_v8InitializeHookEnabled.load(std::memory_order_relaxed));
    SetBool(env, result, "serialize",
            g_serializeV8Initialize.load(std::memory_order_relaxed));
    SetBool(env, result, "forceSuccess",
            g_forceV8InitializeSuccess.load(std::memory_order_relaxed));
    SetBool(env, result, "disableNodeStartupSnapshot",
            g_disableNodeStartupSnapshot.load(std::memory_order_relaxed));
    SetString(env, result, "startupFlags", GetConfiguredV8StartupFlags());
    SetBool(env, result, "startupFlagsApplied",
            g_v8StartupFlagsApplied.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsResolveFailed",
            g_v8StartupFlagsResolveFailed.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsSymbolResolved",
            g_v8StartupFlagsSymbolResolved.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsEnvPresent",
            g_v8StartupFlagsEnvPresent.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsEnvEmpty",
            g_v8StartupFlagsEnvEmpty.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsUsingDefault",
            g_v8StartupFlagsUsingDefault.load(std::memory_order_relaxed));
    SetUint32(env, result, "startupFlagsLength",
              g_v8StartupFlagsLength.load(std::memory_order_relaxed));
    SetUint64(env, result, "startupFlagsApplyAttempts",
              g_v8StartupFlagsApplyAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "startupFlagsEmptySkips",
              g_v8StartupFlagsEmptySkips.load(std::memory_order_relaxed));
    return result;
}

static napi_value GetV8InitializeHookStats(napi_env env,
                                           napi_callback_info info) {
    std::call_once(g_v8InitializeConfigOnce, InitV8InitializeHookConfig);
    std::call_once(g_snapshotAllocConfigOnce, InitSnapshotAllocHookConfig);

    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "enabled",
            g_v8InitializeHookEnabled.load(std::memory_order_relaxed));
    SetBool(env, result, "serialize",
            g_serializeV8Initialize.load(std::memory_order_relaxed));
    SetBool(env, result, "forceSuccess",
            g_forceV8InitializeSuccess.load(std::memory_order_relaxed));
    SetBool(env, result, "disableNodeStartupSnapshot",
            g_disableNodeStartupSnapshot.load(std::memory_order_relaxed));
    SetString(env, result, "startupFlags", GetConfiguredV8StartupFlags());
    SetBool(env, result, "startupFlagsApplied",
            g_v8StartupFlagsApplied.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsResolveFailed",
            g_v8StartupFlagsResolveFailed.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsSymbolResolved",
            g_v8StartupFlagsSymbolResolved.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsEnvPresent",
            g_v8StartupFlagsEnvPresent.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsEnvEmpty",
            g_v8StartupFlagsEnvEmpty.load(std::memory_order_relaxed));
    SetBool(env, result, "startupFlagsUsingDefault",
            g_v8StartupFlagsUsingDefault.load(std::memory_order_relaxed));
    SetUint32(env, result, "startupFlagsLength",
              g_v8StartupFlagsLength.load(std::memory_order_relaxed));
    SetUint64(env, result, "startupFlagsApplyAttempts",
              g_v8StartupFlagsApplyAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "startupFlagsEmptySkips",
              g_v8StartupFlagsEmptySkips.load(std::memory_order_relaxed));
    SetBool(env, result, "realSymbolResolved",
            g_realV8IsolateInitialize != nullptr);
    SetBool(env, result, "snapshotMmapFallbackEnabled",
            g_snapshotMmapFallbackEnabled.load(std::memory_order_relaxed));
    SetInt(env, result, "snapshotMmapMinBytes",
           g_snapshotMmapMinBytes.load(std::memory_order_relaxed));
    SetInt(env, result, "snapshotMmapMaxBytes",
           g_snapshotMmapMaxBytes.load(std::memory_order_relaxed));
    SetUint32(env, result, "targetCallOffset",
              static_cast<uint32_t>(kElectronV8InitializeCallOffset));
    SetUint32(env, result, "targetReturnOffset",
              static_cast<uint32_t>(kElectronV8InitializeReturnOffset));
    SetUint32(env, result, "lastCallerOffset",
              static_cast<uint32_t>(g_lastV8InitializeCallerOffset.load(
                  std::memory_order_relaxed)));
    SetUint32(env, result, "activeInitializations",
              g_activeV8Initializations.load(std::memory_order_relaxed));
    SetUint32(env, result, "maxConcurrentInitializations",
              g_maxConcurrentV8Initializations.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "initializeCalls",
              g_v8InitializeCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "targetHits",
              g_v8InitializeTargetHits.load(std::memory_order_relaxed));
    SetUint64(env, result, "serializedCalls",
              g_v8InitializeSerializedCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "passThroughCalls",
              g_v8InitializePassThroughCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "failedCalls",
              g_v8InitializeFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "forcedSuccesses",
              g_v8InitializeForcedSuccesses.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobClears",
              g_v8SnapshotBlobClears.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobNulls",
              g_v8SnapshotBlobNulls.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsAddress",
              g_lastV8CreateParamsAddress.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot0",
              g_lastV8CreateParamsSlot0.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot1",
              g_lastV8CreateParamsSlot1.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot2",
              g_lastV8CreateParamsSlot2.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot3",
              g_lastV8CreateParamsSlot3.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot4",
              g_lastV8CreateParamsSlot4.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot5",
              g_lastV8CreateParamsSlot5.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastSnapshotBlobAddress",
              g_lastV8SnapshotBlobAddress.load(std::memory_order_relaxed));
    SetString(env, result, "createParamsSlotsHex",
              GetV8CreateParamsSlotsHex());
    SetUint64(env, result, "snapshotNothrowNewCalls",
              g_snapshotNothrowNewCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotNothrowNewFailures",
              g_snapshotNothrowNewFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapFallbacks",
              g_snapshotMmapFallbacks.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapFallbackFailures",
              g_snapshotMmapFallbackFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapDeletes",
              g_snapshotMmapDeletes.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapBytes",
              g_snapshotMmapBytes.load(std::memory_order_relaxed));
    SetUint32(env, result, "activeSnapshotMmapAllocations",
              g_activeMmapAllocationCount.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotDecompressCalls",
              g_snapshotDecompressCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotDecompressBytesIn",
              g_snapshotDecompressBytesIn.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotCompressedSize",
              g_lastSnapshotCompressedSize.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotDecompressedSize",
              g_lastSnapshotDecompressedSize.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotAllocCallerOffset",
              static_cast<uint32_t>(g_lastSnapshotAllocCallerOffset.load(
                  std::memory_order_relaxed)));
    return result;
}

static napi_value ResetV8InitializeHookStats(napi_env env,
                                             napi_callback_info info) {
    g_v8InitializeCalls.store(0, std::memory_order_relaxed);
    g_v8InitializeTargetHits.store(0, std::memory_order_relaxed);
    g_v8InitializeSerializedCalls.store(0, std::memory_order_relaxed);
    g_v8InitializePassThroughCalls.store(0, std::memory_order_relaxed);
    g_v8InitializeFailures.store(0, std::memory_order_relaxed);
    g_v8InitializeForcedSuccesses.store(0, std::memory_order_relaxed);
    g_v8SetSnapshotDataBlobCalls.store(0, std::memory_order_relaxed);
    g_lastV8SnapshotDataBlobAddress.store(0, std::memory_order_relaxed);
    g_lastV8SnapshotDataBlobDataAddress.store(0, std::memory_order_relaxed);
    g_lastV8SnapshotDataBlobRawSize.store(0, std::memory_order_relaxed);
    g_lastEffectiveV8SnapshotDataBlobAddress.store(
        0, std::memory_order_relaxed);
    g_lastEffectiveV8SnapshotDataBlobDataAddress.store(
        0, std::memory_order_relaxed);
    g_lastEffectiveV8SnapshotDataBlobRawSize.store(
        0, std::memory_order_relaxed);
    g_snapshotBlobSkips.store(0, std::memory_order_relaxed);
    g_snapshotBlobReplacementAttempts.store(0, std::memory_order_relaxed);
    g_snapshotBlobReplacements.store(0, std::memory_order_relaxed);
    g_snapshotBlobReplacementFailures.store(0, std::memory_order_relaxed);
    g_v8InitializeExternalStartupDataCalls.store(0,
                                                 std::memory_order_relaxed);
    g_v8InitializeExternalStartupDataFromFileCalls.store(
        0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_v8ExternalStartupDataPathMutex);
        g_lastV8ExternalStartupDataPath.clear();
    }
    g_v8InitWithSnapshotCalls.store(0, std::memory_order_relaxed);
    g_v8InitWithSnapshotSuccesses.store(0, std::memory_order_relaxed);
    g_v8InitWithSnapshotFailures.store(0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotCallerOffset.store(0,
                                               std::memory_order_relaxed);
    g_lastV8InitWithSnapshotIsolate.store(0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotReadOnlyAddress.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotReadOnlyDataAddress.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotReadOnlyLength.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotSharedAddress.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotSharedDataAddress.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotSharedLength.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotStartupAddress.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotStartupDataAddress.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotStartupLength.store(
        0, std::memory_order_relaxed);
    g_lastV8InitWithSnapshotCanRehash.store(false,
                                            std::memory_order_relaxed);
    g_lastV8InitWithSnapshotResult.store(false, std::memory_order_relaxed);
    g_v8SnapshotBlobClears.store(0, std::memory_order_relaxed);
    g_v8SnapshotBlobNulls.store(0, std::memory_order_relaxed);
    g_lastV8CreateParamsAddress.store(0, std::memory_order_relaxed);
    g_lastV8CreateParamsSlot0.store(0, std::memory_order_relaxed);
    g_lastV8CreateParamsSlot1.store(0, std::memory_order_relaxed);
    g_lastV8CreateParamsSlot2.store(0, std::memory_order_relaxed);
    g_lastV8CreateParamsSlot3.store(0, std::memory_order_relaxed);
    g_lastV8CreateParamsSlot4.store(0, std::memory_order_relaxed);
    g_lastV8CreateParamsSlot5.store(0, std::memory_order_relaxed);
    g_lastV8SnapshotBlobAddress.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_createParamsDumpMutex);
        g_lastV8CreateParamsSlotsHex.clear();
    }
    g_maxConcurrentV8Initializations.store(
        g_activeV8Initializations.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    g_lastV8InitializeCallerOffset.store(0, std::memory_order_relaxed);
    g_snapshotNothrowNewCalls.store(0, std::memory_order_relaxed);
    g_snapshotNothrowNewFailures.store(0, std::memory_order_relaxed);
    g_snapshotMmapFallbacks.store(0, std::memory_order_relaxed);
    g_snapshotMmapFallbackFailures.store(0, std::memory_order_relaxed);
    g_snapshotMmapDeletes.store(0, std::memory_order_relaxed);
    g_snapshotMmapBytes.store(0, std::memory_order_relaxed);
    g_snapshotDecompressCalls.store(0, std::memory_order_relaxed);
    g_snapshotDecompressBytesIn.store(0, std::memory_order_relaxed);
    g_lastSnapshotCompressedSize.store(0, std::memory_order_relaxed);
    g_lastSnapshotDecompressedSize.store(0, std::memory_order_relaxed);
    g_lastSnapshotAllocCallerOffset.store(0, std::memory_order_relaxed);

    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "success", true);
    return result;
}

static napi_value InstallElectronPltHooksWrapper(napi_env env,
                                                 napi_callback_info info) {
    StartElectronPltHookMonitor();
    const bool installed = InstallElectronPltHooksNow(true, false);
    if (installed) {
        ApplyV8StartupFlagsOnce();
    }

    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "installed", installed);
    SetBool(env, result, "monitorStarted",
            g_electronPltPatchThreadStarted.load(std::memory_order_acquire));
    SetBool(env, result, "electronPreloadAttempted",
            g_electronPreloadAttempted.load(std::memory_order_acquire));
    SetBool(env, result, "electronPreloadSucceeded",
            g_electronPreloadSucceeded.load(std::memory_order_acquire));
    SetUint64(env, result, "attempts",
              g_electronPltPatchAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "patchedSlots",
              g_electronPltPatchedSlots.load(std::memory_order_relaxed));
    SetUint64(env, result, "failures",
              g_electronPltPatchFailures.load(std::memory_order_relaxed));
    SetUint32(env, result, "runs",
              g_electronPltPatchRuns.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeCalls",
              g_v8InitializeCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeTargetHits",
              g_v8InitializeTargetHits.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeSerializedCalls",
              g_v8InitializeSerializedCalls.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeFailures",
              g_v8InitializeFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeForcedSuccesses",
              g_v8InitializeForcedSuccesses.load(
                  std::memory_order_relaxed));
    SetBool(env, result, "v8InitializeForceSuccessEnabled",
            g_forceV8InitializeSuccess.load(std::memory_order_relaxed));
    SetBool(env, result, "disableNodeStartupSnapshot",
            g_disableNodeStartupSnapshot.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobClears",
              g_v8SnapshotBlobClears.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobNulls",
              g_v8SnapshotBlobNulls.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsAddress",
              g_lastV8CreateParamsAddress.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot0",
              g_lastV8CreateParamsSlot0.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot1",
              g_lastV8CreateParamsSlot1.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot2",
              g_lastV8CreateParamsSlot2.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot3",
              g_lastV8CreateParamsSlot3.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot4",
              g_lastV8CreateParamsSlot4.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot5",
              g_lastV8CreateParamsSlot5.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastSnapshotBlobAddress",
              g_lastV8SnapshotBlobAddress.load(std::memory_order_relaxed));
    SetString(env, result, "createParamsSlotsHex",
              GetV8CreateParamsSlotsHex());
    SetBool(env, result, "v8StartupFlagsApplied",
            g_v8StartupFlagsApplied.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsResolveFailed",
            g_v8StartupFlagsResolveFailed.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsSymbolResolved",
            g_v8StartupFlagsSymbolResolved.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsEnvPresent",
            g_v8StartupFlagsEnvPresent.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsEnvEmpty",
            g_v8StartupFlagsEnvEmpty.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsUsingDefault",
            g_v8StartupFlagsUsingDefault.load(std::memory_order_relaxed));
    SetString(env, result, "v8StartupFlags", GetConfiguredV8StartupFlags());
    SetUint32(env, result, "v8StartupFlagsLength",
              g_v8StartupFlagsLength.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8StartupFlagsApplyAttempts",
              g_v8StartupFlagsApplyAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8StartupFlagsEmptySkips",
              g_v8StartupFlagsEmptySkips.load(std::memory_order_relaxed));
    AppendV8StartupDataStats(env, result);
    AppendNodeStartupStats(env, result);
    SetUint64(env, result, "snapshotNothrowNewCalls",
              g_snapshotNothrowNewCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotNothrowNewFailures",
              g_snapshotNothrowNewFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapFallbacks",
              g_snapshotMmapFallbacks.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapFallbackFailures",
              g_snapshotMmapFallbackFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapBytes",
              g_snapshotMmapBytes.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotDecompressCalls",
              g_snapshotDecompressCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotDecompressBytesIn",
              g_snapshotDecompressBytesIn.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotCompressedSize",
              g_lastSnapshotCompressedSize.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotDecompressedSize",
              g_lastSnapshotDecompressedSize.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotAllocCallerOffset",
              static_cast<uint32_t>(g_lastSnapshotAllocCallerOffset.load(
                  std::memory_order_relaxed)));
    SetUint32(env, result, "nodeInitializeContextSlots",
              g_patchedNodeInitializeContextSlots.load(
                  std::memory_order_relaxed));
    SetBool(env, result, "nodeInitializeContextInlineInstalled",
            g_nodeInitializeContextInlineInstalled.load(
                std::memory_order_acquire));
    SetUint32(env, result, "nodeInitializeContextInlineEntrypoints",
              g_patchedNodeInitializeContextInlineEntrypoints.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodeInitializeContextInlineFailures",
              g_nodeInitializeContextInlineFailures.load(
                  std::memory_order_relaxed));
    SetBool(env, result, "nodePlatformForIsolateInlineInstalled",
            g_nodePlatformForIsolateInlineInstalled.load(
                std::memory_order_acquire));
    SetUint32(env, result, "nodePlatformForIsolateInlineEntrypoints",
              g_patchedNodePlatformForIsolateInlineEntrypoints.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformForIsolateInlineFailures",
              g_nodePlatformForIsolateInlineFailures.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformForIsolateCalls",
              g_nodePlatformForIsolateCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodeInitializeContextCalls",
              g_nodeInitializeContextCalls.load(std::memory_order_relaxed));
    SetBool(env, result, "nodePlatformHookEnabled",
            g_nodePlatformHookEnabled.load(std::memory_order_relaxed));
    SetBool(env, result, "nodePlatformRegisterOnLookupEnabled",
            g_nodePlatformRegisterOnLookupEnabled.load(
                std::memory_order_relaxed));
    SetBool(env, result, "nodePlatformResolveFailed",
            g_nodePlatformResolveFailed.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformRegisterAttempts",
              g_nodePlatformRegisterAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformRegisterSuccesses",
              g_nodePlatformRegisterSuccesses.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformRegisterDuplicateSkips",
              g_nodePlatformRegisterDuplicateSkips.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformRegisterMissingVtable",
              g_nodePlatformRegisterMissingVtable.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformLookupHits",
              g_nodePlatformLookupHits.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformLookupMisses",
              g_nodePlatformLookupMisses.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformLookupFallbacks",
              g_nodePlatformLookupFallbacks.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformLookupFakeFallbacks",
              g_nodePlatformLookupFakeFallbacks.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastNodePlatformDataAddress",
              g_lastNodePlatformDataAddress.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastNodePlatformRegisterAddress",
              g_lastNodePlatformRegisterAddress.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "adapterChildProcessSlots",
              GetPatchedAdapterChildProcessSlots());
    SetUint64(env, result, "adapterChildProcessCalls",
              g_adapterChildProcessCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "adapterChildProcessPassThrough",
              g_adapterChildProcessPassThrough.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "adapterCrashpadBlocks",
              g_adapterCrashpadBlocks.load(std::memory_order_relaxed));
    return result;
}

static napi_value GetElectronPltHookStats(napi_env env,
                                          napi_callback_info info) {
    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "installed",
            g_electronPltPatchInstalled.load(std::memory_order_acquire));
    SetBool(env, result, "monitorStarted",
            g_electronPltPatchThreadStarted.load(std::memory_order_acquire));
    SetBool(env, result, "electronPreloadAttempted",
            g_electronPreloadAttempted.load(std::memory_order_acquire));
    SetBool(env, result, "electronPreloadSucceeded",
            g_electronPreloadSucceeded.load(std::memory_order_acquire));
    SetUint64(env, result, "attempts",
              g_electronPltPatchAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "patchedSlots",
              g_electronPltPatchedSlots.load(std::memory_order_relaxed));
    SetUint64(env, result, "failures",
              g_electronPltPatchFailures.load(std::memory_order_relaxed));
    SetUint32(env, result, "runs",
              g_electronPltPatchRuns.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeCalls",
              g_v8InitializeCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeTargetHits",
              g_v8InitializeTargetHits.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeSerializedCalls",
              g_v8InitializeSerializedCalls.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeFailures",
              g_v8InitializeFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8InitializeForcedSuccesses",
              g_v8InitializeForcedSuccesses.load(
                  std::memory_order_relaxed));
    SetBool(env, result, "v8InitializeForceSuccessEnabled",
            g_forceV8InitializeSuccess.load(std::memory_order_relaxed));
    SetBool(env, result, "disableNodeStartupSnapshot",
            g_disableNodeStartupSnapshot.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobClears",
              g_v8SnapshotBlobClears.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotBlobNulls",
              g_v8SnapshotBlobNulls.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsAddress",
              g_lastV8CreateParamsAddress.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot0",
              g_lastV8CreateParamsSlot0.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot1",
              g_lastV8CreateParamsSlot1.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot2",
              g_lastV8CreateParamsSlot2.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot3",
              g_lastV8CreateParamsSlot3.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot4",
              g_lastV8CreateParamsSlot4.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastCreateParamsSlot5",
              g_lastV8CreateParamsSlot5.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastSnapshotBlobAddress",
              g_lastV8SnapshotBlobAddress.load(std::memory_order_relaxed));
    SetString(env, result, "createParamsSlotsHex",
              GetV8CreateParamsSlotsHex());
    SetBool(env, result, "v8StartupFlagsApplied",
            g_v8StartupFlagsApplied.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsResolveFailed",
            g_v8StartupFlagsResolveFailed.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsSymbolResolved",
            g_v8StartupFlagsSymbolResolved.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsEnvPresent",
            g_v8StartupFlagsEnvPresent.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsEnvEmpty",
            g_v8StartupFlagsEnvEmpty.load(std::memory_order_relaxed));
    SetBool(env, result, "v8StartupFlagsUsingDefault",
            g_v8StartupFlagsUsingDefault.load(std::memory_order_relaxed));
    SetString(env, result, "v8StartupFlags", GetConfiguredV8StartupFlags());
    SetUint32(env, result, "v8StartupFlagsLength",
              g_v8StartupFlagsLength.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8StartupFlagsApplyAttempts",
              g_v8StartupFlagsApplyAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "v8StartupFlagsEmptySkips",
              g_v8StartupFlagsEmptySkips.load(std::memory_order_relaxed));
    AppendV8StartupDataStats(env, result);
    AppendNodeStartupStats(env, result);
    SetUint64(env, result, "snapshotNothrowNewCalls",
              g_snapshotNothrowNewCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotNothrowNewFailures",
              g_snapshotNothrowNewFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapFallbacks",
              g_snapshotMmapFallbacks.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapFallbackFailures",
              g_snapshotMmapFallbackFailures.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotMmapBytes",
              g_snapshotMmapBytes.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotDecompressCalls",
              g_snapshotDecompressCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "snapshotDecompressBytesIn",
              g_snapshotDecompressBytesIn.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotCompressedSize",
              g_lastSnapshotCompressedSize.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotDecompressedSize",
              g_lastSnapshotDecompressedSize.load(std::memory_order_relaxed));
    SetUint32(env, result, "lastSnapshotAllocCallerOffset",
              static_cast<uint32_t>(g_lastSnapshotAllocCallerOffset.load(
                  std::memory_order_relaxed)));
    SetUint32(env, result, "epollWaitSlots",
              g_patchedEpollWaitSlots.load(std::memory_order_relaxed));
    SetUint32(env, result, "v8InitializeSlots",
              g_patchedV8InitializeSlots.load(std::memory_order_relaxed));
    SetUint32(env, result, "v8SetSnapshotDataBlobSlots",
              g_patchedV8SetSnapshotDataBlobSlots.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "v8InitializeExternalStartupDataSlots",
              g_patchedV8InitializeExternalStartupDataSlots.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "v8InitializeExternalStartupDataFromFileSlots",
              g_patchedV8InitializeExternalStartupDataFromFileSlots.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "v8InitWithSnapshotSlots",
              g_patchedV8InternalInitWithSnapshotSlots.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "snapshotDecompressSlots",
              g_patchedSnapshotDecompressSlots.load(std::memory_order_relaxed));
    SetUint32(env, result, "arrayNothrowNewSlots",
              g_patchedArrayNothrowNewSlots.load(std::memory_order_relaxed));
    SetUint32(env, result, "scalarNothrowNewSlots",
              g_patchedScalarNothrowNewSlots.load(std::memory_order_relaxed));
    SetUint32(env, result, "arrayDeleteSlots",
              g_patchedArrayDeleteSlots.load(std::memory_order_relaxed));
    SetUint32(env, result, "scalarDeleteSlots",
              g_patchedScalarDeleteSlots.load(std::memory_order_relaxed));
    SetUint32(env, result, "nodeInitializeContextSlots",
              g_patchedNodeInitializeContextSlots.load(
                  std::memory_order_relaxed));
    SetBool(env, result, "nodeInitializeContextInlineInstalled",
            g_nodeInitializeContextInlineInstalled.load(
                std::memory_order_acquire));
    SetUint32(env, result, "nodeInitializeContextInlineEntrypoints",
              g_patchedNodeInitializeContextInlineEntrypoints.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodeInitializeContextInlineFailures",
              g_nodeInitializeContextInlineFailures.load(
                  std::memory_order_relaxed));
    SetBool(env, result, "nodePlatformForIsolateInlineInstalled",
            g_nodePlatformForIsolateInlineInstalled.load(
                std::memory_order_acquire));
    SetUint32(env, result, "nodePlatformForIsolateInlineEntrypoints",
              g_patchedNodePlatformForIsolateInlineEntrypoints.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformForIsolateInlineFailures",
              g_nodePlatformForIsolateInlineFailures.load(
                  std::memory_order_relaxed));
    SetBool(env, result, "nodePlatformHookEnabled",
            g_nodePlatformHookEnabled.load(std::memory_order_relaxed));
    SetBool(env, result, "nodePlatformRegisterOnLookupEnabled",
            g_nodePlatformRegisterOnLookupEnabled.load(
                std::memory_order_relaxed));
    SetBool(env, result, "nodePlatformResolveFailed",
            g_nodePlatformResolveFailed.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodeInitializeContextCalls",
              g_nodeInitializeContextCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformRegisterAttempts",
              g_nodePlatformRegisterAttempts.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformRegisterSuccesses",
              g_nodePlatformRegisterSuccesses.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformRegisterDuplicateSkips",
              g_nodePlatformRegisterDuplicateSkips.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformRegisterMissingVtable",
              g_nodePlatformRegisterMissingVtable.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformLookupHits",
              g_nodePlatformLookupHits.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformLookupMisses",
              g_nodePlatformLookupMisses.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformLookupFallbacks",
              g_nodePlatformLookupFallbacks.load(std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformLookupFakeFallbacks",
              g_nodePlatformLookupFakeFallbacks.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "nodePlatformForIsolateCalls",
              g_nodePlatformForIsolateCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastNodePlatformIsolate",
              g_lastNodePlatformIsolate.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastNodePlatformAddress",
              g_lastNodePlatformAddress.load(std::memory_order_relaxed));
    SetUint64(env, result, "lastNodePlatformRegisterAddress",
              g_lastNodePlatformRegisterAddress.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "lastNodePlatformDataAddress",
              g_lastNodePlatformDataAddress.load(
                  std::memory_order_relaxed));
    SetUint32(env, result, "adapterChildProcessSlots",
              GetPatchedAdapterChildProcessSlots());
    SetUint64(env, result, "adapterChildProcessCalls",
              g_adapterChildProcessCalls.load(std::memory_order_relaxed));
    SetUint64(env, result, "adapterChildProcessPassThrough",
              g_adapterChildProcessPassThrough.load(
                  std::memory_order_relaxed));
    SetUint64(env, result, "adapterCrashpadBlocks",
              g_adapterCrashpadBlocks.load(std::memory_order_relaxed));
    return result;
}

static napi_value ClearPool(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_mutex);

    size_t count = g_isolateMap.size();
    g_isolateMap.clear();
    g_ptrToWindowId.clear();

    Log("Cleared pool: %zu isolates", count);

    napi_value result;
    napi_create_object(env, &result);
    napi_value success;
    napi_get_boolean(env, true, &success);
    napi_set_named_property(env, result, "success", success);
    napi_value cleared;
    napi_create_uint32(env, count, &cleared);
    napi_set_named_property(env, result, "cleared", cleared);

    return result;
}

static napi_value SanitizeNodeEnvironment(napi_env env,
                                          napi_callback_info info) {
    const char* blocked[] = {
        "NODE_OPTIONS",
        "NODE_EXTRA_CA_CERTS",
        "NODE_REPL_EXTERNAL_MODULE",
    };

    uint32_t cleared = 0;
    std::string clearedNames;
    for (const char* name : blocked) {
        if (getenv(name) == nullptr) {
            continue;
        }
        if (unsetenv(name) == 0) {
            if (!clearedNames.empty()) {
                clearedNames += ",";
            }
            clearedNames += name;
            ++cleared;
        } else {
            Log("WARNING: unsetenv(%s) failed: errno=%d", name, errno);
        }
    }

    napi_value result;
    napi_create_object(env, &result);
    SetBool(env, result, "success", true);
    SetUint32(env, result, "cleared", cleared);
    SetString(env, result, "clearedVariables", clearedNames);
    return result;
}

// Module initialization
EXTERN_C_START

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"registerIsolate", nullptr, RegisterIsolateWrapper, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"poolIsolate", nullptr, PoolIsolateWrapper, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unregisterIsolate", nullptr, UnregisterIsolateWrapper, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getPooledIsolate", nullptr, GetPooledIsolateWrapper, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hasIsolate", nullptr, HasIsolateWrapper, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getPoolStats", nullptr, GetPoolStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearPool", nullptr, ClearPool, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"configureEpollHook", nullptr, ConfigureEpollHook, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getEpollHookStats", nullptr, GetEpollHookStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resetEpollHookStats", nullptr, ResetEpollHookStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"configureV8InitializeHook", nullptr, ConfigureV8InitializeHook, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getV8InitializeHookStats", nullptr, GetV8InitializeHookStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resetV8InitializeHookStats", nullptr, ResetV8InitializeHookStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"installElectronPltHooks", nullptr, InstallElectronPltHooksWrapper, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getElectronPltHookStats", nullptr, GetElectronPltHookStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sanitizeNodeEnvironment", nullptr, SanitizeNodeEnvironment, nullptr, nullptr, nullptr, napi_default, nullptr}
    };

    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    Log("V8 Pool Hook module initialized");
    return exports;
}

EXTERN_C_END

static napi_module v8PoolHookModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "v8poolhook",
    .nm_priv = nullptr,
    .reserved = {0}
};

extern "C" __attribute__((constructor)) void RegisterV8PoolHookModule() {
    std::call_once(g_epollConfigOnce, InitEpollHookConfig);
    std::call_once(g_v8InitializeConfigOnce, InitV8InitializeHookConfig);
    std::call_once(g_snapshotAllocConfigOnce, InitSnapshotAllocHookConfig);
    std::call_once(g_nodePlatformHookConfigOnce, InitNodePlatformHookConfig);
    std::call_once(g_adapterChildHookConfigOnce, InitAdapterChildHookConfig);
    napi_module_register(&v8PoolHookModule);
    Log("epoll_wait preload hook: enabled=%d maxWaitMs=%d targetOffset=0x%zx",
        g_epollHookEnabled.load(std::memory_order_relaxed),
        g_chromeIoThreadMaxWaitMs.load(std::memory_order_relaxed),
        static_cast<size_t>(kChromeIoThreadEpollCallOffset));
    Log("V8 isolate initialize preload hook: enabled=%d serialize=%d "
        "targetOffset=0x%zx flags=\"%s\"",
        g_v8InitializeHookEnabled.load(std::memory_order_relaxed),
        g_serializeV8Initialize.load(std::memory_order_relaxed),
        static_cast<size_t>(kElectronV8InitializeCallOffset),
        GetConfiguredV8StartupFlags().c_str());
    Log("V8 snapshot mmap fallback: enabled=%d minBytes=%d maxBytes=%d",
        g_snapshotMmapFallbackEnabled.load(std::memory_order_relaxed),
        g_snapshotMmapMinBytes.load(std::memory_order_relaxed),
        g_snapshotMmapMaxBytes.load(std::memory_order_relaxed));
    Log("NodePlatform register hook: enabled=%d initContextOffset=0x%zx "
        "forIsolateOffset=0x%zx registerVtableOffset=0x%zx",
        g_nodePlatformHookEnabled.load(std::memory_order_relaxed),
        static_cast<size_t>(kNodeInitializeContextOffset),
        static_cast<size_t>(kNodePlatformForIsolateOffset),
        static_cast<size_t>(kNodePlatformRegisterIsolateVtableOffset));
    Log("Adapter crashpad child-process hook: enabled=%d",
        g_adapterCrashpadBlockEnabled.load(std::memory_order_relaxed));
    StartElectronPltHookMonitor();
}
