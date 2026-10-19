#include "emberdb/dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "emberdb/int_parse.hpp"
#include "emberdb/resp_writer.hpp"

namespace emberdb {
namespace {

// `log` is filled only when the command changed state, with the form that replays correctly.
using Handler = std::string (*)(Store&, const Command&, Command& log);

struct CommandSpec {
    Handler handler;
    int arity;  // >= 0: exactly that many tokens (name included). < 0: at least -arity.
};

constexpr const char* kNotInteger = "value is not an integer or out of range";

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool arity_ok(int arity, std::size_t argc) {
    if (arity >= 0) {
        return argc == static_cast<std::size_t>(arity);
    }
    return argc >= static_cast<std::size_t>(-arity);
}

// Commands that change data. A read-only follower refuses these from clients.
bool is_write(std::string_view name) {
    static const std::unordered_set<std::string_view> kWrites = {
        "SET", "DEL", "EXPIRE", "PEXPIRE", "PEXPIREAT", "PERSIST", "INCR", "DECR", "FLUSHALL"};
    return kWrites.count(name) > 0;
}

// --- Handlers: arity is already validated, so indexing is safe ---

std::string cmd_ping(Store&, const Command&, Command&) {
    return resp::simple_string("PONG");
}

std::string cmd_set(Store& store, const Command& cmd, Command& log) {
    store.set(cmd[1], cmd[2]);
    log = cmd;
    return resp::simple_string("OK");
}

std::string cmd_get(Store& store, const Command& cmd, Command&) {
    auto value = store.get(cmd[1]);
    if (!value) {
        return resp::null_bulk();
    }
    return resp::bulk_string(*value);
}

std::string cmd_del(Store& store, const Command& cmd, Command& log) {
    long long removed = 0;
    for (std::size_t i = 1; i < cmd.size(); ++i) {
        if (store.del(cmd[i])) {
            ++removed;
        }
    }
    if (removed > 0) {
        log = cmd;
    }
    return resp::integer(removed);
}

std::string cmd_exists(Store& store, const Command& cmd, Command&) {
    long long found = 0;
    for (std::size_t i = 1; i < cmd.size(); ++i) {
        if (store.exists(cmd[i])) {
            ++found;
        }
    }
    return resp::integer(found);
}

std::string cmd_flushall(Store& store, const Command& cmd, Command& log) {
    store.clear();
    log = cmd;
    return resp::simple_string("OK");
}

// Counts keys not yet purged, so it can include expired ones. Handy for comparing a leader
// with its follower.
std::string cmd_dbsize(Store& store, const Command&, Command&) {
    return resp::integer(static_cast<long long>(store.size()));
}

// Shared tail of EXPIRE, PEXPIRE and PEXPIREAT. The log always records an absolute time,
// or a DEL when the deadline was already past.
std::string apply_expire_at(Store& store, const std::string& key, long long at_ms, Command& log) {
    switch (store.expire_at(key, at_ms)) {
        case ExpireResult::kNoSuchKey:
            return resp::integer(0);
        case ExpireResult::kDeadlineSet:
            log = Command{"PEXPIREAT", key, std::to_string(at_ms)};
            return resp::integer(1);
        case ExpireResult::kKeyDeleted:
            log = Command{"DEL", key};
            return resp::integer(1);
    }
    return resp::error("internal error");
}

// Shared by EXPIRE (unit = 1000 ms) and PEXPIRE (unit = 1 ms).
std::string expire_relative(Store& store, const Command& cmd, long long unit_ms, const char* name,
                            Command& log) {
    const auto amount = parse_int64(cmd[2]);
    if (!amount) {
        return resp::error(kNotInteger);
    }
    long long ms = 0;
    if (__builtin_mul_overflow(*amount, unit_ms, &ms)) {
        return resp::error(std::string("invalid expire time in '") + name + "' command");
    }
    return apply_expire_at(store, cmd[1], saturating_add(store.now_ms(), ms), log);
}

std::string cmd_expire(Store& store, const Command& cmd, Command& log) {
    return expire_relative(store, cmd, 1000, "expire", log);
}

std::string cmd_pexpire(Store& store, const Command& cmd, Command& log) {
    return expire_relative(store, cmd, 1, "pexpire", log);
}

std::string cmd_pexpireat(Store& store, const Command& cmd, Command& log) {
    const auto at = parse_int64(cmd[2]);
    if (!at) {
        return resp::error(kNotInteger);
    }
    return apply_expire_at(store, cmd[1], *at, log);
}

std::string cmd_ttl(Store& store, const Command& cmd, Command&) {
    const long long ms = store.ttl_ms(cmd[1]);
    if (ms < 0) {
        return resp::integer(ms);  // -2: no such key, -1: no deadline
    }
    return resp::integer((ms + 500) / 1000);  // round to the nearest second
}

std::string cmd_pttl(Store& store, const Command& cmd, Command&) {
    return resp::integer(store.ttl_ms(cmd[1]));
}

std::string cmd_persist(Store& store, const Command& cmd, Command& log) {
    const bool changed = store.persist(cmd[1]);
    if (changed) {
        log = cmd;
    }
    return resp::integer(changed ? 1 : 0);
}

std::string incr_reply(const IncrResult& r) {
    switch (r.status) {
        case IncrResult::Status::kOk:
            return resp::integer(r.value);
        case IncrResult::Status::kNotInteger:
            return resp::error(kNotInteger);
        case IncrResult::Status::kOverflow:
            return resp::error("increment or decrement would overflow");
    }
    return resp::error("internal error");
}

std::string cmd_incr(Store& store, const Command& cmd, Command& log) {
    const IncrResult r = store.incr_by(cmd[1], 1);
    if (r.status == IncrResult::Status::kOk) {
        log = cmd;
    }
    return incr_reply(r);
}

std::string cmd_decr(Store& store, const Command& cmd, Command& log) {
    const IncrResult r = store.incr_by(cmd[1], -1);
    if (r.status == IncrResult::Status::kOk) {
        log = cmd;
    }
    return incr_reply(r);
}

// The "route table": command name -> handler + arity.
const std::unordered_map<std::string_view, CommandSpec>& command_table() {
    static const std::unordered_map<std::string_view, CommandSpec> table = {
        {"PING", {cmd_ping, 1}},
        {"SET", {cmd_set, 3}},
        {"GET", {cmd_get, 2}},
        {"DEL", {cmd_del, -2}},
        {"EXISTS", {cmd_exists, -2}},
        {"FLUSHALL", {cmd_flushall, 1}},
        {"DBSIZE", {cmd_dbsize, 1}},
        {"EXPIRE", {cmd_expire, 3}},
        {"PEXPIRE", {cmd_pexpire, 3}},
        {"PEXPIREAT", {cmd_pexpireat, 3}},
        {"TTL", {cmd_ttl, 2}},
        {"PTTL", {cmd_pttl, 2}},
        {"PERSIST", {cmd_persist, 2}},
        {"INCR", {cmd_incr, 2}},
        {"DECR", {cmd_decr, 2}},
    };
    return table;
}

}  // namespace

std::string execute(Store& store, const Command& command, Command* propagate, bool read_only) {
    if (propagate != nullptr) {
        propagate->clear();
    }
    if (command.empty()) {
        return resp::error("empty command");
    }

    const std::string name = to_upper(command[0]);
    const auto& table = command_table();
    const auto it = table.find(std::string_view(name));
    if (it == table.end()) {
        return resp::error("unknown command '" + command[0] + "'");
    }

    const CommandSpec& spec = it->second;
    if (!arity_ok(spec.arity, command.size())) {
        return resp::error("wrong number of arguments for '" + name + "' command");
    }
    if (read_only && is_write(name)) {
        return resp::error("You can't write against a read only replica.");
    }

    Command log;
    std::string reply = spec.handler(store, command, log);
    if (propagate != nullptr) {
        *propagate = std::move(log);
    }
    return reply;
}

}  // namespace emberdb
