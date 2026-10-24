#include "emberdb/http.hpp"

namespace emberdb {
namespace {

const char* reason_phrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 431:
            return "Request Header Fields Too Large";
        case 503:
            return "Service Unavailable";
        default:
            return "Status";
    }
}

}  // namespace

HttpParseResult parse_http_request(std::string_view buffer) {
    HttpParseResult result;
    if (buffer.find("\r\n\r\n") == std::string_view::npos) {
        return result;  // the head is not complete yet
    }
    result.status = HttpParse::kBad;  // from here on, anything wrong is a bad request

    const std::string_view line = buffer.substr(0, buffer.find("\r\n"));
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0) {
        return result;
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return result;
    }
    const std::string_view method = line.substr(0, first_space);
    std::string_view target = line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = line.substr(second_space + 1);

    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        return result;
    }
    for (const char c : method) {
        if (c < 'A' || c > 'Z') {
            return result;
        }
    }
    if (target.empty() || target.front() != '/') {
        return result;
    }
    for (const char c : target) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7f) {
            return result;  // control characters, spaces and non-ASCII bytes
        }
    }
    const std::size_t cut = target.find_first_of("?#");
    if (cut != std::string_view::npos) {
        target = target.substr(0, cut);
    }

    result.status = HttpParse::kComplete;
    result.request.method = std::string(method);
    result.request.path = std::string(target);
    return result;
}

std::string http_response(int status, std::string_view content_type, std::string_view body,
                          std::string_view extra_headers) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) + "\r\n";
    out += "Content-Type: ";
    out += content_type;
    out += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    out += "Connection: close\r\n";
    out += extra_headers;
    out += "\r\n";
    out += body;
    return out;
}

}  // namespace emberdb
