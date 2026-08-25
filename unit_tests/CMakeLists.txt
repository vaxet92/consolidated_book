# unit_tests/CMakeLists.txt
find_package(GTest REQUIRED)

add_executable(unit_tests
    src/test_venue_book.cpp
)

target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    md_core
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
