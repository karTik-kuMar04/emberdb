#include "emberdb/config.hpp"

#include <cctype>
#include <stdexcept>

namespace emberdb {
namespace {

bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (is_blank(s.front()) || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (is_blank(s.back()) || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

std::vector<ConfigEntry> parse_config(std::string_view text) {
    std::vector<ConfigEntry> entries;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = text.find('\n', pos);
        const bool last = end == std::string_view::npos;
        const std::string_view raw = text.substr(pos, last ? std::string_view::npos : end - pos);
        pos = last ? text.size() + 1 : end + 1;
        ++line_no;

        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::size_t split = 0;
        while (split < line.size() && !is_blank(line[split])) {
            ++split;
        }
        ConfigEntry entry;
        entry.line = line_no;
        for (const char ch : line.substr(0, split)) {
            entry.key += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        const std::string_view value = trim(line.substr(split));
        if (value.empty()) {
            throw std::runtime_error("line " + std::to_string(line_no) + ": option '" + entry.key +
                                     "' needs a value");
        }
        entry.value = std::string(value);
        entries.push_back(std::move(entry));
    }
    return entries;
}

}  // namespace emberdb
