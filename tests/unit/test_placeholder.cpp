// T020: Unit test for arena allocator
#include <catch2/catch_test_macros.hpp>
#include "core/arena.h"

using namespace codetopo;

TEST_CASE("Arena basic allocation", "[arena]") {
    Arena arena(1024);
    REQUIRE(arena.capacity() == 1024);
    REQUIRE(arena.used() == 0);

    void* p1 = arena.allocate(100);
    REQUIRE(p1 != nullptr);
    REQUIRE(arena.used() >= 100);
}

TEST_CASE("Arena reset reclaims all memory", "[arena]") {
    Arena arena(1024);
    arena.allocate(500);
    REQUIRE(arena.used() >= 500);
    arena.reset();
    REQUIRE(arena.used() == 0);
}

TEST_CASE("Arena overflow falls back to malloc", "[arena]") {
    Arena arena(256);
    void* p1 = arena.allocate(200);
    REQUIRE(p1 != nullptr);
    REQUIRE_FALSE(arena.overflowed());
    void* p2 = arena.allocate(200);  // Exceeds 256 — falls back to malloc
    REQUIRE(p2 != nullptr);          // malloc fallback succeeds
    REQUIRE(arena.overflowed());     // overflow flag is set
    arena.reset();
    REQUIRE_FALSE(arena.overflowed()); // reset clears overflow flag
}

TEST_CASE("Arena calloc zeroes memory", "[arena]") {
    Arena arena(1024);
    void* p = arena.allocate_zeroed(10, 4);
    REQUIRE(p != nullptr);
    auto* bytes = static_cast<uint8_t*>(p);
    for (int i = 0; i < 40; ++i) {
        REQUIRE(bytes[i] == 0);
    }
}

TEST_CASE("Arena realloc copies data", "[arena]") {
    Arena arena(4096);
    void* p1 = arena.allocate(16);
    REQUIRE(p1 != nullptr);
    std::memset(p1, 0xAB, 16);

    void* p2 = arena.reallocate(p1, 16, 32);
    REQUIRE(p2 != nullptr);
    REQUIRE(p2 != p1);
    auto* bytes = static_cast<uint8_t*>(p2);
    for (int i = 0; i < 16; ++i) {
        REQUIRE(bytes[i] == 0xAB);
    }
}

TEST_CASE("Arena malloc/realloc with header", "[arena]") {
    Arena arena(4096);
    void* p1 = arena_malloc(arena, 64);
    REQUIRE(p1 != nullptr);

    std::memset(p1, 0xCD, 64);

    void* p2 = arena_realloc(arena, p1, 128);
    REQUIRE(p2 != nullptr);
    auto* bytes = static_cast<uint8_t*>(p2);
    for (int i = 0; i < 64; ++i) {
        REQUIRE(bytes[i] == 0xCD);
    }
}

TEST_CASE("Arena free is no-op", "[arena]") {
    Arena arena(1024);
    void* p = arena_malloc(arena, 100);
    size_t used_before = arena.used();
    arena_free(p);
    REQUIRE(arena.used() == used_before);
}
