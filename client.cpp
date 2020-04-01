#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        std::cerr << "socket failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(8080);
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int result =
        connect(socket_fd, reinterpret_cast<const sockaddr*>(&server_address),
                sizeof(server_address));
    if (result == -1) {
        std::cerr << "connect failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }

    close(socket_fd);

    return 0;
}