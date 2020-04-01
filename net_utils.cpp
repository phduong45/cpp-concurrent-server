#include "net_utils.h"
#include <cerrno>
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