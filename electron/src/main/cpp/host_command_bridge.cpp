#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <thread>
#include <chrono>
#include <atomic>

static std::atomic<bool> g_hostCmdBridgeRunning(false);

static int ConnectUnixSocketWithRetry(const char* path)
{
    while (true) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;

        if (strlen(path) >= sizeof(addr.sun_path)) {
            close(fd);
            return -1;
        }

        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }

        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

static std::string RunHostShellCommand(const std::string& command)
{
    std::string fullCommand = command + " 2>&1";

    FILE* fp = popen(fullCommand.c_str(), "r");
    if (!fp) {
        return std::string("popen failed: ") + strerror(errno) + "\n";
    }

    std::string output;
    char buffer[1024];

    while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
        output += buffer;
    }

    int status = pclose(fp);

    output += "\n[host exit status: ";
    output += std::to_string(status);
    output += "]\n";

    return output;
}

static bool IsCommandAllowed(const std::string& command)
{
    if (command.empty()) {
        return false;
    }
    
    return true;
}

static void HostCommandBridgeLoop(const char* socketPath)
{
    fprintf(stderr, "[hostcmd] bridge connecting to %s\n", socketPath);

    int fd = ConnectUnixSocketWithRetry(socketPath);
    if (fd < 0) {
        fprintf(stderr, "[hostcmd] failed to connect socket\n");
        return;
    }

    fprintf(stderr, "[hostcmd] bridge connected\n");

    std::string line;
    char ch;

    while (true) {
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) {
            fprintf(stderr, "[hostcmd] socket closed n=%zd errno=%d\n", n, errno);
            break;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch != '\n') {
            line.push_back(ch);
            continue;
        }

        std::string command = line;
        line.clear();

        fprintf(stderr, "[hostcmd] command: %s\n", command.c_str());

        std::string response;

        if (!IsCommandAllowed(command)) {
            response = "Command is not allowed.\n";
        } else {
            response = RunHostShellCommand(command);
        }

        response += "__OHCODE_HOSTCMD_END__\n";

        const char* data = response.c_str();
        size_t left = response.size();

        while (left > 0) {
            ssize_t written = write(fd, data, left);
            if (written <= 0) {
                break;
            }

            data += written;
            left -= written;
        }
    }

    close(fd);
    g_hostCmdBridgeRunning.store(false);
}

void StartHostCommandBridge(const char* socketPath)
{
    if (g_hostCmdBridgeRunning.exchange(true)) {
        return;
    }

    std::thread t([socketPath]() {
        HostCommandBridgeLoop(socketPath);
    });

    t.detach();
}