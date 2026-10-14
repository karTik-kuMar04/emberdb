include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

add_executable(store_test store_test.cpp)
target_link_libraries(store_test PRIVATE emberdb_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(store_test)


add_executable(resp_parser_test resp_parser_test.cpp)
target_link_libraries(resp_parser_test PRIVATE emberdb_lib GTest::gtest_main)
gtest_discover_tests(resp_parser_test)