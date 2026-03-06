// T022: Unit test for stable_key generation and collision ordinals
#include <catch2/catch_test_macros.hpp>
#include "index/stable_key.h"

using namespace codetopo;

TEST_CASE("make_stable_key basic format", "[stable_key]") {
    auto key = make_stable_key("src/foo.cpp", "function", "ns::Foo::Bar");
    REQUIRE(key == "src/foo.cpp::function::ns::Foo::Bar");
}

TEST_CASE("make_file_stable_key", "[stable_key]") {
    auto key = make_file_stable_key("src/main.cpp");
    REQUIRE(key == "src/main.cpp::file");
}

TEST_CASE("resolve_collisions no duplicates", "[stable_key]") {
    std::vector<KeyCandidate> candidates = {
        {"src/foo.cpp::function::bar", 10},
        {"src/foo.cpp::function::baz", 20},
    };
    auto result = resolve_collisions(candidates);
    REQUIRE(result.size() == 2);
    REQUIRE(result[0] == "src/foo.cpp::function::bar");
    REQUIRE(result[1] == "src/foo.cpp::function::baz");
}

TEST_CASE("resolve_collisions with overloads", "[stable_key]") {
    std::vector<KeyCandidate> candidates = {
        {"src/foo.cpp::function::overloaded", 10},
        {"src/foo.cpp::function::overloaded", 30},
        {"src/foo.cpp::function::overloaded", 20},
    };
    auto result = resolve_collisions(candidates);
    REQUIRE(result.size() == 3);
    // Sorted by start_line: 10, 20, 30
    REQUIRE(result[0] == "src/foo.cpp::function::overloaded");
    REQUIRE(result[1] == "src/foo.cpp::function::overloaded#2");
    REQUIRE(result[2] == "src/foo.cpp::function::overloaded#3");
}

TEST_CASE("resolve_collisions mixed unique and duplicate", "[stable_key]") {
    std::vector<KeyCandidate> candidates = {
        {"a::function::foo", 5},
        {"a::function::bar", 10},
        {"a::function::foo", 15},
    };
    auto result = resolve_collisions(candidates);
    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == "a::function::foo");
    REQUIRE(result[1] == "a::function::bar");
    REQUIRE(result[2] == "a::function::foo#2");
}
