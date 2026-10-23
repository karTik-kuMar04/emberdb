#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace emberdb {

struct ConfigEntry {
    std::string key;    // lowercased
    std::string value;  // the rest of the line, trimmed
    std::size_t line = 0;
};

// Parses a config file: one "key value" per line, '#' at the start of a line makes a comment
// (so a password may contain '#'). Throws std::runtime_error naming the line on a mistake.
std::vector<ConfigEntry> parse_config(std::string_view text);

}  // namespace emberdb
