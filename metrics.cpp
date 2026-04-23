#include "metrics.h"

#include <string>
#include <unistd.h>

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
