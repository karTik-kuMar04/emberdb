cmake_minimum_required(VERSION 3.21)
project(emberdb VERSION 0.12.0 LANGUAGES CXX)

option(EMBERDB_BUILD_TESTS "Build the unit and integration tests (downloads GoogleTest)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(emberdb_lib
    src/store.cpp
    src/resp_parser.cpp
    src/resp_writer.cpp
    src/dispatcher.cpp
    src/snapshot.cpp
    src/security.cpp
    src/config.cpp
    src/metrics.cpp
    src/http.cpp
)
target_include_directories(emberdb_lib PUBLIC include)
target_compile_options(emberdb_lib PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(emberdb_lib PRIVATE src/server.cpp src/aof.cpp)
    add_executable(emberdb src/main.cpp)
    target_link_libraries(emberdb PRIVATE emberdb_lib)
    target_compile_options(emberdb PRIVATE -Wall -Wextra -Wpedantic)
else()
    message(STATUS "Server needs epoll (Linux): building core library and tests only")
endif()

if(EMBERDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
