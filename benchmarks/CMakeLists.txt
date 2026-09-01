# benchmarks/CMakeLists.txt
#
# Latency micro-benchmarks. Not part of the normal build - enable with
# -DBUILD_BENCHMARKS=ON. No test framework: each benchmark is a plain
# executable that prints numbers.

add_executable(bench_binance_parser
    bench_binance_parser.cpp
    latency_benchmark.h
)

# The source includes "md_provider/binance/binance_parser.h", so the project
# root must be on the include path (same style as the unit tests).
target_include_directories(bench_binance_parser PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(bench_binance_parser PRIVATE
    md_provider
)
