#include <iostream>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstdlib>
// Signal handler for child processes (zombie cleanup)
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

    if (mount("none", "/proc", "proc", 0, "") != 0) {
        perror("[FreeDot Init] Failed to mount /proc");
    }
    if (mount("none", "/sys", "sysfs", 0, "") != 0) {
        perror("[FreeDot Init] Failed to mount /sys");
    }
    if (mount("none", "/dev", "devtmpfs", 0, "") != 0) {
        perror("[FreeDot Init] Failed to mount /dev");
    }

    // 3. Register signal handler to prevent zombie processes
    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, nullptr) != 0) {
        perror("[FreeDot Init] Failed to set SIGCHLD handler");
    }

    std::cout << "[FreeDot Init] Core mounts and signals configured.\n";
    std::cout << "[FreeDot Init] Launching interactive shell (/bin/sh)...\n\n";

    // 4. Fork and execute the shell as a child process
    while (true) {
        pid_t child = fork();

        if (child < 0) {
            perror("[FreeDot Init] Fork failed");
            sleep(2);
            continue;
        }

        if (child == 0) {
            // In Child Process: replace process image with BusyBox shell
            char* const args[] = {(char*)"/bin/sh", nullptr};
            char* const env[] = {(char*)"PATH=/bin:/sbin:/usr/bin:/usr/sbin", (char*)"TERM=vt100", nullptr};
            execve("/bin/sh", args, env);

            // If execve returns, it encountered an error
            perror("[FreeDot Init] execve failed");
            exit(1);
        } else {
            // In Parent (PID 1): wait for the shell to exit, then respawn it
            int status;
            waitpid(child, &status, 0);
            std::cout << "\n[FreeDot Init] Shell exited. Respawning in 1 second...\n";
            sleep(1);
        }
    }

    return 0;
}