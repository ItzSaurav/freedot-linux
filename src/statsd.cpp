// statsd — Minimal system metrics daemon for FreeDot Linux
// Polls kernel metrics via the sysinfo() syscall every 5 seconds and logs them to /var/log/stats.log.

#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/sysinfo.h>

int main() {
    std::cout << "[statsd] FreeDot system metrics daemon started.\n";

    while (true) {
        struct sysinfo info;
        // Query the Linux kernel for uptime, memory usage, and process count
        if (sysinfo(&info) == 0) {
            std::ofstream log_file("/var/log/stats.log", std::ios::app);
            if (log_file.is_open()) {
                long total_ram = info.totalram * info.mem_unit / (1024 * 1024);
                long free_ram = info.freeram * info.mem_unit / (1024 * 1024);
                log_file << "[Uptime: " << info.uptime << "s] "
                         << "RAM Free: " << free_ram << "MB / " << total_ram << "MB | "
                         << "Processes: " << info.procs << "\n";
                log_file.close();
            }
        }
        sleep(5); // Run every 5 seconds
    }
    return 0;
}