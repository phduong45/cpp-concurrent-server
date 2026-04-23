#pragma once

#include "metrics.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

struct Connection {
    int fd;
    std::string request;
    std::string response;
    std::size_t bytes_sent = 0;
    bool waiting_for_worker = false;
    std::uint64_t task_id = 0;
};

bool update_epoll_interest(int epoll_fd, const Connection& connection);

void close_connection(int fd, int epoll_fd,
                      std::unordered_map<int, Connection>& connections,
                      ServerMetrics& metrics);

void accept_ready_clients(int socket_fd, int epoll_fd,
                          std::unordered_map<int, Connection>& connections,
                          ServerMetrics& metrics);

bool read_available_data(Connection& connection);

bool write_pending_data(Connection& connection);
