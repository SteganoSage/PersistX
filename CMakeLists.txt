cmake_minimum_required(VERSION 3.20)
project(PersistX
    VERSION     0.1.0
    LANGUAGES   CXX
    DESCRIPTION "A production-grade educational storage engine"
)

# ─── compiler settings ────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings — treat warnings as errors in CI.
add_compile_options(
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
)

# ─── core library ─────────────────────────────────────────────────────────────

add_library(persistx_lib
    src/page.cpp
    src/disk_manager.cpp
    src/replacer.cpp
    src/buffer_manager.cpp
    src/log_manager.cpp
)

target_include_directories(persistx_lib PUBLIC include)

# ─── test executables ─────────────────────────────────────────────────────────

add_executable(test_page tests/test_page.cpp)
target_link_libraries(test_page PRIVATE persistx_lib)

add_executable(test_disk_manager tests/test_disk_manager.cpp)
target_link_libraries(test_disk_manager PRIVATE persistx_lib)

add_executable(test_buffer_manager tests/test_buffer_manager.cpp)
target_link_libraries(test_buffer_manager PRIVATE persistx_lib)
