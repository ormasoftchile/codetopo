cmake_minimum_required(VERSION 3.20)

# --- Auto-bootstrap vcpkg when no toolchain file is provided ---
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    else()
        set(_VCPKG_DIR "${CMAKE_SOURCE_DIR}/vcpkg")
        if(NOT EXISTS "${_VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
            message(STATUS "vcpkg not found — cloning into ${_VCPKG_DIR}...")
            find_package(Git REQUIRED)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none
                    https://github.com/microsoft/vcpkg.git "${_VCPKG_DIR}"
                RESULT_VARIABLE _vcpkg_clone_result
            )
            if(NOT _vcpkg_clone_result EQUAL 0)
                message(FATAL_ERROR "Failed to clone vcpkg. Check your network connection.")
            endif()
            message(STATUS "Bootstrapping vcpkg...")
            if(WIN32)
                set(_VCPKG_BOOTSTRAP "${_VCPKG_DIR}/bootstrap-vcpkg.bat")
            else()
                set(_VCPKG_BOOTSTRAP "${_VCPKG_DIR}/bootstrap-vcpkg.sh")
            endif()
            execute_process(
                COMMAND "${_VCPKG_BOOTSTRAP}" -disableMetrics
                WORKING_DIRECTORY "${_VCPKG_DIR}"
                RESULT_VARIABLE _vcpkg_bs_result
            )
            if(NOT _vcpkg_bs_result EQUAL 0)
                message(FATAL_ERROR "Failed to bootstrap vcpkg.")
            endif()
        endif()
        set(CMAKE_TOOLCHAIN_FILE "${_VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    endif()
endif()

project(codetopo VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable async exception handling on MSVC for SEH-to-C++ translation
if(MSVC)
    add_compile_options(/EHa)
endif()

# --- Dependencies via vcpkg / find_package ---
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)

# --- Tree-sitter grammars ---
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(TreeSitterGrammars)

ts_grammars_init()

add_ts_grammar(NAME c       REPO tree-sitter/tree-sitter-c       TAG v0.21.0)
add_ts_grammar(NAME cpp     REPO tree-sitter/tree-sitter-cpp     TAG v0.21.0 SCANNER)
add_ts_grammar(NAME csharp  REPO tree-sitter/tree-sitter-c-sharp TAG v0.23.1 SCANNER V23)
add_ts_grammar(NAME go      REPO tree-sitter/tree-sitter-go      TAG v0.21.0)
add_ts_grammar(NAME yaml    REPO tree-sitter-grammars/tree-sitter-yaml TAG v0.6.1 SCANNER C11
    SCANNER_DEPS schema.core.c schema.json.c
    EXTRA_INCLUDE_DIRS "${TS_GRAMMAR_DIR}/yaml")
add_ts_grammar(NAME typescript REPO tree-sitter/tree-sitter-typescript TAG v0.23.2 SCANNER V23
    SRC_PATH typescript/src
    EXTRA_DOWNLOADS common/scanner.h
    EXTRA_INCLUDE_DIRS "${TS_GRAMMAR_DIR}/typescript/typescript/src")
add_ts_grammar(NAME javascript REPO tree-sitter/tree-sitter-javascript TAG v0.23.1 SCANNER V23)
add_ts_grammar(NAME python  REPO tree-sitter/tree-sitter-python   TAG v0.23.6 SCANNER V23)
add_ts_grammar(NAME rust    REPO tree-sitter/tree-sitter-rust     TAG v0.23.2 SCANNER V23)
add_ts_grammar(NAME java    REPO tree-sitter/tree-sitter-java     TAG v0.23.4 V23)
add_ts_grammar(NAME bash    REPO tree-sitter/tree-sitter-bash     TAG v0.23.3 SCANNER V23)

# --- Main executable ---
set(CODETOPO_SOURCES
    src/main.cpp
    src/core/arena.cpp
    src/core/config.cpp
    src/cli/cmd_index.cpp
    src/index/extractor.cpp
    src/index/supervisor.cpp
    src/mcp/tools.cpp
    src/util/process.cpp
)

add_executable(codetopo ${CODETOPO_SOURCES})

target_include_directories(codetopo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(codetopo PRIVATE
    unofficial::sqlite3::sqlite3
    CLI11::CLI11
    yyjson::yyjson
    ftxui::screen
    ftxui::dom
    ftxui::component
    unofficial::tree-sitter::tree-sitter
    ${TS_GRAMMAR_TARGETS}
    xxHash::xxhash
)

# Platform-specific linking
if(WIN32)
    # ReadDirectoryChangesW is in kernel32 (linked by default)
elseif(APPLE)
    target_link_libraries(codetopo PRIVATE "-framework CoreServices")
endif()

# --- Test executable ---
set(CODETOPO_TEST_SOURCES
    tests/contract/test_error_envelope.cpp
    tests/contract/test_mcp_schemas.cpp
    tests/integration/test_crash_recovery.cpp
    tests/integration/test_doctor.cpp
    tests/integration/test_index_real_repo.cpp
    tests/integration/test_mcp_enhancements.cpp
    tests/integration/test_mcp_round_trip.cpp
    tests/integration/test_watch.cpp
    tests/unit/test_change_detect.cpp
    tests/unit/test_cli_exit_codes.cpp
    tests/unit/test_freshness.cpp
    tests/unit/test_fts_search.cpp
    tests/unit/test_gitignore.cpp
    tests/unit/test_lock.cpp
    tests/unit/test_path_normalize.cpp
    tests/unit/test_placeholder.cpp
    tests/unit/test_process.cpp
    tests/unit/test_resource_limits.cpp
    tests/unit/test_stable_key.cpp
    tests/unit/test_parse_timeout.cpp
    tests/unit/test_extraction_timeout.cpp
    tests/unit/test_watchdog.cpp
    tests/unit/test_supervisor_recovery.cpp
    tests/unit/test_persist_pipeline.cpp
    tests/unit/test_turbo_mode.cpp
    tests/unit/test_file_summary_nodeid.cpp
    tests/unit/test_source_at.cpp
    tests/unit/test_csharp_extractor.cpp
)

# Source files (excluding main.cpp) needed by tests
set(CODETOPO_LIB_SOURCES
    src/core/arena.cpp
    src/core/config.cpp
    src/cli/cmd_index.cpp
    src/index/extractor.cpp
    src/index/supervisor.cpp
    src/mcp/tools.cpp
    src/util/process.cpp
)

add_executable(codetopo_tests ${CODETOPO_TEST_SOURCES} ${CODETOPO_LIB_SOURCES})

target_include_directories(codetopo_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(codetopo_tests PRIVATE
    Catch2::Catch2WithMain
    unofficial::sqlite3::sqlite3
    CLI11::CLI11
    yyjson::yyjson
    ftxui::screen
    ftxui::dom
    ftxui::component
    unofficial::tree-sitter::tree-sitter
    ${TS_GRAMMAR_TARGETS}
    xxHash::xxhash
)

if(APPLE)
    target_link_libraries(codetopo_tests PRIVATE "-framework CoreServices")
endif()

include(CTest)
include(Catch)
catch_discover_tests(codetopo_tests)
