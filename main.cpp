#include "blocking_queue.h"
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
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

volatile sig_atomic_t stop_requested = 0;
volatile sig_atomic_t wake_event_fd = -1;

void handle_signal(int) {
    int saved_errno = errno;
    stop_requested = 1;

    if (wake_event_fd != -1) {
        std::uint64_t value = 1;
        write(static_cast<int>(wake_event_fd), &value, sizeof(value));
    }

    errno = saved_errno;
}

struct ServerMetrics {
    std::atomic<std::size_t> total_requests{0};
    std::atomic<std::size_t> active_connections{0};
    std::atomic<std::size_t> status_200{0};
    std::atomic<std::size_t> status_404{0};
    std::atomic<std::size_t> status_503{0};
    std::atomic<std::size_t> status_504{0};
};

struct Connection {
    int fd;
    std::string request;
    std::string response;
    std::size_t bytes_sent = 0;
    bool waiting_for_worker = false;
    std::uint64_t task_id = 0;
};

struct RequestTask {
    int fd;
    std::uint64_t id;
    std::string request;
};

struct CompletedResponse {
    int fd;
    std::uint64_t id;
    std::string response;
};

struct Deadline {
    std::chrono::steady_clock::time_point expires_at;
    int fd;
    std::uint64_t task_id;
};

struct DeadlineLater {
    bool operator()(const Deadline& left, const Deadline& right) const {
        return left.expires_at > right.expires_at;
    }
};

using DeadlineQueue =
    std::priority_queue<Deadline, std::vector<Deadline>, DeadlineLater>;

std::string make_metrics_body(const ServerMetrics& metrics) {
    std::string body;
    body += "process_id " + std::to_string(getpid()) + "\n";
    body += "total_requests " + std::to_string(metrics.total_requests.load()) +
            "\n";
    body += "active_connections " +
            std::to_string(metrics.active_connections.load()) + "\n";
    body += "status_200 " + std::to_string(metrics.status_200.load()) + "\n";
    body += "status_404 " + std::to_string(metrics.status_404.load()) + "\n";
    body += "status_503 " + std::to_string(metrics.status_503.load()) + "\n";
    body += "status_504 " + std::to_string(metrics.status_504.load()) + "\n";
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

    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) ==
        -1) {
        std::cerr << "setsockopt SO_REUSEPORT failed: " << std::strerror(errno)
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
    } else if (!connection.waiting_for_worker) {
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

bool notify_event_loop(int event_fd) {
    std::uint64_t value = 1;

    while (true) {
        ssize_t bytes_written = write(event_fd, &value, sizeof(value));
        if (bytes_written == sizeof(value)) {
            return true;
        }

        if (bytes_written == -1 && errno == EINTR) {
            continue;
        }

        return false;
    }
}

void drain_eventfd(int event_fd) {
    std::uint64_t value = 0;

    while (true) {
        ssize_t bytes_read = read(event_fd, &value, sizeof(value));
        if (bytes_read == sizeof(value)) {
            continue;
        }

        if (bytes_read == -1 && errno == EINTR) {
            continue;
        }

        if (bytes_read == -1 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        return;
    }
}

void worker_loop(BlockingQueue<RequestTask>& tasks,
                 BlockingQueue<CompletedResponse>& completed, int event_fd) {
    while (auto task = tasks.wait_and_pop()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        completed.push(CompletedResponse{
            task->fd, task->id, make_http_response("200 OK", "slow OK")});
        notify_event_loop(event_fd);
    }
}

void drain_completed_responses(
    int epoll_fd, std::unordered_map<int, Connection>& connections,
    BlockingQueue<CompletedResponse>& completed, ServerMetrics& metrics) {
    while (auto result = completed.try_pop()) {
        auto it = connections.find(result->fd);
        if (it == connections.end()) {
            continue;
        }

        Connection& connection = it->second;
        if (!connection.waiting_for_worker ||
            connection.task_id != result->id) {
            continue;
        }

        connection.response = std::move(result->response);
        connection.bytes_sent = 0;
        connection.waiting_for_worker = false;
        metrics.status_200.fetch_add(1);

        if (!update_epoll_interest(epoll_fd, connection)) {
            std::cerr << "epoll update failed: " << std::strerror(errno)
                      << "\n";
        }
    }
}

void process_expired_deadlines(
    int epoll_fd, std::unordered_map<int, Connection>& connections,
    DeadlineQueue& deadlines, ServerMetrics& metrics) {
    auto now = std::chrono::steady_clock::now();

    while (!deadlines.empty() && deadlines.top().expires_at <= now) {
        Deadline deadline = deadlines.top();
        deadlines.pop();

        auto it = connections.find(deadline.fd);
        if (it == connections.end()) {
            continue;
        }

        Connection& connection = it->second;
        if (!connection.waiting_for_worker ||
            connection.task_id != deadline.task_id) {
            continue;
        }

        connection.response =
            make_http_response("504 Gateway Timeout", "Gateway Timeout");
        connection.bytes_sent = 0;
        connection.waiting_for_worker = false;
        metrics.status_504.fetch_add(1);

        if (!update_epoll_interest(epoll_fd, connection)) {
            std::cerr << "epoll update failed: " << std::strerror(errno)
                      << "\n";
        }
    }
}

int next_deadline_timeout(const DeadlineQueue& deadlines) {
    if (deadlines.empty()) {
        return -1;
    }

    auto now = std::chrono::steady_clock::now();
    if (deadlines.top().expires_at <= now) {
        return 0;
    }

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadlines.top().expires_at - now);
    return static_cast<int>(remaining.count());
}

void join_workers(std::vector<std::thread>& workers) {
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::string route_request(Connection& connection, ServerMetrics& metrics,
                          BlockingQueue<RequestTask>& tasks,
                          DeadlineQueue& deadlines,
                          std::uint64_t& next_task_id) {
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
        connection.waiting_for_worker = true;
        connection.task_id = next_task_id++;
        if (!tasks.push(RequestTask{connection.fd, connection.task_id,
                                    std::move(connection.request)})) {
            connection.waiting_for_worker = false;
            metrics.status_503.fetch_add(1);
            return make_http_response("503 Service Unavailable",
                                      "Server busy");
        }

        deadlines.push(Deadline{std::chrono::steady_clock::now() +
                                    std::chrono::seconds(1),
                                connection.fd, connection.task_id});
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

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }

    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd == -1) {
        std::cerr << "eventfd failed: " << std::strerror(errno) << "\n";
        close(epoll_fd);
        close(socket_fd);
        return 1;
    }
    wake_event_fd = event_fd;

    epoll_event listen_event{};
    listen_event.data.fd = socket_fd;
    listen_event.events = EPOLLIN;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &listen_event) == -1) {
        std::cerr << "epoll add listen socket failed: " << std::strerror(errno)
                  << "\n";
        wake_event_fd = -1;
        close(event_fd);
        close(epoll_fd);
        close(socket_fd);
        return 1;
    }

    epoll_event worker_event{};
    worker_event.data.fd = event_fd;
    worker_event.events = EPOLLIN;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &worker_event) == -1) {
        std::cerr << "epoll add eventfd failed: " << std::strerror(errno)
                  << "\n";
        wake_event_fd = -1;
        close(event_fd);
        close(epoll_fd);
        close(socket_fd);
        return 1;
    }

    std::cout << "server listening on 127.0.0.1:8080 (epoll, pid "
              << getpid() << ")\n";

    BlockingQueue<RequestTask> task_queue(128);
    BlockingQueue<CompletedResponse> completed_queue;
    std::vector<std::thread> workers;
    constexpr int worker_count = 4;
    for (int i = 0; i < worker_count; ++i) {
        workers.emplace_back([&] {
            worker_loop(task_queue, completed_queue, event_fd);
        });
    }

    std::unordered_map<int, Connection> connections;
    ServerMetrics metrics;
    std::vector<epoll_event> events(64);
    DeadlineQueue deadlines;
    std::uint64_t next_task_id = 1;

    while (!stop_requested) {
        process_expired_deadlines(epoll_fd, connections, deadlines, metrics);

        int ready = epoll_wait(epoll_fd, events.data(), events.size(),
                               next_deadline_timeout(deadlines));
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

            if (fd == event_fd) {
                drain_eventfd(event_fd);
                drain_completed_responses(epoll_fd, connections,
                                          completed_queue, metrics);
                if (stop_requested) {
                    break;
                }
                continue;
            }

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
                        route_request(connection, metrics, task_queue,
                                      deadlines, next_task_id);
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

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, socket_fd, nullptr);
    task_queue.close();
    join_workers(workers);
    drain_completed_responses(epoll_fd, connections, completed_queue, metrics);
    completed_queue.close();

    for (auto& [fd, connection] : connections) {
        close(fd);
    }

    wake_event_fd = -1;
    close(event_fd);
    close(epoll_fd);
    close(socket_fd);
    return 0;
}
