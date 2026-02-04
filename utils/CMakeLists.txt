# types/CMakeLists.txt
find_package(FastFloat CONFIG REQUIRED)

add_library(utilities INTERFACE)

target_include_directories(utilities INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(utilities INTERFACE
    FastFloat::fast_float
)