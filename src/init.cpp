#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <signal.h>
#include <cstdlib>

enum class ServiceType {
    DAEMON,
    INTERACTIVE_SHELL
};

struct Service {
    std::string name;
    std::string path;
    std::vector<std::string> args;
    ServiceType type;
    pid_t pid;
    bool respawn;
};

static std::vector<Service> services;
static volatile sig_atomic_t poweroff_requested = 0;
static volatile sig_atomic_t reboot_requested = 0;

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
        // Child Process
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
        // Parent Process (PID 1)
        svc.pid = pid;
        std::cout << "[FreeDot Init] Started " << svc.name << " (PID: " << pid << ")\n";
    }
}

void perform_shutdown(int cmd) {
    std::cout << "\n=========================================\n";
    std::cout << "  FreeDot Init: Shutting down system...  \n";
    std::cout << "=========================================\n";

    // 1. Terminate running user processes
    std::cout << "[FreeDot Init] Sending SIGTERM to processes...\n";
    kill(-1, SIGTERM);
    sleep(1);
    std::cout << "[FreeDot Init] Sending SIGKILL to remaining processes...\n";
    kill(-1, SIGKILL);
    sleep(1);

    // 2. Sync all dirty buffers to storage
    std::cout << "[FreeDot Init] Syncing filesystem buffers...\n";
    sync();

    // 3. Unmount filesystems
    std::cout << "[FreeDot Init] Unmounting virtual filesystems...\n";
    umount("/proc");
    umount("/sys");
    umount("/dev");

    // 4. Issue the power management kernel syscall
    if (cmd == RB_POWER_OFF) {
        std::cout << "[FreeDot Init] Powering off system.\n";
        reboot(RB_POWER_OFF);
    } else {
        std::cout << "[FreeDot Init] Rebooting system.\n";
        reboot(RB_AUTOBOOT);
    }

    while (true) pause();
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

    // 1. Mount virtual kernel filesystems
    std::cout << "[FreeDot Init] Mounting /proc, /sys, /dev...\n";
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mkdir("/var", 0755);
    mkdir("/var/log", 0755);

    mount("none", "/proc", "proc", 0, "");
    mount("none", "/sys", "sysfs", 0, "");
    mount("none", "/dev", "devtmpfs", 0, "");

    // 2. Register signal handlers
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

    // 3. Define multi-service configuration
    services = {
        {
            "Metrics Daemon",
            "/bin/freedot-statsd",
            {"freedot-statsd"},
            ServiceType::DAEMON,
            -1,
            true
        },
        {
            "Interactive Shell",
            "/bin/sh",
            {"sh"},
            ServiceType::INTERACTIVE_SHELL,
            -1,
            true
        }
    };

    // 4. Launch all configured services
    for (auto& svc : services) {
        spawn_service(svc);
    }

    // 5. Main supervision loop
    while (true) {
        if (poweroff_requested) {
            perform_shutdown(RB_POWER_OFF);
        }
        if (reboot_requested) {
            perform_shutdown(RB_AUTOBOOT);
        }

        for (auto& svc : services) {
            if (svc.pid == -1 && svc.respawn) {
                std::cout << "\n[FreeDot Init] Service " << svc.name << " stopped. Respawning...\n";
                sleep(1);
                spawn_service(svc);
            }
        }

        sleep(1);
    }

    return 0;
}
