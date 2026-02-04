cmake_minimum_required(VERSION 3.20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg integration (only if not already set)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(consolidated_book CXX)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_SERVER "Build server app" ON)

add_subdirectory(types)
add_subdirectory(utils)
add_subdirectory(candle_manager)

if (BUILD_SERVER)
  add_subdirectory(server)
endif()

if (BUILD_TESTS)
  enable_testing()
  add_subdirectory(unit_tests)
endif()
