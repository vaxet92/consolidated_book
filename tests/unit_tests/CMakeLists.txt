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
    src/test_venue_registry.cpp
    src/test_instrument_registry.cpp
    src/test_core_venue_lifecycle.cpp
    src/test_spsc_queue.cpp
    src/test_provider_message.cpp
    src/test_core_queued_path.cpp
    src/test_config_json.cpp
    src/test_aggregator_routing.cpp
)

target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    aggregator
    md_provider
    config
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
