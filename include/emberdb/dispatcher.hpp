#pragma once

#include <string>

#include "emberdb/resp_parser.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

// Runs one parsed command against the store and returns the RESP reply bytes.
// If `propagate` is given, it is set to the command that should be appended to the AOF
// (possibly rewritten, e.g. EXPIRE -> PEXPIREAT), or left empty if nothing changed.
std::string execute(Store& store, const Command& command, Command* propagate = nullptr);

}  // namespace emberdb
