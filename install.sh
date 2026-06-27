#!/usr/bin/env bash
# install.sh — Bootstrap codetopo on macOS and Linux.
# Usage: bash install.sh
#
# What it does:
#   1. Checks prerequisites (cmake ≥3.20, C++20 compiler, vcpkg)
#   2. Builds codetopo in release mode
#   3. Installs the binary to $HOME/.local/bin/codetopo
#   4. Validates the installed binary
#   5. Prints a next-step hint
#
# NOTE — cmake --install support:
#   CMakeLists.txt does not currently define an install() target, so
#   `cmake --install build --prefix /usr/local` is not yet supported.
#   To add it, add the following to CMakeLists.txt after add_executable():
#
#     install(TARGETS codetopo
#             RUNTIME DESTINATION bin)
#
#   That would allow: cmake --install build --prefix "$HOME/.local"
#   and is the CMake-idiomatic way to handle installation.
#   Until then, this script copies the binary directly.
#
# Safe to re-run (idempotent).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${HOME}/.local/bin"
BINARY_NAME="codetopo"
BUILD_BINARY="${SCRIPT_DIR}/build/${BINARY_NAME}"

# ─── Colors ────────────────────────────────────────────────────────────────────
if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; RESET=''
fi

info()    { echo -e "${CYAN}  →${RESET} $*"; }
ok()      { echo -e "${GREEN}  ✓${RESET} $*"; }
warn()    { echo -e "${YELLOW}  ⚠${RESET} $*"; }
die()     { echo -e "${RED}  ✗ ERROR:${RESET} $*" >&2; exit 1; }
section() { echo -e "\n${BOLD}$*${RESET}"; }

# ─── 1. Prerequisites ──────────────────────────────────────────────────────────
section "Checking prerequisites…"

# cmake ≥ 3.20
if ! command -v cmake &>/dev/null; then
    die "cmake not found. Install cmake ≥ 3.20 (brew install cmake on macOS)."
fi

cmake_version_str=$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
cmake_major=$(echo "$cmake_version_str" | cut -d. -f1)
cmake_minor=$(echo "$cmake_version_str" | cut -d. -f2)
if [ "$cmake_major" -lt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -lt 20 ]; }; then
    die "cmake $cmake_version_str found, but ≥ 3.20 is required."
fi
ok "cmake $cmake_version_str"

# C++20 compiler
if [[ "$(uname -s)" == "Darwin" ]]; then
    # Prefer clang (ships with Xcode CLT on macOS)
    if ! command -v clang++ &>/dev/null; then
        die "clang++ not found. Install Xcode Command Line Tools: xcode-select --install"
    fi
    cxx_version=$(clang++ --version | head -1)
    ok "Compiler: $cxx_version"
else
    # Linux: accept g++ or clang++
    if command -v g++ &>/dev/null; then
        cxx_version=$(g++ --version | head -1)
        ok "Compiler: $cxx_version"
    elif command -v clang++ &>/dev/null; then
        cxx_version=$(clang++ --version | head -1)
        ok "Compiler: $cxx_version"
    else
        die "No C++ compiler found. Install g++ or clang++ (e.g., apt install build-essential)."
    fi
fi

# vcpkg — CMakeLists.txt will auto-clone if missing, but warn so the user knows.
if [ -z "${VCPKG_ROOT:-}" ]; then
    warn "VCPKG_ROOT is not set. CMake will auto-clone vcpkg into ${SCRIPT_DIR}/vcpkg (requires internet)."
    warn "To skip the clone, set VCPKG_ROOT to an existing vcpkg installation:"
    warn "  export VCPKG_ROOT=/path/to/vcpkg"
else
    if [ ! -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]; then
        warn "VCPKG_ROOT=${VCPKG_ROOT} is set but vcpkg.cmake not found there. CMake will auto-clone."
    else
        ok "vcpkg at ${VCPKG_ROOT}"
    fi
fi

# ─── 2. Build ──────────────────────────────────────────────────────────────────
section "Building codetopo (Release)…"

cd "${SCRIPT_DIR}"

info "Configuring…"
cmake --preset release

info "Compiling… (this may take a few minutes on first run)"
cmake --build build --config Release --target codetopo

ok "Build complete: ${BUILD_BINARY}"

# ─── 3. Install ────────────────────────────────────────────────────────────────
section "Installing to ${INSTALL_DIR}…"

mkdir -p "${INSTALL_DIR}"

cp -f "${BUILD_BINARY}" "${INSTALL_DIR}/${BINARY_NAME}"
chmod +x "${INSTALL_DIR}/${BINARY_NAME}"

# macOS: clear quarantine attribute and apply ad-hoc codesign so Gatekeeper
# doesn't kill the binary when run from a new location.
if [[ "$(uname)" == "Darwin" ]]; then
    xattr -cr "${INSTALL_DIR}/${BINARY_NAME}" 2>/dev/null || true
    codesign -s - "${INSTALL_DIR}/${BINARY_NAME}" 2>/dev/null || true
fi

ok "Installed: ${INSTALL_DIR}/${BINARY_NAME}"

# Check if INSTALL_DIR is on PATH
if [[ ":${PATH}:" != *":${INSTALL_DIR}:"* ]]; then
    echo ""
    warn "${INSTALL_DIR} is not on your PATH."
    echo -e "  Add this to your shell config (~/.zshrc, ~/.bashrc, etc.):"
    echo -e "  ${BOLD}export PATH=\"\$HOME/.local/bin:\$PATH\"${RESET}"
    echo ""
    # Export for the rest of this script so validation below works
    export PATH="${INSTALL_DIR}:${PATH}"
fi

# ─── 4. Validate ───────────────────────────────────────────────────────────────
section "Validating…"

if "${INSTALL_DIR}/${BINARY_NAME}" --version &>/dev/null; then
    installed_version=$("${INSTALL_DIR}/${BINARY_NAME}" --version 2>&1 | head -1)
    ok "codetopo is working: $installed_version"
elif "${INSTALL_DIR}/${BINARY_NAME}" --help &>/dev/null; then
    ok "codetopo is working (--help succeeded)"
else
    die "Installed binary did not respond to --version or --help. Check the build output above."
fi

# ─── 5. Next steps ─────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}✓ codetopo installed successfully!${RESET}"
echo ""
echo -e "  ${BOLD}Next step:${RESET} Index your project and configure your editor:"
echo -e "  ${CYAN}codetopo init --root /path/to/your/project${RESET}"
echo ""
echo -e "  Options:"
echo -e "    --editors vscode,cursor,copilot   # choose editor targets"
echo -e "    --watch                           # enable file-watching mode"
echo ""
