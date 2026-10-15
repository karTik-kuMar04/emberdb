#pragma once

#include <string>

#include "emberdb/resp_parser.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

// Runs one parsed command against the store and returns the RESP reply bytes.
    std::string execute(Store& store, const Command& command);
} // namespace emberdb

