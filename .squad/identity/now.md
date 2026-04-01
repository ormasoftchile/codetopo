---
updated_at: 2026-04-01T16:00:00Z
focus_area: Persist still bottleneck; R1 (ThreadPool stacks) + R2 (pipelined writer) next
active_issues: []
---

# What We're Focused On

Post-DEC-032 profiling complete (Otho). **Result: persist still the bottleneck**, not workers. Workers can deliver 788 files/s; persist caps at 617. Contention (35% wall) from bursty arrival due to largest-first sort.

**Blocker:** ThreadPool uses 64MB stacks (DEC-021 fix never applied) = 1.15GB committed memory.

**Immediate actions:**
1. **R1 (HIGH, 1-line):** Fix ThreadPool stack 64→8MB. Zero risk, 1GB+ RAM savings.
2. **R2 (HIGH, ~200 LOC):** Pipelined persist thread — move persist to dedicated writer thread via lock-free queue. Eliminates 35% contention.
3. **R3 (MEDIUM, if needed):** Parallel WAL persist (DEC-026 R7).

**Parse/extract insights:** Parse is 46-70% of worker time (irreducible tree-sitter cost, skip). Extract 3-6ms/file (well-controlled). file_read 9.4x slower at 1024KB (validates 512KB default).

**Test suite:** 173 tests pass (914 assertions), all green. Build: 0 errors, 0 warnings.

**Next decision:** After R1+R2, measure again. May eliminate need for DEC-026 R7.

Updated by Scribe at 2026-04-01T16:00:00Z after Otho profiling session completion.


