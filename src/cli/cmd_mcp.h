#pragma once

#include "core/config.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/workspace.h"
#include "mcp/server.h"
#include "mcp/tools.h"
#include "util/log.h"
#include "util/process.h"
#include "watch/watcher.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

namespace codetopo {

// R8: Manages a spawned child indexer — deduplicates rapid triggers, monitors completion.
struct ReindexState {
    std::atomic<bool> running{false};
    std::atomic<bool> queued{false};
    std::thread monitor_thread;

    ~ReindexState() {
        // Detached threads manage their own lifetime; if still joinable, detach.
        if (monitor_thread.joinable()) monitor_thread.detach();
    }

    void trigger(const std::string& root, const std::string& db,
                 std::function<void()> on_complete) {
        if (running.exchange(true)) {
            queued = true;  // collapse into next run
            mcp_log("reindex: already running, queued");
            return;
        }
        if (monitor_thread.joinable()) monitor_thread.detach();
        monitor_thread = std::thread([=, this]() {
            do {
                queued = false;
                auto started = std::chrono::steady_clock::now();
                mcp_log("reindex: started");
                auto exe = get_self_executable_path();
                int rc = spawn_and_wait(exe,
                    {"index", "--root", root, "--db", db, "--supervised"});
                auto elapsed = std::chrono::steady_clock::now() - started;
                if (rc == 0) {
                    mcp_log("reindex: done (" + format_duration_seconds(elapsed) + ")");
                    if (on_complete) on_complete();
                } else {
                    mcp_log("reindex: failed (" + format_duration_seconds(elapsed)
                            + ", exit=" + std::to_string(rc) + ")");
                }
            } while (queued.load());
            running = false;
        });
        monitor_thread.detach();
    }
};

// T075: Wire cmd_mcp — start MCP server over stdio.
// R8+R9: Updated to accept freshness policy and debounce, with startup reconciliation.
// P2: --watch flag embeds filesystem watcher for auto-reindex.
inline int run_mcp(const std::string& db_path, const std::string& root_hint,
                   int tool_timeout, int idle_timeout,
                   FreshnessPolicy freshness = FreshnessPolicy::normal,
                   int debounce_ms = 1000,
                   bool watch = false,
                   const std::string& trajectory_log = "") {
    namespace fs = std::filesystem;

    // Warn about legacy workspace.sqlite — it is no longer used.
    {
        std::string legacy_ws = (fs::path(root_hint) / ".codetopo" / "workspace.sqlite").string();
        if (fs::exists(legacy_ws)) {
            mcp_log("warning: workspace.sqlite is no longer used. Run 'codetopo workspace add <path> --root "
                    + root_hint + "' to re-add extra roots into index.sqlite.");
        }
    }

    if (!fs::exists(db_path)) {
        mcp_log("error: database not found: " + db_path);
        return 1;
    }

    Connection conn(db_path, true);  // read-only

    // Schema version check
    int version = schema::get_schema_version(conn);
    if (version != CURRENT_SCHEMA_VERSION) {
        mcp_log("error: schema version mismatch (db=" + std::to_string(version)
                + " expected=" + std::to_string(CURRENT_SCHEMA_VERSION) + ")");
        return 3;
    }

    auto repo_root = schema::get_kv(conn, "repo_root", ".");

    // R8: Startup reconciliation — spawn codetopo index to catch up on missed changes.
    // Behavior depends on freshness policy (R9):
    //   eager:  block until child finishes — guaranteed fresh on first query
    //   normal: spawn in background — first queries may be stale, fresh within seconds
    //   lazy/off: skip startup reindex
    ReindexState reindex;
    if (freshness == FreshnessPolicy::eager) {
        auto started = std::chrono::steady_clock::now();
        mcp_log("reindex: started");
        auto exe = get_self_executable_path();
        int rc = spawn_and_wait(exe,
            {"index", "--root", repo_root, "--db", db_path, "--supervised"});
        if (rc != 0) {
            mcp_log("reindex: failed (" + format_duration_seconds(std::chrono::steady_clock::now() - started)
                    + ", exit=" + std::to_string(rc) + ")");
        } else {
            mcp_log("reindex: done (" + format_duration_seconds(std::chrono::steady_clock::now() - started)
                    + ")");
        }
        // QueryCache is constructed fresh with the connection below, so no clear() needed.
    } else if (freshness == FreshnessPolicy::normal) {
        reindex.trigger(repo_root, db_path, [&]() {
            // Note: The child wrote to the DB. Since we open conn read-only and
            // SQLite WAL allows concurrent readers, the next query will pick up
            // fresh data once the read transaction is renewed. QueryCache::clear()
            // would help if the MCP held long-lived prepared statements across
            // reindex boundaries — but the cache is per-Connection and the conn
            // is opened once, so clearing is a no-op here. Future watch mode (P2)
            // will need to integrate cache invalidation more tightly.
        });
    }
    // lazy and off: no startup reindex

    McpServer server(conn, repo_root, tool_timeout, idle_timeout);
    if (!trajectory_log.empty()) {
        server.set_trajectory_log(trajectory_log);
        mcp_log("trajectory: logging to " + trajectory_log);
    }

    // Register all tools with descriptions and parameter schemas
    server.register_tool("server_info", tools::server_info,
        "Get server capabilities, schema version, database path, and uptime.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("repo_stats", tools::repo_stats,
        "Get repository statistics: file count, symbol count, edge count, last index time.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("get_architecture", tools::get_architecture,
        "Summarize repository architecture from the indexed graph: clusters, hotspots, boundaries, and overall stats. Uses directory-based clustering with graph-driven cohesion and coupling metrics.",
        R"J({"type":"object","properties":{"scope":{"type":"string","description":"Optional file or directory prefix to scope the analysis (for example 'src/' or 'src/mcp')"},"aspects":{"type":"array","items":{"type":"string","enum":["clusters","hotspots","boundaries","summary"]},"description":"Sections to return. Omit for all sections."},"limit":{"type":"integer","description":"Max items per section (default 20, max 100)"}}})J");

    server.register_tool("file_search", tools::file_search,
        "Search for files by path pattern (GLOB syntax). Use to find files containing a keyword in their path, e.g. '*numa*' finds sosnumap.h. Supports wildcards: * matches any chars, ? matches one char, [abc] matches char class.",
        R"J({"type":"object","properties":{"pattern":{"type":"string","description":"GLOB pattern to match against file paths (e.g. '*numa*', 'Sql/DkTemp/sos/**/*.h')"},"language":{"type":"string","description":"Optional language filter (c, cpp, csharp, etc.)"},"limit":{"type":"integer","description":"Max results (default 50, max 500)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":["pattern"]})J");

    server.register_tool("dir_list", tools::dir_list,
        "List files and subdirectories in a given directory. Use to explore the neighborhood of a known file — find sibling source files in the same directory.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Directory path relative to repo root (e.g. 'Sql/DkTemp/sos/include')"},"limit":{"type":"integer","description":"Max entries returned (default 200, max 2000)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":["path"]})J");

    server.register_tool("dir_tree", tools::dir_tree,
        "Return full directory subtree up to depth N with file sizes and language. Use instead of repeated dir_list calls.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Root path to traverse (default: '.')"},"depth":{"type":"integer","description":"Max depth (default: 2, 0=unlimited)"},"max_files":{"type":"integer","description":"Cap visible file entries by truncating deepest directories first (default: 500)"}}})J");

    server.register_tool("symbol_search", tools::symbol_search,
        "Search for symbols (functions, classes, macros, variables) by name. Use query='*' with kind filter to list all symbols of a kind without FTS. Returns kind, name, file_path, span, and an internal node_id handle for chaining into other tools (context_for, callers_approx, impact_of). Never mention node_id values to the user — refer to symbols by name and file location instead.",
        R"J({"type":"object","properties":{"query":{"type":"string","description":"Symbol name or prefix to search for. Use '*' to list all (combine with kind filter)"},"kind":{"type":"string","description":"Filter by symbol kind: function, class, struct, variable, field, etc."},"file_pattern":{"type":"string","description":"Optional GLOB pattern to restrict results to specific file paths, e.g. '/Volumes/Projects/kibana/**' or 'src/mcp/*'"},"limit":{"type":"integer","description":"Max results (default 50, max 500)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":["query"]})J");

    server.register_tool("symbol_list", tools::symbol_list,
        "List and filter symbols without full-text search. Default output is listing-safe (kind, name, signature when present, file, start_line) and omits internal handles/spans. Responses include total_candidates, filtered_hidden, and hidden_public_count so kind/min_span filters are never silent; min_span_lines is lossy and public/API-like symbols bypass span pruning.",
        R"J({"type":"object","properties":{"kind":{"type":"string","description":"Filter by symbol kind: function, class, struct, variable, field, macro, etc."},"file_path":{"type":"string","description":"Filter to symbols in this file (relative path)"},"name_glob":{"type":"string","description":"SQLite GLOB pattern for symbol name (e.g. 'Get*', '*Handler')"},"compact":{"type":"boolean","description":"Legacy handle output shape when include_handles=true (collapses span to array)"},"include_handles":{"type":"boolean","description":"Opt in to internal node_id handles and spans for graph follow-up (default false)"},"fields":{"type":"array","items":{"type":"string"},"description":"Fields to include: node_id, name, qualname, kind, signature, start_line, end_line, span, file_path, file. Overrides default listing."},"min_span_lines":{"type":"integer","description":"Lossy minimum symbol span in lines; public/API-like symbols bypass this pruning and response metadata reports hidden counts"},"max_bytes":{"type":"integer","description":"Soft response budget before pagination (default 16000, 0 disables, max 100000)"},"limit":{"type":"integer","description":"Max results (default 200, max 2000)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":[]})J");

    server.register_tool("symbols_in_path", tools::symbols_in_path,
        "List symbols found under a directory path. Default output is listing-safe (kind, name, signature when present, file, start_line) and omits internal handles/spans. Responses include total_candidates, filtered_hidden, and hidden_public_count; min_span_lines is lossy and public/API-like symbols bypass span pruning.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Directory path to search under"},"kind":{"type":"array","items":{"type":"string"},"description":"Symbol kinds to include (empty = all)"},"recursive":{"type":"boolean","default":true},"compact":{"type":"boolean","description":"Legacy handle output shape when include_handles=true (collapses span to array)"},"include_handles":{"type":"boolean","description":"Opt in to internal node_id handles and spans for graph follow-up (default false)"},"fields":{"type":"array","items":{"type":"string"},"description":"Fields to include: node_id, name, qualname, kind, signature, start_line, end_line, span, file_path, file. Overrides default listing."},"min_span_lines":{"type":"integer","description":"Lossy minimum symbol span in lines; public/API-like symbols bypass this pruning and response metadata reports hidden counts","default":0},"max_bytes":{"type":"integer","description":"Soft response budget before pagination (default 16000, 0 disables, max 100000)"},"limit":{"type":"integer","default":200},"offset":{"type":"integer","default":0}}})J");

    server.register_tool("symbol_get", tools::symbol_get,
        "Get detailed information about a specific symbol by its internal node_id handle, including source code snippet. Do not mention node_id to the user.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id from symbol_search results"},"symbol":{"type":"string","description":"Symbol name (alternative to node_id)"},"file":{"type":"string","description":"File path relative to repo root (used with symbol)"}})J");

    server.register_tool("symbol_get_batch", tools::symbol_get_batch,
        "Get details for multiple symbols at once by their internal node_id handles. Do not mention node_ids to the user.",
        R"J({"type":"object","properties":{"node_ids":{"type":"array","items":{"type":"integer"},"description":"Array of node_ids to fetch"}},"required":["node_ids"])J");

    server.register_tool("callers_approx", tools::callers_approx,
        "Find exact callers from call graph edges. If exact callers are empty, also returns lean refs-backed candidate_results for overloaded/common member calls such as .set; use response_mode='lean' for grep-weight summaries. Use min_confidence to filter noise on generic method names (e.g. find, get) — try 0.5 or higher to get only attributed callers.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol to find callers for"},"min_confidence":{"type":"number","description":"Minimum confidence threshold (0.0–1.0). Use 0.5+ to filter noise on generic method names. Default: 0.0 (no filter)."},"group_by":{"type":"string","enum":["file","module","symbol"],"description":"Group exact results by file path, module (directory), or symbol name. Omit for flat list."},"compact":{"type":"boolean","description":"Compact output for exact results: use generic symbol fields, omit qualname=name, collapse span to [start,end], and use 'file' instead of 'file_path'."},"response_mode":{"type":"string","enum":["lean","verbose"],"description":"Use 'lean' for summary counts, buckets, and top-N callers only. Omit or 'verbose' preserves the legacy full response."},"lean":{"type":"boolean","description":"Shortcut for response_mode='lean'."},"top_n":{"type":"integer","description":"Lean-mode top caller/candidate rows (default 10, max 100)."},"buckets":{"type":"boolean","description":"Lean-mode candidate buckets by arity, heuristic, and file (default true)."},"include_candidates":{"type":"boolean","description":"When true, always add refs-backed candidate_results; when false, return exact edges only. Omitted means add candidates only if exact results are empty."},"mode":{"type":"string","enum":["exact","exact_then_candidates","exact_plus_candidates"],"description":"Candidate collection mode. Default exact_then_candidates keeps exact edges in results/groups and puts approximate refs matches in candidate_results only when exact edges are empty."},"receiver":{"type":"string","description":"Optional receiver type/text filter for candidates (e.g. LinkedMap); response includes candidate_total and candidate_filtered_hidden."},"include_handles":{"type":"boolean","description":"Opt in to candidate ref_id/caller_node_id/span/evidence fields. Default false keeps candidates lean."},"max_bytes":{"type":"integer","description":"Soft byte budget for candidate_results (default 16000, lean default 4096, 0 disables, max 100000)."},"limit":{"type":"integer","description":"Max exact results and candidate results (default 50, max 500)"}},"required":["node_id"]})J");

    server.register_tool("callees_approx", tools::callees_approx,
        "Find all functions/symbols that the given symbol calls or references. Optionally group results by file, module, or symbol.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol to find callees for"},"group_by":{"type":"string","enum":["file","module","symbol"],"description":"Group results by file path, module (directory), or symbol name. Omit for flat list."},"compact":{"type":"boolean","description":"Compact output: use generic symbol fields, omit qualname=name, collapse span to [start,end], and use 'file' instead of 'file_path'."}},"required":["node_id"]})J");

    server.register_tool("references", tools::references,
        "Find all references to a symbol across the codebase.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol to find references for"},"symbol":{"type":"string","description":"Symbol name (alternative to node_id)"},"file":{"type":"string","description":"File path relative to repo root (used with symbol)"}})J");

    server.register_tool("file_summary", tools::file_summary,
        "List all symbols defined in a file: functions, classes, structs, macros, variables.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Relative or absolute file path within the repository or workspace"},"file_path":{"type":"string","description":"Backward-compatible alias for path"},"limit":{"type":"integer","description":"Max symbols returned (default 200, max 2000)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":[]})J");

    server.register_tool("file_overview", tools::file_overview,
        "Structural overview of a file: top-level symbols with signatures and doc comments. Faster than context_for for large files.",
        R"J({"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"File path (relative or absolute)"}}})J");

    server.register_tool("context_for", tools::context_for,
        "Get the full structural context of a symbol: definition, source, exact callers/callees, container, siblings, and bases. Candidate callers are lean by default; receiver narrows approximate member-call sites and metadata reports hidden candidates.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol"},"symbol":{"type":"string","description":"Symbol name (alternative to node_id)"},"file":{"type":"string","description":"File path relative to repo root (used with symbol)"},"include_source":{"type":"boolean","description":"When false, omit the source field entirely. Defaults to true."},"max_source_lines":{"type":"integer","description":"Truncate source to at most N lines; 0 keeps the current full source behavior."},"max_callers":{"type":"integer","description":"Cap exact callers and candidate_callers returned after query collection; 0 disables the exact caller cap and caps candidates at 500. If omitted, preserves the current default behavior."},"max_callees":{"type":"integer","description":"Cap callees returned after query collection; 0 disables the cap. If omitted, preserves the current default behavior."},"include_candidates":{"type":"boolean","description":"When true, always add candidate_callers; when false, return exact callers only. Omitted means add candidates only if exact callers are empty."},"mode":{"type":"string","enum":["exact","exact_then_candidates","exact_plus_candidates"],"description":"Candidate collection mode for candidate_callers. Default exact_then_candidates."},"receiver":{"type":"string","description":"Optional receiver type/text filter for candidate_callers (e.g. LinkedMap); response includes candidate_callers_total and candidate_callers_filtered_hidden."},"include_handles":{"type":"boolean","description":"Opt in to candidate ref_id/caller_node_id/span/evidence fields. Default false keeps candidates lean."},"max_bytes":{"type":"integer","description":"Soft byte budget for candidate_callers (default 16000, 0 disables, max 100000)."}}})J");

    server.register_tool("context_by_name", tools::context_by_name,
        "Find a symbol by name and, when uniquely resolved, return the same full structural context as context_for. If multiple matches exist, returns a disambiguation list instead.",
        R"J({"type":"object","required":["name"],"properties":{"name":{"type":"string","description":"Symbol name to look up"},"file_pattern":{"type":"string","description":"Optional GLOB pattern to narrow by file path"}}})J");

    server.register_tool("entrypoints", tools::entrypoints,
        "Find entry point functions (main, DllMain, etc.) in the codebase. Optionally scope to a file path or directory prefix.",
        R"J({"type":"object","properties":{"scope":{"type":"string","description":"Optional file path or directory prefix to restrict results (e.g. 'src/mcp/' or 'src/cli/main.cpp')"},"limit":{"type":"integer","description":"Max results (default 20)"}},"required":[]})J");

    server.register_tool("impact_of", tools::impact_of,
        "Compute the blast radius of changing a symbol from exact graph edges. Candidate impact is first-hop only, lean by default, and can be narrowed by receiver with metadata for hidden candidates.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol to analyze"},"depth":{"type":"integer","description":"How many levels of transitive callers to follow (default 2, max 3)"},"max_nodes":{"type":"integer","description":"Max exact impacted nodes and candidate_impacted rows (default 50, max 200)"},"include_candidates":{"type":"boolean","description":"When true, always add candidate_impacted; when false, return exact impact only. Omitted means add candidates only if exact impact is empty."},"mode":{"type":"string","enum":["exact","exact_then_candidates","exact_plus_candidates"],"description":"Candidate collection mode for candidate_impacted. Default exact_then_candidates."},"receiver":{"type":"string","description":"Optional receiver type/text filter for candidate_impacted (e.g. LinkedMap); response includes candidate_impacted_total and candidate_impacted_filtered_hidden."},"include_handles":{"type":"boolean","description":"Opt in to candidate ref_id/caller_node_id/span/evidence fields. Default false keeps candidates lean."},"max_bytes":{"type":"integer","description":"Soft byte budget for candidate_impacted (default 16000, 0 disables, max 100000)."}},"required":["node_id"])J");

    server.register_tool("detect_changes", tools::detect_changes,
        "Given a git ref, list changed files and symbols, then walk reverse call edges to estimate blast radius for PR review.",
        R"J({"type":"object","properties":{"repo_root":{"type":"string","description":"Path to the git repository root"},"since":{"type":"string","description":"Git ref to diff against HEAD (branch, SHA, HEAD~N, etc.)"},"file_pattern":{"type":"string","description":"Optional GLOB filter for changed files (e.g. 'src/**/*.go')"},"depth":{"type":"integer","description":"BFS depth for impacted callers (default 2, max 5)"},"min_confidence":{"type":"number","description":"Minimum calls-edge confidence to follow (default 0.5)"}},"required":["repo_root","since"]})J");

    server.register_tool("file_deps", tools::file_deps,
        "Get file-level dependencies: which files does a file include/import, and which files include it.",
        R"J({"type":"object","properties":{"file_path":{"type":"string","description":"Relative file path"}},"required":["file_path"])J");

    server.register_tool("subgraph", tools::subgraph,
        "Extract a local dependency neighborhood graph around one or more seed symbols. Optionally filter by edge kinds.",
        R"J({"type":"object","properties":{"seed_symbols":{"type":"array","items":{"type":"integer"},"description":"Array of node_ids to use as seeds"},"depth":{"type":"integer","description":"Graph traversal depth (default 2)"},"edge_kinds":{"oneOf":[{"type":"string"},{"type":"array","items":{"type":"string"}}],"description":"Filter traversal to specific edge kinds (e.g. 'calls', 'inherits', 'contains', 'references'). Omit to traverse all."}},"required":["seed_symbols"]})J");

    server.register_tool("shortest_path", tools::shortest_path,
        "Find the shortest dependency path between two symbols in the code graph. Supports multiple candidate paths and relation type filtering.",
        R"J({"type":"object","properties":{"src_id":{"type":"integer","description":"Source node_id"},"dst_id":{"type":"integer","description":"Destination node_id"},"max_paths":{"type":"integer","description":"Number of diverse paths to return (default 1, max 5). When >1, returns 'paths' array instead of single 'path'."},"relation_types":{"oneOf":[{"type":"string"},{"type":"array","items":{"type":"string"}}],"description":"Restrict traversal to specific edge kinds (e.g. 'calls', 'inherits'). Omit for all."}},"required":["src_id","dst_id"]})J");

    server.register_tool("find_implementations", tools::find_implementations,
        "Find types that implement or inherit from a given base type/interface. Uses 'inherits' edges in the code graph.",
        R"J({"type":"object","properties":{"symbol":{"type":"string","description":"Name of the base type, interface, or trait to find implementations of"},"limit":{"type":"integer","description":"Max results (default 50, max 500)"}},"required":["symbol"]})J");

    server.register_tool("method_fields", tools::method_fields,
        "List all 'this.X' field accesses (reads and writes) and outgoing calls made by a method, classified as calls_self (same class) or calls_external. Useful for understanding a TypeScript/JavaScript method's state usage.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the method symbol to analyze"},"symbol":{"type":"string","description":"Symbol name (alternative to node_id)"},"file":{"type":"string","description":"File path relative to repo root (used with symbol)"}}})J");

    server.register_tool("dependency_cluster", tools::dependency_cluster,
        "Group methods in a file by shared field access patterns, weighted by read/write direction. Returns clusters with extractability scores (1.0=pure read, easily extractable; 0.0=heavily writes state, tightly coupled). Use to plan refactoring decomposition.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"File path (relative to repo root)"},"class_id":{"type":"integer","description":"Node ID of a class to analyze (alternative to path)"}}})J");

    server.register_tool("source_at", tools::source_at,
        "Read source code lines from a file. Returns the raw source text for the specified line range. Use when you need to see code at specific locations without knowing the symbol node_id.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Relative file path from repository root"},"start_line":{"type":"integer","description":"First line to read (1-based)"},"end_line":{"type":"integer","description":"Last line to read (1-based, inclusive)"}},"required":["path","start_line","end_line"]})J");

    server.register_tool("code_search", tools::code_search,
        "Search for arbitrary text patterns across all source file contents. Uses a trigram index for fast substring matching. Default is grep-like: file, line, and matched text with no context duplication; set context_lines to include surrounding lines.",
        R"J({"type":"object","properties":{"query":{"type":"string","description":"Text to search for (minimum 3 characters). Matches arbitrary substrings in source code."},"file_pattern":{"type":"string","description":"Optional GLOB pattern to restrict search to specific file paths (e.g. '*.cpp', 'src/mcp/*')"},"limit":{"type":"integer","description":"Max files to return (default 50, max 500)"},"context_lines":{"type":"integer","description":"Lines of context around each match (default 0, max 5). When context is returned, matched text is not duplicated in a separate text field."},"max_bytes":{"type":"integer","description":"Soft response budget (default 16000, 0 disables, max 100000)"},"case_sensitive":{"type":"boolean","description":"Case-sensitive matching (default false)"}},"required":["query"]})J");

    server.register_tool("list_http_calls", tools::list_http_calls,
        "List extracted HTTP client call refs with their URL paths. Use to inspect protocol-aware refs captured during indexing.",
        R"J({"type":"object","properties":{"file_pattern":{"type":"string","description":"Optional GLOB filter for file paths"},"limit":{"type":"integer","description":"Max results (default 100, max 500)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}}})J");

    server.register_tool("reindex",
        [&reindex, &repo_root, &db_path](yyjson_val* /*params*/, Connection& /*conn*/,
                                          QueryCache& cache, const std::string& /*root*/) -> std::string {
            reindex.trigger(repo_root, db_path, [&cache]() {
                cache.clear();
            });
            return R"({"status":"started","message":"Re-indexing in background. Queries will reflect updated state once complete."})";
        },
        "Trigger a re-index of the repository. Runs in the background — subsequent tool calls will use fresh data once complete. Call this after making file changes (renames, moves, extractions) to ensure the index is up to date.",
        R"J({"type":"object","properties":{}})J");

    // Workspace tools — multi-root management
    server.register_tool("workspace_add", tools::workspace_add,
        "Add a directory root to the multi-root workspace. Indexes the target if needed, then merges into index.sqlite with globally unique node IDs.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Absolute path to the directory to add as a workspace root"}},"required":["path"]})J");

    server.register_tool("workspace_remove", tools::workspace_remove,
        "Remove a root from the multi-root workspace. Cascade-deletes all files, symbols, and edges for that root.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Absolute path of the workspace root to remove"}},"required":["path"]})J");

    server.register_tool("workspace_list", tools::workspace_list,
        "List all roots in the multi-root workspace with file/symbol/edge counts.",
        R"J({"type":"object","properties":{}})J");

    mcp_log("codetopo mcp started");
    mcp_log("db: " + db_path);
    mcp_log("repo: " + repo_root);
    mcp_log("schema: v" + std::to_string(version) + "  tools: " + std::to_string(server.tool_count()));

    // P2: Start filesystem watcher for auto-reindex when --watch is enabled.
    // Cross-thread contract: watcher thread -> reindex monitor thread -> atomic flag
    //   -> main thread picks up flag before next tool dispatch.
    // CRITICAL: Filter out .codetopo/ and .git/ (except .git/HEAD) events to avoid
    // infinite reindex loop (indexer writes to .codetopo/index.sqlite which the
    // watcher would see as a change, triggering another reindex, ad infinitum).
    std::unique_ptr<Watcher> watcher;
    if (watch && freshness != FreshnessPolicy::off) {
        auto debounce = std::chrono::milliseconds(debounce_ms);
        watcher = std::make_unique<Watcher>(
            repo_root,
            [&](const std::vector<WatchEvent>& events) {
                // Filter: ignore .codetopo/ writes (our own DB) and .git/ internals
                // (except .git/HEAD which signals branch switch)
                bool has_relevant = false;
                for (const auto& ev : events) {
                    auto s = ev.path.generic_string();
                    if (s.find(".codetopo/") != std::string::npos) continue;
                    if (s.find(".codetopo\\") != std::string::npos) continue;
                    if (ev.type == FileEvent::BranchSwitch) { has_relevant = true; break; }
                    // Skip .git/ internals (pack files, index, refs updates, etc.)
                    if (s.find(".git/") != std::string::npos || s.find(".git\\") != std::string::npos) continue;
                    has_relevant = true;
                    break;
                }
                if (!has_relevant) return;

                mcp_log("watcher: change detected, triggering reindex");
                reindex.trigger(repo_root, db_path, [&]() {
                    server.request_refresh();
                    mcp_log("watcher: reindex complete, cache invalidated");
                });
            },
            debounce
        );
        watcher->start();
        mcp_log("watcher: started (" + std::to_string(debounce_ms) + "ms debounce)");
    }

    int rc = server.run();

    // Watcher::~Watcher() calls stop(), but be explicit about shutdown order.
    if (watcher) {
        watcher->stop();
        mcp_log("watcher: stopped");
    }
    mcp_log("codetopo mcp stopped (rc=" + std::to_string(rc) + ")");
    return rc;
}

// T076: Wire cmd_query — CLI wrapper for tool invocations.
inline int run_query(const std::string& db_path, const std::string& tool_name,
                      const std::string& params_json) {
    namespace fs = std::filesystem;

    if (!fs::exists(db_path)) {
        mcp_log("error: database not found: " + db_path);
        return 1;
    }

    Connection conn(db_path, true);
    auto repo_root = schema::get_kv(conn, "repo_root", ".");
    QueryCache cache(conn);

    // Parse params
    auto doc = json_parse(params_json);
    yyjson_val* params = doc ? doc.root() : nullptr;

    // Find the tool
    using ToolFn = std::function<std::string(yyjson_val*, Connection&, QueryCache&, const std::string&)>;
    std::unordered_map<std::string, ToolFn> all_tools = {
        {"server_info", tools::server_info},
        {"repo_stats", tools::repo_stats},
        {"get_architecture", tools::get_architecture},
        {"file_search", tools::file_search},
        {"dir_list", tools::dir_list},
        {"dir_tree", tools::dir_tree},
        {"symbol_search", tools::symbol_search},
        {"symbol_list", tools::symbol_list},
        {"symbols_in_path", tools::symbols_in_path},
        {"symbol_get", tools::symbol_get},
        {"symbol_get_batch", tools::symbol_get_batch},
        {"callers_approx", tools::callers_approx},
        {"callees_approx", tools::callees_approx},
        {"references", tools::references},
        {"file_summary", tools::file_summary},
        {"file_overview", tools::file_overview},
        {"context_for", tools::context_for},
        {"context_by_name", tools::context_by_name},
        {"entrypoints", tools::entrypoints},
        {"impact_of", tools::impact_of},
        {"detect_changes", tools::detect_changes},
        {"file_deps", tools::file_deps},
        {"method_fields", tools::method_fields},
        {"subgraph", tools::subgraph},
        {"shortest_path", tools::shortest_path},
        {"find_implementations", tools::find_implementations},
        {"code_search", tools::code_search},
        {"list_http_calls", tools::list_http_calls},
        {"workspace_add", tools::workspace_add},
        {"workspace_remove", tools::workspace_remove},
        {"workspace_list", tools::workspace_list},
    };

    auto it = all_tools.find(tool_name);
    if (it == all_tools.end()) {
        mcp_log("error: unknown tool: " + tool_name);
        return 2;
    }

    try {
        std::string result = it->second(params, conn, cache, repo_root);
        std::cout << result << "\n";
        return 0;
    } catch (const std::exception& e) {
        mcp_log("error: " + truncate_for_log(e.what()));
        return 1;
    }
}

} // namespace codetopo
