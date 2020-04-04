#include "net_utils.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <unistd.h>

bool write_all(int fd, const char* data, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
        ssize_t bytes_written = write(fd, data + total, size - total);

        if (bytes_written == -1) {
            if (errno != EINTR) {
                return false;
            } else {
                continue;
            }
        }
        if (bytes_written == 0) {
            return false;
        }
        total += bytes_written;
    }
    return true;
}

bool read_all(int fd, char* data, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
        ssize_t bytes_read = read(fd, data + total, size - total);

        if (bytes_read == -1) {
            if (errno != EINTR) {
                return false;
            } else {
                continue;
            }
        }
        if (bytes_read == 0) {
            return false;
        }
        total += bytes_read;
    }
    return true;
}

bool send_message(int fd, std::string_view payload) {
    if (payload.size() > UINT32_MAX) {
        return false;
    }

    uint32_t length = htonl(payload.size());
    // write header
    if (!write_all(fd, reinterpret_cast<const char*>(&length),
                   sizeof(length))) {
        return false;
    }

    // write payload
    if (!write_all(fd, payload.data(), payload.size())) {
        return false;
    }

    return true;
}

bool read_message(int fd, std::string& out) {
    uint32_t net_length = 0;

    // read network length
    if (!read_all(fd, reinterpret_cast<char*>(&net_length),
                  sizeof(net_length))) {
        return false;
    }

    uint32_t payload_length = ntohl(net_length);
    out.resize(payload_length);

    // read payload
    if (!read_all(fd, out.data(), out.size())) {
        return false;
    }

    return true;
}