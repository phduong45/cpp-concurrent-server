#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

bool write_all(int fd, const char* data, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
        ssize_t bytes_written = write(fd, data + total, size - total);

        if (bytes_written == -1) {
            if (errno != EINTR) {
                return false;
            } else {
                continue;
            }
        }
        if (bytes_written == 0) {
            return false;
        }
        total += bytes_written;
    }
    return true;
}

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

    const char message[] = "hello from client\n";

    if (!write_all(socket_fd, message, std::strlen(message))) {
        std::cerr << "write failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }

    if (shutdown(socket_fd, SHUT_WR) == -1) {
        std::cerr << "shutdown failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }

    char buffer[1024];
    while (true) {
        ssize_t bytes_read = read(socket_fd, buffer, sizeof(buffer));

        if (bytes_read > 0) {
            std::cout.write(buffer, bytes_read);
            continue;
        }

        if (bytes_read == 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        std::cerr << "read failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }

    close(socket_fd);

    return 0;
}