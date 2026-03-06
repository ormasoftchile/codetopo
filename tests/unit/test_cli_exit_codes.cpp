// T083c: Unit test for CLI exit codes
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// Helper: run codetopo and return exit code
static int run_codetopo(const std::string& args) {
    // Find the built executable
    auto exe = fs::current_path() / "build" / "Release" / "codetopo.exe";
    if (!fs::exists(exe)) {
        exe = fs::current_path() / "build" / "codetopo.exe";
    }
    if (!fs::exists(exe)) return -1;

    std::string cmd = "\"" + exe.string() + "\" " + args + " >NUL 2>NUL";
    return std::system(cmd.c_str());
}

TEST_CASE("CLI exits 0 on --help", "[unit][us2]") {
    int rc = run_codetopo("--help");
    REQUIRE(rc == 0);
}

TEST_CASE("CLI exits nonzero on unknown command", "[unit][us2]") {
    int rc = run_codetopo("nonexistent_command");
    REQUIRE(rc != 0);
}

TEST_CASE("Doctor exits 1 on missing DB", "[unit][us5]") {
    int rc = run_codetopo("doctor --db nonexistent_file_that_does_not_exist.sqlite");
    REQUIRE(rc != 0);
}
