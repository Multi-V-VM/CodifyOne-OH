#include "napi/native_api.h"
#include "host_command_bridge.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <unistd.h>

typedef int (*QemuSystemEntry)(int, const char **);

static std::atomic<bool> g_qemuStarted(false);
static QemuSystemEntry g_qemuEntry = nullptr;
static void* g_qemuHandle = nullptr;

static const char* KERNEL_PATH = "/data/storage/el2/base/files/qemu/Image";
static const char* ROOTFS_PATH = "/data/storage/el2/base/files/qemu/rootfs.cpio.gz";
static const char* HOSTCMD_SOCKET = "/data/storage/el2/base/files/qemu/hostcmd.sock";
static const char* CONSOLE_LOG_PATH = "/data/storage/el2/base/files/qemu/console.log";

static const char* HOST_WORKSPACE_PATH = "/storage/Users/currentUser";

static QemuSystemEntry LoadQemuEntry()
{
    if (g_qemuEntry != nullptr) {
        return g_qemuEntry;
    }

    const char* libPath = "libqemu-system-aarch64.so";

    g_qemuHandle = dlopen(libPath, RTLD_LAZY | RTLD_LOCAL);
    if (!g_qemuHandle) {
        fprintf(stderr, "[qemu] dlopen failed: %s\n", dlerror());
        return nullptr;
    }

    g_qemuEntry = reinterpret_cast<QemuSystemEntry>(
        dlsym(g_qemuHandle, "qemu_system_entry")
    );

    if (!g_qemuEntry) {
        fprintf(stderr, "[qemu] dlsym qemu_system_entry failed: %s\n", dlerror());
        return nullptr;
    }

    fprintf(stderr, "[qemu] qemu_system_entry loaded: %p\n", reinterpret_cast<void*>(g_qemuEntry));
    return g_qemuEntry;
}

static void RunQemuThread()
{
    QemuSystemEntry entry = LoadQemuEntry();
    if (!entry) {
        g_qemuStarted.store(false);
        return;
    }

    setenv("HOME", "/storage/Users/currentUser", 1);
    setenv("TMPDIR", "/data/storage/el2/base/cache", 1);
    
    unlink(HOSTCMD_SOCKET);
    StartHostCommandBridge(HOSTCMD_SOCKET);

    std::vector<std::string> args = {
        "qemu-system-aarch64",
    
        "-nodefaults",
        "-no-user-config",
    
        "-M",
        "virt",
    
        "-cpu",
        "cortex-a57",
    
        "-m",
        "512M",
    
        "-kernel",
        KERNEL_PATH,
    
        "-initrd",
        ROOTFS_PATH,
    
        "-append",
        "console=ttyAMA0,115200 rdinit=/sbin/init",
    
        "-display",
        "none",
    
        "-monitor",
        "none",
    
        "-serial",
        std::string("file:") + CONSOLE_LOG_PATH,
    
        "-netdev",
        "user,id=net0,hostfwd=tcp:127.0.0.1:39001-:7681",

        "-device",

        "virtio-net-pci,netdev=net0,romfile=",
        
        "-fsdev",
        std::string("local,security_model=mapped-file,id=fsdev0,path=") + HOST_WORKSPACE_PATH,
        
        "-device",
        "virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=usershare",
        
        "-device",
        "virtio-serial-pci,id=virtio-serial0",

        "-chardev",
        std::string("socket,path=") + HOSTCMD_SOCKET + ",server=on,wait=off,id=hostcmd0",

        "-device",
        "virtserialport,chardev=hostcmd0,bus=virtio-serial0.0,name=ohcode.hostcmd"
    };

    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);

    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }

    argv.push_back(nullptr);

    fprintf(stderr, "[qemu] starting qemu_system_entry argc=%zu\n", args.size());

    int ret = entry(static_cast<int>(args.size()), argv.data());

    fprintf(stderr, "[qemu] qemu_system_entry exited ret=%d\n", ret);

    g_qemuStarted.store(false);
}

static napi_value StartQemu(napi_env env, napi_callback_info info)
{
    if (g_qemuStarted.load()) {
        napi_value ret;
        napi_get_boolean(env, true, &ret);
        return ret;
    }

    g_qemuStarted.store(true);

    std::thread t(RunQemuThread);
    t.detach();

    napi_value ret;
    napi_get_boolean(env, true, &ret);
    return ret;
}

static napi_value IsQemuStarted(napi_env env, napi_callback_info info)
{
    napi_value ret;
    napi_get_boolean(env, g_qemuStarted.load(), &ret);
    return ret;
}

EXTERN_C_START

static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {
            "startQemu",
            nullptr,
            StartQemu,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr
        },
        {
            "isQemuStarted",
            nullptr,
            IsQemuStarted,
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

static napi_module qemuModule = {
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
    napi_module_register(&qemuModule);
}