// T056: Unit test for resource limits enforcement
#include <catch2/catch_test_macros.hpp>
#include "core/config.h"
#include "core/arena.h"
#include "core/arena_pool.h"
#include "index/extractor.h"
#include "index/parser.h"
#include <string>

using namespace codetopo;

namespace codetopo {
    void set_thread_arena(Arena* arena);
    void register_arena_allocator();
}

TEST_CASE("Extractor truncates at max symbols", "[unit][us1]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    std::string source;
    for (int i = 0; i < 120; ++i) {
        source += "int var_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }

    Parser parser;
    REQUIRE(parser.set_language("c"));
    auto tree = TreeGuard(parser.parse(source));
    REQUIRE(tree);

    // Set max symbols to a small number
    Extractor extractor(50, 500);
    auto result = extractor.extract(tree.tree, source, "c", "test.c");

    // Should be truncated
    REQUIRE(result.truncated);
    REQUIRE(result.truncation_reason.find("max_symbols") != std::string::npos);
    REQUIRE(result.symbols.size() <= 50);
}

TEST_CASE("Extractor handles empty file", "[unit][us1]") {
    register_arena_allocator();
    ArenaPool pool(1, 4 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    std::string source = "";

    Parser parser;
    REQUIRE(parser.set_language("c"));
    auto tree = TreeGuard(parser.parse(source));

    if (tree) {
        Extractor extractor(50000, 500);
        auto result = extractor.extract(tree.tree, source, "c", "empty.c");
        REQUIRE_FALSE(result.truncated);
        REQUIRE(result.symbols.empty());
    }
    // If tree is null for empty source, that's acceptable
    REQUIRE(true);
}

TEST_CASE("Config max_file_size_bytes calculation", "[unit][us1]") {
    Config cfg;
    cfg.max_file_size_kb = 10240;
    REQUIRE(cfg.max_file_size_bytes() == 10240ULL * 1024);
}

TEST_CASE("Config arena_size_bytes calculation", "[unit][us1]") {
    Config cfg;
    cfg.arena_size_mb = 128;
    REQUIRE(cfg.arena_size_bytes() == 128ULL * 1024 * 1024);
}
