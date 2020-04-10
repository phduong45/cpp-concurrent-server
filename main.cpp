#include "net_utils.h"
#include <cerrno>
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

void handle_client(int client_fd) {
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

    if (!parsed_request || !body_complete) {
        std::string response =
            make_http_response("400 Bad Request", "Bad Request");
        write_all(client_fd, response.data(), response.size());
    } else if (parsed_request->method == "GET" &&
               parsed_request->path == "/health") {
        std::string response = make_http_response("200 OK", "OK");
        write_all(client_fd, response.data(), response.size());
    } else if (parsed_request->method == "POST" &&
               parsed_request->path == "/echo") {
        std::string response = make_http_response("200 OK", body);
        write_all(client_fd, response.data(), response.size());
    } else {
        std::string response = make_http_response("404 Not Found", "Not Found");
        write_all(client_fd, response.data(), response.size());
    }
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

    // accept() blocks until a client connects and returns a new connected fd.
    while (true) {
        int client_fd = accept(socket_fd, nullptr, nullptr);
        if (client_fd == -1) {
            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            continue;
        }
        std::cout << "Client connected: " << client_fd << "\n";

        std::thread client_thread([client_fd]() {
            handle_client(client_fd);

            if (close(client_fd) == -1) {
                std::cerr << "close client socket failed: "
                          << std::strerror(errno) << "\n";
            }
        });

        client_thread.detach();
    }

    // Close the connected socket before the longer-lived listening
    if (close(socket_fd) == -1) {
        std::cerr << "close listening socket failed: " << std::strerror(errno)
                  << '\n';
        return 1;
    }

    return 0;
}
