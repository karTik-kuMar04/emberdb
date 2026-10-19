#include "emberdb/snapshot.hpp"

#include <optional>
#include <string>

#include "emberdb/resp_writer.hpp"

namespace emberdb {

std::size_t write_snapshot(const Store& store, const Sink& sink, std::size_t chunk_bytes) {
    std::string buffer;
    std::size_t keys = 0;
    store.for_each_live([&](const std::string& key, const std::string& value,
                            std::optional<std::int64_t> expire_at) {
        buffer += resp::command({"SET", key, value});
        if (expire_at) {
            buffer += resp::command({"PEXPIREAT", key, std::to_string(*expire_at)});
        }
        ++keys;
        if (buffer.size() >= chunk_bytes) {
            sink(buffer);
            buffer.clear();
        }
    });
    if (!buffer.empty()) {
        sink(buffer);
    }
    return keys;
}

}  // namespace emberdb
