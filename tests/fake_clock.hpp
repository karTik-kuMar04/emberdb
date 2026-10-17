#pragma once

#include <cstdint>

#include "emberdb/store.hpp"

// A clock the test controls: time only moves when the test says so.
struct FakeClock {
    std::int64_t now = 1000;
    emberdb::Clock fn() {
        return [this] { return now; };
    }
};
