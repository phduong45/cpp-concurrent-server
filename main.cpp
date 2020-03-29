#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    // Create an IPv4 TCP endpoint managed by the kernel.
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    // Restrict the first project stage to clients running on this machine.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8080);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int result = bind(socket_fd, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address));

    if (result == -1) {
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return 1;
    }
    std::cout << "Bound TCP socket to 127.0.0.1:8080\n";

    if (close(socket_fd) == -1) {
        std::cerr << "close failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    return 0;
}
