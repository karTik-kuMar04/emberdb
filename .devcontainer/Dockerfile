FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git gdb valgrind \
    clang clang-format clang-tidy clangd \
    redis-tools ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
