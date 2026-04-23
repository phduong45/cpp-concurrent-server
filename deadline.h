#pragma once

#include "connection.h"
#include "metrics.h"

#include <chrono>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

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

void process_expired_deadlines(
    int epoll_fd, std::unordered_map<int, Connection>& connections,
    DeadlineQueue& deadlines, ServerMetrics& metrics);

int next_deadline_timeout(const DeadlineQueue& deadlines);
