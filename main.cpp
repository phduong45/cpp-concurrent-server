#include "net_utils.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

volatile sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
};

struct Connection {
    int fd;
    std::string request;
};

std::optional<HttpRequest> parse_request_line(std::string_view request) {
    auto line_end = request.find("\r\n");
    if (line_end == std::string_view::npos) {
        return std::nullopt;
    }

    std::string request_line(request.substr(0, line_end));
    std::istringstream iss(request_line);
    HttpRequest parsed;
    if (!(iss >> parsed.method >> parsed.path >> parsed.version)) {
        return std::nullopt;
    }

    return parsed;
}

std::string make_http_response(std::string_view status, std::string_view body) {
    std::string response;
    response += "HTTP/1.1 ";
    response += status;
    response += "\r\n";
    response += "Content-Length: ";
    response += std::to_string(body.size());
    response += "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;
    return response;
}

bool bind_and_listen(int socket_fd, int port) {
    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) ==
        -1) {
        std::cerr << "setsockopt SO_REUSEADDR failed: " << std::strerror(errno)
                  << "\n";
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) == -1) {
        std::cerr << "bind failed: " << std::strerror(errno) << "\n";
        return false;
    }

    if (listen(socket_fd, 16) == -1) {
        std::cerr << "listen failed: " << std::strerror(errno) << "\n";
        return false;
    }

    return true;
}

void accept_ready_clients(int socket_fd,
                          std::unordered_map<int, Connection>& connections) {
    while (true) {
        int client_fd = accept(socket_fd, nullptr, nullptr);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            break;
        }

        if (!set_nonblocking(client_fd)) {
            std::cerr << "set client nonblocking failed: "
                      << std::strerror(errno) << "\n";
            close(client_fd);
            continue;
        }

        std::cout << "Client accepted: " << client_fd << "\n";
        connections.emplace(client_fd, Connection{client_fd, ""});
    }
}

bool read_available_data(Connection& connection) {
    char buffer[1024];

    while (true) {
        ssize_t bytes_read = read(connection.fd, buffer, sizeof(buffer));

        if (bytes_read > 0) {
            connection.request.append(buffer, bytes_read);
            continue;
        }

        if (bytes_read == 0) {
            return false;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        return false;
    }
}

bool request_header_complete(const Connection& connection) {
    return connection.request.find("\r\n\r\n") != std::string::npos;
}

std::string route_request(std::string_view request) {
    auto parsed = parse_request_line(request);
    if (parsed && parsed->method == "GET" && parsed->path == "/health") {
        return make_http_response("200 OK", "OK");
    }

    return make_http_response("404 Not Found", "Not Found");
}

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        std::cerr << "socket failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    if (!bind_and_listen(socket_fd, 8080)) {
        close(socket_fd);
        return 1;
    }

    if (!set_nonblocking(socket_fd)) {
        std::cerr << "set listening socket nonblocking failed: "
                  << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }

    std::cout << "Event-loop server listening on 127.0.0.1:8080\n";
    std::unordered_map<int, Connection> connections;

    while (!stop_requested) {
        std::vector<pollfd> poll_fds;

        pollfd listening{};
        listening.fd = socket_fd;
        listening.events = POLLIN;
        poll_fds.push_back(listening);

        for (const auto& [fd, connection] : connections) {
            pollfd client{};
            client.fd = fd;
            client.events = POLLIN;
            poll_fds.push_back(client);
        }

        int ready = poll(poll_fds.data(), poll_fds.size(), 1000);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "poll failed: " << std::strerror(errno) << "\n";
            continue;
        }

        if (ready == 0) {
            continue;
        }

        if (poll_fds[0].revents & POLLIN) {
            accept_ready_clients(socket_fd, connections);
        }

        std::vector<int> to_close;

        for (std::size_t i = 1; i < poll_fds.size(); ++i) {
            int fd = poll_fds[i].fd;

            if (poll_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                to_close.push_back(fd);
                continue;
            }

            if (!(poll_fds[i].revents & POLLIN)) {
                continue;
            }

            auto it = connections.find(fd);
            if (it == connections.end()) {
                continue;
            }

            Connection& connection = it->second;
            if (!read_available_data(connection)) {
                to_close.push_back(fd);
                continue;
            }

            if (request_header_complete(connection)) {
                std::string response = route_request(connection.request);
                write_all(fd, response.data(), response.size());
                to_close.push_back(fd);
            }
        }

        for (int fd : to_close) {
            close(fd);
            connections.erase(fd);
        }
    }

    for (auto& [fd, connection] : connections) {
        close(fd);
    }

    close(socket_fd);
    return 0;
}
