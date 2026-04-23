#include "worker.h"

#include "http.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <utility>

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

void join_workers(std::vector<std::thread>& workers) {
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}
