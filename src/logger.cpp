#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <unistd.h>

int main() {
    std::cout << "[logger] FreeDot system logger daemon initialized.\n";
    std::ofstream log_file("/var/log/syslog.log", std::ios::app);
    if (log_file.is_open()) {
        log_file << "[FreeDot Logger] Boot logging active.\n";
        log_file.close();
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    return 0;
}
