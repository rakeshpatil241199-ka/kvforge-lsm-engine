include(FetchContent)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/release-1.10.0.tar.gz
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(kvforge_tests
    test_bloom_wal.cpp
    test_db_compaction.cpp
)

target_link_libraries(kvforge_tests PRIVATE kvforge_core gtest_main)

include(GoogleTest)
gtest_discover_tests(kvforge_tests)
