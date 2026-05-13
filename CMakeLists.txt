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

add_compile_options(
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
)

# ─── ensure test data directory exists in build/ ──────────────────────────────
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/storage")

# ─── core library ─────────────────────────────────────────────────────────────

add_library(persistx_lib
    src/page.cpp
    src/disk_manager.cpp
    src/replacer.cpp
    src/buffer_manager.cpp
    src/log_manager.cpp
    src/transaction_manager.cpp     # Phase 6
    src/checkpoint_manager.cpp      # Phase 8
    src/recovery_manager.cpp        # Phase 9
    src/btree_page.cpp
    src/btree_index.cpp
)

target_include_directories(persistx_lib PUBLIC include)

# Link pthreads — required for std::mutex on Linux.
find_package(Threads REQUIRED)
target_link_libraries(persistx_lib PUBLIC Threads::Threads)

# ─── existing test executables ────────────────────────────────────────────────

add_executable(test_page tests/test_page.cpp)
target_link_libraries(test_page PRIVATE persistx_lib)

add_executable(test_disk_manager tests/test_disk_manager.cpp)
target_link_libraries(test_disk_manager PRIVATE persistx_lib)

add_executable(test_buffer_manager tests/test_buffer_manager.cpp)
target_link_libraries(test_buffer_manager PRIVATE persistx_lib)

# ─── Phase 7: new test executables ───────────────────────────────────────────

add_executable(test_wal tests/test_wal.cpp)
target_link_libraries(test_wal PRIVATE persistx_lib)

add_executable(test_transaction tests/test_transaction.cpp)
target_link_libraries(test_transaction PRIVATE persistx_lib)

# ─── Phase 8: checkpoint test ────────────────────────────────────────────────

add_executable(test_checkpoint tests/test_checkpoint.cpp)
target_link_libraries(test_checkpoint PRIVATE persistx_lib)

# ─── Phase 9: recovery test ─────────────────────────────────────────────────

add_executable(test_recovery tests/test_recovery.cpp)
target_link_libraries(test_recovery PRIVATE persistx_lib)


# btrees
add_executable(test_btree tests/test_btree.cpp)
target_link_libraries(test_btree PRIVATE persistx_lib)

add_executable(overall_tests tests/overall_tests.cpp)
target_link_libraries(overall_tests PRIVATE persistx_lib)