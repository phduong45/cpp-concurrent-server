#pragma once

#include "blocking_queue.h"
#include "connection.h"
#include "metrics.h"

#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

bool notify_event_loop(int event_fd);

void drain_eventfd(int event_fd);

void worker_loop(BlockingQueue<RequestTask>& tasks,
                 BlockingQueue<CompletedResponse>& completed, int event_fd);

void drain_completed_responses(
    int epoll_fd, std::unordered_map<int, Connection>& connections,
    BlockingQueue<CompletedResponse>& completed, ServerMetrics& metrics);

void join_workers(std::vector<std::thread>& workers);
