# Consolidated Book

A real-time market-data aggregation service that builds a consolidated order book and candle stream from multiple exchanges.

Subscribe to the timeframes and exchanges you need, and receive unified market data through a single publisher interface. The service aggregates incoming exchange data into consolidated order books and OHLCV candles, then distributes the normalized results to downstream subscribers.

## Features

- Multi-exchange market-data aggregation
- Configurable candle timeframes
- Consolidated order book generation
- Real-time publishing for subscribers
- Unified, normalized data interface across exchanges

<!-- build all -->
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure

<!-- libs + tests -->
cmake -S . -B build -DBUILD_SERVER=OFF -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure


<!-- server -->
cmake -S . -B build -DBUILD_TESTS=OFF -DBUILD_SERVER=ON
cmake --build build -j


cmake --preset vcpkg-arm64
cmake --preset vcpkg-arm64-debug
cmake --build build -j --target candle_manager

cmake --build --preset vcpkg-arm64-debug --target unit_tests
