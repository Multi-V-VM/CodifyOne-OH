// Copyright (c) 2024 V8 Isolate Hook for HarmonyOS Electron
// LD_PRELOAD hook to intercept and pool V8 isolates

#include <dlfcn.h>
#include <link.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

// External symbols from libelectron.so and v8
extern "C" {

// Hook tracking structure
struct IsolateInfo {
    void* isolate_addr;
    uint32_t ref_count;
    uint64_t last_used;
    bool is_pooled;
};

// Global state
static std::unordered_map<std::string, void*> g_window_id_to_isolate;
static std::unordered_map<void*, std::string> g_isolate_to_window_id;
static std::unordered_map<void*, IsolateInfo> g_isolate_pool;
static std::mutex g_hook_mutex;
static bool g_hook_enabled = true;
static size_t g_max_pool_size = 5;

// Original function pointers
static void* (*original_v8_isolate_new)(void*, void*, void*, bool) = nullptr;
static void* (*original_v8_isolate_dispose)(void*) = nullptr;
static void* (*original_web_contents_new)(void*, void*) = nullptr;
static void* (*original_web_contents_destroy)(void*) = nullptr;

// Logging
static void hook_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[V8Hook] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// Get current thread ID
static uint64_t get_tid() {
    return (uint64_t)pthread_self();
}

// Find window ID from call stack
static std::string get_window_id_from_context() {
    // Try to get window ID from environment or thread-local storage
    const char* window_id = getenv("ELECTRON_WINDOW_ID");
    if (window_id && strlen(window_id) > 0) {
        return std::string(window_id);
    }

    // Try to get from TLS (set by our TypeScript layer)
    void* tls_addr = pthread_getspecific(1);  // Use a well-known TLS slot
    if (tls_addr) {
        return std::string(static_cast<char*>(tls_addr));
    }

    return "";
}

// Set window ID for current context (to be called from native bridge)
extern "C" void v8_hook_set_window_id(const char* window_id) {
    if (window_id) {
        setenv("ELECTRON_WINDOW_ID", window_id, 1);
        hook_log("Set window ID context: %s", window_id);
    }
}

// Mark window for pooling
extern "C" void v8_hook_pool_window(const char* window_id) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);

    auto it = g_isolate_to_window_id.begin();
    while (it != g_isolate_to_window_id.end()) {
        if (it->second == window_id) {
            void* isolate = it->first;
            if (g_isolate_pool.find(isolate) != g_isolate_pool.end()) {
                g_isolate_pool[isolate].is_pooled = true;
                g_isolate_pool[isolate].last_used = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                hook_log("Pooled isolate %p for window %s", isolate, window_id);
            }
            break;
        }
        ++it;
    }
}

// Mark window as destroyed
extern "C" void v8_hook_notify_destroyed(const char* window_id) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);

    auto it = g_isolate_to_window_id.begin();
    while (it != g_isolate_to_window_id.end()) {
        if (it->second == window_id) {
            void* isolate = it->first;
            g_isolate_pool.erase(isolate);
            g_window_id_to_isolate.erase(window_id);
            hook_log("Removed isolate %p for destroyed window %s", isolate, window_id);
            g_isolate_to_window_id.erase(it);
            break;
        }
        ++it;
    }
}

// Get pooled isolate for window
extern "C" void* v8_hook_get_pooled_isolate(const char* window_id) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);

    auto it = g_window_id_to_isolate.find(window_id);
    if (it != g_window_id_to_isolate.end()) {
        void* isolate = it->second;
        if (g_isolate_pool.find(isolate) != g_isolate_pool.end()) {
            g_isolate_pool[isolate].is_pooled = false;
            g_isolate_pool[isolate].last_used = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            hook_log("Reusing pooled isolate %p for window %s", isolate, window_id);
            return isolate;
        }
    }
    return nullptr;
}

// Configure hook
extern "C" void v8_hook_configure(bool enabled, size_t max_pool_size) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    g_hook_enabled = enabled;
    g_max_pool_size = max_pool_size;
    hook_log("Hook configured: enabled=%d, max_pool=%zu", enabled, max_pool_size);
}

// Get pool statistics
extern "C" void v8_hook_get_stats(size_t* pool_size, size_t* active_count) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    *pool_size = g_isolate_pool.size();
    *active_count = g_window_id_to_isolate.size();
}

// Clear pool
extern "C" void v8_hook_clear_pool() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    size_t count = g_isolate_pool.size();
    g_isolate_pool.clear();
    g_window_id_to_isolate.clear();
    g_isolate_to_window_id.clear();
    hook_log("Cleared pool: %zu isolates", count);
}

// Constructor - runs when library is loaded
__attribute__((constructor))
static void v8_hook_init() {
    hook_log("V8 Isolate Hook loaded");
    hook_log("Hook enabled for V8 isolate pooling");
}

// Destructor - runs when library is unloaded
__attribute__((destructor))
static void v8_hook_cleanup() {
    v8_hook_clear_pool();
    hook_log("V8 Isolate Hook unloaded");
}

}  // extern "C"

// Hook using PLT/GOT interception
// These functions will be called instead of the originals

// Intercept V8 Isolate::New
extern "C" void* v8_Isolate_New(
    void* snapshot_data,
    void* external_references,
    void* blob,
    bool ownership_of_external_references
) {
    static std::mutex init_mutex;
    static bool initialized = false;
    static bool hooks_installed = false;

    std::lock_guard<std::mutex> lock(init_mutex);

    // Initialize original function pointer on first call
    if (!initialized) {
        original_v8_isolate_new = (void*(*)(void*, void*, void*, bool))
            dlsym(RTLD_NEXT, "_ZN2v88Isolate3NewEPNS_12SnapshotDataEPNS_21ExternalReferencesEPN_10StartupDataEb");

        if (!original_v8_isolate_new) {
            // Try alternative mangled names
            original_v8_isolate_new = (void*(*)(void*, void*, void*, bool))
                dlsym(RTLD_NEXT, "_ZN2v88Isolate3NewENS_12SnapshotDataEPNS_21ExternalReferencesEPN_10StartupDataEb");
        }

        if (original_v8_isolate_new) {
            hook_log("Found V8::Isolate::New at %p", original_v8_isolate_new);
        } else {
            hook_log("WARNING: Could not find V8::Isolate::New symbol");
        }
        initialized = true;
    }

    std::string window_id = get_window_id_from_context();

    // Check if we should reuse an existing isolate
    if (g_hook_enabled && !window_id.empty()) {
        void* pooled_isolate = v8_hook_get_pooled_isolate(window_id.c_str());
        if (pooled_isolate) {
            hook_log("Reusing V8 isolate %p for window %s (NEW HOOK)", pooled_isolate, window_id);
            return pooled_isolate;
        }
    }

    // Call original function
    if (original_v8_isolate_new) {
        void* new_isolate = original_v8_isolate_new(
            snapshot_data, external_references, blob, ownership_of_external_references
        );

        // Track the new isolate
        if (new_isolate && !window_id.empty()) {
            std::lock_guard<std::mutex> lock2(g_hook_mutex);
            g_window_id_to_isolate[window_id] = new_isolate;
            g_isolate_to_window_id[new_isolate] = window_id;

            IsolateInfo info;
            info.isolate_addr = new_isolate;
            info.ref_count = 1;
            info.last_used = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            info.is_pooled = false;
            g_isolate_pool[new_isolate] = info;

            hook_log("Tracked new V8 isolate %p for window %s", new_isolate, window_id);
        }

        return new_isolate;
    }

    hook_log("ERROR: V8::Isolate::New not found, cannot create isolate");
    return nullptr;
}

// Simpler approach: hook at the dlopen level
extern "C" void* dlopen(const char* filename, int flag) {
    static void* (*real_dlopen)(const char*, int) = nullptr;
    if (!real_dlopen) {
        real_dlopen = (void*(*)(const char*, int))dlsym(RTLD_NEXT, "dlopen");
    }

    void* result = real_dlopen(filename, flag);
    hook_log("dlopen: %s -> %p", filename, result);

    return result;
}
