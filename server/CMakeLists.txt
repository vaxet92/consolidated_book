# server/CMakeLists.txt
add_executable(server_app
    main.cpp
    config/config.cpp
)

target_link_libraries(server_app PRIVATE
    md_core
    logger
)
