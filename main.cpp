#include "blocking_queue.h"
#include "net_utils.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

volatile sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

struct ServerMetrics {
    std::atomic<std::size_t> total_requests{0};
    std::atomic<std::size_t> active_connections{0};
    std::atomic<std::size_t> status_200{0};
    std::atomic<std::size_t> status_400{0};
    std::atomic<std::size_t> status_404{0};
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
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
    body += "status_400 " + std::to_string(metrics.status_400.load()) + "\n";
    body += "status_404 " + std::to_string(metrics.status_404.load()) + "\n";
    return body;
}

void handle_client(int client_fd, ServerMetrics& metrics) {
    metrics.active_connections.fetch_add(1);

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

    auto header_end = request.find("\r\n\r\n");
    std::string body;
    bool body_complete = header_end != std::string::npos;

    if (body_complete) {
        std::string_view headers{request.data(), header_end + 4};
        body = request.substr(header_end + 4);

        if (auto content_length = get_content_length(headers)) {
            while (body.size() < *content_length) {
                ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
                if (bytes_read == -1 || bytes_read == 0) {
                    body_complete = false;
                    break;
                }

                body.append(buffer, bytes_read);
            }

            if (body.size() > *content_length) {
                body.resize(*content_length);
            }
        }
    }

    std::cout << "received: " << request << "\n";
    if (!body.empty()) {
        std::cout << "body: " << body << "\n";
    }

    auto parsed_request = parse_request_line(request);
    metrics.total_requests.fetch_add(1);

    if (!parsed_request || !body_complete) {
        metrics.status_400.fetch_add(1);
        std::string response =
            make_http_response("400 Bad Request", "Bad Request");
        write_all(client_fd, response.data(), response.size());
    } else if (parsed_request->method == "GET" &&
               parsed_request->path == "/health") {
        metrics.status_200.fetch_add(1);
        std::string response = make_http_response("200 OK", "OK");
        write_all(client_fd, response.data(), response.size());
    } else if (parsed_request->method == "GET" &&
               parsed_request->path == "/metrics") {
        metrics.status_200.fetch_add(1);
        std::string response =
            make_http_response("200 OK", make_metrics_body(metrics));
        write_all(client_fd, response.data(), response.size());
    } else if (parsed_request->method == "GET" &&
               parsed_request->path == "/slow") {
        metrics.status_200.fetch_add(1);

        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::string response = make_http_response("200 OK", "slow OK");
        write_all(client_fd, response.data(), response.size());
    } else if (parsed_request->method == "POST" &&
               parsed_request->path == "/echo") {
        metrics.status_200.fetch_add(1);
        std::string response = make_http_response("200 OK", body);
        write_all(client_fd, response.data(), response.size());
    } else {
        metrics.status_404.fetch_add(1);
        std::string response = make_http_response("404 Not Found", "Not Found");
        write_all(client_fd, response.data(), response.size());
    }

    metrics.active_connections.fetch_sub(1);
}

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

    // accept() blocks until a client connects and returns a new connected
    // fd.
    BlockingQueue<int> connections;
    std::vector<std::thread> workers;
    constexpr int workers_count = 4;
    ServerMetrics metrics;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    for (int i = 0; i < workers_count; ++i) {
        workers.emplace_back([&connections, &metrics] {
            while (auto client_fd = connections.wait_and_pop()) {
                handle_client(*client_fd, metrics);

                if (close(*client_fd) == -1) {
                    std::cerr << "close client socket failed: "
                              << std::strerror(errno) << "\n";
                }
            }
        });
    }

    while (!stop_requested) {
        int client_fd = accept(socket_fd, nullptr, nullptr);
        if (client_fd == -1) {
            if (errno == EINTR && stop_requested) {
                break;
            }

            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            continue;
        }
        std::cout << "Client queued: " << client_fd << "\n";
        if (!connections.push(client_fd)) {
            close(client_fd);
        }
    }

    connections.close();

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Close the connected socket before the longer-lived listening
    if (close(socket_fd) == -1) {
        std::cerr << "close listening socket failed: " << std::strerror(errno)
                  << '\n';
        return 1;
    }

    return 0;
}
