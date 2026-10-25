#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace emberdb {

inline constexpr const char* kVersion = "0.12.0";

// Upper bounds of the command latency histogram buckets, in nanoseconds.
inline constexpr std::array<std::uint64_t, 18> kLatencyBoundsNs = {
    500,       1'000,      2'500,      5'000,       10'000,      25'000,
    50'000,    100'000,    250'000,    500'000,     1'000'000,   2'500'000,
    5'000'000, 10'000'000, 50'000'000, 100'000'000, 500'000'000, 1'000'000'000};

// Counts observations in fixed buckets, the way Prometheus histograms work.
class Histogram {
public:
    // An observation lands in the first bucket whose bound is >= the value.
    void observe(std::uint64_t ns) noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t sum_ns() const noexcept { return sum_ns_; }
    // Observations that were <= kLatencyBoundsNs[i].
    [[nodiscard]] std::uint64_t cumulative(std::size_t i) const noexcept;

private:
    // The last slot holds observations bigger than every bound (the "+Inf" bucket).
    std::array<std::uint64_t, kLatencyBoundsNs.size() + 1> buckets_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ns_ = 0;
};

// Why the server closed a connection on purpose.
enum class DropReason : std::size_t {
    kMaxClients,
    kAuthTimeout,
    kProtocolError,
    kOutputLimit,
    kFollowerLag,
};
inline constexpr std::size_t kDropReasonCount = 5;
const char* drop_reason_name(DropReason reason);

// Everything the server counts. The event loop is single-threaded, so plain integers are enough.
class Metrics {
public:
    // `command_names` are the uppercase names that get their own series. Any other name is
    // counted under "other", so a client cannot create new series by inventing commands.
    explicit Metrics(std::vector<std::string> command_names);

    void record_command(const std::string& upper_name, std::uint64_t ns, bool error);
    void connection_dropped(DropReason reason) noexcept {
        ++dropped_[static_cast<std::size_t>(reason)];
    }

    [[nodiscard]] const std::vector<std::pair<std::string, std::uint64_t>>& commands() const noexcept {
        return commands_;
    }
    [[nodiscard]] std::uint64_t other_commands() const noexcept { return other_commands_; }
    [[nodiscard]] std::uint64_t dropped(DropReason reason) const noexcept {
        return dropped_[static_cast<std::size_t>(reason)];
    }
    [[nodiscard]] const Histogram& command_latency() const noexcept { return latency_; }

    std::uint64_t connections_accepted = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t command_errors = 0;
    std::uint64_t auth_failures = 0;
    std::uint64_t aof_rewrites = 0;
    std::uint64_t aof_rewrite_failures = 0;
    std::uint64_t full_syncs_sent = 0;
    std::uint64_t replicated_commands_applied = 0;
    std::uint64_t loop_busy_ns = 0;
    std::uint64_t scrapes = 0;

private:
    std::vector<std::pair<std::string, std::uint64_t>> commands_;  // sorted by name
    std::unordered_map<std::string, std::size_t> index_;           // name -> position in commands_
    std::uint64_t other_commands_ = 0;
    std::array<std::uint64_t, kDropReasonCount> dropped_{};
    Histogram latency_;
};

// Values that are measured at scrape time rather than counted along the way.
struct MetricsSnapshot {
    double uptime_seconds = 0;
    std::int64_t start_time_unix = 0;
    std::size_t connected_clients = 0;
    std::size_t connected_followers = 0;
    std::size_t follower_max_pending_bytes = 0;
    std::size_t keys = 0;
    std::size_t used_memory = 0;
    std::size_t max_memory = 0;
    std::size_t expired_keys = 0;
    std::size_t evicted_keys = 0;
    bool aof_enabled = false;
    std::size_t aof_size_bytes = 0;
    bool auth_required = false;
    bool is_follower = false;
    bool replication_link_up = false;
};

// Renders everything in the Prometheus text exposition format (version 0.0.4).
std::string render_prometheus(const Metrics& metrics, const MetricsSnapshot& snapshot);

}  // namespace emberdb
