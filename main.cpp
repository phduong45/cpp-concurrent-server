#include "http.h"
#include "net_utils.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <queue>
#include <string>
#include <sys/epoll.h>
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

struct Connection {
    int fd;
    std::string request;
    std::string response;
    std::size_t bytes_sent = 0;
    bool waiting_for_timer = false;
    std::uint64_t timer_id = 0;
};

struct Timer {
    std::chrono::steady_clock::time_point ready_at;
    int fd;
    std::uint64_t id;
};

struct TimerLater {
    bool operator()(const Timer& left, const Timer& right) const {
        return left.ready_at > right.ready_at;
    }
};

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

bool update_epoll_interest(int epoll_fd, const Connection& connection) {
    epoll_event event{};
    event.data.fd = connection.fd;
    event.events = EPOLLERR | EPOLLHUP;

    if (connection.bytes_sent < connection.response.size()) {
        event.events |= EPOLLOUT;
    } else if (!connection.waiting_for_timer) {
        event.events |= EPOLLIN;
    }

    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, connection.fd, &event) != -1;
}

void close_connection(int fd, int epoll_fd,
                      std::unordered_map<int, Connection>& connections,
                      ServerMetrics& metrics) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    if (connections.erase(fd) > 0) {
        metrics.active_connections.fetch_sub(1);
    }
    close(fd);
}

void accept_ready_clients(int socket_fd, int epoll_fd,
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

        Connection connection{};
        connection.fd = client_fd;
        connections.emplace(client_fd, std::move(connection));
        metrics.active_connections.fetch_add(1);

        epoll_event event{};
        event.data.fd = client_fd;
        event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
            std::cerr << "epoll add client failed: " << std::strerror(errno)
                      << "\n";
            close_connection(client_fd, epoll_fd, connections, metrics);
            continue;
        }

        std::cout << "Client accepted: " << client_fd << "\n";
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

using TimerQueue = std::priority_queue<Timer, std::vector<Timer>, TimerLater>;

std::string route_request(Connection& connection, ServerMetrics& metrics,
                          TimerQueue& timers, std::uint64_t& next_timer_id) {
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
        connection.waiting_for_timer = true;
        connection.timer_id = next_timer_id++;
        timers.push(Timer{std::chrono::steady_clock::now() +
                              std::chrono::seconds(2),
                          connection.fd, connection.timer_id});
        return {};
    }

    if (parsed && parsed->method == "POST" && parsed->path == "/echo") {
        metrics.status_200.fetch_add(1);
        return make_http_response("200 OK", request_body(connection.request));
    }

    metrics.status_404.fetch_add(1);
    return make_http_response("404 Not Found", "Not Found");
}

void process_expired_timers(int epoll_fd,
                            std::unordered_map<int, Connection>& connections,
                            TimerQueue& timers) {
    auto now = std::chrono::steady_clock::now();

    while (!timers.empty() && timers.top().ready_at <= now) {
        Timer timer = timers.top();
        timers.pop();

        auto it = connections.find(timer.fd);
        if (it == connections.end()) {
            continue;
        }

        Connection& connection = it->second;
        if (!connection.waiting_for_timer || connection.timer_id != timer.id) {
            continue;
        }

        connection.response = make_http_response("200 OK", "slow OK");
        connection.bytes_sent = 0;
        connection.waiting_for_timer = false;

        if (!update_epoll_interest(epoll_fd, connection)) {
            std::cerr << "epoll update failed: " << std::strerror(errno)
                      << "\n";
        }
    }
}

int next_epoll_timeout(const TimerQueue& timers) {
    if (timers.empty()) {
        return -1;
    }

    auto now = std::chrono::steady_clock::now();
    if (timers.top().ready_at <= now) {
        return 0;
    }

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        timers.top().ready_at - now);
    return static_cast<int>(remaining.count());
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

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }

    epoll_event listen_event{};
    listen_event.data.fd = socket_fd;
    listen_event.events = EPOLLIN;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &listen_event) == -1) {
        std::cerr << "epoll add listen socket failed: " << std::strerror(errno)
                  << "\n";
        close(epoll_fd);
        close(socket_fd);
        return 1;
    }

    std::cout << "server listening on 127.0.0.1:8080 (epoll)\n";

    std::unordered_map<int, Connection> connections;
    ServerMetrics metrics;
    std::vector<epoll_event> events(64);
    TimerQueue timers;
    std::uint64_t next_timer_id = 1;

    while (!stop_requested) {
        process_expired_timers(epoll_fd, connections, timers);

        int ready = epoll_wait(epoll_fd, events.data(), events.size(),
                               next_epoll_timeout(timers));
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "epoll_wait failed: " << std::strerror(errno) << "\n";
            continue;
        }

        std::vector<int> to_close;

        for (int i = 0; i < ready; ++i) {
            int fd = events[i].data.fd;
            uint32_t event_flags = events[i].events;

            if (fd == socket_fd) {
                if (event_flags & EPOLLIN) {
                    accept_ready_clients(socket_fd, epoll_fd, connections,
                                         metrics);
                }
                continue;
            }

            auto it = connections.find(fd);
            if (it == connections.end()) {
                continue;
            }

            Connection& connection = it->second;

            if (event_flags & (EPOLLERR | EPOLLHUP)) {
                to_close.push_back(fd);
                continue;
            }

            if (event_flags & EPOLLIN) {
                if (!read_available_data(connection)) {
                    to_close.push_back(fd);
                    continue;
                }

                if (request_complete(connection.request)) {
                    connection.response =
                        route_request(connection, metrics, timers,
                                      next_timer_id);
                    connection.bytes_sent = 0;
                }
            }

            if (event_flags & EPOLLOUT) {
                if (!write_pending_data(connection)) {
                    to_close.push_back(fd);
                    continue;
                }

                if (connection.bytes_sent >= connection.response.size()) {
                    to_close.push_back(fd);
                    continue;
                }
            }

            if (!update_epoll_interest(epoll_fd, connection)) {
                to_close.push_back(fd);
            }
        }

        for (int fd : to_close) {
            close_connection(fd, epoll_fd, connections, metrics);
        }
    }

    for (auto& [fd, connection] : connections) {
        close(fd);
    }

    close(epoll_fd);
    close(socket_fd);
    return 0;
}
