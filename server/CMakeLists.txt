# server/CMakeLists.txt
add_executable(server_app
    main.cpp
)

target_link_libraries(server_app PRIVATE
    md_core
    logger
)
