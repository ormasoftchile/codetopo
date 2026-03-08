cmake_minimum_required(VERSION 3.20)
project(codetopo VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/EHsc)
endif()

# --- Dependencies via vcpkg / find_package ---
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)

# --- Tree-sitter grammars via FetchContent ---
include(FetchContent)

# --- Tree-sitter grammars: download source files directly ---
set(TS_GRAMMAR_DIR "${CMAKE_BINARY_DIR}/ts_grammars")
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/c" "${TS_GRAMMAR_DIR}/cpp" "${TS_GRAMMAR_DIR}/tree_sitter")

# Download the private parser.h header from within a grammar repo (v0.21.0 — used by C, C++, Go, YAML)
if(NOT EXISTS "${TS_GRAMMAR_DIR}/tree_sitter/parser.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-c/refs/tags/v0.21.0/src/tree_sitter/parser.h"
        "${TS_GRAMMAR_DIR}/tree_sitter/parser.h"
        STATUS _dl_status
    )
endif()

# Download alloc.h and array.h (needed by newer grammar scanners)
if(NOT EXISTS "${TS_GRAMMAR_DIR}/tree_sitter/alloc.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter/v0.25.10/cli/generate/src/templates/alloc.h"
        "${TS_GRAMMAR_DIR}/tree_sitter/alloc.h"
        STATUS _dl_status
    )
endif()
if(NOT EXISTS "${TS_GRAMMAR_DIR}/tree_sitter/array.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter/v0.25.10/cli/generate/src/templates/array.h"
        "${TS_GRAMMAR_DIR}/tree_sitter/array.h"
        STATUS _dl_status
    )
endif()

# Download v0.23.0 parser.h for C# grammar (has TSCharacterRange, TSFieldMapSlice)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/tree_sitter_v23")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/tree_sitter_v23/parser.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter/refs/tags/v0.23.0/lib/src/parser.h"
        "${TS_GRAMMAR_DIR}/tree_sitter_v23/parser.h"
        STATUS _dl_status
    )
endif()

# Download C grammar parser.c (v0.21.0)
if(NOT EXISTS "${TS_GRAMMAR_DIR}/c/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-c/refs/tags/v0.21.0/src/parser.c"
        "${TS_GRAMMAR_DIR}/c/parser.c"
        STATUS _dl_status
    )
endif()

# Download C++ grammar parser.c and scanner.c (v0.21.0)
if(NOT EXISTS "${TS_GRAMMAR_DIR}/cpp/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-cpp/refs/tags/v0.21.0/src/parser.c"
        "${TS_GRAMMAR_DIR}/cpp/parser.c"
        STATUS _dl_status
    )
    # Note: scanner.c must be obtained from the v0.21.0 archive if raw download fails
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-cpp/refs/tags/v0.21.0/src/scanner.c"
        "${TS_GRAMMAR_DIR}/cpp/scanner.c"
        STATUS _dl_status
    )
endif()

# Download C# grammar parser.c and scanner.c (v0.23.1)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/csharp" "${TS_GRAMMAR_DIR}/csharp_headers/tree_sitter")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/csharp/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-c-sharp/refs/tags/v0.23.1/src/parser.c"
        "${TS_GRAMMAR_DIR}/csharp/parser.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-c-sharp/refs/tags/v0.23.1/src/scanner.c"
        "${TS_GRAMMAR_DIR}/csharp/scanner.c"
        STATUS _dl_status
    )
endif()
# Copy v0.23 parser.h into csharp_headers so C# can resolve it
if(NOT EXISTS "${TS_GRAMMAR_DIR}/csharp_headers/tree_sitter/parser.h")
    file(COPY_FILE
        "${TS_GRAMMAR_DIR}/tree_sitter_v23/parser.h"
        "${TS_GRAMMAR_DIR}/csharp_headers/tree_sitter/parser.h"
    )
endif()

# Download Go grammar parser.c (v0.21.0)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/go")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/go/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-go/refs/tags/v0.21.0/src/parser.c"
        "${TS_GRAMMAR_DIR}/go/parser.c"
        STATUS _dl_status
    )
endif()

# Download YAML grammar parser.c, scanner.c, and schema files (v0.6.1)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/yaml")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/yaml/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter-grammars/tree-sitter-yaml/refs/tags/v0.6.1/src/parser.c"
        "${TS_GRAMMAR_DIR}/yaml/parser.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter-grammars/tree-sitter-yaml/refs/tags/v0.6.1/src/scanner.c"
        "${TS_GRAMMAR_DIR}/yaml/scanner.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter-grammars/tree-sitter-yaml/refs/tags/v0.6.1/src/schema.core.c"
        "${TS_GRAMMAR_DIR}/yaml/schema.core.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter-grammars/tree-sitter-yaml/refs/tags/v0.6.1/src/schema.json.c"
        "${TS_GRAMMAR_DIR}/yaml/schema.json.c"
        STATUS _dl_status
    )
endif()

# Download TypeScript grammar (v0.23.2 — nested under typescript/ in the repo)
# The scanner.c references ../../common/scanner.h, so we replicate the repo layout:
#   ts_grammars/ts_typescript/typescript/src/{parser.c,scanner.c}
#   ts_grammars/ts_typescript/common/scanner.h
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/ts_typescript/typescript/src" "${TS_GRAMMAR_DIR}/ts_typescript/common")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/ts_typescript/typescript/src/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-typescript/refs/tags/v0.23.2/typescript/src/parser.c"
        "${TS_GRAMMAR_DIR}/ts_typescript/typescript/src/parser.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-typescript/refs/tags/v0.23.2/typescript/src/scanner.c"
        "${TS_GRAMMAR_DIR}/ts_typescript/typescript/src/scanner.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-typescript/refs/tags/v0.23.2/common/scanner.h"
        "${TS_GRAMMAR_DIR}/ts_typescript/common/scanner.h"
        STATUS _dl_status
    )
endif()

# Download JavaScript grammar (v0.23.1)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/javascript")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/javascript/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-javascript/refs/tags/v0.23.1/src/parser.c"
        "${TS_GRAMMAR_DIR}/javascript/parser.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-javascript/refs/tags/v0.23.1/src/scanner.c"
        "${TS_GRAMMAR_DIR}/javascript/scanner.c"
        STATUS _dl_status
    )
endif()

# Download Python grammar (v0.23.6)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/python")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/python/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-python/refs/tags/v0.23.6/src/parser.c"
        "${TS_GRAMMAR_DIR}/python/parser.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-python/refs/tags/v0.23.6/src/scanner.c"
        "${TS_GRAMMAR_DIR}/python/scanner.c"
        STATUS _dl_status
    )
endif()

# Download Rust grammar (v0.23.2)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/rust")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/rust/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-rust/refs/tags/v0.23.2/src/parser.c"
        "${TS_GRAMMAR_DIR}/rust/parser.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-rust/refs/tags/v0.23.2/src/scanner.c"
        "${TS_GRAMMAR_DIR}/rust/scanner.c"
        STATUS _dl_status
    )
endif()

# Download Java grammar (v0.23.4 — no scanner)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/java")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/java/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-java/refs/tags/v0.23.4/src/parser.c"
        "${TS_GRAMMAR_DIR}/java/parser.c"
        STATUS _dl_status
    )
endif()

# Download Bash grammar (v0.23.3)
file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/bash")
if(NOT EXISTS "${TS_GRAMMAR_DIR}/bash/parser.c")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-bash/refs/tags/v0.23.3/src/parser.c"
        "${TS_GRAMMAR_DIR}/bash/parser.c"
        STATUS _dl_status
    )
    file(DOWNLOAD
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-bash/refs/tags/v0.23.3/src/scanner.c"
        "${TS_GRAMMAR_DIR}/bash/scanner.c"
        STATUS _dl_status
    )
endif()

# Build grammar object libraries
# Include both the vcpkg tree-sitter headers and our downloaded parser.h
get_target_property(TS_INC unofficial::tree-sitter::tree-sitter INTERFACE_INCLUDE_DIRECTORIES)

add_library(ts_grammar_c OBJECT "${TS_GRAMMAR_DIR}/c/parser.c")
target_include_directories(ts_grammar_c PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR})

add_library(ts_grammar_cpp OBJECT
    "${TS_GRAMMAR_DIR}/cpp/parser.c"
    "${TS_GRAMMAR_DIR}/cpp/scanner.c"
)
target_include_directories(ts_grammar_cpp PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR})

# C# grammar (v0.23.1 — uses v0.23 parser.h, requires C11 for designated initializers)
add_library(ts_grammar_csharp OBJECT
    "${TS_GRAMMAR_DIR}/csharp/parser.c"
    "${TS_GRAMMAR_DIR}/csharp/scanner.c"
)
# C# uses tree_sitter v0.23 parser.h (has TSCharacterRange/TSFieldMapSlice)
# Put csharp_headers BEFORE so #include "tree_sitter/parser.h" resolves to v0.23
target_include_directories(ts_grammar_csharp BEFORE PRIVATE
    "${TS_GRAMMAR_DIR}/csharp_headers")
target_include_directories(ts_grammar_csharp PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR})
if(MSVC)
    target_compile_options(ts_grammar_csharp PRIVATE /std:c11)
endif()

# Go grammar
add_library(ts_grammar_go OBJECT "${TS_GRAMMAR_DIR}/go/parser.c")
target_include_directories(ts_grammar_go PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR})

# YAML grammar
add_library(ts_grammar_yaml OBJECT
    "${TS_GRAMMAR_DIR}/yaml/parser.c"
    "${TS_GRAMMAR_DIR}/yaml/scanner.c"
)
target_include_directories(ts_grammar_yaml PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR} "${TS_GRAMMAR_DIR}/yaml")

# TypeScript grammar (v0.23 parser.h, C11)
add_library(ts_grammar_typescript OBJECT
    "${TS_GRAMMAR_DIR}/ts_typescript/typescript/src/parser.c"
    "${TS_GRAMMAR_DIR}/ts_typescript/typescript/src/scanner.c"
)
target_include_directories(ts_grammar_typescript BEFORE PRIVATE "${TS_GRAMMAR_DIR}/csharp_headers")
target_include_directories(ts_grammar_typescript PRIVATE ${TS_INC} "${TS_GRAMMAR_DIR}" "${TS_GRAMMAR_DIR}/ts_typescript/typescript/src")
if(MSVC)
    target_compile_options(ts_grammar_typescript PRIVATE /std:c11)
endif()

# JavaScript grammar (v0.23 parser.h, C11)
add_library(ts_grammar_javascript OBJECT
    "${TS_GRAMMAR_DIR}/javascript/parser.c"
    "${TS_GRAMMAR_DIR}/javascript/scanner.c"
)
target_include_directories(ts_grammar_javascript BEFORE PRIVATE "${TS_GRAMMAR_DIR}/csharp_headers")
target_include_directories(ts_grammar_javascript PRIVATE ${TS_INC} "${TS_GRAMMAR_DIR}")
if(MSVC)
    target_compile_options(ts_grammar_javascript PRIVATE /std:c11)
endif()

# Python grammar (v0.23 parser.h, C11)
add_library(ts_grammar_python OBJECT
    "${TS_GRAMMAR_DIR}/python/parser.c"
    "${TS_GRAMMAR_DIR}/python/scanner.c"
)
target_include_directories(ts_grammar_python BEFORE PRIVATE "${TS_GRAMMAR_DIR}/csharp_headers")
target_include_directories(ts_grammar_python PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR})
if(MSVC)
    target_compile_options(ts_grammar_python PRIVATE /std:c11)
endif()

# Rust grammar (v0.23 parser.h, C11)
add_library(ts_grammar_rust OBJECT
    "${TS_GRAMMAR_DIR}/rust/parser.c"
    "${TS_GRAMMAR_DIR}/rust/scanner.c"
)
target_include_directories(ts_grammar_rust BEFORE PRIVATE "${TS_GRAMMAR_DIR}/csharp_headers")
target_include_directories(ts_grammar_rust PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR})
if(MSVC)
    target_compile_options(ts_grammar_rust PRIVATE /std:c11)
endif()

# Java grammar (v0.23 parser.h, C11, no scanner)
add_library(ts_grammar_java OBJECT
    "${TS_GRAMMAR_DIR}/java/parser.c"
)
target_include_directories(ts_grammar_java BEFORE PRIVATE "${TS_GRAMMAR_DIR}/csharp_headers")
target_include_directories(ts_grammar_java PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR})
if(MSVC)
    target_compile_options(ts_grammar_java PRIVATE /std:c11)
endif()

# Bash grammar (v0.23 parser.h, C11)
add_library(ts_grammar_bash OBJECT
    "${TS_GRAMMAR_DIR}/bash/parser.c"
    "${TS_GRAMMAR_DIR}/bash/scanner.c"
)
target_include_directories(ts_grammar_bash BEFORE PRIVATE "${TS_GRAMMAR_DIR}/csharp_headers")
target_include_directories(ts_grammar_bash PRIVATE ${TS_INC} ${TS_GRAMMAR_DIR})
if(MSVC)
    target_compile_options(ts_grammar_bash PRIVATE /std:c11)
endif()

# SQL grammar — disabled (m-novikov grammar has MSVC compilation issues)
# add_library(ts_grammar_sql OBJECT
#     "${TS_GRAMMAR_DIR}/sql/parser.c"
# )
# target_include_directories(ts_grammar_sql BEFORE PRIVATE "${TS_GRAMMAR_DIR}/csharp_headers")
# target_include_directories(ts_grammar_sql PRIVATE ${TS_INC})
# if(MSVC)
#     target_compile_options(ts_grammar_sql PRIVATE /std:c11)
# endif()

# --- Main executable ---
file(GLOB_RECURSE CODETOPO_SOURCES
    src/*.cpp
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
    ts_grammar_c
    ts_grammar_cpp
    ts_grammar_csharp
    ts_grammar_go
    ts_grammar_yaml
    ts_grammar_typescript
    ts_grammar_javascript
    ts_grammar_python
    ts_grammar_rust
    ts_grammar_java
    ts_grammar_bash
    xxHash::xxhash
)

# Platform-specific linking
if(WIN32)
    # ReadDirectoryChangesW is in kernel32 (linked by default)
elseif(APPLE)
    target_link_libraries(codetopo PRIVATE "-framework CoreServices")
endif()

# --- Test executable ---
file(GLOB_RECURSE TEST_SOURCES
    tests/*.cpp
)

# Also need the main source files (excluding main.cpp) for tests
file(GLOB_RECURSE LIB_SOURCES
    src/*.cpp
)
list(FILTER LIB_SOURCES EXCLUDE REGEX "src/main\\.cpp$")
add_executable(codetopo_tests ${TEST_SOURCES} ${LIB_SOURCES})

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
    ts_grammar_c
    ts_grammar_cpp
    ts_grammar_csharp
    ts_grammar_go
    ts_grammar_yaml
    ts_grammar_typescript
    ts_grammar_javascript
    ts_grammar_python
    ts_grammar_rust
    ts_grammar_java
    ts_grammar_bash
    xxHash::xxhash
)

if(APPLE)
    target_link_libraries(codetopo_tests PRIVATE "-framework CoreServices")
endif()

include(CTest)
include(Catch)
catch_discover_tests(codetopo_tests)
