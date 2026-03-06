// T055: Unit test for gitignore pattern matching
#include <catch2/catch_test_macros.hpp>
#include "index/scanner.h"

using namespace codetopo;

TEST_CASE("GitignoreFilter matches simple patterns", "[unit][us1]") {
    GitignoreFilter filter;
    // Simulate loading patterns directly
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "codetopo_gi_test";
    std::filesystem::create_directories(tmp);

    {
        std::ofstream f(tmp / ".gitignore");
        f << "build/\n"
          << "*.log\n"
          << "!important.log\n"
          << "node_modules/\n"
          << "**/*.tmp\n";
    }

    filter.load(tmp / ".gitignore");

    // Directory match
    REQUIRE(filter.is_ignored("build", true));
    REQUIRE(filter.is_ignored("node_modules", true));

    // Glob match
    REQUIRE(filter.is_ignored("output.log", false));
    REQUIRE(filter.is_ignored("src/debug.log", false));

    // Negation
    REQUIRE_FALSE(filter.is_ignored("important.log", false));

    // Non-matching
    REQUIRE_FALSE(filter.is_ignored("src/main.cpp", false));
    REQUIRE_FALSE(filter.is_ignored("README.md", false));

    std::filesystem::remove_all(tmp);
}

TEST_CASE("GitignoreFilter directory-only patterns", "[unit][us1]") {
    GitignoreFilter filter;
    auto tmp = std::filesystem::temp_directory_path() / "codetopo_gi_test2";
    std::filesystem::create_directories(tmp);

    {
        std::ofstream f(tmp / ".gitignore");
        f << "out/\n";  // Directory-only pattern
    }

    filter.load(tmp / ".gitignore");

    // Should match directories
    REQUIRE(filter.is_ignored("out", true));

    // Should NOT match files (dir_only pattern)
    REQUIRE_FALSE(filter.is_ignored("out", false));

    std::filesystem::remove_all(tmp);
}
