#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstdlib>

void handle_sigchld(int sig) {
    (void)sig;
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
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

    std::cout << "[FreeDot Init] Mounting /proc, /sys, /dev...\n";
    
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);

    mount("none", "/proc", "proc", 0, "");
    mount("none", "/sys", "sysfs", 0, "");
    mount("none", "/dev", "devtmpfs", 0, "");

    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

    std::cout << "[FreeDot Init] Core mounts and signals configured.\n";
    std::cout << "[FreeDot Init] Launching interactive shell session...\n\n";

    while (true) {
        pid_t child = fork();

        if (child < 0) {
            perror("[FreeDot Init] Fork failed");
            sleep(2);
            continue;
        }

        if (child == 0) {
            // 1. Create a new process group and session
            setsid();

            // 2. Open serial console and attach as controlling TTY
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

            // 3. Execute BusyBox shell with full job control
            char* const args[] = {(char*)"sh", nullptr};
            char* const env[] = {
                (char*)"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
                (char*)"TERM=vt100",
                (char*)"HOME=/root",
                (char*)"USER=root",
                nullptr
            };

            execve("/bin/sh", args, env);
            perror("[FreeDot Init] execve failed");
            exit(1);
        } else {
            int status;
            waitpid(child, &status, 0);
            std::cout << "\n[FreeDot Init] Shell exited. Respawning in 1 second...\n";
            sleep(1);
        }
    }

    return 0;
}