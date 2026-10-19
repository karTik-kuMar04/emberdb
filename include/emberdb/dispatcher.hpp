#pragma once

#include <string>

#include "emberdb/resp_parser.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

// Runs one parsed command against the store and returns the RESP reply bytes.
// If `propagate` is given, it is set to the command that should be appended to the AOF and sent
// to followers (possibly rewritten, e.g. EXPIRE -> PEXPIREAT), or emptied if nothing changed.
// With `read_only` set, write commands are refused (used for clients of a follower).
std::string execute(Store& store, const Command& command, Command* propagate = nullptr,
                    bool read_only = false);

}  // namespace emberdb
