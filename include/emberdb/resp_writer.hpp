#pragma once

#include <string>
#include <string_view>

namespace emberdb::resp {
std::string simple_string(std::string_view s); // +OK\r\n
std::string error(std::string_view msg); //-ERR <msg>\r\n
std::string integer(long long n); // :1\r\n
std::string bulk_string(std::string_view s); // $5\r\nember\r\n
std::string null_bulk(); // $-1\r\n

} // namespace emberdb::resp