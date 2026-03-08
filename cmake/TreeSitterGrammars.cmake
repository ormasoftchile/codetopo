# cmake/TreeSitterGrammars.cmake
#
# Reusable macros for downloading and building tree-sitter grammar OBJECT libraries.
#
# Usage:
#   ts_grammars_init()
#   add_ts_grammar(NAME cpp REPO tree-sitter/tree-sitter-cpp TAG v0.21.0 SCANNER)
#   add_ts_grammar(NAME python REPO tree-sitter/tree-sitter-python TAG v0.23.6 SCANNER V23)
#   ...
#   target_link_libraries(myapp PRIVATE ${TS_GRAMMAR_TARGETS})

# --- ts_grammars_init ---
# Downloads shared tree-sitter header files and sets up variables.
# Sets: TS_GRAMMAR_DIR, TS_INC, TS_GRAMMAR_TARGETS
macro(ts_grammars_init)
    set(TS_GRAMMAR_DIR "${CMAKE_BINARY_DIR}/ts_grammars")
    file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/tree_sitter")

    # v0.21 parser.h (base header, used by C/C++/Go/YAML grammars)
    if(NOT EXISTS "${TS_GRAMMAR_DIR}/tree_sitter/parser.h")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/tree-sitter/tree-sitter-c/refs/tags/v0.21.0/src/tree_sitter/parser.h"
            "${TS_GRAMMAR_DIR}/tree_sitter/parser.h"
            STATUS _dl_status)
    endif()

    # alloc.h and array.h (needed by newer grammar scanners)
    if(NOT EXISTS "${TS_GRAMMAR_DIR}/tree_sitter/alloc.h")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/tree-sitter/tree-sitter/v0.25.10/cli/generate/src/templates/alloc.h"
            "${TS_GRAMMAR_DIR}/tree_sitter/alloc.h"
            STATUS _dl_status)
    endif()
    if(NOT EXISTS "${TS_GRAMMAR_DIR}/tree_sitter/array.h")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/tree-sitter/tree-sitter/v0.25.10/cli/generate/src/templates/array.h"
            "${TS_GRAMMAR_DIR}/tree_sitter/array.h"
            STATUS _dl_status)
    endif()

    # v0.23 parser.h (needed by C#, TypeScript, JavaScript, Python, Rust, Java, Bash)
    file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/ts_v23_include/tree_sitter")
    if(NOT EXISTS "${TS_GRAMMAR_DIR}/ts_v23_include/tree_sitter/parser.h")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/tree-sitter/tree-sitter/refs/tags/v0.23.0/lib/src/parser.h"
            "${TS_GRAMMAR_DIR}/ts_v23_include/tree_sitter/parser.h"
            STATUS _dl_status)
    endif()

    # Tree-sitter include dirs from vcpkg
    get_target_property(TS_INC unofficial::tree-sitter::tree-sitter INTERFACE_INCLUDE_DIRECTORIES)

    set(TS_GRAMMAR_TARGETS "")
endmacro()

# --- add_ts_grammar ---
# Downloads grammar source files and creates an OBJECT library target.
#
# Options:
#   SCANNER              - grammar has scanner.c
#   V23                  - needs v0.23 parser.h (implies C11 on MSVC)
#   C11                  - needs C11 on MSVC (for grammars using C11 features)
# Single-value args:
#   NAME   <name>        - short name used for target: ts_grammar_<name>
#   REPO   <org/repo>    - GitHub repository path
#   TAG    <tag>         - Git tag for the release
#   SRC_PATH <path>      - path to source files within repo (default: "src")
# Multi-value args:
#   EXTRA_SOURCES <files...>     - additional .c files to download and compile from SRC_PATH
#   SCANNER_DEPS <files...>      - files #included by scanner.c (download only, not compiled)
#   EXTRA_DOWNLOADS <paths...>   - additional files (paths relative to repo tag root)
#   EXTRA_INCLUDE_DIRS <dirs...> - additional include directories
macro(add_ts_grammar)
    cmake_parse_arguments(_TSG
        "SCANNER;V23;C11"
        "NAME;REPO;TAG;SRC_PATH"
        "EXTRA_SOURCES;SCANNER_DEPS;EXTRA_DOWNLOADS;EXTRA_INCLUDE_DIRS"
        ${ARGN})

    set(_tsg_base_url "https://raw.githubusercontent.com/${_TSG_REPO}/refs/tags/${_TSG_TAG}")

    if(NOT DEFINED _TSG_SRC_PATH OR "${_TSG_SRC_PATH}" STREQUAL "")
        set(_TSG_SRC_PATH "src")
    endif()

    # Local directory layout:
    #   Default (SRC_PATH=src): ${TS_GRAMMAR_DIR}/${NAME}/parser.c
    #   Custom SRC_PATH:        ${TS_GRAMMAR_DIR}/${NAME}/${SRC_PATH}/parser.c
    if("${_TSG_SRC_PATH}" STREQUAL "src")
        set(_tsg_local_dir "${TS_GRAMMAR_DIR}/${_TSG_NAME}")
    else()
        set(_tsg_local_dir "${TS_GRAMMAR_DIR}/${_TSG_NAME}/${_TSG_SRC_PATH}")
    endif()

    file(MAKE_DIRECTORY "${_tsg_local_dir}")

    # Download parser.c (and scanner.c + extras) if parser.c doesn't exist yet
    if(NOT EXISTS "${_tsg_local_dir}/parser.c")
        file(DOWNLOAD
            "${_tsg_base_url}/${_TSG_SRC_PATH}/parser.c"
            "${_tsg_local_dir}/parser.c"
            STATUS _dl_status)

        if(_TSG_SCANNER)
            file(DOWNLOAD
                "${_tsg_base_url}/${_TSG_SRC_PATH}/scanner.c"
                "${_tsg_local_dir}/scanner.c"
                STATUS _dl_status)
        endif()

        foreach(_src ${_TSG_EXTRA_SOURCES})
            file(DOWNLOAD
                "${_tsg_base_url}/${_TSG_SRC_PATH}/${_src}"
                "${_tsg_local_dir}/${_src}"
                STATUS _dl_status)
        endforeach()

        foreach(_dep ${_TSG_SCANNER_DEPS})
            file(DOWNLOAD
                "${_tsg_base_url}/${_TSG_SRC_PATH}/${_dep}"
                "${_tsg_local_dir}/${_dep}"
                STATUS _dl_status)
        endforeach()
    endif()

    # Download extra files (paths relative to repo tag root)
    foreach(_dl ${_TSG_EXTRA_DOWNLOADS})
        get_filename_component(_dl_dir "${TS_GRAMMAR_DIR}/${_TSG_NAME}/${_dl}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dl_dir}")
        if(NOT EXISTS "${TS_GRAMMAR_DIR}/${_TSG_NAME}/${_dl}")
            file(DOWNLOAD
                "${_tsg_base_url}/${_dl}"
                "${TS_GRAMMAR_DIR}/${_TSG_NAME}/${_dl}"
                STATUS _dl_status)
        endif()
    endforeach()

    # Collect source files for the OBJECT library
    set(_tsg_sources "${_tsg_local_dir}/parser.c")
    if(_TSG_SCANNER)
        list(APPEND _tsg_sources "${_tsg_local_dir}/scanner.c")
    endif()
    foreach(_src ${_TSG_EXTRA_SOURCES})
        list(APPEND _tsg_sources "${_tsg_local_dir}/${_src}")
    endforeach()

    # Create OBJECT library
    set(_tsg_target "ts_grammar_${_TSG_NAME}")
    add_library(${_tsg_target} OBJECT ${_tsg_sources})

    # Include directories: v0.23 header first (if needed), then vcpkg + grammar dir
    if(_TSG_V23)
        target_include_directories(${_tsg_target} BEFORE PRIVATE
            "${TS_GRAMMAR_DIR}/ts_v23_include")
    endif()
    target_include_directories(${_tsg_target} PRIVATE ${TS_INC} "${TS_GRAMMAR_DIR}")

    foreach(_inc ${_TSG_EXTRA_INCLUDE_DIRS})
        target_include_directories(${_tsg_target} PRIVATE "${_inc}")
    endforeach()

    # v0.23 grammars need C11 on MSVC for designated initializers
    if((_TSG_V23 OR _TSG_C11) AND MSVC)
        target_compile_options(${_tsg_target} PRIVATE /std:c11)
    endif()

    # Accumulate target name
    list(APPEND TS_GRAMMAR_TARGETS ${_tsg_target})
endmacro()
