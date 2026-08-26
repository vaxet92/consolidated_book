# md_provider/CMakeLists.txt

cmake_policy(SET CMP0167 NEW)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(simdjson CONFIG REQUIRED)

add_library(md_provider STATIC
    md_provider.cpp
    ws/ws.cpp
    binance/binance_provider.cpp
    binance/binance_parser.cpp
    bybit/bybit_provider.cpp
    bybit/bybit_parser.cpp
    okx/okx_provider.cpp
    okx/okx_parser.cpp
)

target_include_directories(md_provider PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ws
)

target_link_libraries(md_provider PUBLIC
    types
    md_core
    logger
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    simdjson::simdjson
)
