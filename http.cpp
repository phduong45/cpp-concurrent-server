#include "http.h"

#include <sstream>

std::optional<HttpRequest> parse_request_line(std::string_view request) {
    auto line_end = request.find("\r\n");
    if (line_end == std::string_view::npos) {
        return std::nullopt;
    }

    std::string request_line(request.substr(0, line_end));
    std::istringstream iss(request_line);
    HttpRequest parsed;
    if (!(iss >> parsed.method >> parsed.path >> parsed.version)) {
        return std::nullopt;
    }

    return parsed;
}

std::optional<std::size_t> get_content_length(std::string_view headers) {
    constexpr std::string_view key = "Content-Length:";

    auto pos = headers.find(key);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }

    auto value_start = pos + key.size();
    auto line_end = headers.find("\r\n", value_start);
    if (line_end == std::string_view::npos) {
        return std::nullopt;
    }

    std::string value{headers.substr(value_start, line_end - value_start)};

    try {
        return static_cast<std::size_t>(std::stoul(value));
    } catch (...) {
        return std::nullopt;
    }
}

bool request_complete(std::string_view request) {
    auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return false;
    }

    std::string_view headers{request.data(), header_end + 4};
    auto content_length = get_content_length(headers);
    if (!content_length) {
        return true;
    }

    std::size_t body_start = header_end + 4;
    std::size_t body_size = request.size() - body_start;
    return body_size >= *content_length;
}

std::string_view request_body(std::string_view request) {
    auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return {};
    }

    std::size_t body_start = header_end + 4;
    return request.substr(body_start);
}

std::string make_http_response(std::string_view status, std::string_view body) {
    std::string response;
    response += "HTTP/1.1 ";
    response += status;
    response += "\r\n";
    response += "Content-Length: ";
    response += std::to_string(body.size());
    response += "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;
    return response;
}
