
# emberdb

Redis-compatible in-memory database written in C++20.

## Status
- [x] Project scaffold, CMake presets, CI
- [x] In-memory key-value store
- [ ] RESP protocol parser
- [ ] TCP server (epoll)
- [ ] TTL / expiry, LRU eviction
- [ ] AOF + snapshot persistence
- [ ] Replication

## Build
cmake --preset debug
cmake --build --preset debug
ctest --preset debug