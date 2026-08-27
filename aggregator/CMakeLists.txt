# aggregator/CMakeLists.txt
add_library(aggregator STATIC
    wire_translation.cpp
    aggregator_service.cpp
)

target_include_directories(aggregator PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(aggregator PUBLIC
    md_core
    md_proto
)

add_executable(aggregator_app
    main.cpp
)

target_link_libraries(aggregator_app PRIVATE
    aggregator
    md_provider
    logger
)
