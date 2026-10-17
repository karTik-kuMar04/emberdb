#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace emberdb {

// Strict base-10 parse. Rejects "", "+5", " 5", "007", "-0", "5x" and out-of-range values.
inline std::optional<long long> parse_int64(std::string_view text) {
    long long value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (text.empty() || ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    if (std::to_string(value) != text) {
        return std::nullopt;  // not the canonical spelling (leading zeros, "-0")
    }
    return value;
}

}  // namespace emberdb
