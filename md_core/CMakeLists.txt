# md_core/CMakeLists.txt
add_library(md_core STATIC
    md_core.cpp
    venue_book.cpp
    consolidated_bbo.cpp
)

target_include_directories(md_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(md_core PUBLIC
    types
    logger
)