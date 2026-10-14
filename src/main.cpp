#include <iostream>

#include "emberdb/store.hpp"

int main() {
    emberdb::Store store;
    store.set("name", "ember");
    if (auto value = store.get("name")) {
        std::cout << "name = " << *value << "\n";
    }
    return 0;
}
