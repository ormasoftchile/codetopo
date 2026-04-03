// DEC-038/039 arena lifetime defense tests — verify heap corruption fixes.
//
// Tests the three P0 fixes:
//   Fix 1: Fresh parser per file (no cross-arena parser reuse)
//   Fix 2: Tree destruction before arena release
//   Fix 3: ArenaLease clears thread-local pointer on destruction
//
// DEC-008 convention: scoped Connection + cleanup() for WAL safety on Windows.

#include <catch2/catch_test_macros.hpp>
#include "core/arena.h"
#include "core/arena_pool.h"
#include "index/parser.h"
#include <string>

using namespace codetopo;

namespace codetopo {
    void set_thread_arena(Arena* arena);
    Arena* get_thread_arena();
    void register_arena_allocator();
}

// ===========================================================================
// TEST GROUP 1: ArenaLease clears thread-local pointer on destruction (Fix 3)
// ===========================================================================

TEST_CASE("ArenaLease: thread-local arena is nullptr after lease destruction",
          "[arena][lifetime][lease]") {
    register_arena_allocator();
    ArenaPool pool(1, 4 * 1024 * 1024);

    // Ensure we start clean
    set_thread_arena(nullptr);
    REQUIRE(get_thread_arena() == nullptr);

    {
        ArenaLease lease(pool);
        set_thread_arena(lease.get());
        REQUIRE(get_thread_arena() != nullptr);
        REQUIRE(get_thread_arena() == lease.get());
    }
    // After lease destruction, ArenaLease destructor calls set_thread_arena(nullptr)
    CHECK(get_thread_arena() == nullptr);
}

TEST_CASE("ArenaLease: arena returned to pool after lease destruction",
          "[arena][lifetime][lease]") {
    register_arena_allocator();
    ArenaPool pool(1, 4 * 1024 * 1024);

    REQUIRE(pool.available_count() == 1);

    {
        ArenaLease lease(pool);
        set_thread_arena(lease.get());
        CHECK(pool.available_count() == 0);
    }

    // Arena returned to pool and thread-local cleared
    CHECK(pool.available_count() == 1);
    CHECK(get_thread_arena() == nullptr);
}

TEST_CASE("ArenaLease: multiple sequential leases each clear thread-local",
          "[arena][lifetime][lease]") {
    register_arena_allocator();
    ArenaPool pool(2, 4 * 1024 * 1024);

    for (int i = 0; i < 5; ++i) {
        {
            ArenaLease lease(pool);
            set_thread_arena(lease.get());
            REQUIRE(get_thread_arena() != nullptr);
        }
        CHECK(get_thread_arena() == nullptr);
    }

    // All arenas back in pool
    CHECK(pool.available_count() == 2);
}

// ===========================================================================
// TEST GROUP 2: Tree destruction before arena release (Fix 2 pattern)
// ===========================================================================

TEST_CASE("Tree destruction before arena release: arena valid after tree reset",
          "[arena][lifetime][tree]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    ArenaLease lease(pool);
    set_thread_arena(lease.get());

    Parser parser;
    REQUIRE(parser.set_language("cpp"));

    std::string src = "int main() { return 42; }";
    TreeGuard tree(parser.parse(src));
    REQUIRE(static_cast<bool>(tree));
    CHECK(ts_node_child_count(tree.root()) > 0);

    CHECK(lease->used() > 0);

    // Explicitly destroy tree (simulates Fix 2: tree = TreeGuard(nullptr))
    tree = TreeGuard(nullptr);
    CHECK(!static_cast<bool>(tree));

    // Arena should still be valid — not reset (arena free is a no-op,
    // so used() stays >= what it was; arena memory is reclaimed on reset())
    CHECK(lease->used() > 0);
    CHECK(lease->capacity() > 0);

    // Arena can still be used for allocations after tree destruction
    void* ptr = lease->allocate(64);
    CHECK(ptr != nullptr);

    set_thread_arena(nullptr);
}

TEST_CASE("Tree destruction before arena release: multiple trees destroyed in order",
          "[arena][lifetime][tree]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    ArenaLease lease(pool);
    set_thread_arena(lease.get());

    Parser parser;
    REQUIRE(parser.set_language("cpp"));

    // Parse three files, destroy trees explicitly before arena release
    TreeGuard t1(parser.parse("int a = 1;"));
    REQUIRE(static_cast<bool>(t1));

    TreeGuard t2(parser.parse("int b = 2;"));
    REQUIRE(static_cast<bool>(t2));

    TreeGuard t3(parser.parse("int c = 3;"));
    REQUIRE(static_cast<bool>(t3));

    // Destroy in reverse order (like the production code pattern)
    t3 = TreeGuard(nullptr);
    t2 = TreeGuard(nullptr);
    t1 = TreeGuard(nullptr);

    // Arena is still valid
    CHECK(lease->used() > 0);
    CHECK(lease->capacity() > 0);

    set_thread_arena(nullptr);
}

TEST_CASE("Tree destruction: scoped TreeGuard destruction before arena scope ends",
          "[arena][lifetime][tree]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);

    {
        ArenaLease lease(pool);
        set_thread_arena(lease.get());

        Parser parser;
        REQUIRE(parser.set_language("python"));

        {
            // Tree is scoped — destroyed when this block exits
            TreeGuard tree(parser.parse("x = 42"));
            REQUIRE(static_cast<bool>(tree));
            CHECK(std::string(ts_node_type(tree.root())) == "module");
        }
        // Tree destroyed, arena still alive, thread-local still set
        CHECK(get_thread_arena() == lease.get());
        CHECK(lease->used() > 0);
    }
    // Now lease is destroyed — thread-local cleared
    CHECK(get_thread_arena() == nullptr);
}

// ===========================================================================
// TEST GROUP 3: Fresh parser per file across arenas (Fix 1)
// ===========================================================================

TEST_CASE("Fresh parser per file: two arenas, fresh parsers, no crash",
          "[arena][lifetime][parser]") {
    register_arena_allocator();
    ArenaPool pool(2, 32 * 1024 * 1024);

    // Arena 1: parse with a fresh parser
    {
        ArenaLease lease1(pool);
        set_thread_arena(lease1.get());

        Parser parser1;
        REQUIRE(parser1.set_language("cpp"));
        TreeGuard tree1(parser1.parse("int main() { return 0; }"));
        REQUIRE(static_cast<bool>(tree1));
        CHECK(std::string(ts_node_type(tree1.root())) == "translation_unit");

        // Explicitly destroy tree before lease ends (Fix 2 pattern)
        tree1 = TreeGuard(nullptr);
    }
    // lease1 destroyed: arena returned to pool, thread-local cleared
    CHECK(get_thread_arena() == nullptr);

    // Arena 2: parse with a different fresh parser
    {
        ArenaLease lease2(pool);
        set_thread_arena(lease2.get());

        Parser parser2;
        REQUIRE(parser2.set_language("cpp"));
        TreeGuard tree2(parser2.parse("namespace foo { class Bar {}; }"));
        REQUIRE(static_cast<bool>(tree2));
        CHECK(std::string(ts_node_type(tree2.root())) == "translation_unit");

        tree2 = TreeGuard(nullptr);
    }
    CHECK(get_thread_arena() == nullptr);
    CHECK(pool.available_count() == 2);
}

TEST_CASE("Fresh parser per file: many arenas cycled with fresh parsers",
          "[arena][lifetime][parser]") {
    register_arena_allocator();
    ArenaPool pool(2, 32 * 1024 * 1024);

    // Simulate production pattern: cycle through arena pool with fresh parsers
    for (int i = 0; i < 20; ++i) {
        ArenaLease lease(pool);
        set_thread_arena(lease.get());

        Parser parser;
        REQUIRE(parser.set_language("python"));

        std::string src = "def func_" + std::to_string(i) + "(): return " + std::to_string(i);
        TreeGuard tree(parser.parse(src));
        REQUIRE(static_cast<bool>(tree));
        CHECK(ts_node_child_count(tree.root()) > 0);

        // Destroy tree before lease ends
        tree = TreeGuard(nullptr);
    }
    CHECK(get_thread_arena() == nullptr);
    CHECK(pool.available_count() == 2);
}

TEST_CASE("Fresh parser per file: different languages across arenas",
          "[arena][lifetime][parser]") {
    register_arena_allocator();
    ArenaPool pool(2, 32 * 1024 * 1024);

    struct TestFile {
        const char* lang;
        const char* source;
        const char* expected_root;
    };

    TestFile files[] = {
        {"cpp",    "int x = 1;",           "translation_unit"},
        {"python", "x = 1",                "module"},
        {"cpp",    "class Foo {};",         "translation_unit"},
        {"csharp", "class Bar {}",          "compilation_unit"},
        {"python", "def f(): pass",         "module"},
        {"cpp",    "void f() {}",           "translation_unit"},
    };

    for (const auto& f : files) {
        ArenaLease lease(pool);
        set_thread_arena(lease.get());

        Parser parser;
        REQUIRE(parser.set_language(f.lang));
        TreeGuard tree(parser.parse(f.source));
        REQUIRE(static_cast<bool>(tree));
        CHECK(std::string(ts_node_type(tree.root())) == f.expected_root);

        tree = TreeGuard(nullptr);
    }
    CHECK(get_thread_arena() == nullptr);
    CHECK(pool.available_count() == 2);
}

// ===========================================================================
// TEST GROUP 4: Combined defense-in-depth scenarios
// ===========================================================================

TEST_CASE("Full lifecycle: parse, destroy tree, verify arena, release lease",
          "[arena][lifetime][integration]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);

    {
        ArenaLease lease(pool);
        set_thread_arena(lease.get());
        REQUIRE(get_thread_arena() == lease.get());

        // Parse
        Parser parser;
        REQUIRE(parser.set_language("cpp"));
        TreeGuard tree(parser.parse("struct S { int x; };"));
        REQUIRE(static_cast<bool>(tree));

        size_t arena_used = lease->used();
        CHECK(arena_used > 0);

        // Destroy tree explicitly (Fix 2)
        tree = TreeGuard(nullptr);

        // Arena not reset — still holds memory (free is a no-op in arena)
        CHECK(lease->used() > 0);

        // set_thread_arena(nullptr) before lease ends (Fix 3 pattern from cmd_index)
        set_thread_arena(nullptr);
        CHECK(get_thread_arena() == nullptr);
    }
    // Lease destructor also calls set_thread_arena(nullptr) — idempotent
    CHECK(get_thread_arena() == nullptr);
    CHECK(pool.available_count() == 1);
}

TEST_CASE("Arena reset on release: memory reclaimed after lease ends",
          "[arena][lifetime]") {
    register_arena_allocator();
    ArenaPool pool(1, 16 * 1024 * 1024);

    Arena* arena_ptr = nullptr;
    {
        ArenaLease lease(pool);
        arena_ptr = lease.get();
        set_thread_arena(arena_ptr);

        Parser parser;
        REQUIRE(parser.set_language("cpp"));
        TreeGuard tree(parser.parse("int main() {}"));
        REQUIRE(static_cast<bool>(tree));

        CHECK(arena_ptr->used() > 0);

        tree = TreeGuard(nullptr);
    }

    // After lease release, ArenaPool::release() resets the arena
    // Re-lease the same arena (pool has only 1) — should be reset
    {
        ArenaLease lease2(pool);
        CHECK(lease2.get() == arena_ptr);
        CHECK(lease2->used() == 0); // Arena was reset on release
    }
}
