#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <map>
#include <queue>
#include <set>
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
    std::vector<std::string> after;
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

bool configure_interface(const std::string& ifname, const std::string& ip, const std::string& netmask) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);

    struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &addr->sin_addr);
    if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
        close(sock);
        return false;
    }

    struct sockaddr_in* mask = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_netmask);
    mask->sin_family = AF_INET;
    inet_pton(AF_INET, netmask.c_str(), &mask->sin_addr);
    if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
        close(sock);
        return false;
    }

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

void setup_networking() {
    std::cout << "[FreeDot Network] Initializing network interfaces...\n";
    if (configure_interface("lo", "127.0.0.1", "255.0.0.0")) {
        std::cout << "[FreeDot Network] Loopback (lo) configured: 127.0.0.1/8\n";
    }
    if (configure_interface("eth0", "10.0.2.15", "255.255.255.0")) {
        std::cout << "[FreeDot Network] Ethernet (eth0) configured: 10.0.2.15/24\n";
    } else {
        std::cout << "[FreeDot Network] eth0 interface deferred.\n";
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
            if (fd < 0) fd = open("/dev/console", O_RDWR);
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

std::vector<Service> resolve_dependencies(const std::vector<Service>& raw_services) {
    std::map<std::string, Service> svc_map;
    std::map<std::string, std::vector<std::string>> adj;
    std::map<std::string, int> in_degree;

    for (const auto& svc : raw_services) {
        svc_map[svc.name] = svc;
        in_degree[svc.name] = 0;
    }

    for (const auto& svc : raw_services) {
        for (const auto& dep : svc.after) {
            if (svc_map.find(dep) != svc_map.end()) {
                // Dependency 'dep' must run before 'svc.name' (dep -> svc.name)
                adj[dep].push_back(svc.name);
                in_degree[svc.name]++;
            }
        }
    }

    std::queue<std::string> q;
    for (const auto& [name, deg] : in_degree) {
        if (deg == 0) q.push(name);
    }

    std::vector<Service> ordered;
    while (!q.empty()) {
        std::string u = q.front();
        q.pop();
        ordered.push_back(svc_map[u]);

        for (const auto& v : adj[u]) {
            if (--in_degree[v] == 0) {
                q.push(v);
            }
        }
    }

    // Check for circular dependencies
    if (ordered.size() != raw_services.size()) {
        std::cerr << "[FreeDot Init] WARNING: Circular dependency detected in unit files! Falling back.\n";
        return raw_services;
    }

    return ordered;
}

void load_services_from_disk() {
    if (!fs::exists(CONFIG_DIR)) {
        std::cerr << "[FreeDot Init] Config directory " << CONFIG_DIR << " not found.\n";
        return;
    }

    std::vector<Service> loaded_services;
    for (const auto& entry : fs::directory_iterator(CONFIG_DIR)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".conf") continue;

        std::ifstream file(entry.path());
        if (!file.is_open()) continue;

        Service svc;
        svc.type = ServiceType::DAEMON;
        svc.respawn = true;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            auto delim = line.find('=');
            if (delim == std::string::npos) continue;

            std::string key = line.substr(0, delim);
            std::string val = line.substr(delim + 1);

            if (key == "name") {
                svc.name = val;
            } else if (key == "exec") {
                svc.path = val;
                svc.args = {val.substr(val.find_last_of('/') + 1)};
            } else if (key == "type") {
                svc.type = (val == "interactive") ? ServiceType::INTERACTIVE_SHELL : ServiceType::DAEMON;
            } else if (key == "respawn") {
                svc.respawn = (val == "true" || val == "1");
            } else if (key == "after") {
                std::stringstream ss(val);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    if (!token.empty()) svc.after.push_back(token);
                }
            }
        }

        if (!svc.name.empty() && !svc.path.empty()) {
            loaded_services.push_back(svc);
        }
    }

    // Resolve dependencies via topological sort
    auto ordered = resolve_dependencies(loaded_services);

    for (const auto& svc : ordered) {
        bool exists = false;
        for (const auto& existing : services) {
            if (existing.name == svc.name) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            services.push_back(svc);
            std::string dep_str = svc.after.empty() ? "none" : "";
            for (size_t i = 0; i < svc.after.size(); ++i) {
                dep_str += svc.after[i] + (i + 1 < svc.after.size() ? ", " : "");
            }
            std::cout << "[FreeDot Init] Registered unit: " << svc.name << " (after: " << dep_str << ")\n";
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
    if (sock < 0) return -1;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    if (listen(sock, 5) < 0) {
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
    } else if (action == "deps") {
        response = "=== FreeDot Startup Dependency Graph ===\n";
        for (size_t i = 0; i < services.size(); ++i) {
            const auto& svc = services[i];
            response += std::to_string(i + 1) + ". " + svc.name;
            if (!svc.after.empty()) {
                response += " (after: ";
                for (size_t j = 0; j < svc.after.size(); ++j) {
                    response += svc.after[j] + (j + 1 < svc.after.size() ? ", " : "");
                }
                response += ")";
            }
            response += "\n";
        }
    } else if (action == "restart") {
        std::string target;
        ss >> target;
        bool found = false;
        for (auto& svc : services) {
            if (svc.name == target || svc.path.find(target) != std::string::npos) {
                found = true;
                if (svc.pid > 0) kill(svc.pid, SIGTERM);
                response = "Restart signaled for service: " + svc.name + "\n";
                break;
            }
        }
        if (!found) response = "Error: Service '" + target + "' not recognized.\n";
    } else if (action == "reload") {
        load_services_from_disk();
        response = "Reloaded service definitions.\n";
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
        response = "Unknown command: " + action + "\nSupported: status, deps, restart <name>, reload, poweroff, reboot\n";
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

    setup_networking();

    std::cout << "[FreeDot Init] Resolving service dependency graph...\n";
    load_services_from_disk();

    // Spawn services in resolved dependency order
    for (auto& svc : services) {
        spawn_service(svc);
    }

    while (true) {
        if (poweroff_requested) perform_shutdown(RB_POWER_OFF);
        if (reboot_requested) perform_shutdown(RB_AUTOBOOT);

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
