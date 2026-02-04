# unit_tests/CMakeLists.txt
find_package(GTest REQUIRED)

add_executable(unit_tests
    src/test_binance_trade_parser.cpp
    src/test_candle_update.cpp
    src/test_bybit_trade_parser.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    candle_manager
    types
    utilities
)

include(GoogleTest)
gtest_discover_tests(unit_tests)