// FreeDot Custom C++ Init System (PID 1)
// Built from scratch to handle Linux userspace initialization without systemd bloat.
// No extra dependencies, just pure C++20 and native Linux syscalls.

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
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
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <cstdlib>

namespace fs = std::filesystem;

// Distinguish background services from interactive console sessions
enum class ServiceType {
    DAEMON,
    INTERACTIVE_SHELL
};

// Unit representation for running services
struct Service {
    std::string name;
    std::string path;
    std::vector<std::string> args;
    ServiceType type;
    pid_t pid = -1;
    bool respawn = true;
};

// Global init state
static std::vector<Service> services;
static volatile sig_atomic_t poweroff_requested = 0;
static volatile sig_atomic_t reboot_requested = 0;
static int server_sock_fd = -1;

// Standard Linux runtime paths
constexpr const char* SOCKET_PATH = "/run/freedot.sock";
constexpr const char* CONFIG_DIR = "/etc/freedot.d";

// In Linux, PID 1 must reap dead child processes or they turn into zombie processes.
// Whenever a child exits, SIGCHLD fires. We catch it and call waitpid with WNOHANG
// so we reap the zombie process without blocking the main event loop.
void handle_sigchld(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (auto& svc : services) {
            if (svc.pid == pid) {
                svc.pid = -1; // Mark service as stopped so the main loop can respawn it if needed
                break;
            }
        }
    }
}

// Trap shutdown signals from kernel or external power buttons
void handle_shutdown_signal(int sig) {
    if (sig == SIGINT || sig == SIGPWR) {
        poweroff_requested = 1;
    } else if (sig == SIGTERM) {
        reboot_requested = 1;
    }
}

// Low-level network configuration via Linux ioctl calls.
// Rather than relying on external tools like ifconfig or iproute2, we configure
// interface IP, netmask, and flags directly through kernel socket ioctls.
bool configure_interface(const std::string& ifname, const std::string& ip, const std::string& netmask) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror(("[FreeDot Network] Socket creation failed for " + ifname).c_str());
        return false;
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);

    // 1. Assign IP address to interface
    struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &addr->sin_addr);
    if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
        close(sock);
        return false;
    }

    // 2. Assign subnet mask to interface
    struct sockaddr_in* mask = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_netmask);
    mask->sin_family = AF_INET;
    inet_pton(AF_INET, netmask.c_str(), &mask->sin_addr);
    if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
        close(sock);
        return false;
    }

    // 3. Bring the interface up and mark it running
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        close(sock);
        return false;
    }
    ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        close(sock);
        return false;
    }

    close(sock);
    return true;
}

// Brings up loopback (lo) and default QEMU virtual ethernet (eth0)
void setup_networking() {
    std::cout << "[FreeDot Network] Initializing network interfaces...\n";

    if (configure_interface("lo", "127.0.0.1", "255.0.0.0")) {
        std::cout << "[FreeDot Network] Loopback (lo) configured: 127.0.0.1/8\n";
    } else {
        std::cerr << "[FreeDot Network] Failed to configure loopback (lo)\n";
    }

    if (configure_interface("eth0", "10.0.2.15", "255.255.255.0")) {
        std::cout << "[FreeDot Network] Ethernet (eth0) configured: 10.0.2.15/24\n";
    } else {
        std::cout << "[FreeDot Network] eth0 not present or deferred.\n";
    }
}

// Fork and execute a service.
// For interactive shells, we attach the process to the serial console (ttyS0 or console)
// using setsid, TIOCSCTTY, and dup2 so the user gets a working terminal.
void spawn_service(Service& svc) {
    pid_t pid = fork();

    if (pid < 0) {
        perror(("[FreeDot Init] Fork failed for " + svc.name).c_str());
        return;
    }

    if (pid == 0) {
        // Child process setup
        if (svc.type == ServiceType::INTERACTIVE_SHELL) {
            setsid(); // Create a new session so this process becomes session leader
            int fd = open("/dev/ttyS0", O_RDWR);
            if (fd < 0) {
                fd = open("/dev/console", O_RDWR);
            }
            if (fd >= 0) {
                ioctl(fd, TIOCSCTTY, 1); // Set controlling terminal
                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) close(fd);
            }
        }

        // Prepare arguments for execve
        std::vector<char*> c_args;
        for (const auto& arg : svc.args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        // Standard minimal Linux environment variables
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
        // Parent process records child PID
        svc.pid = pid;
        std::cout << "[FreeDot Init] Started " << svc.name << " (PID: " << pid << ")\n";
    }
}

// Reads service unit definition files from /etc/freedot.d/*.conf
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

// Clean system shutdown sequence:
// 1. Close IPC sockets
// 2. Politely terminate processes with SIGTERM
// 3. Force kill stragglers with SIGKILL
// 4. Flush all dirty cached disk buffers with sync()
// 5. Unmount all virtual filesystems cleanly
// 6. Invoke reboot() syscall to power down or reboot
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

// Set up a UNIX domain socket at /run/freedot.sock for IPC with freedotctl
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

// Process incoming commands from freedotctl (status, restart, reload, poweroff, reboot)
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

// Entry point for PID 1 (Init)
int main() {
    pid_t pid = getpid();
    std::cout << "\n=========================================\n";
    std::cout << "  FreeDot Custom C++ Init System (PID 1)  \n";
    std::cout << "  Active PID: " << pid << "\n";
    std::cout << "=========================================\n\n";

    if (pid != 1) {
        std::cerr << "[FreeDot Init] WARNING: Not running as PID 1!\n";
    }

    // Mount core virtual filesystems needed by userspace programs
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

    // Register signal handlers for zombie reaping (SIGCHLD) and shutdown signals
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

    // Initialize IPC socket for freedotctl control utility
    server_sock_fd = init_ipc_socket();
    if (server_sock_fd >= 0) {
        std::cout << "[FreeDot Init] IPC socket listening at " << SOCKET_PATH << "\n";
    }

    // Configure network interfaces
    setup_networking();

    // Parse service unit files and spawn initial services
    std::cout << "[FreeDot Init] Parsing unit configurations...\n";
    load_services_from_disk();

    for (auto& svc : services) {
        spawn_service(svc);
    }

    // Main Init loop: monitors shutdown requests, handles IPC commands, and respawns dead services
    while (true) {
        if (poweroff_requested) {
            perform_shutdown(RB_POWER_OFF);
        }
        if (reboot_requested) {
            perform_shutdown(RB_AUTOBOOT);
        }

        handle_ipc_requests();

        // Auto-respawn crashed services marked with respawn=true
        for (auto& svc : services) {
            if (svc.pid == -1 && svc.respawn) {
                std::cout << "\n[FreeDot Init] Service " << svc.name << " stopped. Respawning...\n";
                sleep(1);
                spawn_service(svc);
            }
        }

        usleep(100000); // 100ms tick to keep CPU usage near zero
    }

    return 0;
}