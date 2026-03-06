// T021: Unit test for path normalization
#include <catch2/catch_test_macros.hpp>
#include "util/path.h"
#include <filesystem>
#include <fstream>

using namespace codetopo;
namespace fs = std::filesystem;

TEST_CASE("Path normalize produces forward slashes", "[path]") {
    // Create a temp dir structure for testing
    auto tmp = fs::temp_directory_path() / "codetopo_test_path";
    fs::create_directories(tmp / "src");

    // Create a test file
    std::ofstream(tmp / "src" / "main.cpp") << "int main() {}";

    auto result = path_util::normalize(tmp / "src" / "main.cpp", tmp);
    REQUIRE(result == "src/main.cpp");

    fs::remove_all(tmp);
}

TEST_CASE("Path normalize rejects traversal", "[path]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_path2";
    fs::create_directories(tmp);
    std::ofstream(tmp / "test.txt") << "test";

    // A path outside repo root should return empty
    auto result = path_util::normalize(fs::temp_directory_path() / "nonexistent.txt", tmp);
    REQUIRE(result.empty());

    fs::remove_all(tmp);
}

TEST_CASE("MCP path validation rejects dotdot", "[path]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_mcp";
    fs::create_directories(tmp);

    auto result = path_util::validate_mcp_path("../../../etc/passwd", tmp);
    REQUIRE(result.empty());

    fs::remove_all(tmp);
}

TEST_CASE("MCP path validation rejects absolute paths", "[path]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_mcp2";
    fs::create_directories(tmp);

#ifdef _WIN32
    auto result = path_util::validate_mcp_path("C:\\Windows\\system32\\cmd.exe", tmp);
#else
    auto result = path_util::validate_mcp_path("/etc/passwd", tmp);
#endif
    REQUIRE(result.empty());

    fs::remove_all(tmp);
}

TEST_CASE("Language detection from extension", "[path]") {
    REQUIRE(path_util::detect_language("foo.cpp") == "cpp");
    REQUIRE(path_util::detect_language("bar.cs") == "csharp");
    REQUIRE(path_util::detect_language("baz.ts") == "typescript");
    REQUIRE(path_util::detect_language("main.go") == "go");
    REQUIRE(path_util::detect_language("config.yaml") == "yaml");
    REQUIRE(path_util::detect_language("config.yml") == "yaml");
    REQUIRE(path_util::detect_language("test.c") == "c");
    REQUIRE(path_util::detect_language("unknown.rs") == "rust");
    REQUIRE(path_util::detect_language("App.java") == "java");
    REQUIRE(path_util::detect_language("app.py") == "python");
    REQUIRE(path_util::detect_language("app.js") == "javascript");
    REQUIRE(path_util::detect_language("script.sh") == "bash");
    REQUIRE(path_util::detect_language("unknown.xyz").empty());
}
