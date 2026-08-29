# client/CMakeLists.txt

add_executable(client_app bbo_sub.cpp
                # price_band_sub price_band_sub.cpp,
                # volume_band_sub volume_band_sub.cpp
)

target_include_directories(client_app PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(client_app PRIVATE
    md_proto
    logger
)