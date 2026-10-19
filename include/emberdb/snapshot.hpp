#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

#include "emberdb/store.hpp"

namespace emberdb {

// Receives the snapshot in pieces. The AOF rewrite writes them to a file; replication
// will send them to a follower.
using Sink = std::function<void(std::string_view)>;

// Emits the store's live contents as RESP commands (SET, plus PEXPIREAT for keys with a
// deadline), in chunks of roughly `chunk_bytes`. Returns the number of keys written.
std::size_t write_snapshot(const Store& store, const Sink& sink,
                           std::size_t chunk_bytes = 64UL * 1024);

}  // namespace emberdb
