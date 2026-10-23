#pragma once

#include <cctype>
#include <charconv>
#include <cstdint>
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

// Sizes for command-line flags: "1024", "10k", "10kb", "5mb", "2g" (case-insensitive,
// 1k = 1024 bytes). Returns nullopt for anything else, including overflow.
inline std::optional<std::uint64_t> parse_bytes(std::string_view text) {
    std::size_t split = 0;
    while (split < text.size() && std::isdigit(static_cast<unsigned char>(text[split])) != 0) {
        ++split;
    }
    if (split == 0) {
        return std::nullopt;
    }
    std::uint64_t number = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + split, number);
    if (ec != std::errc{}) {
        return std::nullopt;
    }
    std::string unit;
    for (const char ch : text.substr(split)) {
        unit += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    std::uint64_t factor = 1;
    if (unit.empty() || unit == "b") {
        factor = 1;
    } else if (unit == "k" || unit == "kb") {
        factor = 1024ULL;
    } else if (unit == "m" || unit == "mb") {
        factor = 1024ULL * 1024;
    } else if (unit == "g" || unit == "gb") {
        factor = 1024ULL * 1024 * 1024;
    } else {
        return std::nullopt;
    }
    std::uint64_t bytes = 0;
    if (__builtin_mul_overflow(number, factor, &bytes)) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace emberdb
