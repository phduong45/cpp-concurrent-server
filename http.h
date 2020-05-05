#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
};

std::optional<HttpRequest> parse_request_line(std::string_view request);

std::optional<std::size_t> get_content_length(std::string_view headers);

bool request_complete(std::string_view request);

std::string_view request_body(std::string_view request);

std::string make_http_response(std::string_view status, std::string_view body);
