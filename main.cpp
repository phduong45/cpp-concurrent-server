#include "net_utils.h"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    // Create an IPv4 TCP endpoint managed by the kernel.
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) ==
        -1) {
        std::cerr << "setsockopt SO_REUSEADDR failed: " << std::strerror(errno)
                  << '\n';
        close(socket_fd);
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

    // Mark the bound socket as a listening socket for incoming connections.
    if (listen(socket_fd, 16) == -1) {
        std::cerr << "listen failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }
    std::cout << "Listening on 127.0.0.1:8080\n";

    // accept() blocks until a client connects and returns a new connected fd.
    int client_fd = accept(socket_fd, nullptr, nullptr);
    if (client_fd == -1) {
        std::cerr << "accept failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }
    std::cout << "Client connected\n";

    std::string request;
    char buffer[1024];

    while (request.find("\r\n\r\n") == std::string::npos) {
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
        if (bytes_read == -1) {
            break;
        }

        if (bytes_read == 0) {
            break;
        }

        request.append(buffer, bytes_read);
    }

    std::cout << "received: " << request << "\n";
    auto line_end = request.find("\r\n");
    std::string request_line = request.substr(0, line_end);
    std::istringstream iss(request_line);
    std::string method, path, version;

    if (iss >> method >> path >> version) {
        std::string_view response;
        if (method == "GET" && path == "/health") {
            response = "HTTP/1.1 200 OK\r\n"
                       "Content-Length: 2\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "OK";
        } else {
            response = "HTTP/1.1 404 Not Found\r\n"
                       "Content-Length: 9\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "Not Found";
        }
        if (!write_all(client_fd, response.data(), response.size())) {
            std::cout << "write failed\n";
        }
    }

    // Close the connected socket before the longer-lived listening
    // socket.
    if (close(client_fd) == -1) {
        std::cerr << "close client socket failed: " << std::strerror(errno)
                  << '\n';
        close(socket_fd);
        return 1;
    }

    if (close(socket_fd) == -1) {
        std::cerr << "close listening socket failed: " << std::strerror(errno)
                  << '\n';
        return 1;
    }

    return 0;
}
