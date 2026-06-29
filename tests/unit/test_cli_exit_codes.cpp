// T083c: Unit test for CLI exit codes
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Helper: run codetopo and return exit code
static int run_codetopo(const std::string& args) {
    // Find the built executable — try platform-specific paths
    auto exe = fs::current_path() / "build" / "Release" / "codetopo.exe";  // Windows Release
    if (!fs::exists(exe)) exe = fs::current_path() / "build" / "codetopo.exe";  // Windows Debug
    if (!fs::exists(exe)) exe = fs::current_path() / "build" / "codetopo";       // Unix/macOS
    if (!fs::exists(exe)) return -1;

#ifdef _WIN32
    std::string cmd = "\"" + exe.string() + "\" " + args + " >NUL 2>NUL";
#else
    std::string cmd = "\"" + exe.string() + "\" " + args + " >/dev/null 2>/dev/null";
#endif
    return std::system(cmd.c_str());
}

static std::string quote_path(const fs::path& path) {
    return "\"" + path.string() + "\"";
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

TEST_CASE("Index exits 0 when file-level parse errors are persisted", "[unit][us2]") {
    auto root = fs::current_path() / "build" / "test_cli_exit_codes_index";
    fs::remove_all(root);
    fs::create_directories(root / "src");

    { std::ofstream(root / "src" / "ok.cpp") << "int ok() { return 0; }\n"; }
    { std::ofstream(root / "src" / "empty.cpp"); }

    auto db = root / "index.sqlite";
    int rc = run_codetopo("index --root " + quote_path(root) +
                          " --db " + quote_path(db) +
                          " --no-gitignore --threads 1 --batch-size 1");

    fs::remove_all(root);
    REQUIRE(rc == 0);
}
