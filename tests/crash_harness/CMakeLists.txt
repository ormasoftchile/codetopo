cmake_minimum_required(VERSION 3.20)

project(ts_crash_harness LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(VCPKG_INSTALLED_DIR
    "${REPO_ROOT}/build/vcpkg_installed/arm64-osx"
    CACHE PATH "Path to the prebuilt vcpkg installed tree")

set(TREE_SITTER_INCLUDE_DIR "${VCPKG_INSTALLED_DIR}/include")
set(TREE_SITTER_LIBRARY "${VCPKG_INSTALLED_DIR}/lib/libtree-sitter.a")
set(TS_GRAMMAR_ROOT "${REPO_ROOT}/build/ts_grammars")
set(TS_TYPESCRIPT_SRC_DIR "${TS_GRAMMAR_ROOT}/typescript/typescript/src")
set(TS_V23_INCLUDE_DIR "${TS_GRAMMAR_ROOT}/ts_v23_include")

if(NOT EXISTS "${TREE_SITTER_INCLUDE_DIR}/tree_sitter/api.h")
    message(FATAL_ERROR "tree-sitter headers not found at ${TREE_SITTER_INCLUDE_DIR}")
endif()

if(NOT EXISTS "${TREE_SITTER_LIBRARY}")
    message(FATAL_ERROR "tree-sitter library not found at ${TREE_SITTER_LIBRARY}")
endif()

if(NOT EXISTS "${TS_TYPESCRIPT_SRC_DIR}/parser.c")
    message(FATAL_ERROR "TypeScript parser.c not found at ${TS_TYPESCRIPT_SRC_DIR}/parser.c")
endif()

if(NOT EXISTS "${TS_TYPESCRIPT_SRC_DIR}/scanner.c")
    message(FATAL_ERROR "TypeScript scanner.c not found at ${TS_TYPESCRIPT_SRC_DIR}/scanner.c")
endif()

add_executable(ts_crash_harness
    ts_crash_harness.c
    "${TS_TYPESCRIPT_SRC_DIR}/parser.c"
    "${TS_TYPESCRIPT_SRC_DIR}/scanner.c"
)

target_include_directories(ts_crash_harness PRIVATE
    "${TREE_SITTER_INCLUDE_DIR}"
    "${TS_V23_INCLUDE_DIR}"
    "${TS_GRAMMAR_ROOT}"
)

target_link_libraries(ts_crash_harness PRIVATE "${TREE_SITTER_LIBRARY}")
