#include "emberdb/metrics.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace emberdb {

void Histogram::observe(std::uint64_t ns) noexcept {
    const auto it = std::lower_bound(kLatencyBoundsNs.begin(), kLatencyBoundsNs.end(), ns);
    ++buckets_[static_cast<std::size_t>(it - kLatencyBoundsNs.begin())];
    ++count_;
    sum_ns_ += ns;
}

std::uint64_t Histogram::cumulative(std::size_t i) const noexcept {
    std::uint64_t total = 0;
    for (std::size_t j = 0; j <= i && j < buckets_.size(); ++j) {
        total += buckets_[j];
    }
    return total;
}

const char* drop_reason_name(DropReason reason) {
    switch (reason) {
        case DropReason::kMaxClients:
            return "max_clients";
        case DropReason::kAuthTimeout:
            return "auth_timeout";
        case DropReason::kProtocolError:
            return "protocol_error";
        case DropReason::kOutputLimit:
            return "output_limit";
        case DropReason::kFollowerLag:
            return "follower_lag";
    }
    return "unknown";
}

Metrics::Metrics(std::vector<std::string> command_names) {
    std::sort(command_names.begin(), command_names.end());
    command_names.erase(std::unique(command_names.begin(), command_names.end()),
                        command_names.end());
    for (std::string& name : command_names) {
        index_.emplace(name, commands_.size());
        commands_.emplace_back(std::move(name), 0);
    }
}

void Metrics::record_command(const std::string& upper_name, std::uint64_t ns, bool error) {
    const auto it = index_.find(upper_name);
    if (it == index_.end()) {
        ++other_commands_;
    } else {
        ++commands_[it->second].second;
    }
    if (error) {
        ++command_errors;
    }
    latency_.observe(ns);
}

namespace {

std::string format_double(double value, const char* format) {
    char buffer[64];
    const int n = std::snprintf(buffer, sizeof(buffer), format, value);
    return std::string(buffer, n > 0 ? static_cast<std::size_t>(n) : 0);
}

void header(std::string& out, std::string_view name, std::string_view type,
            std::string_view help) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += "\n# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

void sample(std::string& out, std::string_view series, std::string_view value) {
    out += series;
    out += ' ';
    out += value;
    out += '\n';
}

void counter(std::string& out, std::string_view name, std::string_view help,
             std::uint64_t value) {
    header(out, name, "counter", help);
    sample(out, name, std::to_string(value));
}

void gauge(std::string& out, std::string_view name, std::string_view help, std::uint64_t value) {
    header(out, name, "gauge", help);
    sample(out, name, std::to_string(value));
}

void gauge_double(std::string& out, std::string_view name, std::string_view help, double value,
                  const char* format) {
    header(out, name, "gauge", help);
    sample(out, name, format_double(value, format));
}

}  // namespace

std::string render_prometheus(const Metrics& m, const MetricsSnapshot& s) {
    std::string out;
    out.reserve(8192);

    header(out, "emberdb_build_info", "gauge", "Build information. The value is always 1.");
    sample(out, std::string("emberdb_build_info{version=\"") + kVersion + "\"}", "1");
    gauge(out, "emberdb_start_time_seconds", "Unix time when the server started.",
          static_cast<std::uint64_t>(s.start_time_unix));
    gauge_double(out, "emberdb_uptime_seconds", "Seconds since the server started.",
                 s.uptime_seconds, "%.3f");

    gauge(out, "emberdb_connected_clients", "Client connections open right now.",
          s.connected_clients);
    counter(out, "emberdb_connections_accepted_total", "Client connections accepted.",
            m.connections_accepted);
    header(out, "emberdb_connections_dropped_total", "counter",
           "Connections the server closed on purpose, by reason.");
    for (std::size_t i = 0; i < kDropReasonCount; ++i) {
        const auto reason = static_cast<DropReason>(i);
        sample(out,
               std::string("emberdb_connections_dropped_total{reason=\"") +
                   drop_reason_name(reason) + "\"}",
               std::to_string(m.dropped(reason)));
    }
    counter(out, "emberdb_network_read_bytes_total", "Bytes received from clients and peers.",
            m.bytes_read);
    counter(out, "emberdb_network_written_bytes_total", "Bytes sent to clients and peers.",
            m.bytes_written);

    header(out, "emberdb_commands_processed_total", "counter",
           "Commands executed, by name. Unknown names are grouped under other.");
    for (const auto& [name, count] : m.commands()) {
        sample(out, "emberdb_commands_processed_total{command=\"" + name + "\"}",
               std::to_string(count));
    }
    sample(out, "emberdb_commands_processed_total{command=\"other\"}",
           std::to_string(m.other_commands()));
    counter(out, "emberdb_command_errors_total",
            "Commands that were refused or failed (the reply starts with a minus sign).",
            m.command_errors);

    header(out, "emberdb_command_duration_seconds", "histogram",
           "Time spent executing a command inside the server. Network time is not included.");
    const Histogram& latency = m.command_latency();
    for (std::size_t i = 0; i < kLatencyBoundsNs.size(); ++i) {
        const double bound = static_cast<double>(kLatencyBoundsNs[i]) / 1e9;
        sample(out,
               "emberdb_command_duration_seconds_bucket{le=\"" + format_double(bound, "%g") +
                   "\"}",
               std::to_string(latency.cumulative(i)));
    }
    sample(out, "emberdb_command_duration_seconds_bucket{le=\"+Inf\"}",
           std::to_string(latency.count()));
    sample(out, "emberdb_command_duration_seconds_sum",
           format_double(static_cast<double>(latency.sum_ns()) / 1e9, "%.9f"));
    sample(out, "emberdb_command_duration_seconds_count", std::to_string(latency.count()));

    header(out, "emberdb_event_loop_busy_seconds_total", "counter",
           "Time the event loop spent working instead of waiting. Its rate is the utilization.");
    sample(out, "emberdb_event_loop_busy_seconds_total",
           format_double(static_cast<double>(m.loop_busy_ns) / 1e9, "%.9f"));

    gauge(out, "emberdb_keys", "Keys in memory, including expired ones not yet removed.", s.keys);
    counter(out, "emberdb_keys_expired_total", "Keys removed because their deadline passed.",
            s.expired_keys);
    counter(out, "emberdb_keys_evicted_total", "Keys removed to stay under the memory limit.",
            s.evicted_keys);
    gauge(out, "emberdb_memory_used_bytes", "Estimated memory used by keys and values.",
          s.used_memory);
    gauge(out, "emberdb_memory_limit_bytes", "Configured memory limit. 0 means unlimited.",
          s.max_memory);

    gauge(out, "emberdb_aof_enabled", "1 when the append-only file is on.",
          s.aof_enabled ? 1U : 0U);
    gauge(out, "emberdb_aof_size_bytes", "Size of the append-only file.", s.aof_size_bytes);
    counter(out, "emberdb_aof_rewrites_total", "Completed append-only file rewrites.",
            m.aof_rewrites);
    counter(out, "emberdb_aof_rewrite_failures_total", "Append-only file rewrites that failed.",
            m.aof_rewrite_failures);

    gauge(out, "emberdb_auth_required", "1 when clients must authenticate.",
          s.auth_required ? 1U : 0U);
    counter(out, "emberdb_auth_failures_total", "Wrong passwords received.", m.auth_failures);

    gauge(out, "emberdb_replication_follower", "1 when this server follows a leader.",
          s.is_follower ? 1U : 0U);
    gauge(out, "emberdb_replication_link_up", "1 when the link to the leader is receiving data.",
          s.replication_link_up ? 1U : 0U);
    gauge(out, "emberdb_replication_connected_followers", "Followers connected to this server.",
          s.connected_followers);
    gauge(out, "emberdb_replication_follower_max_pending_bytes",
          "Unsent bytes queued for the slowest follower. A measure of replication lag.",
          s.follower_max_pending_bytes);
    counter(out, "emberdb_replication_full_syncs_sent_total", "Full syncs this leader has served.",
            m.full_syncs_sent);
    counter(out, "emberdb_replication_commands_applied_total",
            "Commands this follower applied from its leader.", m.replicated_commands_applied);

    counter(out, "emberdb_metrics_scrapes_total", "Requests served by the metrics endpoint.",
            m.scrapes);
    return out;
}

}  // namespace emberdb
