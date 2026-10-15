#include "emberdb/dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>
#include <unordered_map>

#include "emberdb/resp_parser.hpp"
#include "emberdb/resp_writer.hpp"

namespace emberdb {
namespace {

using Handler = std::string (*)(Store&, const Command&);

struct CommandSpec {
    Handler handler;
    int arity;  // >= 0: exactly that many tokens (name included). < 0: at least -arity.
};

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}
bool arity_ok(int arity, std::size_t argc) {
    if (arity >=0) {
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
    auto val = store.get(cmd[1]);
    if(!val) {
        return resp::null_bulk();
    }
    return resp::bulk_string(*val);
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

// The "route table": command name -> handler + arity.
const std::unordered_map<std::string_view, CommandSpec>& command_table() {
    static const std::unordered_map<std::string_view, CommandSpec> table = {
        {"PING", {cmd_ping, 1}},
        {"SET", {cmd_set, 3}},
        {"GET", {cmd_get, 2}},
        {"DEL", {cmd_del, -2}},
    };
    return table;
}

}  // namespace

std::string execute (Store& store, const Command& command) {
    if (command.empty()) {
        return resp::error("empty command");
    }

    const std::string name = to_upper(command[0]);
    const auto& table = command_table();
    const auto it = table.find(std::string_view (name));
    if (it == table.end()) {
        return resp::error("unknown command '" + command[0] + "'");
    }

    const CommandSpec& spec = it->second;

    if (!arity_ok(spec.arity, command.size())) {
        return resp::error("wrong number of arguments for '" + name + "' command");
    }
    return spec.handler(store, command);
}
}// namespace emberdb