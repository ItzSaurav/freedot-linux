#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <cstdlib>

namespace fs = std::filesystem;

enum class ServiceType {
    DAEMON,
    INTERACTIVE_SHELL
};

struct Service {
    std::string name;
    std::string path;
    std::vector<std::string> args;
    ServiceType type;
    pid_t pid = -1;
    bool respawn = true;
};

static std::vector<Service> services;
static volatile sig_atomic_t poweroff_requested = 0;
static volatile sig_atomic_t reboot_requested = 0;
static int server_sock_fd = -1;

constexpr const char* SOCKET_PATH = "/run/freedot.sock";
constexpr const char* CONFIG_DIR = "/etc/freedot.d";

void handle_sigchld(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (auto& svc : services) {
            if (svc.pid == pid) {
                svc.pid = -1;
                break;
            }
        }
    }
}

void handle_shutdown_signal(int sig) {
    if (sig == SIGINT || sig == SIGPWR) {
        poweroff_requested = 1;
    } else if (sig == SIGTERM) {
        reboot_requested = 1;
    }
}

void spawn_service(Service& svc) {
    pid_t pid = fork();

    if (pid < 0) {
        perror(("[FreeDot Init] Fork failed for " + svc.name).c_str());
        return;
    }

    if (pid == 0) {
        if (svc.type == ServiceType::INTERACTIVE_SHELL) {
            setsid();
            int fd = open("/dev/ttyS0", O_RDWR);
            if (fd < 0) {
                fd = open("/dev/console", O_RDWR);
            }
            if (fd >= 0) {
                ioctl(fd, TIOCSCTTY, 1);
                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) close(fd);
            }
        }

        std::vector<char*> c_args;
        for (const auto& arg : svc.args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        char* const env[] = {
            (char*)"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
            (char*)"TERM=vt100",
            (char*)"HOME=/root",
            (char*)"USER=root",
            nullptr
        };

        execve(svc.path.c_str(), c_args.data(), env);
        perror(("[FreeDot Init] execve failed for " + svc.name).c_str());
        exit(1);
    } else {
        svc.pid = pid;
        std::cout << "[FreeDot Init] Started " << svc.name << " (PID: " << pid << ")\n";
    }
}

void load_services_from_disk() {
    if (!fs::exists(CONFIG_DIR)) {
        std::cerr << "[FreeDot Init] Config directory " << CONFIG_DIR << " not found.\n";
        return;
    }

    std::vector<fs::path> config_files;
    for (const auto& entry : fs::directory_iterator(CONFIG_DIR)) {
        if (entry.is_regular_file() && entry.path().extension() == ".conf") {
            config_files.push_back(entry.path());
        }
    }
    std::sort(config_files.begin(), config_files.end());

    for (const auto& file_path : config_files) {
        std::ifstream file(file_path);
        if (!file.is_open()) continue;

        Service svc;
        svc.type = ServiceType::DAEMON;
        svc.respawn = true;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            auto delimiter_pos = line.find('=');
            if (delimiter_pos == std::string::npos) continue;

            std::string key = line.substr(0, delimiter_pos);
            std::string value = line.substr(delimiter_pos + 1);

            if (key == "name") {
                svc.name = value;
            } else if (key == "exec") {
                svc.path = value;
                svc.args = {value.substr(value.find_last_of('/') + 1)};
            } else if (key == "type") {
                if (value == "interactive") {
                    svc.type = ServiceType::INTERACTIVE_SHELL;
                } else {
                    svc.type = ServiceType::DAEMON;
                }
            } else if (key == "respawn") {
                svc.respawn = (value == "true" || value == "1");
            }
        }

        if (!svc.name.empty() && !svc.path.empty()) {
            bool exists = false;
            for (const auto& existing : services) {
                if (existing.name == svc.name) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                services.push_back(svc);
                std::cout << "[FreeDot Init] Loaded unit file: " << file_path.filename().string() << " (" << svc.name << ")\n";
            }
        }
    }
}

void perform_shutdown(int cmd) {
    std::cout << "\n=========================================\n";
    std::cout << "  FreeDot Init: Shutting down system...  \n";
    std::cout << "=========================================\n";

    if (server_sock_fd >= 0) {
        close(server_sock_fd);
        unlink(SOCKET_PATH);
    }

    std::cout << "[FreeDot Init] Sending SIGTERM to processes...\n";
    kill(-1, SIGTERM);
    sleep(1);
    std::cout << "[FreeDot Init] Sending SIGKILL to remaining processes...\n";
    kill(-1, SIGKILL);
    sleep(1);

    std::cout << "[FreeDot Init] Syncing filesystem buffers...\n";
    sync();

    std::cout << "[FreeDot Init] Unmounting filesystems...\n";
    umount("/run");
    umount("/proc");
    umount("/sys");
    umount("/dev");

    if (cmd == RB_POWER_OFF) {
        std::cout << "[FreeDot Init] Powering off system.\n";
        reboot(RB_POWER_OFF);
    } else {
        std::cout << "[FreeDot Init] Rebooting system.\n";
        reboot(RB_AUTOBOOT);
    }

    while (true) pause();
}

int init_ipc_socket() {
    unlink(SOCKET_PATH);
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        perror("[FreeDot Init] Failed to create IPC socket");
        return -1;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[FreeDot Init] Failed to bind IPC socket");
        close(sock);
        return -1;
    }

    if (listen(sock, 5) < 0) {
        perror("[FreeDot Init] Failed to listen on IPC socket");
        close(sock);
        return -1;
    }

    chmod(SOCKET_PATH, 0666);
    return sock;
}

void handle_ipc_requests() {
    if (server_sock_fd < 0) return;

    int client_fd = accept(server_sock_fd, nullptr, nullptr);
    if (client_fd < 0) return;

    char buffer[512];
    std::memset(buffer, 0, sizeof(buffer));
    ssize_t bytes = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes <= 0) {
        close(client_fd);
        return;
    }

    std::string cmd(buffer);
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
        cmd.pop_back();
    }

    std::stringstream ss(cmd);
    std::string action;
    ss >> action;

    std::string response;

    if (action == "status") {
        response = "=== FreeDot Service Status ===\n";
        for (const auto& svc : services) {
            response += "  * " + svc.name + ": ";
            if (svc.pid > 0) {
                response += "RUNNING (PID " + std::to_string(svc.pid) + ")\n";
            } else {
                response += "STOPPED\n";
            }
        }
    } else if (action == "restart") {
        std::string target;
        ss >> target;
        bool found = false;
        for (auto& svc : services) {
            if (svc.name == target || svc.path.find(target) != std::string::npos) {
                found = true;
                if (svc.pid > 0) {
                    kill(svc.pid, SIGTERM);
                }
                response = "Restart signaled for service: " + svc.name + "\n";
                break;
            }
        }
        if (!found) {
            response = "Error: Service '" + target + "' not recognized.\n";
        }
    } else if (action == "reload") {
        load_services_from_disk();
        response = "Reloaded service definitions from " + std::string(CONFIG_DIR) + "\n";
    } else if (action == "poweroff") {
        response = "System poweroff initiated...\n";
        write(client_fd, response.c_str(), response.length());
        close(client_fd);
        poweroff_requested = 1;
        return;
    } else if (action == "reboot") {
        response = "System reboot initiated...\n";
        write(client_fd, response.c_str(), response.length());
        close(client_fd);
        reboot_requested = 1;
        return;
    } else {
        response = "Unknown command: " + action + "\nSupported: status, restart <name>, reload, poweroff, reboot\n";
    }

    write(client_fd, response.c_str(), response.length());
    close(client_fd);
}

int main() {
    pid_t pid = getpid();
    std::cout << "\n=========================================\n";
    std::cout << "  FreeDot Custom C++ Init System (PID 1)  \n";
    std::cout << "  Active PID: " << pid << "\n";
    std::cout << "=========================================\n\n";

    if (pid != 1) {
        std::cerr << "[FreeDot Init] WARNING: Not running as PID 1!\n";
    }

    std::cout << "[FreeDot Init] Mounting /proc, /sys, /dev, /run...\n";
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mkdir("/run", 0755);
    mkdir("/var", 0755);
    mkdir("/var/log", 0755);

    mount("none", "/proc", "proc", 0, "");
    mount("none", "/sys", "sysfs", 0, "");
    mount("none", "/dev", "devtmpfs", 0, "");
    mount("none", "/run", "tmpfs", 0, "mode=0755");

    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

    struct sigaction sa_pwr;
    sa_pwr.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa_pwr.sa_mask);
    sa_pwr.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_pwr, nullptr);
    sigaction(SIGPWR, &sa_pwr, nullptr);
    sigaction(SIGTERM, &sa_pwr, nullptr);

    server_sock_fd = init_ipc_socket();
    if (server_sock_fd >= 0) {
        std::cout << "[FreeDot Init] IPC socket listening at " << SOCKET_PATH << "\n";
    }

    std::cout << "[FreeDot Init] Parsing unit configurations...\n";
    load_services_from_disk();

    for (auto& svc : services) {
        spawn_service(svc);
    }

    while (true) {
        if (poweroff_requested) {
            perform_shutdown(RB_POWER_OFF);
        }
        if (reboot_requested) {
            perform_shutdown(RB_AUTOBOOT);
        }

        handle_ipc_requests();

        for (auto& svc : services) {
            if (svc.pid == -1 && svc.respawn) {
                std::cout << "\n[FreeDot Init] Service " << svc.name << " stopped. Respawning...\n";
                sleep(1);
                spawn_service(svc);
            }
        }

        usleep(100000);
    }

    return 0;
}
