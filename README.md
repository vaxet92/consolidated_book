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
