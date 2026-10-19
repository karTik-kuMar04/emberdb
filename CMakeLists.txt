cmake_minimum_required(VERSION 3.21)
project(emberdb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Library: all real logic lives here so tests can link against it ---
add_library(emberdb_lib
    src/store.cpp
    src/resp_parser.cpp
    src/resp_writer.cpp
    src/dispatcher.cpp
)
target_include_directories(emberdb_lib PUBLIC include)
target_compile_options(emberdb_lib PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

# --- Server + executable: epoll is Linux-only ---
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(emberdb_lib PRIVATE src/server.cpp src/aof.cpp)
    add_executable(emberdb src/main.cpp)
    target_link_libraries(emberdb PRIVATE emberdb_lib)
    target_compile_options(emberdb PRIVATE -Wall -Wextra -Wpedantic)
else()
    message(STATUS "Server needs epoll (Linux): building core library and tests only")
endif()

# --- Tests ---
enable_testing()
add_subdirectory(tests)
