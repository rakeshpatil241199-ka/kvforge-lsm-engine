cmake_minimum_required(VERSION 3.14)
project(kvforge_lsm_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KVFORGE_BUILD_TESTS "Build GoogleTest unit and integration suites" ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(kvforge_core STATIC
    src/crc32.cpp
    src/wal.cpp
    src/memtable.cpp
    src/bloom_filter.cpp
    src/sstable.cpp
    src/compactor.cpp
    src/db.cpp
)

target_include_directories(kvforge_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

add_executable(kvforge_cli src/main.cpp)
target_link_libraries(kvforge_cli PRIVATE kvforge_core)

if(KVFORGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
