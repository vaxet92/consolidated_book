# server/CMakeLists.txt
find_package(FastFloat CONFIG REQUIRED)

add_executable(server_app
    main.cpp
    # другие cpp
)

target_link_libraries(server_app PRIVATE
    candle_manager
    FastFloat::fast_float
    # md_provider и т.д.
)
