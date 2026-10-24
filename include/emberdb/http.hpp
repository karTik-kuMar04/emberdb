#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace emberdb {

// A request this large without a blank line is rejected.
inline constexpr std::size_t kMaxHttpRequestBytes = 8192;

enum class HttpParse { kIncomplete, kBad, kComplete };

struct HttpRequest {
    std::string method;
    std::string path;  // without any query string or fragment
};

struct HttpParseResult {
    HttpParse status = HttpParse::kIncomplete;
    HttpRequest request;
};

// Looks at the head of a request (up to the blank line). Only the request line is used: headers
// are ignored, and so is any body, because every connection serves one request and then closes.
HttpParseResult parse_http_request(std::string_view buffer);

// Builds a complete response. `extra_headers` must be whole lines ending in "\r\n".
std::string http_response(int status, std::string_view content_type, std::string_view body,
                          std::string_view extra_headers = {});

}  // namespace emberdb
