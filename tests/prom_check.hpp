#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Returns "" when `text` is valid Prometheus exposition text, otherwise a description of the
// first problem found.
inline std::string check_exposition(const std::string& text) {
    if (text.empty() || text.back() != '\n') {
        return "the output must end with a newline";
    }
    std::map<std::string, std::string> types;
    std::set<std::string> helped;
    std::set<std::string> seen;
    std::map<std::string, std::vector<double>> buckets;
    std::map<std::string, double> counts;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            return "found a blank line";
        }
        if (line.rfind("# HELP ", 0) == 0) {
            const std::string rest = line.substr(7);
            helped.insert(rest.substr(0, rest.find(' ')));
            continue;
        }
        if (line.rfind("# TYPE ", 0) == 0) {
            std::istringstream parts(line.substr(7));
            std::string name;
            std::string type;
            parts >> name >> type;
            if (type != "counter" && type != "gauge" && type != "histogram") {
                return "unsupported type for " + name;
            }
            if (!types.emplace(name, type).second) {
                return "two TYPE lines for " + name;
            }
            if (type == "counter" && !name.ends_with("_total")) {
                return "counter " + name + " should end in _total";
            }
            continue;
        }
        if (line.front() == '#') {
            continue;
        }

        const std::size_t space = line.rfind(' ');
        if (space == std::string::npos) {
            return "sample without a value: " + line;
        }
        const std::string series = line.substr(0, space);
        const std::string value_text = line.substr(space + 1);
        char* end = nullptr;
        const double value = std::strtod(value_text.c_str(), &end);
        if (end == value_text.c_str() || *end != '\0') {
            return "value is not a number: " + line;
        }
        if (!seen.insert(series).second) {
            return "the same series appears twice: " + series;
        }

        const std::string name = series.substr(0, series.find('{'));
        for (std::size_t i = 0; i < name.size(); ++i) {
            const auto ch = static_cast<unsigned char>(name[i]);
            const bool ok = std::isalpha(ch) != 0 || ch == '_' || ch == ':' ||
                            (i > 0 && std::isdigit(ch) != 0);
            if (!ok) {
                return "bad metric name: " + name;
            }
        }
        std::string family = name;
        if (types.count(family) == 0) {
            for (const char* raw : {"_bucket", "_sum", "_count"}) {
                const std::string suffix = raw;
                if (!name.ends_with(suffix)) {
                    continue;
                }
                const std::string base = name.substr(0, name.size() - suffix.size());
                const auto it = types.find(base);
                if (it == types.end() || it->second != "histogram") {
                    continue;
                }
                family = base;
                if (suffix == "_bucket") {
                    buckets[base].push_back(value);
                }
                if (suffix == "_count") {
                    counts[base] = value;
                }
                break;
            }
        }
        if (types.count(family) == 0) {
            return "sample without a TYPE line: " + name;
        }
        if (helped.count(family) == 0) {
            return "no HELP line for " + family;
        }
    }
    for (const auto& [base, values] : buckets) {
        for (std::size_t i = 1; i < values.size(); ++i) {
            if (values[i] < values[i - 1]) {
                return "histogram " + base + " buckets are not cumulative";
            }
        }
        if (counts.count(base) == 0 || counts[base] != values.back()) {
            return "histogram " + base + ": _count must equal the +Inf bucket";
        }
    }
    return "";
}
