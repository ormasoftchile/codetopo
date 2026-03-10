// Unit tests for util/process.h — process spawning utility
#include <catch2/catch_test_macros.hpp>
#include "util/process.h"
#include <filesystem>

namespace fs = std::filesystem;
using namespace codetopo;

// ---------------------------------------------------------------------------
// get_self_executable_path
// ---------------------------------------------------------------------------

TEST_CASE("get_self_executable_path returns non-empty string", "[process]") {
    auto path = get_self_executable_path();
    REQUIRE_FALSE(path.empty());
}

TEST_CASE("get_self_executable_path points to an existing file", "[process]") {
    auto path = get_self_executable_path();
    REQUIRE(fs::exists(path));
}

// ---------------------------------------------------------------------------
// spawn_and_wait — success
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait returns 0 for successful command", "[process]") {
#ifdef _WIN32
    int rc = spawn_and_wait("cmd.exe", {"/c", "echo hello"});
#else
    int rc = spawn_and_wait("/bin/sh", {"-c", "echo hello"});
#endif
    REQUIRE(rc == 0);
}

// ---------------------------------------------------------------------------
// spawn_and_wait — exit code propagation
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait returns non-zero exit code", "[process]") {
#ifdef _WIN32
    int rc = spawn_and_wait("cmd.exe", {"/c", "exit 42"});
    REQUIRE(rc == 42);
#else
    int rc = spawn_and_wait("/bin/sh", {"-c", "exit 42"});
    REQUIRE(rc == 42);
#endif
}

TEST_CASE("spawn_and_wait returns 1 for exit 1", "[process]") {
#ifdef _WIN32
    int rc = spawn_and_wait("cmd.exe", {"/c", "exit 1"});
#else
    int rc = spawn_and_wait("/bin/sh", {"-c", "exit 1"});
#endif
    REQUIRE(rc == 1);
}

// ---------------------------------------------------------------------------
// spawn_and_wait — non-existent executable
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait fails gracefully for missing executable", "[process]") {
    int rc = spawn_and_wait("this_executable_does_not_exist_xyz", {});
    REQUIRE(rc != 0);
}

// ---------------------------------------------------------------------------
// spawn_and_wait — zero-arg invocation
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait works with no arguments", "[process]") {
#ifdef _WIN32
    // cmd.exe /c with no further args exits 0
    int rc = spawn_and_wait("cmd.exe", {"/c"});
    REQUIRE(rc == 0);
#elif defined(__APPLE__)
    int rc = spawn_and_wait("/usr/bin/true", {});
    REQUIRE(rc == 0);
#else
    int rc = spawn_and_wait("/bin/true", {});
    REQUIRE(rc == 0);
#endif
}

// ---------------------------------------------------------------------------
// get_self_executable_path — additional checks
// ---------------------------------------------------------------------------

TEST_CASE("get_self_executable_path returns an absolute path", "[process]") {
    auto path = get_self_executable_path();
    REQUIRE(fs::path(path).is_absolute());
}

// ---------------------------------------------------------------------------
// spawn_and_wait — exit code boundary values
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait propagates exit code 255", "[process]") {
#ifdef _WIN32
    int rc = spawn_and_wait("cmd.exe", {"/c", "exit 255"});
#else
    int rc = spawn_and_wait("/bin/sh", {"-c", "exit 255"});
#endif
    REQUIRE(rc == 255);
}

TEST_CASE("spawn_and_wait propagates exit code 0 from /bin/false equivalent", "[process]") {
#ifdef _WIN32
    int rc = spawn_and_wait("cmd.exe", {"/c", "exit 0"});
#else
    int rc = spawn_and_wait("/bin/sh", {"-c", "exit 0"});
#endif
    REQUIRE(rc == 0);
}

// ---------------------------------------------------------------------------
// spawn_and_wait — arguments with spaces (exercises Win32 quoting logic)
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait handles arguments containing spaces", "[process]") {
#ifdef _WIN32
    // echo with a space-containing argument — cmd.exe /c will print it
    int rc = spawn_and_wait("cmd.exe", {"/c", "echo hello world"});
    REQUIRE(rc == 0);
#else
    int rc = spawn_and_wait("/bin/sh", {"-c", "echo 'hello world'"});
    REQUIRE(rc == 0);
#endif
}

// ---------------------------------------------------------------------------
// spawn_and_wait — multiple arguments
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait passes multiple arguments correctly", "[process]") {
#ifdef _WIN32
    // Use cmd /c "echo A && echo B" to exercise multi-arg passing
    int rc = spawn_and_wait("cmd.exe", {"/c", "echo A && echo B"});
    REQUIRE(rc == 0);
#else
    // /bin/sh -c with a compound command
    int rc = spawn_and_wait("/bin/sh", {"-c", "echo A; echo B"});
    REQUIRE(rc == 0);
#endif
}

// ---------------------------------------------------------------------------
// spawn_and_wait — empty executable string
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait fails gracefully for empty executable", "[process]") {
    int rc = spawn_and_wait("", {});
    REQUIRE(rc != 0);
}

// ---------------------------------------------------------------------------
// spawn_and_wait — process that writes to stderr
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait succeeds when child writes to stderr", "[process]") {
#ifdef _WIN32
    int rc = spawn_and_wait("cmd.exe", {"/c", "echo error message 1>&2"});
    REQUIRE(rc == 0);
#else
    int rc = spawn_and_wait("/bin/sh", {"-c", "echo error >&2"});
    REQUIRE(rc == 0);
#endif
}

// ---------------------------------------------------------------------------
// spawn_and_wait — rapid successive spawns (no resource leak)
// ---------------------------------------------------------------------------

TEST_CASE("spawn_and_wait handles rapid successive calls", "[process]") {
    for (int i = 0; i < 10; ++i) {
#ifdef _WIN32
        int rc = spawn_and_wait("cmd.exe", {"/c", "echo iteration"});
#else
        int rc = spawn_and_wait("/bin/sh", {"-c", "true"});
#endif
        REQUIRE(rc == 0);
    }
}
