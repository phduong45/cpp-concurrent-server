#include "net_utils.h"

#include <atomic>
#include <cerrno>
#include <chrono>
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

struct ServerMetrics {
    std::atomic<std::size_t> total_requests{0};
    std::atomic<std::size_t> active_connections{0};
    std::atomic<std::size_t> status_200{0};
    std::atomic<std::size_t> status_404{0};
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
};

struct Connection {
    int fd;
    std::string request;
    std::string response;
    std::size_t bytes_sent = 0;
    std::optional<std::chrono::steady_clock::time_point> ready_at;
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

std::optional<std::size_t> get_content_length(std::string_view headers) {
    constexpr std::string_view key = "Content-Length:";

    auto pos = headers.find(key);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }

    auto value_start = pos + key.size();
    auto line_end = headers.find("\r\n", value_start);
    if (line_end == std::string_view::npos) {
        return std::nullopt;
    }

    std::string value{headers.substr(value_start, line_end - value_start)};

    try {
        return static_cast<std::size_t>(std::stoul(value));
    } catch (...) {
        return std::nullopt;
    }
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

std::string make_metrics_body(const ServerMetrics& metrics) {
    std::string body;
    body += "total_requests " + std::to_string(metrics.total_requests.load()) +
            "\n";
    body += "active_connections " +
            std::to_string(metrics.active_connections.load()) + "\n";
    body += "status_200 " + std::to_string(metrics.status_200.load()) + "\n";
    body += "status_404 " + std::to_string(metrics.status_404.load()) + "\n";
    return body;
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
                          std::unordered_map<int, Connection>& connections,
                          ServerMetrics& metrics) {
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
        Connection connection{};
        connection.fd = client_fd;
        connections.emplace(client_fd, std::move(connection));
        metrics.active_connections.fetch_add(1);
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

bool request_complete(std::string_view request) {
    auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return false;
    }

    std::string_view headers{request.data(), header_end + 4};
    auto content_length = get_content_length(headers);
    if (!content_length) {
        return true;
    }

    std::size_t body_start = header_end + 4;
    std::size_t body_size = request.size() - body_start;
    return body_size >= *content_length;
}

std::string_view request_body(std::string_view request) {
    auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return {};
    }

    std::size_t body_start = header_end + 4;
    return request.substr(body_start);
}

bool has_pending_response(const Connection& connection) {
    return connection.bytes_sent < connection.response.size();
}

bool waiting_for_timer(const Connection& connection) {
    return connection.ready_at.has_value() && connection.response.empty();
}

bool write_pending_data(Connection& connection) {
    while (connection.bytes_sent < connection.response.size()) {
        const char* data = connection.response.data() + connection.bytes_sent;
        std::size_t remaining =
            connection.response.size() - connection.bytes_sent;

        ssize_t bytes_written = write(connection.fd, data, remaining);

        if (bytes_written > 0) {
            connection.bytes_sent += bytes_written;
            continue;
        }

        if (bytes_written == 0) {
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

    return true;
}

std::string route_request(Connection& connection, ServerMetrics& metrics) {
    metrics.total_requests.fetch_add(1);

    auto parsed = parse_request_line(connection.request);
    if (parsed && parsed->method == "GET" && parsed->path == "/health") {
        metrics.status_200.fetch_add(1);
        return make_http_response("200 OK", "OK");
    }

    if (parsed && parsed->method == "GET" && parsed->path == "/metrics") {
        metrics.status_200.fetch_add(1);
        return make_http_response("200 OK", make_metrics_body(metrics));
    }

    if (parsed && parsed->method == "GET" && parsed->path == "/slow") {
        metrics.status_200.fetch_add(1);
        connection.ready_at =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        return {};
    }

    if (parsed && parsed->method == "POST" && parsed->path == "/echo") {
        metrics.status_200.fetch_add(1);
        return make_http_response("200 OK", request_body(connection.request));
    }

    metrics.status_404.fetch_add(1);
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
    ServerMetrics metrics;

    while (!stop_requested) {
        auto now = std::chrono::steady_clock::now();
        for (auto& [fd, connection] : connections) {
            if (connection.ready_at && now >= *connection.ready_at) {
                connection.response = make_http_response("200 OK", "slow OK");
                connection.bytes_sent = 0;
                connection.ready_at.reset();
            }
        }

        std::vector<pollfd> poll_fds;

        pollfd listening{};
        listening.fd = socket_fd;
        listening.events = POLLIN;
        poll_fds.push_back(listening);

        for (const auto& [fd, connection] : connections) {
            pollfd client{};
            client.fd = fd;
            if (has_pending_response(connection)) {
                client.events = POLLOUT;
            } else if (waiting_for_timer(connection)) {
                client.events = 0;
            } else {
                client.events = POLLIN;
            }
            poll_fds.push_back(client);
        }

        int ready = poll(poll_fds.data(), poll_fds.size(), 100);
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
            accept_ready_clients(socket_fd, connections, metrics);
        }

        std::vector<int> to_close;

        for (std::size_t i = 1; i < poll_fds.size(); ++i) {
            int fd = poll_fds[i].fd;

            if (poll_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                to_close.push_back(fd);
                continue;
            }

            auto it = connections.find(fd);
            if (it == connections.end()) {
                continue;
            }

            Connection& connection = it->second;

            if (poll_fds[i].revents & POLLIN) {
                if (!read_available_data(connection)) {
                    to_close.push_back(fd);
                    continue;
                }

                if (request_complete(connection.request)) {
                    connection.response = route_request(connection, metrics);
                    if (!connection.response.empty()) {
                        connection.bytes_sent = 0;
                    }
                }
            }

            if (poll_fds[i].revents & POLLOUT) {
                if (!write_pending_data(connection)) {
                    to_close.push_back(fd);
                    continue;
                }

                if (!has_pending_response(connection)) {
                    to_close.push_back(fd);
                }
            }
        }

        for (int fd : to_close) {
            if (connections.erase(fd) > 0) {
                metrics.active_connections.fetch_sub(1);
            }
            close(fd);
        }
    }

    for (auto& [fd, connection] : connections) {
        close(fd);
    }

    close(socket_fd);
    return 0;
}
