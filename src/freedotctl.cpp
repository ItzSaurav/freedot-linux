// freedotctl — Lightweight CLI client for FreeDot Init
// Communicates with PID 1 via the /run/freedot.sock UNIX domain socket.

#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

constexpr const char* SOCKET_PATH = "/run/freedot.sock";

int main(int argc, char* argv[]) {
    // Basic argument check
    if (argc < 2) {
        std::cerr << "Usage: freedotctl <status | restart <name> | poweroff | reboot>\n";
        return 1;
    }

    // Assemble the command arguments into a single string payload
    std::string command = argv[1];
    for (int i = 2; i < argc; ++i) {
        command += " ";
        command += argv[i];
    }

    // Create standard UNIX domain stream socket
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[freedotctl] Failed to create socket");
        return 1;
    }

    // Target the IPC socket created by PID 1
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Connect to PID 1 init process
    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[freedotctl] Cannot connect to FreeDot init socket (/run/freedot.sock)");
        close(sock);
        return 1;
    }

    // Send command to init
    command += "\n";
    if (write(sock, command.c_str(), command.length()) < 0) {
        perror("[freedotctl] Failed to send command");
        close(sock);
        return 1;
    }

    // Read and print back the response from init
    char buffer[1024];
    ssize_t bytes_read;
    while ((bytes_read = read(sock, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        std::cout << buffer;
    }

    close(sock);
    return 0;
}