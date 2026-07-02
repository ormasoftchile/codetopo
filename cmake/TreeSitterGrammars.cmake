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

# --- ts_download_verified ---
# Download a URL to a destination, retrying on failure and verifying the result
# is a non-empty file. Plain file(DOWNLOAD) captures STATUS but the callers used
# to ignore it, so a transient network error (rate-limit, 5xx) left a 0-byte file
# that `if(NOT EXISTS)` then skipped forever — producing grammar objects with no
# tree_sitter_<lang> symbol and a confusing link failure. This fails loudly instead.
function(ts_download_verified url dest)
    # Re-download if the file is missing OR empty (a prior failed download).
    if(EXISTS "${dest}")
        file(SIZE "${dest}" _existing_size)
        if(_existing_size GREATER 0)
            return()
        endif()
    endif()

    set(_attempts 3)
    foreach(_i RANGE 1 ${_attempts})
        file(DOWNLOAD "${url}" "${dest}" STATUS _st TIMEOUT 60)
        list(GET _st 0 _code)
        if(_code EQUAL 0 AND EXISTS "${dest}")
            file(SIZE "${dest}" _size)
            if(_size GREATER 0)
                return()
            endif()
        endif()
        # Failed or empty — remove the stub so the next attempt/build retries cleanly.
        file(REMOVE "${dest}")
        message(WARNING "tree-sitter download attempt ${_i}/${_attempts} failed for ${url} (status: ${_st})")
    endforeach()

    message(FATAL_ERROR
        "Failed to download ${url} after ${_attempts} attempts. "
        "This is usually a transient network/rate-limit issue — re-run the build. "
        "A missing grammar source produces an empty object and an 'Undefined symbol: "
        "tree_sitter_<lang>' link error.")
endfunction()

# --- ts_grammars_init ---
# Downloads shared tree-sitter header files and sets up variables.
# Sets: TS_GRAMMAR_DIR, TS_INC, TS_GRAMMAR_TARGETS
macro(ts_grammars_init)
    set(TS_GRAMMAR_DIR "${CMAKE_BINARY_DIR}/ts_grammars")
    file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/tree_sitter")

    # v0.21 parser.h (base header, used by C/C++/Go/YAML grammars)
    ts_download_verified(
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter-c/refs/tags/v0.21.0/src/tree_sitter/parser.h"
        "${TS_GRAMMAR_DIR}/tree_sitter/parser.h")

    # alloc.h and array.h (needed by newer grammar scanners)
    ts_download_verified(
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter/v0.25.10/cli/generate/src/templates/alloc.h"
        "${TS_GRAMMAR_DIR}/tree_sitter/alloc.h")
    ts_download_verified(
        "https://raw.githubusercontent.com/tree-sitter/tree-sitter/v0.25.10/cli/generate/src/templates/array.h"
        "${TS_GRAMMAR_DIR}/tree_sitter/array.h")

    # v0.23 parser.h — download from tree-sitter v0.25.10 (lib/src/parser.h) which
    # has TSMapSlice, TSLexerMode, and the expanded TSLanguage struct needed by
    # grammars v0.23.3+. We append backwards-compat aliases so older grammars
    # (using TSFieldMapSlice / .version) still compile.
    file(MAKE_DIRECTORY "${TS_GRAMMAR_DIR}/ts_v23_include/tree_sitter")
    if(NOT EXISTS "${TS_GRAMMAR_DIR}/ts_v23_include/tree_sitter/parser.h")
        ts_download_verified(
            "https://raw.githubusercontent.com/tree-sitter/tree-sitter/refs/tags/v0.25.10/lib/src/parser.h"
            "${TS_GRAMMAR_DIR}/ts_v23_include/tree_sitter/parser.h")
        # Append backwards-compatibility aliases for old grammar generators
        file(APPEND "${TS_GRAMMAR_DIR}/ts_v23_include/tree_sitter/parser.h" "\n\
// Backwards-compat aliases for grammars generated before tree-sitter 0.24.\n\
// TSFieldMapSlice was renamed TSMapSlice; .version field was renamed\n\
// .abi_version in the TSLanguage struct.\n\
//\n\
// IMPORTANT: do NOT alias TSLexMode -> TSLexerMode. tree-sitter 0.24 split\n\
// the 4-byte TSLexMode (abi<=14) into a 6-byte TSLexerMode (abi>=15, adds\n\
// reserved_word_set_id). abi-14 grammars declare ts_lex_modes[] with the\n\
// native 4-byte TSLexMode and the runtime reads that array with a 4-byte\n\
// stride for abi 14. Aliasing forces a 6-byte build, misaligning every\n\
// lex-mode entry -> corrupt lex states -> spurious regex/ERROR tokens ->\n\
// malformed trees (and infinite loops in some external scanners). Both\n\
// TSLexMode and TSLexerMode are already defined in this header, so abi-14\n\
// grammars (TSLexMode) and abi-15 grammars (TSLexerMode, e.g. C#) each\n\
// compile against the correct struct with no alias.\n\
#ifndef TSFieldMapSlice\n\
#  define TSFieldMapSlice TSMapSlice\n\
#endif\n\
#ifndef version\n\
#  define version abi_version\n\
#endif\n")
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

    # Download parser.c (and scanner.c + extras). ts_download_verified re-downloads
    # missing OR empty files and fails loudly on persistent failure, so a transient
    # network error can't leave an empty stub that links into a broken binary.
    ts_download_verified(
        "${_tsg_base_url}/${_TSG_SRC_PATH}/parser.c"
        "${_tsg_local_dir}/parser.c")

    if(_TSG_SCANNER)
        ts_download_verified(
            "${_tsg_base_url}/${_TSG_SRC_PATH}/scanner.c"
            "${_tsg_local_dir}/scanner.c")
    endif()

    foreach(_src ${_TSG_EXTRA_SOURCES})
        ts_download_verified(
            "${_tsg_base_url}/${_TSG_SRC_PATH}/${_src}"
            "${_tsg_local_dir}/${_src}")
    endforeach()

    foreach(_dep ${_TSG_SCANNER_DEPS})
        ts_download_verified(
            "${_tsg_base_url}/${_TSG_SRC_PATH}/${_dep}"
            "${_tsg_local_dir}/${_dep}")
    endforeach()

    # Download extra files (paths relative to repo tag root)
    foreach(_dl ${_TSG_EXTRA_DOWNLOADS})
        get_filename_component(_dl_dir "${TS_GRAMMAR_DIR}/${_TSG_NAME}/${_dl}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dl_dir}")
        ts_download_verified(
            "${_tsg_base_url}/${_dl}"
            "${TS_GRAMMAR_DIR}/${_TSG_NAME}/${_dl}")
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

    # abi-14 grammars assign a native TSLexMode[] array to the TSLanguage
    # .lex_modes field (declared TSLexerMode* in the 0.25 header). This is an
    # intentional, benign incompatible-pointer-type: the DATA must stay 4-byte
    # so the runtime's abi-14 stride matches. Suppress the warning so builds
    # with -Werror don't fail. (Aliasing the types to silence it is what caused
    # the lex-mode corruption — see the shim header comment.)
    if(_TSG_V23 AND NOT MSVC)
        target_compile_options(${_tsg_target} PRIVATE -Wno-incompatible-pointer-types)
    endif()

    # Accumulate target name
    list(APPEND TS_GRAMMAR_TARGETS ${_tsg_target})
endmacro()
