---
updated_at: 2026-04-01T16:30:00Z
focus_area: Persist pipeline decoupled; workers now true bottleneck at 207 files/s
active_issues: File node leak on re-persist (low severity, Joan found)
---

# What We're Focused On

DEC-034 R1+R2 complete (Grag & Joan). **Status: Persist pipeline decoupled from main thread.**

**Achievements:**
- R1 (ThreadPool stacks 64→8MB): One-liner, saves 1.15GB committed RAM
- R2 (Pipelined persist thread): Dedicated writer thread, batch drains queue, progress on persist thread
- Tests: 208 tests (1112 assertions), all green. Joan found file node leak on re-persist (low severity).
- Benchmark: 207 files/s on fsm (4145 files), 20s wall

**New bottleneck**: Workers (file_read 73.6%, parse 17.2%, extract 9.2%). Parse is irreducible tree-sitter cost.

**Next actions:**
1. **Prepared statement caching (DEC-027 pattern):** Estimated drop persist 4ms → 1.5ms/file (+166% throughput). High ROI.
2. **File node leak fix:** Optional (low severity, current usage unaffected). Candidate for separate bug fix or DEC-027 phase.
3. **Parallel WAL persist (DEC-026 R7):** May be unnecessary after stmt caching. Re-profile after caching.

**Profiler insights:** Main thread now idle (waiting for workers, high contention % healthy). Extract timeout working correctly (3-6ms/file, 20-28% of worker time). Large files (1024KB) hit I/O limits; 512KB default optimal.

**Key decision:** Prepared statement caching is next high-impact optimization. Profile again after implementation to confirm worker efficiency gains.

Updated by Scribe at 2026-04-01T16:30:00Z after DEC-034 R1+R2 completion.



