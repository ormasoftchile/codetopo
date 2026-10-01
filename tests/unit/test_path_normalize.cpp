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

TEST_CASE("Lookup normalization preserves lexical path identity", "[path][lookup-api]") {
    REQUIRE(path_util::lookup_path("./src/main.cpp") == "src/main.cpp");
    REQUIRE(path_util::lookup_path("src/./main.cpp") == "src/main.cpp");
    REQUIRE(path_util::lookup_path("src/") == "src");
    REQUIRE(path_util::lookup_path("src/name..cpp") == "src/name..cpp");
    REQUIRE(path_util::lookup_path("../src/main.cpp").empty());
    REQUIRE(path_util::lookup_path("src/../main.cpp").empty());
    REQUIRE(path_util::lookup_path(std::string("src\0/main.cpp", 13)).empty());
#ifdef _WIN32
    REQUIRE(path_util::lookup_path(R"(src\main.cpp)") == "src/main.cpp");
    REQUIRE(path_util::lookup_path(R"(C:\repo/src\main.cpp)") == "C:/repo/src/main.cpp");
    REQUIRE(path_util::lookup_path(R"(src\new\test.cpp)") == "src/new/test.cpp");
    REQUIRE(path_util::lookup_path(R"(C:src\main.cpp)").empty());
    REQUIRE(path_util::lookup_path(R"(src\..\main.cpp)").empty());
#else
    REQUIRE(path_util::lookup_path(R"(src\main.cpp)") == R"(src\main.cpp)");
    REQUIRE(path_util::lookup_path("src/Main.cpp") != path_util::lookup_path("src/main.cpp"));
#endif
}

TEST_CASE("Absolute lookup scopes map only inside the requested root", "[path][lookup-api]") {
    auto root = std::filesystem::current_path();
    REQUIRE(path_util::lookup_path_in_root(root.string(), root.string()) == ".");
    REQUIRE(path_util::lookup_path_in_root((root / "src" / "main.cpp").string(), root.string()) == "src/main.cpp");
    REQUIRE(path_util::lookup_path_in_root((root / "*").string(), root.string()) == "*");
    REQUIRE(path_util::lookup_path_in_root((root / "**").generic_string(), root.string()) == "**");
    REQUIRE(path_util::lookup_path_in_root("src/main.cpp", root.string()).empty());
    REQUIRE(path_util::lookup_path_in_root(root.string() + "-other/main.cpp", root.string()).empty());
    REQUIRE(path_util::lookup_path_in_root((root / ".." / "main.cpp").string(), root.string()).empty());
#ifdef _WIN32
    REQUIRE(path_util::lookup_path_in_root(R"(C:\repo\*)", "C:/repo") == "*");
    REQUIRE(path_util::lookup_path_in_root("C:/repo/**", R"(C:\repo)") == "**");
    REQUIRE(path_util::lookup_path_in_root(R"(C:\repo\src\main.cpp)", "C:/repo") == "src/main.cpp");
#endif
}
