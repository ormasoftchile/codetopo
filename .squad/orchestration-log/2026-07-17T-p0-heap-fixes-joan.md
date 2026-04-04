# Orchestration Log: Joan — Arena Lifetime Defense Tests

**Date:** 2026-07-17T00:00:00Z  
**Agent:** Joan (QA)  
**Task:** Test suite for P0 heap corruption fixes (DEC-038/039)  
**Status:** COMPLETE  

## Test Implementation Summary

Created `tests/unit/test_arena_lifetime.cpp` with 11 new test cases (143 assertions) to comprehensively cover Grag's three P0 heap corruption fixes:

### Test Coverage by Fix

#### Fix 1: ArenaLease Thread-Local Clearing (3 tests)
- `[arena][lifetime][lease]` — Lease destructor clears thread-local arena pointer
- Verifies `get_thread_arena()` returns `nullptr` after lease destruction
- Validates sequential leases all clear correctly
- Confirms arena is returned to pool after clearing

#### Fix 2: Explicit Tree Destruction Before Arena Release (3 tests)
- `[arena][lifetime][tree]` — Tree destruction completes before arena reset
- Verifies `TreeGuard` reset before arena scope exit
- Tests multiple trees destroyed in dependency order
- Confirms scoped destruction semantics

#### Fix 3: Fresh Parser Per File Across Arenas (3 tests)
- `[arena][lifetime][parser]` — No parser reuse across arena boundaries
- Two arenas with fresh parsers per file (no reuse)
- 20 arena cycles with fresh parsers in sequence
- Cross-language parsing (C++, Python, C#) across arena boundaries
- All confirm no crash + clean pool state

#### Integration Tests (2 tests)
- `[arena][lifetime][integration]` — Full parse→destroy→clear→release lifecycle
- Defense-in-depth verification: arena reset confirmation `used() == 0` on re-lease
- Combined lifecycle test with error resilience

#### Existing Parser Reuse Tests (5 tests from test_dec039_optimizations.cpp)
- All 5 still pass — single-arena reuse was never broken
- Cross-arena reuse was the root cause (now eliminated)

## Key Learning

`ts_tree_delete()` routes through arena's free function (no-op), but tree-sitter may trigger internal allocations during deletion. This means `arena->used()` can *increase* slightly after tree destruction rather than staying constant. Tests assert `used() > 0` (arena not reset) rather than exact byte equality.

## Build & Test Results

- **Build:** Clean compile (all 11 tests + existing suite)
- **Total Test Suite:** 146 tests, 1039 assertions — all green
- **No Regressions:** Existing parser reuse tests (5 cases) all passing
- **Files:** 
  - `tests/unit/test_arena_lifetime.cpp` (created)
  - `CMakeLists.txt` (test registration)

## Defensive Posture

These tests form the front line against heap corruption regression:
1. Verify thread-local clearing happens on all exit paths
2. Verify tree destruction happens before arena reset
3. Verify parser state isolation across arena boundaries
4. Catch any future parser reuse regression attempts

---

**Deliverables:**
- `tests/unit/test_arena_lifetime.cpp` (11 tests, 143 assertions)
- `CMakeLists.txt` (test registration)
- Full suite: 146/146 tests pass
- No regressions in existing test coverage
