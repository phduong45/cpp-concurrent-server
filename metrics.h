#pragma once

#include <atomic>
#include <cstddef>
#include <string>

struct ServerMetrics {
    std::atomic<std::size_t> total_requests{0};
    std::atomic<std::size_t> active_connections{0};
    std::atomic<std::size_t> status_200{0};
    std::atomic<std::size_t> status_404{0};
    std::atomic<std::size_t> status_503{0};
    std::atomic<std::size_t> status_504{0};
};

std::string make_metrics_body(const ServerMetrics& metrics);
