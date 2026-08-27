# client/CMakeLists.txt

add_executable(client_app
    main.cpp
)

target_include_directories(client_app PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(client_app PRIVATE
    md_proto
    logger
)