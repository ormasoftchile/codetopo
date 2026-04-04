# Session Log — Parse+Extract Profiling (2026-04-01T16:00Z)

**Agent:** Otho  
**Session:** Profile parse+extract bottleneck after DEC-032 persist optimization

## Summary

Profiled 4 subsets (500–4145 files, 512KB–1MB max-file-size) with 16-thread worker pool. Key finding: **persist remains the bottleneck**, not workers as expected. Workers can deliver 788 files/s; persist caps at 617 files/s. Contention (35% of wall) from bursty arrival due to largest-first sort.

## Metrics

| Subset | Files | Wall | Rate | Persist % | Contention % |
|--------|-------|------|------|-----------|--------------|
| tiny   | 500   | 3.5s | 141/s | 73% | 3% |
| small  | 2000  | 9.7s | 206/s | 50% | 3% |
| fsm    | 4145  | 22.5s| 185/s | 30% | 35% |
| fsm@1MB| 4145  | 34.0s| 122/s | 31% | 22% |

## Top Findings

1. Persist still #1 bottleneck (workers 788 files/s, persist 617)
2. Contention 35% from bursty arrival (largest-first sort wave effects)
3. ThreadPool 64MB stacks (DEC-021 never applied) = 1.15GB committed memory
4. Parse 46-70% of worker time (tree-sitter irreducible cost)
5. file_read 9.4x slower at 1024KB (validates 512KB default)

## Recommendations

- **R1 (HIGH):** Fix ThreadPool stack 64→8MB (1-line fix)
- **R2 (HIGH):** Pipelined persist thread (eliminate contention)
- **R3 (MEDIUM):** Parallel WAL persist (if R2 insufficient)

## Decision

DEC-034 created with full analysis and ranked recommendations.

---

*Brief profiling session report. Full details in DEC-034.*
