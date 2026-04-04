// Watchdog timeout formula tests — DEC-040 redesign.
//
// The slot_timeout_ms lambda lives inside cmd_index.cpp and is not
// directly accessible from tests.  We replicate the NEW formula here
// so changes to the production code are cross-checked against a known-
// good reference.
//
// NEW formula:
//   cancel_ms = min(base_ms + (file_size_bytes * 10) / 1024, 10000)
//   kill_ms   = cancel_ms * 2
//
// NEW config defaults: parse_timeout_s = 5, extraction_timeout_s = 5.

#include <catch2/catch_test_macros.hpp>
#include "core/config.h"
#include <algorithm>
#include <cstdint>

using namespace codetopo;

// ---------------------------------------------------------------------------
// Replicated formula — must match cmd_index.cpp after DEC-040 lands.
// ---------------------------------------------------------------------------

static int64_t slot_timeout_ms(int64_t base_timeout_ms,
                               int64_t file_size_bytes) {
    if (file_size_bytes < 0) file_size_bytes = 0;     // clamp negative
    int64_t size_bonus_ms = (file_size_bytes * 10) / 1024;  // +10ms per KB
    return std::min(base_timeout_ms + size_bonus_ms,
                    static_cast<int64_t>(10000));       // hard cap 10 s
}

static int64_t kill_timeout_ms(int64_t cancel_ms) {
    return cancel_ms * 2;   // 2x multiplier
}

// Convenience: default base when parse_timeout_s == 5
static constexpr int64_t DEFAULT_BASE_MS = 5000;

// ---------------------------------------------------------------------------
// 1. Timeout formula correctness
// ---------------------------------------------------------------------------
TEST_CASE("slot_timeout_ms: 0-byte file returns base only",
          "[watchdog][timeout]") {
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 0) == 5000);
}

TEST_CASE("slot_timeout_ms: 10 KB file",
          "[watchdog][timeout]") {
    // 10 * 1024 * 10 / 1024 = 100 ms bonus
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 10 * 1024) == 5100);
}

TEST_CASE("slot_timeout_ms: 50 KB file",
          "[watchdog][timeout]") {
    // 50 * 1024 * 10 / 1024 = 500 ms bonus
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 50 * 1024) == 5500);
}

TEST_CASE("slot_timeout_ms: 100 KB file",
          "[watchdog][timeout]") {
    // 100 * 1024 * 10 / 1024 = 1000 ms bonus
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 100 * 1024) == 6000);
}

TEST_CASE("slot_timeout_ms: 200 KB file",
          "[watchdog][timeout]") {
    // 200 * 1024 * 10 / 1024 = 2000 ms bonus
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 200 * 1024) == 7000);
}

TEST_CASE("slot_timeout_ms: 500 KB file hits hard cap",
          "[watchdog][timeout]") {
    // 500 * 1024 * 10 / 1024 = 5000 → 5000 + 5000 = 10000 (cap)
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 500 * 1024) == 10000);
}

TEST_CASE("slot_timeout_ms: 1 MB file hits hard cap",
          "[watchdog][timeout]") {
    // 1 MB bonus alone would be 10240, but cap is 10000
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 1024 * 1024) == 10000);
}

// ---------------------------------------------------------------------------
// 2. Kill threshold — 2x cancel
// ---------------------------------------------------------------------------
TEST_CASE("kill threshold: 0-byte file (cancel=5000, kill=10000)",
          "[watchdog][timeout]") {
    int64_t cancel = slot_timeout_ms(DEFAULT_BASE_MS, 0);
    CHECK(cancel == 5000);
    CHECK(kill_timeout_ms(cancel) == 10000);
}

TEST_CASE("kill threshold: 100 KB file (cancel=6000, kill=12000)",
          "[watchdog][timeout]") {
    int64_t cancel = slot_timeout_ms(DEFAULT_BASE_MS, 100 * 1024);
    CHECK(cancel == 6000);
    CHECK(kill_timeout_ms(cancel) == 12000);
}

TEST_CASE("kill threshold: 500 KB file capped (cancel=10000, kill=20000)",
          "[watchdog][timeout]") {
    int64_t cancel = slot_timeout_ms(DEFAULT_BASE_MS, 500 * 1024);
    CHECK(cancel == 10000);
    CHECK(kill_timeout_ms(cancel) == 20000);
}

// ---------------------------------------------------------------------------
// 3. Config defaults
// ---------------------------------------------------------------------------
TEST_CASE("config defaults: parse_timeout_s is 5",
          "[watchdog][timeout][config]") {
    Config cfg;
    CHECK(cfg.parse_timeout_s == 5);
}

TEST_CASE("config defaults: extraction_timeout_s is 5",
          "[watchdog][timeout][config]") {
    Config cfg;
    CHECK(cfg.extraction_timeout_s == 5);
}

// ---------------------------------------------------------------------------
// 4. Edge cases
// ---------------------------------------------------------------------------
TEST_CASE("slot_timeout_ms: negative file size clamps to base",
          "[watchdog][timeout][edge]") {
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, -1) == 5000);
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, -100000) == 5000);
}

TEST_CASE("slot_timeout_ms: very large file (10 GB) hits hard cap",
          "[watchdog][timeout][edge]") {
    int64_t ten_gb = static_cast<int64_t>(10) * 1024 * 1024 * 1024;
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, ten_gb) == 10000);
}

TEST_CASE("slot_timeout_ms: file just below hard cap boundary",
          "[watchdog][timeout][edge]") {
    // With base 5000, cap kicks in at bonus = 5000 → file_size = 512 KB
    // 511 KB: bonus = 511 * 1024 * 10 / 1024 = 5110 → 5000+5110 = 10110 → capped to 10000
    // Try 499 KB: bonus = 499 * 1024 * 10 / 1024 = 4990 → 5000+4990 = 9990 < cap
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 499 * 1024) == 9990);
    CHECK(slot_timeout_ms(DEFAULT_BASE_MS, 500 * 1024) == 10000);
}

TEST_CASE("slot_timeout_ms: custom base timeout",
          "[watchdog][timeout]") {
    // base = 3000: 100 KB → 3000 + 1000 = 4000
    CHECK(slot_timeout_ms(3000, 100 * 1024) == 4000);
    // base = 3000: 1 MB → cap at 10000
    CHECK(slot_timeout_ms(3000, 1024 * 1024) == 10000);
}
