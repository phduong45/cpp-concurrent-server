#include "deadline.h"

#include "http.h"

#include <chrono>
#include <cstring>
#include <iostream>

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
