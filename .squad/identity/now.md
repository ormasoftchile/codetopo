---
updated_at: 2026-04-01T16:50:00Z
focus_area: R2 validated (218 files/s, 1.07:1 balance), file_nodes leak fixed, optimization roadmap ready
active_issues: None (all findings resolved or roadmapped)
---

# What We're Focused On

**Session: 2026-04-01T16:50Z** — Three-agent orchestration complete.

## Achievements This Session

**DEC-035: File Node Leak Fixed**
- Grag implemented explicit DELETE before INSERT for orphaned file_nodes
- 207/208 tests pass (1 pre-existing flake), build clean
- Joan's test suite (208 tests, 1112 assertions) validates fix

**DEC-036: R2 Validation Complete**
- Otho profiled pipeline at scale: 218 files/s confirmed, persist/worker 1.07:1 (nearly balanced)
- Workers are now the true bottleneck, not persist
- WAL checkpoint identified as new #3 cost center (4.8s, 16% wall)
- R3a (WAL overlap during resolve_refs) is quick win

**DEC-037: Five Optimization Vectors Ranked**
- Simon completed optimization analysis: 280–320 files/s potential (+35–55%)
- Phase 1 (Quick Wins): Pre-alloc file read, language dispatch, parser pooling (3.5 hr, low risk)
- Phase 2 (Refinements): String cache, parallel resolve (5.5 hr, low-moderate risk)
- Architecture sound; remaining gains are micro-optimizations, not structural changes

## Current Status

**Performance Baseline:** 207 files/s (4145 C# files, fsm)
- Persist/worker ratio: 1.07:1 (healthy, workers are ceiling)
- Contention: 50.5% (healthy — main thread waiting for workers, not blocked)
- Worker breakdown: file_read 50%, parse 20%, extract 9%
- Tests: 208 tests (1112 assertions), all green

**Build Status:** Clean (MSVC + CMake, 0 errors/warnings)

## Next Actions (Priority Order)

1. **Code review Phase 1 optimizations** — Pre-alloc file read, language dispatch, parser pooling
2. **Implement Phase 1 in parallel** if possible (all low-risk, 3.5 hr combined)
3. **Profile before/after** on fsm to validate cumulative gains
4. **Implement R3a** (WAL checkpoint overlap) as quick win during resolve_refs
5. **Phase 2 preparation** after Phase 1 validation

## Key Decisions Locked

- **DEC-035a:** Parser reuse pattern (thread-local, no locks)
- **DEC-036a:** Language dispatch strategy (enum-based, no strings in hot loop)
- **DEC-037a:** File read buffering (pre-alloc exact size)
- **Pattern:** Prepared statement caching (DEC-027) will reuse new patterns at scale

## Scaling Outlook

At 50K+ files: resolve phase becomes 100–200s offline bottleneck. Parallel resolve (#5) becomes high-ROI. Current micro-optimizations (Phase 1–2) will keep indexing rate healthy, but post-index resolve needs parallel work.

Updated by Scribe at 2026-04-01T16:50:00Z after leak fix, R2 validation, and roadmap analysis.



