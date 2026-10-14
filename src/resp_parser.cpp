#include "emberdb/resp_parser.hpp"

#include <charconv>
#include <optional>

namespace emberdb {
namespace {

constexpr std::size_t kMaxArrayLen = 1024 * 1024;
constexpr long long kMaxBulkLen = 512LL * 1024 * 1024;

ParseResult incomplete() { return ParseResult{ParseStatus::kIncomplete, {}, 0, {}}; }

ParseResult error(std::string msg) {
    return ParseResult{ParseStatus::kError, {}, 0, std::move(msg)};
}

// Reads one "\r\n"-terminated line starting at `pos`.
// On success returns the line (without CRLF) and advances `pos` past the CRLF.
std::optional<std::string_view> read_line(std::string_view buf, std::size_t& pos) {
    const std::size_t end = buf.find("\r\n", pos);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view line = buf.substr(pos, end - pos);
    pos = end + 2;
    return line;
}

// Parses a base-10 integer from `text`. Returns nullopt if it isn't a valid integer.
std::optional<long long> parse_int(std::string_view text) {
    long long value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || text.empty()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

ParseResult parse_command(std::string_view buffer) {
    if (buffer.empty()) {
        return incomplete();
    }
    if (buffer[0] != '*') {
        return error("expected '*' at start of command");
    }

    std::size_t pos = 1;

    // --- Array header: *<count>\r\n ---
    auto header = read_line(buffer, pos);
    if (!header) {
        return incomplete();
    }
    auto count = parse_int(*header);
    if (!count || *count < 1 || static_cast<std::size_t>(*count) > kMaxArrayLen) {
        return error("invalid array length");
    }

    Command command;
    command.reserve(static_cast<std::size_t>(*count));

    // --- Each element: $<len>\r\n<len bytes>\r\n ---
    for (long long i = 0; i < *count; ++i) {
        if (pos >= buffer.size()) {
            return incomplete();
        }
        if (buffer[pos] != '$') {
            return error("expected '$' before bulk string");
        }
        ++pos;

        auto len_line = read_line(buffer, pos);
        if (!len_line) {
            return incomplete();
        }
        auto len = parse_int(*len_line);
        if (!len || *len < 0 || *len > kMaxBulkLen) {
            return error("invalid bulk string length");
        }

        const auto n = static_cast<std::size_t>(*len);
        // Need n payload bytes plus the trailing CRLF.
        if (buffer.size() - pos < n + 2) {
            return incomplete();
        }
        if (buffer[pos + n] != '\r' || buffer[pos + n + 1] != '\n') {
            return error("bulk string not terminated by CRLF");
        }

        command.emplace_back(buffer.substr(pos, n));
        pos += n + 2;
    }

    return ParseResult{ParseStatus::kComplete, std::move(command), pos, {}};
}

}  // namespace emberdb
