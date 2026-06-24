#include "napi/native_api.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

struct wasm_engine_t;
struct wasm_store_t;
struct wasm_module_t;
struct wasm_instance_t;
struct wasm_extern_t;
struct wasm_func_t;
struct wasm_trap_t;
struct wasm_val_t;
struct wasi_config_t;
struct wasi_env_t;

typedef char wasm_byte_t;

typedef struct wasm_byte_vec_t {
    size_t size;
    wasm_byte_t* data;
} wasm_byte_vec_t;

typedef wasm_byte_vec_t wasm_message_t;

typedef struct wasm_extern_vec_t {
    size_t size;
    wasm_extern_t** data;
} wasm_extern_vec_t;

typedef struct wasm_val_vec_t {
    size_t size;
    wasm_val_t* data;
} wasm_val_vec_t;

using wasm_engine_new_fn = wasm_engine_t* (*)();
using wasm_engine_delete_fn = void (*)(wasm_engine_t*);
using wasm_store_new_fn = wasm_store_t* (*)(wasm_engine_t*);
using wasm_store_delete_fn = void (*)(wasm_store_t*);
using wasm_byte_vec_new_uninitialized_fn = void (*)(wasm_byte_vec_t*, size_t);
using wasm_byte_vec_delete_fn = void (*)(wasm_byte_vec_t*);
using wasm_module_new_fn = wasm_module_t* (*)(wasm_store_t*, const wasm_byte_vec_t*);
using wasm_module_delete_fn = void (*)(wasm_module_t*);
using wasm_extern_vec_delete_fn = void (*)(wasm_extern_vec_t*);
using wasm_instance_new_fn = wasm_instance_t* (*)(
    wasm_store_t*, const wasm_module_t*, const wasm_extern_vec_t*, wasm_trap_t**);
using wasm_instance_delete_fn = void (*)(wasm_instance_t*);
using wasm_func_call_fn = wasm_trap_t* (*)(const wasm_func_t*, const wasm_val_vec_t*, wasm_val_vec_t*);
using wasm_func_delete_fn = void (*)(wasm_func_t*);
using wasm_trap_message_fn = void (*)(const wasm_trap_t*, wasm_message_t*);
using wasm_trap_delete_fn = void (*)(wasm_trap_t*);

using wasi_config_new_fn = wasi_config_t* (*)(const char*);
using wasi_config_arg_fn = void (*)(wasi_config_t*, const char*);
using wasi_config_env_fn = void (*)(wasi_config_t*, const char*, const char*);
using wasi_config_preopen_dir_fn = bool (*)(wasi_config_t*, const char*);
using wasi_config_mapdir_fn = bool (*)(wasi_config_t*, const char*, const char*);
using wasi_config_capture_stdout_fn = void (*)(wasi_config_t*);
using wasi_config_capture_stderr_fn = void (*)(wasi_config_t*);
using wasi_env_new_fn = wasi_env_t* (*)(wasm_store_t*, wasi_config_t*);
using wasi_env_delete_fn = void (*)(wasi_env_t*);
using wasi_get_imports_fn = bool (*)(const wasm_store_t*, wasi_env_t*, const wasm_module_t*, wasm_extern_vec_t*);
using wasi_env_initialize_instance_fn = bool (*)(wasi_env_t*, wasm_store_t*, wasm_instance_t*);
using wasi_get_start_function_fn = wasm_func_t* (*)(wasm_instance_t*);
using wasi_env_read_stdout_fn = ssize_t (*)(wasi_env_t*, char*, size_t);
using wasi_env_read_stderr_fn = ssize_t (*)(wasi_env_t*, char*, size_t);

using wasmer_last_error_length_fn = int (*)();
using wasmer_last_error_message_fn = int (*)(char*, int);

struct WasmerApi {
    void* handle = nullptr;

    wasm_engine_new_fn wasm_engine_new = nullptr;
    wasm_engine_delete_fn wasm_engine_delete = nullptr;
    wasm_store_new_fn wasm_store_new = nullptr;
    wasm_store_delete_fn wasm_store_delete = nullptr;
    wasm_byte_vec_new_uninitialized_fn wasm_byte_vec_new_uninitialized = nullptr;
    wasm_byte_vec_delete_fn wasm_byte_vec_delete = nullptr;
    wasm_module_new_fn wasm_module_new = nullptr;
    wasm_module_delete_fn wasm_module_delete = nullptr;
    wasm_extern_vec_delete_fn wasm_extern_vec_delete = nullptr;
    wasm_instance_new_fn wasm_instance_new = nullptr;
    wasm_instance_delete_fn wasm_instance_delete = nullptr;
    wasm_func_call_fn wasm_func_call = nullptr;
    wasm_func_delete_fn wasm_func_delete = nullptr;
    wasm_trap_message_fn wasm_trap_message = nullptr;
    wasm_trap_delete_fn wasm_trap_delete = nullptr;

    wasi_config_new_fn wasi_config_new = nullptr;
    wasi_config_arg_fn wasi_config_arg = nullptr;
    wasi_config_env_fn wasi_config_env = nullptr;
    wasi_config_preopen_dir_fn wasi_config_preopen_dir = nullptr;
    wasi_config_mapdir_fn wasi_config_mapdir = nullptr;
    wasi_config_capture_stdout_fn wasi_config_capture_stdout = nullptr;
    wasi_config_capture_stderr_fn wasi_config_capture_stderr = nullptr;
    wasi_env_new_fn wasi_env_new = nullptr;
    wasi_env_delete_fn wasi_env_delete = nullptr;
    wasi_get_imports_fn wasi_get_imports = nullptr;
    wasi_env_initialize_instance_fn wasi_env_initialize_instance = nullptr;
    wasi_get_start_function_fn wasi_get_start_function = nullptr;
    wasi_env_read_stdout_fn wasi_env_read_stdout = nullptr;
    wasi_env_read_stderr_fn wasi_env_read_stderr = nullptr;

    wasmer_last_error_length_fn wasmer_last_error_length = nullptr;
    wasmer_last_error_message_fn wasmer_last_error_message = nullptr;
};

struct WasiRunResult {
    int exitCode = 1;
    std::string stdoutText;
    std::string stderrText;
    std::string error;
};

static WasmerApi g_wasmer;
static std::atomic<bool> g_wasmerReady(false);
static std::mutex g_wasmerMutex;
static std::string g_lastHostError;

static void SetLastHostError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(g_wasmerMutex);
    g_lastHostError = error;
}

static std::string Basename(const std::string& path)
{
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos || pos + 1 >= path.size()) {
        return path.empty() ? "wasm" : path;
    }
    return path.substr(pos + 1);
}

template <typename T>
static bool ResolveRequired(void* handle, T& fn, const char* name)
{
    fn = reinterpret_cast<T>(dlsym(handle, name));
    if (fn == nullptr) {
        g_lastHostError = std::string("missing Wasmer C API symbol: ") + name;
        return false;
    }
    return true;
}

template <typename T>
static void ResolveOptional(void* handle, T& fn, const char* name)
{
    fn = reinterpret_cast<T>(dlsym(handle, name));
}

static std::string ReadWasmerLastError()
{
    if (g_wasmer.wasmer_last_error_length == nullptr || g_wasmer.wasmer_last_error_message == nullptr) {
        return "";
    }

    int len = g_wasmer.wasmer_last_error_length();
    if (len <= 0) {
        return "";
    }

    std::vector<char> buffer(static_cast<size_t>(len), '\0');
    int written = g_wasmer.wasmer_last_error_message(buffer.data(), len);
    if (written <= 0) {
        return "";
    }

    return std::string(buffer.data());
}

static bool LoadWasmer()
{
    if (g_wasmerReady.load()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_wasmerMutex);
    if (g_wasmerReady.load()) {
        return true;
    }

    const char* candidates[] = {
        "libwasmer.so",
        "/data/storage/el2/base/files/wasmer/libwasmer.so",
        "/data/storage/el2/base/files/libs/libwasmer.so"
    };

    void* handle = nullptr;
    std::string dlErrors;
    for (const char* candidate : candidates) {
        handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
        if (handle != nullptr) {
            fprintf(stderr, "[wasmer] loaded %s\n", candidate);
            break;
        }
        const char* err = dlerror();
        dlErrors += candidate;
        dlErrors += ": ";
        dlErrors += err == nullptr ? "unknown dlopen error" : err;
        dlErrors += "\n";
    }

    if (handle == nullptr) {
        g_lastHostError = "dlopen libwasmer.so failed:\n" + dlErrors;
        fprintf(stderr, "[wasmer] %s\n", g_lastHostError.c_str());
        return false;
    }

    WasmerApi api;
    api.handle = handle;

    bool ok = true;
    ok &= ResolveRequired(handle, api.wasm_engine_new, "wasm_engine_new");
    ok &= ResolveRequired(handle, api.wasm_engine_delete, "wasm_engine_delete");
    ok &= ResolveRequired(handle, api.wasm_store_new, "wasm_store_new");
    ok &= ResolveRequired(handle, api.wasm_store_delete, "wasm_store_delete");
    ok &= ResolveRequired(handle, api.wasm_byte_vec_new_uninitialized, "wasm_byte_vec_new_uninitialized");
    ok &= ResolveRequired(handle, api.wasm_byte_vec_delete, "wasm_byte_vec_delete");
    ok &= ResolveRequired(handle, api.wasm_module_new, "wasm_module_new");
    ok &= ResolveRequired(handle, api.wasm_module_delete, "wasm_module_delete");
    ok &= ResolveRequired(handle, api.wasm_extern_vec_delete, "wasm_extern_vec_delete");
    ok &= ResolveRequired(handle, api.wasm_instance_new, "wasm_instance_new");
    ok &= ResolveRequired(handle, api.wasm_instance_delete, "wasm_instance_delete");
    ok &= ResolveRequired(handle, api.wasm_func_call, "wasm_func_call");
    ok &= ResolveRequired(handle, api.wasm_func_delete, "wasm_func_delete");
    ok &= ResolveRequired(handle, api.wasm_trap_message, "wasm_trap_message");
    ok &= ResolveRequired(handle, api.wasm_trap_delete, "wasm_trap_delete");

    ok &= ResolveRequired(handle, api.wasi_config_new, "wasi_config_new");
    ok &= ResolveRequired(handle, api.wasi_config_arg, "wasi_config_arg");
    ok &= ResolveRequired(handle, api.wasi_config_env, "wasi_config_env");
    ok &= ResolveRequired(handle, api.wasi_config_preopen_dir, "wasi_config_preopen_dir");
    ok &= ResolveRequired(handle, api.wasi_config_capture_stdout, "wasi_config_capture_stdout");
    ok &= ResolveRequired(handle, api.wasi_env_new, "wasi_env_new");
    ok &= ResolveRequired(handle, api.wasi_env_delete, "wasi_env_delete");
    ok &= ResolveRequired(handle, api.wasi_get_imports, "wasi_get_imports");
    ok &= ResolveRequired(handle, api.wasi_env_initialize_instance, "wasi_env_initialize_instance");
    ok &= ResolveRequired(handle, api.wasi_get_start_function, "wasi_get_start_function");
    ok &= ResolveRequired(handle, api.wasi_env_read_stdout, "wasi_env_read_stdout");

    ResolveOptional(handle, api.wasi_config_mapdir, "wasi_config_mapdir");
    ResolveOptional(handle, api.wasi_config_capture_stderr, "wasi_config_capture_stderr");
    ResolveOptional(handle, api.wasi_env_read_stderr, "wasi_env_read_stderr");
    ResolveOptional(handle, api.wasmer_last_error_length, "wasmer_last_error_length");
    ResolveOptional(handle, api.wasmer_last_error_message, "wasmer_last_error_message");

    if (!ok) {
        dlclose(handle);
        fprintf(stderr, "[wasmer] failed to resolve required C API symbols\n");
        return false;
    }

    g_wasmer = api;
    g_lastHostError.clear();
    g_wasmerReady.store(true);
    return true;
}

static bool ReadFileIntoWasmerVec(const std::string& path, wasm_byte_vec_t* out, std::string* error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        *error = "failed to open wasm module: " + path;
        return false;
    }

    std::streamsize size = file.tellg();
    if (size <= 0) {
        *error = "wasm module is empty: " + path;
        return false;
    }

    file.seekg(0, std::ios::beg);
    g_wasmer.wasm_byte_vec_new_uninitialized(out, static_cast<size_t>(size));
    if (out->data == nullptr) {
        *error = "wasm_byte_vec_new_uninitialized returned null";
        return false;
    }

    if (!file.read(out->data, size)) {
        g_wasmer.wasm_byte_vec_delete(out);
        *error = "failed to read wasm module: " + path;
        return false;
    }

    return true;
}

static std::string TrapMessage(wasm_trap_t* trap)
{
    if (trap == nullptr) {
        return "";
    }

    wasm_message_t message {0, nullptr};
    g_wasmer.wasm_trap_message(trap, &message);

    std::string text;
    if (message.data != nullptr && message.size > 0) {
        text.assign(message.data, message.size);
        if (!text.empty() && text.back() == '\0') {
            text.pop_back();
        }
        g_wasmer.wasm_byte_vec_delete(&message);
    }

    g_wasmer.wasm_trap_delete(trap);
    return text;
}

static std::string ReadCapturedWasiPipe(wasi_env_t* env, ssize_t (*reader)(wasi_env_t*, char*, size_t))
{
    if (reader == nullptr) {
        return "";
    }

    std::string output;
    char buffer[4096];
    while (true) {
        ssize_t read = reader(env, buffer, sizeof(buffer));
        if (read < 0) {
            std::string err = ReadWasmerLastError();
            if (!err.empty()) {
                output += err;
            }
            break;
        }

        if (read == 0) {
            break;
        }

        output.append(buffer, static_cast<size_t>(read));
        if (static_cast<size_t>(read) < sizeof(buffer)) {
            break;
        }
    }
    return output;
}

static WasiRunResult RunWasiModuleInternal(
    const std::string& modulePath,
    const std::vector<std::string>& args,
    const std::string& preopenDir)
{
    WasiRunResult result;

    if (!LoadWasmer()) {
        result.error = g_lastHostError;
        return result;
    }

    wasm_engine_t* engine = g_wasmer.wasm_engine_new();
    if (engine == nullptr) {
        result.error = "wasm_engine_new failed: " + ReadWasmerLastError();
        return result;
    }

    wasm_store_t* store = g_wasmer.wasm_store_new(engine);
    if (store == nullptr) {
        result.error = "wasm_store_new failed: " + ReadWasmerLastError();
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    wasm_byte_vec_t binary {0, nullptr};
    std::string readError;
    if (!ReadFileIntoWasmerVec(modulePath, &binary, &readError)) {
        result.error = readError;
        g_wasmer.wasm_store_delete(store);
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    wasm_module_t* module = g_wasmer.wasm_module_new(store, &binary);
    g_wasmer.wasm_byte_vec_delete(&binary);
    if (module == nullptr) {
        result.error = "wasm_module_new failed: " + ReadWasmerLastError();
        g_wasmer.wasm_store_delete(store);
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    std::string programName = Basename(modulePath);
    wasi_config_t* config = g_wasmer.wasi_config_new(programName.c_str());
    if (config == nullptr) {
        result.error = "wasi_config_new failed: " + ReadWasmerLastError();
        g_wasmer.wasm_module_delete(module);
        g_wasmer.wasm_store_delete(store);
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    g_wasmer.wasi_config_capture_stdout(config);
    if (g_wasmer.wasi_config_capture_stderr != nullptr) {
        g_wasmer.wasi_config_capture_stderr(config);
    }

    g_wasmer.wasi_config_env(config, "HOME", preopenDir.c_str());
    g_wasmer.wasi_config_env(config, "TMPDIR", preopenDir.c_str());
    g_wasmer.wasi_config_env(config, "OHCODE_WASMER", "1");

    if (!preopenDir.empty()) {
        bool mapped = false;
        if (g_wasmer.wasi_config_mapdir != nullptr) {
            mapped = g_wasmer.wasi_config_mapdir(config, "/workspace", preopenDir.c_str());
        }
        if (!mapped && !g_wasmer.wasi_config_preopen_dir(config, preopenDir.c_str())) {
            std::string preopenError = ReadWasmerLastError();
            fprintf(stderr, "[wasmer] preopen failed for %s: %s\n", preopenDir.c_str(), preopenError.c_str());
        }
    }

    for (const std::string& arg : args) {
        g_wasmer.wasi_config_arg(config, arg.c_str());
    }

    wasi_env_t* wasiEnv = g_wasmer.wasi_env_new(store, config);
    config = nullptr;
    if (wasiEnv == nullptr) {
        result.error = "wasi_env_new failed: " + ReadWasmerLastError();
        g_wasmer.wasm_module_delete(module);
        g_wasmer.wasm_store_delete(store);
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    wasm_extern_vec_t imports {0, nullptr};
    if (!g_wasmer.wasi_get_imports(store, wasiEnv, module, &imports)) {
        result.error = "wasi_get_imports failed: " + ReadWasmerLastError();
        g_wasmer.wasi_env_delete(wasiEnv);
        g_wasmer.wasm_module_delete(module);
        g_wasmer.wasm_store_delete(store);
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    wasm_trap_t* trap = nullptr;
    wasm_instance_t* instance = g_wasmer.wasm_instance_new(store, module, &imports, &trap);
    g_wasmer.wasm_extern_vec_delete(&imports);
    if (instance == nullptr) {
        result.error = "wasm_instance_new failed: ";
        result.error += trap != nullptr ? TrapMessage(trap) : ReadWasmerLastError();
        g_wasmer.wasi_env_delete(wasiEnv);
        g_wasmer.wasm_module_delete(module);
        g_wasmer.wasm_store_delete(store);
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    if (!g_wasmer.wasi_env_initialize_instance(wasiEnv, store, instance)) {
        result.error = "wasi_env_initialize_instance failed: " + ReadWasmerLastError();
        g_wasmer.wasm_instance_delete(instance);
        g_wasmer.wasi_env_delete(wasiEnv);
        g_wasmer.wasm_module_delete(module);
        g_wasmer.wasm_store_delete(store);
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    wasm_func_t* start = g_wasmer.wasi_get_start_function(instance);
    if (start == nullptr) {
        result.error = "wasi_get_start_function failed: " + ReadWasmerLastError();
        g_wasmer.wasm_instance_delete(instance);
        g_wasmer.wasi_env_delete(wasiEnv);
        g_wasmer.wasm_module_delete(module);
        g_wasmer.wasm_store_delete(store);
        g_wasmer.wasm_engine_delete(engine);
        return result;
    }

    wasm_val_vec_t emptyArgs {0, nullptr};
    wasm_val_vec_t emptyResults {0, nullptr};
    wasm_trap_t* callTrap = g_wasmer.wasm_func_call(start, &emptyArgs, &emptyResults);
    if (callTrap != nullptr) {
        result.error = "wasm_func_call trapped: " + TrapMessage(callTrap);
    } else {
        result.exitCode = 0;
    }

    result.stdoutText = ReadCapturedWasiPipe(wasiEnv, g_wasmer.wasi_env_read_stdout);
    result.stderrText = ReadCapturedWasiPipe(wasiEnv, g_wasmer.wasi_env_read_stderr);

    g_wasmer.wasm_func_delete(start);
    g_wasmer.wasm_instance_delete(instance);
    g_wasmer.wasi_env_delete(wasiEnv);
    g_wasmer.wasm_module_delete(module);
    g_wasmer.wasm_store_delete(store);
    g_wasmer.wasm_engine_delete(engine);

    return result;
}

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

static std::vector<std::string> GetStringArray(napi_env env, napi_value value)
{
    std::vector<std::string> result;
    bool isArray = false;
    if (napi_is_array(env, value, &isArray) != napi_ok || !isArray) {
        return result;
    }

    uint32_t length = 0;
    if (napi_get_array_length(env, value, &length) != napi_ok) {
        return result;
    }

    for (uint32_t i = 0; i < length; ++i) {
        napi_value item = nullptr;
        if (napi_get_element(env, value, i, &item) != napi_ok) {
            continue;
        }

        std::string text;
        if (GetString(env, item, &text)) {
            result.push_back(text);
        }
    }

    return result;
}

static void SetObjectString(napi_env env, napi_value object, const char* key, const std::string& value)
{
    napi_value jsValue;
    napi_create_string_utf8(env, value.c_str(), value.size(), &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

static void SetObjectInt(napi_env env, napi_value object, const char* key, int value)
{
    napi_value jsValue;
    napi_create_int32(env, value, &jsValue);
    napi_set_named_property(env, object, key, jsValue);
}

static napi_value CreateRunResult(napi_env env, const WasiRunResult& result)
{
    napi_value object;
    napi_create_object(env, &object);
    SetObjectInt(env, object, "exitCode", result.exitCode);
    SetObjectString(env, object, "stdout", result.stdoutText);
    SetObjectString(env, object, "stderr", result.stderrText);
    SetObjectString(env, object, "error", result.error);
    return object;
}

static napi_value StartWasmer(napi_env env, napi_callback_info info)
{
    setenv("HOME", "/storage/Users/currentUser", 1);
    setenv("TMPDIR", "/data/storage/el2/base/cache", 1);

    bool ok = LoadWasmer();

    napi_value ret;
    napi_get_boolean(env, ok, &ret);
    return ret;
}

static napi_value IsWasmerReady(napi_env env, napi_callback_info info)
{
    napi_value ret;
    napi_get_boolean(env, g_wasmerReady.load(), &ret);
    return ret;
}

static napi_value GetWasmerLastError(napi_env env, napi_callback_info info)
{
    std::string error = ReadWasmerLastError();
    if (error.empty()) {
        std::lock_guard<std::mutex> lock(g_wasmerMutex);
        error = g_lastHostError;
    }

    napi_value ret;
    napi_create_string_utf8(env, error.c_str(), error.size(), &ret);
    return ret;
}

static napi_value RunWasiModule(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "runWasiModule requires a wasm module path");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    std::string modulePath;
    if (!GetString(env, argv[0], &modulePath) || modulePath.empty()) {
        napi_throw_type_error(env, nullptr, "runWasiModule module path must be a non-empty string");
        napi_value ret;
        napi_get_undefined(env, &ret);
        return ret;
    }

    std::vector<std::string> args;
    if (argc >= 2 && argv[1] != nullptr) {
        args = GetStringArray(env, argv[1]);
    }

    std::string preopenDir = "/data/storage/el2/base/files/wasmer/workspace";
    if (argc >= 3 && argv[2] != nullptr) {
        std::string requestedPreopen;
        if (GetString(env, argv[2], &requestedPreopen) && !requestedPreopen.empty()) {
            preopenDir = requestedPreopen;
        }
    }

    WasiRunResult result = RunWasiModuleInternal(modulePath, args, preopenDir);
    return CreateRunResult(env, result);
}

EXTERN_C_START

static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {
            "startWasmer",
            nullptr,
            StartWasmer,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "isWasmerReady",
            nullptr,
            IsWasmerReady,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "getWasmerLastError",
            nullptr,
            GetWasmerLastError,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "runWasiModule",
            nullptr,
            RunWasiModule,
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

static napi_module wasmerModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0}
};

extern "C" __attribute__((constructor)) void RegisterEntryModule()
{
    napi_module_register(&wasmerModule);
}
