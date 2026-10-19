#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "emberdb/socket.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

// Append-only file: every state change is appended as a RESP command and replayed on startup.
class Aof {
public:
    enum class Fsync { kAlways, kEverySecond, kNever };

    struct LoadStats {
        std::size_t commands = 0;     // commands replayed
        std::size_t valid_bytes = 0;  // length of the good part of the file
        std::size_t tail_bytes = 0;   // incomplete trailing bytes that were cut off
    };

    // Replays `path` into `store` (if it exists), repairs a torn tail, then opens the file
    // for appending. Throws std::runtime_error if the file is corrupt or I/O fails.
    static std::unique_ptr<Aof> open(const std::string& path, Fsync policy, Store& store,
                                     LoadStats* stats = nullptr);

    static std::optional<Fsync> parse_fsync(std::string_view text);

    ~Aof();
    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    void append(std::string_view bytes);  // buffer a record in memory
    void commit();                        // write buffered records; fsync if policy is "always"
    void tick();                          // commit + the once-per-second fsync for "everysec"

private:
    Aof(Socket file, Fsync policy);
    void write_pending();
    void sync();

    Socket file_;
    Fsync policy_;
    std::string pending_;
    bool unsynced_ = false;
    std::chrono::steady_clock::time_point last_sync_;
};

}  // namespace emberdb
