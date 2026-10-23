#include "emberdb/resp_writer.hpp"

namespace emberdb::resp {

std::string simple_string(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 3);
    out += '+';
    out += s;
    out += "\r\n";
    return out;
}

std::string error_code(std::string_view code, std::string_view msg) {
    std::string out = "-";
    out.reserve(code.size() + msg.size() + 8);
    out += code;
    out += ' ';
    for (char c : msg) {
        // A raw CR/LF would end the reply early and let a client inject
        // fake protocol lines, so we neutralize them.
        out += (c == '\r' || c == '\n') ? ' ' : c;
    }
    out += "\r\n";
    return out;
}

std::string error(std::string_view msg) {
    return error_code("ERR", msg);
}

std::string integer(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

std::string bulk_string(std::string_view s) {
    std::string out = "$" + std::to_string(s.size()) + "\r\n";
    out += s;
    out += "\r\n";
    return out;
}

std::string null_bulk() {
    return "$-1\r\n";
}

std::string command(const std::vector<std::string>& parts) {
    std::string out = "*" + std::to_string(parts.size()) + "\r\n";
    for (const std::string& part : parts) {
        out += '$';
        out += std::to_string(part.size());
        out += "\r\n";
        out += part;
        out += "\r\n";
    }
    return out;
}

}  // namespace emberdb::resp
