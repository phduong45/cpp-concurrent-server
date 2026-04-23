#include "connection.h"

#include "net_utils.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

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
