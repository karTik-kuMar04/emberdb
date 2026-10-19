#pragma once

#include <charconv>
#include <limits>
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

// a + b, clamped to the limits of long long instead of overflowing.
inline long long saturating_add(long long a, long long b) {
    long long result = 0;
    if (!__builtin_add_overflow(a, b, &result)) {
        return result;
    }
    return b > 0 ? std::numeric_limits<long long>::max() : std::numeric_limits<long long>::min();
}

}  // namespace emberdb
