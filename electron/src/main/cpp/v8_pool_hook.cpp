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
#include <pthread.h>

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

using EpollWaitFn = int (*)(int, struct epoll_event*, int, int);

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

// Logging
void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[V8PoolHook] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
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

static EpollWaitFn GetRealEpollWait() {
    std::call_once(g_realEpollWaitOnce, []() {
        g_realEpollWait = reinterpret_cast<EpollWaitFn>(
            dlsym(RTLD_NEXT, "epoll_wait"));
    });
    return g_realEpollWait;
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
        {"resetEpollHookStats", nullptr, ResetEpollHookStats, nullptr, nullptr, nullptr, napi_default, nullptr}
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
    napi_module_register(&v8PoolHookModule);
    Log("epoll_wait preload hook: enabled=%d maxWaitMs=%d targetOffset=0x%zx",
        g_epollHookEnabled.load(std::memory_order_relaxed),
        g_chromeIoThreadMaxWaitMs.load(std::memory_order_relaxed),
        static_cast<size_t>(kChromeIoThreadEpollCallOffset));
}
