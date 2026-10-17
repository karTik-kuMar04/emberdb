#include "emberdb/dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>

#include "emberdb/int_parse.hpp"
#include "emberdb/resp_writer.hpp"

namespace emberdb {
namespace {

using Handler = std::string (*)(Store&, const Command&);

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

// --- Handlers: arity is already validated, so indexing is safe ---

std::string cmd_ping(Store&, const Command&) {
    return resp::simple_string("PONG");
}

std::string cmd_set(Store& store, const Command& cmd) {
    store.set(cmd[1], cmd[2]);
    return resp::simple_string("OK");
}

std::string cmd_get(Store& store, const Command& cmd) {
    auto value = store.get(cmd[1]);
    if (!value) {
        return resp::null_bulk();
    }
    return resp::bulk_string(*value);
}

std::string cmd_del(Store& store, const Command& cmd) {
    long long removed = 0;
    for (std::size_t i = 1; i < cmd.size(); ++i) {
        if (store.del(cmd[i])) {
            ++removed;
        }
    }
    return resp::integer(removed);
}

std::string cmd_exists(Store& store, const Command& cmd) {
    long long found = 0;
    for (std::size_t i = 1; i < cmd.size(); ++i) {
        if (store.exists(cmd[i])) {
            ++found;
        }
    }
    return resp::integer(found);
}

// Shared by EXPIRE (unit = 1000 ms) and PEXPIRE (unit = 1 ms).
std::string expire_with_unit(Store& store, const Command& cmd, long long unit_ms,
                             const char* name) {
    const auto amount = parse_int64(cmd[2]);
    if (!amount) {
        return resp::error(kNotInteger);
    }
    long long ms = 0;
    if (__builtin_mul_overflow(*amount, unit_ms, &ms)) {
        return resp::error(std::string("invalid expire time in '") + name + "' command");
    }
    return resp::integer(store.expire_in(cmd[1], ms) ? 1 : 0);
}

std::string cmd_expire(Store& store, const Command& cmd) {
    return expire_with_unit(store, cmd, 1000, "expire");
}

std::string cmd_pexpire(Store& store, const Command& cmd) {
    return expire_with_unit(store, cmd, 1, "pexpire");
}

std::string cmd_ttl(Store& store, const Command& cmd) {
    const long long ms = store.ttl_ms(cmd[1]);
    if (ms < 0) {
        return resp::integer(ms);  // -2: no such key, -1: no deadline
    }
    return resp::integer((ms + 500) / 1000);  // round to the nearest second
}

std::string cmd_pttl(Store& store, const Command& cmd) {
    return resp::integer(store.ttl_ms(cmd[1]));
}

std::string cmd_persist(Store& store, const Command& cmd) {
    return resp::integer(store.persist(cmd[1]) ? 1 : 0);
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

std::string cmd_incr(Store& store, const Command& cmd) {
    return incr_reply(store.incr_by(cmd[1], 1));
}

std::string cmd_decr(Store& store, const Command& cmd) {
    return incr_reply(store.incr_by(cmd[1], -1));
}

// The "route table": command name -> handler + arity.
const std::unordered_map<std::string_view, CommandSpec>& command_table() {
    static const std::unordered_map<std::string_view, CommandSpec> table = {
        {"PING", {cmd_ping, 1}},
        {"SET", {cmd_set, 3}},
        {"GET", {cmd_get, 2}},
        {"DEL", {cmd_del, -2}},
        {"EXISTS", {cmd_exists, -2}},
        {"EXPIRE", {cmd_expire, 3}},
        {"PEXPIRE", {cmd_pexpire, 3}},
        {"TTL", {cmd_ttl, 2}},
        {"PTTL", {cmd_pttl, 2}},
        {"PERSIST", {cmd_persist, 2}},
        {"INCR", {cmd_incr, 2}},
        {"DECR", {cmd_decr, 2}},
    };
    return table;
}

}  // namespace

std::string execute(Store& store, const Command& command) {
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
    return spec.handler(store, command);
}

}  // namespace emberdb
