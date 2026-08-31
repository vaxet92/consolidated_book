# unit_tests/CMakeLists.txt
find_package(GTest REQUIRED)

add_executable(unit_tests
    src/test_venue_book.cpp
    src/test_consolidated_bbo.cpp
    src/test_binance_parser.cpp
    src/test_bybit_parser.cpp
    src/test_okx_parser.cpp
    src/test_conflated_channel.cpp
    src/test_wire_translation.cpp
    src/test_continuity.cpp
    src/test_seq_dedup.cpp
    src/test_consolidated_book.cpp
    src/test_venue_health.cpp
)

target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    aggregator
    md_provider
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
