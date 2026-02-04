# candle_manager/CMakeLists.txt

find_package(simdjson CONFIG REQUIRED)


add_library(candle_manager
    src/candle_manager.cpp
)

target_include_directories(candle_manager
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(candle_manager PUBLIC
    types
    utilities
    simdjson::simdjson
)
