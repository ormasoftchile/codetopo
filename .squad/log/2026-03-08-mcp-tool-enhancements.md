# Session: MCP Tool Enhancements

**Date:** 2026-03-08

## What Happened

1. Cristián brought `checkthis.md` containing 10 query API proposals for the MCP server.
2. Dijkstra analyzed all 10 against the existing 18 registered MCP tools.
3. Finding: 9 of 10 proposals are already covered by existing tools (`symbol_search`, `context_for`, `callers_approx`, `callees_approx`, `subgraph`, `shortest_path`, `impact_of`, `references`, `entrypoints`). Only `find_implementations` (#8) is genuinely missing.
4. Dijkstra recommended: 1 new tool + 5 enhancements to existing tools (add missing filter/grouping params).
5. Cristián approved all 6 enhancements.
6. Booch assigned to implement in `src/mcp/tools.h` and `src/mcp/tools.cpp`.
7. Lambert assigned to write tests for all 6 enhancements.

## Decisions Made

- Do NOT create 10 duplicate tools with different names — enhance existing ones instead.
- `find_implementations` is the only new tool (infrastructure already exists in DB).
- See `.squad/decisions.md` for full decision entries.
