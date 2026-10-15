#include "emberdb/resp_writer.hpp"
#include <string>
#include <string_view>

namespace emberdb::resp {
std::string simple_string(std::string_view s) {

    std::string out;
    out.reserve(s.size() + 3);
    out += '+';
    out += s;
    out += "\r\n";
    return out;
}

std::string error(std::string_view msg) {
    std::string out = "-ERR ";
    out.reserve(msg.size() + 8);
    for (char ch : msg) {
        // A raw CR/LF would end the reply early and let a client inject
        // fake protocol lines, so we neutralize them.
        out += (ch == '\r' || ch == '\n') ? ' ' : ch;
    }
    out +="\r\n";
    return out;
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

} // namespace emberdb::resp
