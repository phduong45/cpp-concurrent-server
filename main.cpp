#include "blocking_queue.h"
#include "connection.h"
#include "deadline.h"
#include "http.h"
#include "metrics.h"
#include "net_utils.h"
#include "worker.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
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
