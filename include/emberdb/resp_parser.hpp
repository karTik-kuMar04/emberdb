#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace emberdb {

// A parsed client command, e.g. {"SET", "name", "ember"}.
using Command = std::vector<std::string>;

enum class ParseStatus {
    kComplete,    // a full command was parsed
    kIncomplete,  // need more bytes; call again after more data arrives
    kError,       // malformed input (protocol violation)
};

struct ParseResult {
    ParseStatus status = ParseStatus::kIncomplete;
    Command command;            // valid only when status == kComplete
    std::size_t consumed = 0;   // bytes to discard from the buffer (valid only when kComplete)
    std::string error;          // valid only when status == kError
};

// Tries to parse ONE command from the front of `buffer`.
// Does not modify the buffer: the caller erases `consumed` bytes on success.
ParseResult parse_command(std::string_view buffer);

}  // namespace emberdb
