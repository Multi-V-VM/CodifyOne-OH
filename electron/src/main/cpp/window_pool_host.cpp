// Copyright (c) 2024 Window Pool for HarmonyOS Electron
// Native NDK handler for ability window pooling
// This module tracks pooled windows and provides infrastructure for V8 isolate reuse

#include "napi/native_api.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

// Window ID to V8 Context tracking
// When libelectron.so source is available, we'll store actual WebContents* pointers here
struct PooledWindowInfo {
    std::string windowId;
    std::string url;
    int32_t originWindowId;
    uint64_t lastUsed;
    bool isActive;

    PooledWindowInfo() : originWindowId(-1), lastUsed(0), isActive(false) {}
};

// Global pool state
static std::unordered_map<std::string, PooledWindowInfo> g_pooledWindows;
static std::mutex g_poolMutex;
static bool g_poolEnabled = true;
static size_t g_maxPoolSize = 5;

// Helper: Get string from napi_value
static bool GetString(napi_env env, napi_value value, std::string* out)
{
    size_t len = 0;
    napi_status status = napi_get_value_string_utf8(env, value, nullptr, 0, &len);
    if (status != napi_ok) {
        return false;
    }

    std::vector<char> buffer(len + 1, '\0');
    status = napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &len);
    if (status != napi_ok) {
        return false;
    }

    out->assign(buffer.data(), len);
    return true;
}

// Helper: Get int32 from napi_value
static bool GetInt32(napi_env env, napi_value value, int32_t* out)
{
    return napi_get_value_int32(env, value, out) == napi_ok;
}

// Helper: Create object with string property
static void SetObjectString(napi_env env, napi_value object, const char* key, const std::string& value)
{
    napi_value jsValue;
    napi_create_string_utf8(env, value.c_str(), value.size(), &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

// Helper: Create object with int32 property
static void SetObjectInt(napi_env env, napi_value object, const char* key, int32_t value)
{
    napi_value jsValue;
    napi_create_int32(env, value, &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

// Helper: Create object with bool property
static void SetObjectBool(napi_env env, napi_value object, const char* key, bool value)
{
    napi_value jsValue;
    napi_get_boolean(env, value, &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

// Register a window as available for pooling
// Called from TypeScript when a window is hidden instead of destroyed
static napi_value PoolWindow(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_type_error(env, nullptr, "poolWindow requires windowId and originWindowId");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    std::string windowId;
    int32_t originWindowId;

    if (!GetString(env, argv[0], &windowId)) {
        napi_throw_type_error(env, nullptr, "windowId must be a string");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    if (!GetInt32(env, argv[1], &originWindowId)) {
        napi_throw_type_error(env, nullptr, "originWindowId must be a number");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    std::string url;
    if (argc >= 3 && argv[2] != nullptr) {
        GetString(env, argv[2], &url);
    }

    std::lock_guard<std::mutex> lock(g_poolMutex);

    // Check if pool is at capacity
    size_t activeCount = 0;
    for (const auto& pair : g_pooledWindows) {
        if (pair.second.isActive) {
            activeCount++;
        }
    }

    PooledWindowInfo pooledInfo;
    pooledInfo.windowId = windowId;
    pooledInfo.url = url;
    pooledInfo.originWindowId = originWindowId;
    pooledInfo.lastUsed = 0;  // Will be set when acquired
    pooledInfo.isActive = false;  // Currently idle in pool

    g_pooledWindows[windowId] = pooledInfo;

    napi_value result;
    napi_create_object(env, &result);
    SetObjectBool(env, result, "success", true);
    SetObjectString(env, result, "message", "Window added to pool");

    return result;
}

// Acquire a window from the pool for reuse
// Returns the pooled window info if available, or null if pool is empty
static napi_value AcquireFromPool(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string abilityType;  // "EntryAbility" or "StatelessAbility"
    if (argc >= 1 && argv[0] != nullptr) {
        GetString(env, argv[0], &abilityType);
    }

    std::lock_guard<std::mutex> lock(g_poolMutex);

    // Find the most recently used idle window
    std::string bestCandidate;
    uint64_t latestTime = 0;

    for (auto& pair : g_pooledWindows) {
        PooledWindowInfo& info = pair.second;
        if (!info.isActive) {
            // This window is idle, available for reuse
            if (info.lastUsed > latestTime) {
                latestTime = info.lastUsed;
                bestCandidate = info.windowId;
            }
        }
    }

    napi_value result;
    napi_create_object(env, &result);

    if (!bestCandidate.empty()) {
        // Mark as active
        PooledWindowInfo& info = g_pooledWindows[bestCandidate];
        info.isActive = true;
        info.lastUsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        SetObjectBool(env, result, "found", true);
        SetObjectString(env, result, "windowId", bestCandidate);
        SetObjectString(env, result, "url", info.url);
        SetObjectInt(env, result, "originWindowId", info.originWindowId);
    } else {
        SetObjectBool(env, result, "found", false);
        SetObjectString(env, result, "message", "No windows available in pool");
    }

    return result;
}

// Mark a window as destroyed (remove from pool)
// Called when a window is actually destroyed, not hidden
static napi_value NotifyDestroyed(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string windowId;
    if (argc < 1 || !GetString(env, argv[0], &windowId)) {
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    std::lock_guard<std::mutex> lock(g_poolMutex);
    auto it = g_pooledWindows.find(windowId);
    if (it != g_pooledWindows.end()) {
        g_pooledWindows.erase(it);
    }

    napi_value result;
    napi_create_object(env, &result);
    SetObjectBool(env, result, "success", true);

    return result;
}

// Get current pool statistics
static napi_value GetPoolStats(napi_env env, napi_callback_info info)
{
    std::lock_guard<std::mutex> lock(g_poolMutex);

    size_t idleCount = 0;
    size_t activeCount = 0;

    for (const auto& pair : g_pooledWindows) {
        if (pair.second.isActive) {
            activeCount++;
        } else {
            idleCount++;
        }
    }

    napi_value result;
    napi_create_object(env, &result);
    SetObjectInt(env, result, "totalIdle", static_cast<int32_t>(idleCount));
    SetObjectInt(env, result, "active", static_cast<int32_t>(activeCount));
    SetObjectInt(env, result, "maxPoolSize", static_cast<int32_t>(g_maxPoolSize));
    SetObjectBool(env, result, "enabled", g_poolEnabled);

    return result;
}

// Configure pool settings
static napi_value ConfigurePool(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    bool enabled = true;
    int32_t maxSize = 5;

    if (argc >= 1 && argv[0] != nullptr) {
        napi_get_value_bool(env, argv[0], &enabled);
    }
    if (argc >= 2 && argv[1] != nullptr) {
        GetInt32(env, argv[1], &maxSize);
    }

    std::lock_guard<std::mutex> lock(g_poolMutex);
    g_poolEnabled = enabled;
    if (maxSize > 0) {
        g_maxPoolSize = static_cast<size_t>(maxSize);
    }

    napi_value result;
    napi_create_object(env, &result);
    SetObjectBool(env, result, "success", true);
    SetObjectBool(env, result, "enabled", g_poolEnabled);
    SetObjectInt(env, result, "maxPoolSize", static_cast<int32_t>(g_maxPoolSize));

    return result;
}

// Clear all windows from pool
static napi_value ClearPool(napi_env env, napi_callback_info info)
{
    std::lock_guard<std::mutex> lock(g_poolMutex);
    size_t count = g_pooledWindows.size();
    g_pooledWindows.clear();

    napi_value result;
    napi_create_object(env, &result);
    SetObjectBool(env, result, "success", true);
    SetObjectInt(env, result, "clearedCount", static_cast<int32_t>(count));

    return result;
}

// Handle kReuseWindow command
// This is called from TypeScript when reusing a pooled window
// Currently a placeholder - when libelectron.so source is available,
// this will:
// 1. Look up existing WebContents by windowId
// 2. Instead of creating new V8 isolate, reuse existing one
// 3. Update URL if provided
static napi_value HandleReuseWindow(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "reuseWindow requires windowId");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    std::string windowId;
    if (!GetString(env, argv[0], &windowId)) {
        napi_throw_type_error(env, nullptr, "windowId must be a string");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    std::string url;
    int32_t boundsLeft = 0, boundsTop = 0, boundsWidth = 0, boundsHeight = 0;

    if (argc >= 2 && argv[1] != nullptr) {
        GetString(env, argv[1], &url);
    }
    if (argc >= 3 && argv[2] != nullptr) {
        GetInt32(env, argv[2], &boundsLeft);
    }
    if (argc >= 4 && argv[3] != nullptr) {
        GetInt32(env, argv[3], &boundsTop);
    }
    if (argc >= 5 && argv[4] != nullptr) {
        GetInt32(env, argv[4], &boundsWidth);
    }
    // Note: height would be argv[5]

    // TODO: When libelectron.so source is available:
    // 1. Get WebContents* by windowId from tracked map
    // 2. Call web_contents->GetController().LoadURL(GURL(url))
    // 3. Update window bounds if provided
    // 4. Focus the WebContents
    // For now, just log and return success

    fprintf(stderr, "[WindowPool] reuseWindow called: windowId=%s, url=%s\n",
            windowId.c_str(), url.c_str());

    napi_value result;
    napi_create_object(env, &result);
    SetObjectBool(env, result, "success", true);
    SetObjectString(env, result, "message", "Window reuse handled (native placeholder)");

    return result;
}

// Export module
EXTERN_C_START

static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {
            "poolWindow",
            nullptr,
            PoolWindow,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "acquireFromPool",
            nullptr,
            AcquireFromPool,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "notifyDestroyed",
            nullptr,
            NotifyDestroyed,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "getPoolStats",
            nullptr,
            GetPoolStats,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "configurePool",
            nullptr,
            ConfigurePool,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "clearPool",
            nullptr,
            ClearPool,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "reuseWindow",
            nullptr,
            HandleReuseWindow,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        }
    };

    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

EXTERN_C_END

static napi_module windowPoolModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "windowpool",
    .nm_priv = nullptr,
    .reserved = {0}
};

extern "C" __attribute__((constructor)) void RegisterWindowPoolModule()
{
    napi_module_register(&windowPoolModule);
}
