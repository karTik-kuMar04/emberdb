#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace emberdb::resp {

std::string simple_string(std::string_view s);                        // +OK\r\n
std::string error(std::string_view msg);                              // -ERR <msg>\r\n
std::string error_code(std::string_view code, std::string_view msg);  // -<code> <msg>\r\n
std::string integer(long long n);                                     // :1\r\n
std::string bulk_string(std::string_view s);                          // $5\r\nember\r\n
std::string null_bulk();                                              // $-1\r\n

// Encodes a command the way a client sends it: *N\r\n$len\r\npart\r\n...
// This is the AOF record format.
std::string command(const std::vector<std::string>& parts);

}  // namespace emberdb::resp
