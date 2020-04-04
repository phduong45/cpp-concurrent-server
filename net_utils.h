#include <cstddef>
#include <string>
#include <string_view>

bool write_all(int fd, const char* data, std::size_t size);

bool read_all(int fd, char* data, std::size_t size);

bool send_message(int fd, std::string_view payload);

bool read_message(int fd, std::string& out);