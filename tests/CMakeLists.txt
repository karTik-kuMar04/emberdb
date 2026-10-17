include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(store_test store_test.cpp)
target_link_libraries(store_test PRIVATE emberdb_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(store_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(server_test server_test.cpp)
    target_link_libraries(server_test PRIVATE emberdb_lib Threads::Threads GTest::gtest_main)
    gtest_discover_tests(server_test)
endif()


add_executable(resp_parser_test resp_parser_test.cpp)
target_link_libraries(resp_parser_test PRIVATE emberdb_lib GTest::gtest_main)
gtest_discover_tests(resp_parser_test)

add_executable(dispatcher_test dispatcher_test.cpp)
target_link_libraries(dispatcher_test PRIVATE emberdb_lib GTest::gtest_main)
gtest_discover_tests(dispatcher_test)