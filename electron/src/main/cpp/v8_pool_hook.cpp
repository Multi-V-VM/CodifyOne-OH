// Copyright (c) 2024 V8 Pool Hook for HarmonyOS Electron
// NDK module to manage V8 isolate lifecycle from TypeScript

#include "napi/native_api.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <cstdio>

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

// Logging
void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[V8PoolHook] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
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
        {"clearPool", nullptr, ClearPool, nullptr, nullptr, nullptr, napi_default, nullptr}
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
    napi_module_register(&v8PoolHookModule);
}
