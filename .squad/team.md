# Squad Team

> codetopo — MCP server in C++ for indexing symbols and relations across large codebases

## Coordinator

| Name | Role | Notes |
|------|------|-------|
| Squad | Coordinator | Routes work, enforces handoffs and reviewer gates. |

## Members

| Name | Role | Scope | Emoji |
|------|------|-------|-------|
| Simon | Lead / Architect | Architecture, code review, system design, decisions | 🏗️ |
| Grag | Systems Engineer | C++ core, threading, concurrency, file I/O, data structures, indexer pipeline | 🔧 |
| Otho | Performance Engineer | Arenas, memory management, optimization, benchmarking, ML modeling | ⚡ |
| Joan | Tester / QA | Validity tests, efficiency scenarios, regression, crash reproduction | 🧪 |
| Scribe | Session Logger | Memory, decisions, session logs | 📋 |
| Ralph | Work Monitor | Work queue, backlog, keep-alive | 🔄 |

## Project Context

- **Project:** codetopo
- **Stack:** C++20, CMake, vcpkg, SQLite, tree-sitter, MCP protocol (JSON-RPC over stdio)
- **User:** Cristiano
- **Created:** 2026-03-30
- **Universe:** Captain Future
- **Goal:** Fast, robust symbol/relation indexing for codebases with 100k+ files; exposes MCP tools so GitHub Copilot can query structural code topology instead of text search
