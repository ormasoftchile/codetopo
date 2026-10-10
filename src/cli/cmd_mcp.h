#pragma once

#include "core/config.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/workspace.h"
#include "index/ownership.h"
#include "mcp/reindex.h"
#include "mcp/server.h"
#include "mcp/tools.h"
#include "util/log.h"
#include "util/lock.h"
#include "util/git.h"
#include "index/worktree_reconcile.h"
#include "watch/watcher.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace codetopo {

inline std::vector<std::string> split_git_paths(const std::string& output) {
    std::vector<std::string> paths;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) paths.push_back(line);
    }
    return paths;
}

inline void append_unique_paths(std::vector<std::string>& paths,
                                std::unordered_set<std::string>& seen,
                                const std::vector<std::string>& add) {
    for (const auto& path : add) {
        if (seen.insert(path).second) paths.push_back(path);
    }
}

inline std::optional<std::string> repo_relative_watch_path(const std::filesystem::path& root,
                                                           const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    fs::path rel;
    if (path.is_absolute()) {
        auto norm_root = root.lexically_normal();
        auto norm_path = path.lexically_normal();
        auto root_s = norm_root.generic_string();
        auto path_s = norm_path.generic_string();
        if (path_s != root_s &&
            (path_s.size() <= root_s.size() || path_s.compare(0, root_s.size(), root_s) != 0 ||
             path_s[root_s.size()] != '/')) {
            return std::nullopt;
        }
        rel = norm_path.lexically_relative(norm_root);
    } else {
        rel = path.lexically_normal();
    }
    auto rel_s = rel.generic_string();
    while (rel_s.rfind("./", 0) == 0) rel_s.erase(0, 2);
    if (rel_s.empty() || rel_s == "." || rel_s == ".." || rel_s.rfind("../", 0) == 0)
        return std::nullopt;
    return rel_s;
}

inline std::vector<std::string> git_changed_paths_for_heads(const std::string& repo_root,
                                                            const std::string& old_head,
                                                            const std::string& new_head,
                                                            bool* empty_head_diff = nullptr) {
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;
    if (!old_head.empty() && !new_head.empty() && old_head != new_head) {
        auto head_paths = split_git_paths(
            git_command(repo_root, "diff --name-only " + old_head + " " + new_head));
        if (head_paths.empty() && empty_head_diff) *empty_head_diff = true;
        append_unique_paths(paths, seen, head_paths);
    }
    append_unique_paths(paths, seen,
        split_git_paths(git_command(repo_root, "diff --name-only")));
    append_unique_paths(paths, seen,
        split_git_paths(git_command(repo_root, "diff --name-only --cached")));
    return paths;
}

// T075: Wire cmd_mcp — start MCP server over stdio.
// R8+R9: Updated to accept freshness policy and debounce, with startup reconciliation.
// P2: --watch flag embeds filesystem watcher for auto-reindex.
inline int run_mcp(const std::string& db_path, const std::string& root_hint,
                   int tool_timeout, int idle_timeout,
                   FreshnessPolicy freshness = FreshnessPolicy::normal,
                   int debounce_ms = 1000,
                   bool watch = false,
                   const std::string& trajectory_log = "",
                   bool root_was_explicit = true) try {
    namespace fs = std::filesystem;
    const auto startup_started = std::chrono::steady_clock::now();
    const auto startup_wall_time = std::chrono::system_clock::now();
    mcp_log("lifecycle: startup");

    if (!fs::exists(db_path)) {
        // Zero-config worktree intelligence: if database does not exist, but
        // root_hint is a linked Git worktree whose primary repo has an existing index,
        // automatically bootstrap the worktree index from the primary repository.
        auto wt_info = resolve_worktree_info(root_hint);
        if (wt_info.is_worktree) {
            std::string primary_db = default_db(wt_info.primary_repo_root.string());
            if (fs::exists(primary_db)) {
                mcp_log("lifecycle: auto-bootstrapping worktree index from primary " +
                        wt_info.primary_repo_root.string());
                auto res = bootstrap_worktree_index(root_hint);
                if (res.success) {
                    mcp_log("lifecycle: worktree index ready (" +
                            std::to_string(res.files_changed) + " files reconciled)");
                } else {
                    mcp_log("warning: worktree bootstrap failed: " + res.error);
                }
            }
        }
    }

    if (!fs::exists(db_path)) {
        mcp_log("shutdown: startup_error database not found: " + db_path);
        return 1;
    }
    std::error_code db_ec;
    auto canonical_db = fs::canonical(db_path, db_ec);
    if (db_ec) {
        mcp_log("shutdown: startup_error cannot canonicalize database: " + db_path);
        return 1;
    }
    std::string resolved_db_path = canonical_db.string();

    index_ownership::RootResolution root_resolution;
    {
        Connection probe(resolved_db_path, true);
        root_resolution = index_ownership::resolve_primary_root(
            probe, root_hint, root_was_explicit, resolved_db_path);
    }
    std::string repo_root = root_resolution.root.string();
    auto log_directory = root_resolution.root / ".codetopo" / "logs";
    std::error_code log_ec;
    fs::create_directories(log_directory, log_ec);
    if (log_ec) {
        mcp_log("warning: cannot create diagnostic log directory: " +
                log_directory.string() + ": " + log_ec.message());
    }
    auto diagnostic_path = log_directory /
        ("mcp-" + std::to_string(get_current_process_id()) + ".log");
    ScopedMcpLogFile diagnostic_log(diagnostic_path);
    mcp_log("lifecycle: root_resolved repo=" + repo_root + " db=" + resolved_db_path);
    if (diagnostic_log.enabled()) {
        mcp_log("diagnostics: " + diagnostic_path.string());
    }
    mcp_log("startup: checking schema metadata");

    // Warn about legacy workspace.sqlite — it is no longer used.
    {
        std::string legacy_ws =
            (root_resolution.root / ".codetopo" / "workspace.sqlite").string();
        if (fs::exists(legacy_ws)) {
            mcp_log("warning: workspace.sqlite is no longer used. Run 'codetopo workspace add <path> --root "
                    + repo_root + "' to re-add extra roots into index.sqlite.");
        }
    }

    // Schema version: migrate an older DB IN PLACE here instead of forcing a full
    // reindex. Recent schema bumps only widen a CHECK / add an index / recreate a
    // small table, so the migration is cheap even for very large workspaces —
    // nodes and edges are left untouched. Without this, a version bump would make
    // the MCP server refuse to start on an existing index until it was rebuilt.
    {
        int version = 0;
        {
            Connection probe(resolved_db_path, true);  // read-only
            version = schema::get_schema_version(probe);
        }
        if (version > CURRENT_SCHEMA_VERSION) {
            mcp_log("shutdown: startup_error schema version mismatch (db=" + std::to_string(version)
                    + " expected=" + std::to_string(CURRENT_SCHEMA_VERSION)
                    + ") — database is newer than this binary");
            return 3;
        }
        // Only migrate versions the incremental migration path understands (>=3).
        // Older/empty DBs would require a destructive rebuild, so leave those to
        // the read-only guard below (the user should reindex).
        if (version >= 3 && version < CURRENT_SCHEMA_VERSION) {
            auto lock_path = resolved_db_path;
            lock_path += ".lock";
            FileLock lock(lock_path);
            if (lock.acquire()) {
                mcp_log("schema: migrating db=" + std::to_string(version) + " -> "
                        + std::to_string(CURRENT_SCHEMA_VERSION) + " in place (no reindex)");
                {
                    Connection wconn(resolved_db_path);  // read-write
                    int rc = schema::ensure_schema(wconn);
                    if (rc != 0) {
                        lock.release();
                        mcp_log("shutdown: startup_error schema migration failed (db=" + std::to_string(version)
                                + " expected=" + std::to_string(CURRENT_SCHEMA_VERSION) + ")");
                        return 3;
                    }
                }
                lock.release();
                mcp_log("schema: migration done");
            } else {
                // Another indexer holds the lock and will migrate — wait for it.
                mcp_log("schema: waiting for concurrent indexer (PID "
                        + std::to_string(lock.holder_pid()) + ") to migrate");
                for (int i = 0; i < 300 && version != CURRENT_SCHEMA_VERSION; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    Connection probe(resolved_db_path, true);
                    version = schema::get_schema_version(probe);
                }
            }
        }
    }

    Connection conn(resolved_db_path, true);  // read-only

    // Schema version check
    int version = schema::get_schema_version(conn);
    if (version != CURRENT_SCHEMA_VERSION) {
        mcp_log("shutdown: startup_error schema version mismatch (db=" + std::to_string(version)
                + " expected=" + std::to_string(CURRENT_SCHEMA_VERSION) + ")");
        return 3;
    }

    root_resolution = index_ownership::resolve_primary_root(
        conn, repo_root, true, resolved_db_path);
    repo_root = root_resolution.root.string();
    if (!root_resolution.metadata_present) {
        mcp_log("index ownership: repo_root metadata missing; the configured --root is authoritative "
                "and the first full reconciliation will not prune deletions");
    }
    std::string last_known_head = get_git_head(repo_root);
    if (last_known_head.empty()) {
        last_known_head = schema::get_kv(conn, "git_head", "");
    }

    // R8: Startup reconciliation — spawn codetopo index to catch up on missed changes.
    // Behavior depends on freshness policy (R9):
    //   eager/normal: background child; graph tools read the last committed snapshot.
    //   lazy/off: skip startup reindex
    std::mutex writer_gate;
    McpServer server(conn, repo_root, tool_timeout, idle_timeout, &writer_gate);
    ReindexState reindex;
    std::atomic<bool> ownership_established{root_resolution.metadata_present};
    reindex.writer_gate = &writer_gate;
    server.set_indexing_flag(&reindex.running);
    if (freshness_reconciles_on_startup(freshness)) {
        reindex.trigger(repo_root, resolved_db_path,
            [&]() {
                ownership_established = true;
                server.request_refresh();
            }, {}, true, ReindexReason::startup, startup_wall_time);
    }
    // lazy and off: no startup reindex

    if (!trajectory_log.empty()) {
        server.set_trajectory_log(trajectory_log);
        mcp_log("trajectory: logging to " + trajectory_log);
    }

    // Register all tools with descriptions and parameter schemas
    auto register_node_tool = [&server](const std::string& name, ToolHandler handler,
                                        const std::string& description, const std::string& schema_json) {
        auto parsed = json_parse(schema_json);
        JsonMutDoc doc;
        auto* root = yyjson_val_mut_copy(doc.doc, parsed.root());
        doc.set_root(root);
        auto* properties = yyjson_mut_obj_get(root, "properties");
        for (const char* key : {"symbol", "file"}) {
            auto* property = doc.new_obj();
            yyjson_mut_obj_add_str(doc.doc, property, "type", "string");
            yyjson_mut_obj_add_int(doc.doc, property, "minLength", 1);
            yyjson_mut_obj_add_str(doc.doc, property, "description",
                std::string(key) == "symbol"
                    ? "Exact symbol name or qualified name, used with file. Multiple definitions return ambiguous/candidates; choose a handle."
                    : "Relative to repo root, or absolute indexed workspace file. Windows native, forward and mixed separators are equivalent.");
            yyjson_mut_obj_remove_key(properties, key);
            yyjson_mut_obj_add_val(doc.doc, properties, key, property);
        }
        if (name == "impact_of") {
            auto* confidence = doc.new_obj();
            yyjson_mut_obj_add_str(doc.doc, confidence, "type", "number");
            yyjson_mut_obj_add_real(doc.doc, confidence, "minimum", 0.0);
            yyjson_mut_obj_add_real(doc.doc, confidence, "maximum", 1.0);
            yyjson_mut_obj_add_str(doc.doc, confidence, "description",
                "Minimum indexed-edge confidence to traverse (default 0). "
                "Confidence is a heuristic score, not proof of a resolved receiver.");
            yyjson_mut_obj_add_val(doc.doc, properties, "min_confidence", confidence);
        }
        yyjson_mut_obj_add_int(doc.doc, yyjson_mut_obj_get(properties, "stable_key"), "minLength", 1);
        yyjson_mut_obj_add_int(doc.doc, yyjson_mut_obj_get(properties, "node_id"), "minimum", 0);
        auto alternatives = json_parse(R"({"anyOf":[{"required":["stable_key"]},{"required":["node_id"]},{"required":["symbol","file"]}]})");
        yyjson_mut_obj_add_val(doc.doc, root, "anyOf",
            yyjson_val_mut_copy(doc.doc, yyjson_obj_get(alternatives.root(), "anyOf")));
        server.register_tool(name, std::move(handler), description, doc.to_string());
    };

    server.register_tool("server_info", tools::server_info,
        "Get server capabilities, schema version, root ownership, and explicit index freshness state.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("repo_stats", tools::repo_stats,
        "Get repository metadata and file counts. Graph totals are null (not computed), never global node/edge scans.",
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
        "Search for symbols (functions, classes, macros, variables) by name. Multi-term queries default to match=any (OR) and rank symbols matching more terms higher; set match=all for AND. Use query='*' with kind filter to list all symbols of a kind without FTS. Returns kind, name, file_path, span, node_id, and stable_key. Prefer stable_key for chaining because it is reindex-proof; node_id is a volatile rowid. Never mention node_id values to the user — refer to symbols by name and file location instead.",
        R"J({"type":"object","properties":{"query":{"type":"string","description":"Symbol name or prefix to search for. Use '*' to list all (combine with kind filter)"},"kind":{"type":"string","description":"Filter by symbol kind: function, class, struct, variable, field, etc."},"file_pattern":{"type":"string","description":"Optional GLOB pattern to restrict results to specific file paths, e.g. '/Volumes/Projects/kibana/**' or 'src/mcp/*'"},"match":{"type":"string","enum":["any","all"],"description":"How to combine whitespace-separated query terms. Default any uses OR and ranks more term matches higher; all uses AND."},"limit":{"type":"integer","description":"Max results (default 50, max 500)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":["query"]})J");

    server.register_tool("symbol_list", tools::symbol_list,
        "List and filter symbols without full-text search. Default output is listing-safe (kind, name, signature when present, file, start_line) and omits internal handles/spans. Responses always include total_candidates, filtered_hidden, and hidden_public_count so kind/min_span filters are never silent; min_span_lines is lossy, public/API-like symbols bypass span pruning, and explicit kind=method keeps interface/abstract methods visible.",
        R"J({"type":"object","properties":{"kind":{"type":"string","description":"Filter by symbol kind: function, class, struct, variable, field, macro, etc."},"file_path":{"type":"string","description":"Filter to symbols in this file (relative path)"},"name_glob":{"type":"string","description":"SQLite GLOB pattern for symbol name (e.g. 'Get*', '*Handler')"},"compact":{"type":"boolean","description":"Legacy handle output shape when include_handles=true (collapses span to array)"},"include_handles":{"type":"boolean","description":"Opt in to internal node_id handles and spans for graph follow-up (default false)"},"fields":{"type":"array","items":{"type":"string"},"description":"Fields to include: node_id, name, qualname, kind, signature, start_line, end_line, span, file_path, file. Overrides default listing."},"min_span_lines":{"type":"integer","description":"Lossy minimum symbol span in lines; public/API-like symbols bypass this pruning and response metadata reports hidden counts"},"max_bytes":{"type":"integer","description":"Soft response budget before pagination (default 16000, 0 disables, max 100000)"},"limit":{"type":"integer","description":"Max results (default 200, max 2000)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":[]})J");

    server.register_tool("symbols_in_path", tools::symbols_in_path,
        "List symbols found under a directory path. Default output is listing-safe (kind, name, signature when present, file, start_line) and omits internal handles/spans. Responses always include total_candidates, filtered_hidden, and hidden_public_count; min_span_lines is lossy, public/API-like symbols bypass span pruning, and explicit kind=method keeps interface/abstract methods visible.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Directory path to search under"},"kind":{"type":"array","items":{"type":"string"},"description":"Symbol kinds to include (empty = all)"},"recursive":{"type":"boolean","default":true},"compact":{"type":"boolean","description":"Legacy handle output shape when include_handles=true (collapses span to array)"},"include_handles":{"type":"boolean","description":"Opt in to internal node_id handles and spans for graph follow-up (default false)"},"fields":{"type":"array","items":{"type":"string"},"description":"Fields to include: node_id, name, qualname, kind, signature, start_line, end_line, span, file_path, file. Overrides default listing."},"min_span_lines":{"type":"integer","description":"Lossy minimum symbol span in lines; public/API-like symbols bypass this pruning and response metadata reports hidden counts","default":0},"max_bytes":{"type":"integer","description":"Soft response budget before pagination (default 16000, 0 disables, max 100000)"},"limit":{"type":"integer","default":200},"offset":{"type":"integer","default":0}}})J");

    register_node_tool("symbol_get", tools::symbol_get,
        "Get detailed information about a specific symbol by stable_key (preferred, reindex-proof) or node_id (volatile rowid), including source code snippet. Do not mention node_id to the user.",
        R"J({"type":"object","properties":{"stable_key":{"type":"string","description":"Preferred reindex-proof symbol handle returned by symbol_search/context_for. When provided, node_id is ignored."},"node_id":{"type":"integer","description":"Volatile node_id from symbol_search results; changes on reindex"},"expected_stable_key":{"type":"string","description":"Optional guard for node_id: if the current row's stable_key differs, returns invalid_input instead of using a stale/reused id."}}})J");

    server.register_tool("symbol_get_batch", tools::symbol_get_batch,
        "Get details for multiple symbols at once by their internal node_id handles. Do not mention node_ids to the user.",
        R"J({"type":"object","properties":{"node_ids":{"type":"array","items":{"type":"integer"},"description":"Array of node_ids to fetch"}},"required":["node_ids"])J");

    register_node_tool("callers_approx", tools::callers_approx,
        "Find exact callers from call graph edges. Defaults to response_mode='lean' for summary counts, buckets, and top-N exact/candidate rows; use response_mode='full' (or 'verbose'/verbose=true) for the legacy full dump. If exact symbol callers are empty, refs-backed candidate_results are always returned for overloaded/common member calls such as .set, even when exact-only knobs are present. Receiver-type resolution depends on local type annotations in the indexed codebase; metadata includes resolution_status, candidate_total, candidate_eligible, and receiver_type_hits so unresolved attribution is distinct from no callers. Use min_confidence to filter noise on generic method names (e.g. find, get) — try 0.5 or higher to get only attributed callers.",
        R"J({"type":"object","properties":{"stable_key":{"type":"string","description":"Preferred reindex-proof symbol handle. When provided, node_id is ignored."},"node_id":{"type":"integer","description":"Volatile node_id of the symbol to find callers for"},"expected_stable_key":{"type":"string","description":"Optional guard for node_id staleness/reuse."},"min_confidence":{"type":"number","description":"Minimum confidence threshold (0.0–1.0). Use 0.5+ to filter noise on generic method names. Default: 0.0 (no filter)."},"group_by":{"type":"string","enum":["file","module","symbol"],"description":"Group exact results by file path, module (directory), or symbol name. Omit for flat list."},"compact":{"type":"boolean","description":"Compact output for exact results in full mode: use generic symbol fields, omit qualname=name, collapse span to [start,end], and use 'file' instead of 'file_path'."},"response_mode":{"type":"string","enum":["lean","full","verbose"],"description":"Default lean returns summary counts, buckets, and top-N callers/candidates. Use 'full' or 'verbose' for the legacy full response."},"lean":{"type":"boolean","description":"Shortcut for response_mode='lean'; lean=false opts into full when response_mode is omitted."},"verbose":{"type":"boolean","description":"Shortcut opt-out to the legacy full response when response_mode is omitted."},"top_n":{"type":"integer","description":"Lean-mode top caller/candidate rows (default 10, max 100)."},"buckets":{"type":"boolean","description":"Lean-mode candidate buckets by arity, heuristic, and file (default true)."},"include_candidates":{"type":"boolean","description":"When true, always add refs-backed candidate_results; when false, exact-empty fallback still returns candidate_results. Omitted means add candidates when exact symbol callers are empty."},"mode":{"type":"string","enum":["exact","exact_then_candidates","exact_plus_candidates"],"description":"Candidate collection mode. Default exact_then_candidates keeps exact edges in results/groups and puts approximate refs matches in candidate_results when exact symbol callers are empty; exact-empty fallback is always on."},"receiver":{"type":"string","description":"Optional receiver type/text filter for candidates (e.g. LinkedMap). Receiver-type resolution depends on local type annotations; response includes resolution_status, candidate_total, candidate_eligible, receiver_type_hits, and hidden counts."},"include_handles":{"type":"boolean","description":"Full-mode opt in to candidate ref_id/caller_node_id/span/evidence fields. Default false keeps candidates lean."},"max_bytes":{"type":"integer","description":"Soft byte budget for candidate_results (default 16000, lean default 4096, 0 disables, max 100000)."},"limit":{"type":"integer","description":"Max exact results and candidate results (default 50, max 500)"}}})J");

    register_node_tool("callees_approx", tools::callees_approx,
        "Find all functions/symbols that the given symbol calls or references. Optionally group results by file, module, or symbol.",
        R"J({"type":"object","properties":{"stable_key":{"type":"string","description":"Preferred reindex-proof symbol handle. When provided, node_id is ignored."},"node_id":{"type":"integer","description":"Volatile node_id of the symbol to find callees for"},"expected_stable_key":{"type":"string","description":"Optional guard for node_id staleness/reuse."},"group_by":{"type":"string","enum":["file","module","symbol"],"description":"Group results by file path, module (directory), or symbol name. Omit for flat list."},"compact":{"type":"boolean","description":"Compact output: use generic symbol fields, omit qualname=name, collapse span to [start,end], and use 'file' instead of 'file_path'."}}})J");

    register_node_tool("references", tools::references,
        "Find all references to a symbol across the codebase.",
        R"J({"type":"object","properties":{"stable_key":{"type":"string","description":"Preferred reindex-proof symbol handle. When provided, node_id is ignored."},"node_id":{"type":"integer","description":"Volatile node_id of the symbol to find references for"},"expected_stable_key":{"type":"string","description":"Optional guard for node_id staleness/reuse."}}})J");

    server.register_tool("file_summary", tools::file_summary,
        "List all symbols defined in a file: functions, classes, structs, macros, variables.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Relative or absolute file path within the repository or workspace"},"file_path":{"type":"string","description":"Backward-compatible alias for path"},"limit":{"type":"integer","description":"Max symbols returned (default 200, max 2000)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":[]})J");

    server.register_tool("file_overview", tools::file_overview,
        "Structural overview of a file: top-level symbols with signatures and doc comments. Faster than context_for for large files.",
        R"J({"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"File path (relative or absolute)"}}})J");

    register_node_tool("context_for", tools::context_for,
        "Get the full structural context of a symbol: definition, source, exact callers/callees, container, siblings, and bases. Candidate callers are lean by default; receiver narrows approximate member-call sites. Receiver-type resolution depends on local type annotations and metadata reports resolution_status plus hidden candidates.",
        R"J({"type":"object","properties":{"stable_key":{"type":"string","description":"Preferred reindex-proof handle (returned as symbol.stable_key). When provided, node_id is ignored."},"node_id":{"type":"integer","description":"Volatile node_id of the symbol; changes on reindex"},"expected_stable_key":{"type":"string","description":"Optional guard for node_id: if the current row's stable_key differs, returns invalid_input instead of using a stale/reused id."},"symbol":{"type":"string","description":"Symbol name (alternative to node_id)"},"file":{"type":"string","description":"File path relative to repo root (used with symbol)"},"include_source":{"type":"boolean","description":"When false, omit the source field entirely. Defaults to true."},"max_source_lines":{"type":"integer","description":"Truncate source to at most N lines; 0 keeps the current full source behavior."},"max_callers":{"type":"integer","description":"Cap exact callers and candidate_callers returned after query collection; 0 disables the exact caller cap and caps candidates at 500. If omitted, preserves the current default behavior."},"max_callees":{"type":"integer","description":"Cap callees returned after query collection; 0 disables the cap. If omitted, preserves the current default behavior."},"include_candidates":{"type":"boolean","description":"When true, always add candidate_callers; when false, return exact callers only. Omitted means add candidates only if exact callers are empty."},"mode":{"type":"string","enum":["exact","exact_then_candidates","exact_plus_candidates"],"description":"Candidate collection mode for candidate_callers. Default exact_then_candidates."},"receiver":{"type":"string","description":"Optional receiver type/text filter for candidate_callers (e.g. LinkedMap). Receiver-type resolution depends on local type annotations; response includes candidate_callers_resolution_status, totals, receiver_type_hits, and hidden counts."},"include_handles":{"type":"boolean","description":"Opt in to candidate ref_id/caller_node_id/span/evidence fields. Default false keeps candidates lean."},"max_bytes":{"type":"integer","description":"Soft byte budget for candidate_callers (default 16000, 0 disables, max 100000)."}}})J");

    server.register_tool("context_by_name", tools::context_by_name,
        "Find a symbol by name and, when uniquely resolved, return the same full structural context as context_for. If multiple matches exist, returns a disambiguation list instead.",
        R"J({"type":"object","required":["name"],"properties":{"name":{"type":"string","description":"Symbol name to look up"},"file_pattern":{"type":"string","description":"Optional GLOB pattern to narrow by file path"}}})J");

    server.register_tool("entrypoints", tools::entrypoints,
        "Find entry point functions (main, DllMain, etc.) in the codebase. Optionally scope to a file path or directory prefix.",
        R"J({"type":"object","properties":{"scope":{"type":"string","description":"Optional file path or directory prefix to restrict results (e.g. 'src/mcp/' or 'src/cli/main.cpp')"},"limit":{"type":"integer","description":"Max results (default 20)"}},"required":[]})J");

    register_node_tool("impact_of", tools::impact_of,
        "Compute blast radius from indexed graph edges, exposing confidence, evidence, source, and hop target identity. Stored name-match edges are heuristic, not confirmed dispatch. If first-hop indexed callers are empty, refs-backed candidate fallback is available. Candidate impact is first-hop only and can be narrowed by receiver.",
        R"J({"type":"object","properties":{"stable_key":{"type":"string","description":"Preferred reindex-proof symbol handle. When provided, node_id is ignored."},"node_id":{"type":"integer","description":"Volatile node_id of the symbol to analyze"},"expected_stable_key":{"type":"string","description":"Optional guard for node_id staleness/reuse."},"depth":{"type":"integer","description":"How many levels of transitive callers to follow (default 2, max 3)"},"max_nodes":{"type":"integer","description":"Max exact impacted nodes and candidate_impacted rows (default 50, max 200)"},"include_candidates":{"type":"boolean","description":"When true, always add candidate_impacted; when false, exact-empty fallback still returns candidate_impacted. Omitted means add candidates if exact impact is empty."},"mode":{"type":"string","enum":["exact","exact_then_candidates","exact_plus_candidates"],"description":"Candidate collection mode for candidate_impacted. Default exact_then_candidates; exact-empty fallback is always on."},"receiver":{"type":"string","description":"Optional receiver type/text filter for candidate_impacted (e.g. LinkedMap). Receiver-type resolution depends on local type annotations; response includes candidate_impacted_resolution_status, totals, receiver_type_hits, and hidden counts."},"include_handles":{"type":"boolean","description":"Opt in to candidate ref_id/caller_node_id/span/evidence fields. Default false keeps candidates lean."},"max_bytes":{"type":"integer","description":"Soft byte budget for candidate_impacted (default 16000, 0 disables, max 100000)."}}})J");

    server.register_tool("detect_changes", tools::detect_changes,
        "Resolve commit changes within the requested indexed primary/additional root, conservatively map changed files to current indexed symbols, and estimate callers. Reports root identity, indexed/compared commits, source revision verification, and explicit unresolved/stale mapping diagnostics.",
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

    register_node_tool("find_similar", tools::find_similar,
        "Find near-duplicate functions or methods using MinHash fingerprints of normalized AST leaf trigrams.",
        R"J({"type":"object","properties":{"stable_key":{"type":"string","description":"Preferred reindex-proof symbol handle. When provided, node_id is ignored."},"node_id":{"type":"integer","description":"Volatile node_id of the function or method to compare"},"expected_stable_key":{"type":"string","description":"Optional guard for node_id staleness/reuse."},"symbol":{"type":"string","description":"Symbol name (alternative to node_id)"},"file":{"type":"string","description":"File path relative to repo root (used with symbol)"},"threshold":{"type":"number","description":"Minimum MinHash similarity to return (default 0.8)"},"limit":{"type":"integer","description":"Max results (default 20, max 200)"}}})J");

    register_node_tool("method_fields", tools::method_fields,
        "List all 'this.X' field accesses (reads and writes) and outgoing calls made by a method, classified as calls_self (same class) or calls_external. Useful for understanding a TypeScript/JavaScript method's state usage.",
        R"J({"type":"object","properties":{"stable_key":{"type":"string","description":"Preferred reindex-proof symbol handle. When provided, node_id is ignored."},"node_id":{"type":"integer","description":"Volatile node_id of the method symbol to analyze"},"expected_stable_key":{"type":"string","description":"Optional guard for node_id staleness/reuse."},"symbol":{"type":"string","description":"Symbol name (alternative to node_id)"},"file":{"type":"string","description":"File path relative to repo root (used with symbol)"}}})J");

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

    server.register_tool("ingest_traces", tools::ingest_traces,
        "Ingest observed runtime call traces and boost matching call-graph edge confidence using call counts and latency percentiles.",
        R"J({"type":"object","properties":{"source":{"type":"string","description":"Origin label for the traces, for example 'otlp' or 'manual'"},"traces":{"type":"array","items":{"type":"object","properties":{"caller":{"type":"string"},"callee":{"type":"string"},"count":{"type":"integer"},"p50_ms":{"type":"number"},"p99_ms":{"type":"number"},"error_rate":{"type":"number"}},"required":["caller","callee"]}}},"required":["traces"]})J");

    server.register_tool("get_traces", tools::get_traces,
        "Query ingested runtime traces by caller/callee name and minimum call count, with resolution status against indexed symbols and call edges.",
        R"J({"type":"object","properties":{"caller":{"type":"string","description":"Optional substring filter for caller_name"},"callee":{"type":"string","description":"Optional substring filter for callee_name"},"min_count":{"type":"integer","description":"Minimum call_count filter (default 0)"},"limit":{"type":"integer","description":"Max results (default 50, max 500)"}}})J");

    server.register_tool("reindex",
        [&reindex, &repo_root, &resolved_db_path, &server,
         &ownership_established](yyjson_val* /*params*/, Connection& /*conn*/,
                                          QueryCache& /*cache*/, const std::string& /*root*/) -> std::string {
            reindex.trigger(repo_root, resolved_db_path, [&server, &ownership_established]() {
                ownership_established = true;
                server.request_refresh();
            }, {}, true, ReindexReason::manual);
            return R"({"status":"started","message":"Re-indexing in background. Queries will reflect updated state once complete."})";
        },
        "Trigger a re-index of the repository. Runs in the background — subsequent tool calls will use fresh data once complete. Call this after making file changes (renames, moves, extractions) to ensure the index is up to date.",
        R"J({"type":"object","properties":{}})J");

    // Workspace tools — multi-root management
    server.register_tool("workspace_add", tools::workspace_add,
        "Start a background add job: incrementally index the selected extra root, then merge. Returns job_id; poll workspace_job_status.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Absolute path to the directory to add as a workspace root"}},"required":["path"]})J");

    server.register_tool("workspace_remove", tools::workspace_remove,
        "Start a background removal job. Returns job_id; poll workspace_job_status. Safe removal is not interrupted.",
        R"J({"type":"object","properties":{"path":{"type":"string","description":"Absolute path of the workspace root to remove"}},"required":["path"]})J");

    server.register_tool("workspace_list", tools::workspace_list,
        "List the primary root and all additional workspace roots. File counts are exact; "
        "primary graph totals are null to avoid a repository-wide graph scan.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("workspace_refresh", {},
        "Start a background refresh of an existing extra root only, never the primary root. "
        "Set reparse=true to repair extraction/linking of unchanged source files without clearing the source index. Returns job_id.",
        R"J({"type":"object","properties":{"path":{"type":"string"},"reparse":{"type":"boolean","default":false}},"required":["path"]})J");
    server.register_tool("workspace_job_status", {},
        "Get the retained job status, phase, elapsed_ms, result or error without querying SQLite. Only the latest job is retained per session.",
        R"J({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})J");
    server.register_tool("workspace_job_cancel", {},
        "Request cancellation at the next safe phase boundary. Running index children and merge/removal finish safely; timeout alone does not cancel.",
        R"J({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})J");
    server.register_tool("server_health", {},
        "Metadata-only readiness probe: ready, busy, or degraded for missing/interrupted index metadata.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("graph_quality", tools::graph_quality,
        "Compute comprehensive graph quality, reference disambiguation, confidence distribution, and resolution metrics across languages and edge kinds.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("get_edge_evidence", tools::get_edge_evidence,
        "Query epistemic provenance and runtime observation evidence for edges (static, runtime, semantic, protocol). Supports filtering by caller, callee, symbol, or observation mode.",
        R"J({"type":"object","properties":{"symbol":{"type":"string","description":"Symbol name or qualname to inspect incoming and outgoing edges for"},"node_id":{"type":"integer","description":"Symbol node ID"},"caller":{"type":"string","description":"Caller symbol name or qualname"},"callee":{"type":"string","description":"Callee symbol name or qualname"},"mode":{"type":"string","enum":["all","observed","unobserved_static","runtime_only"],"description":"Filter mode for observations (default: 'all')"},"kind":{"type":"string","description":"Edge kind filter (e.g. 'calls', 'includes')"},"limit":{"type":"integer","description":"Max edges to return (default 50, max 200)"}}})J");

    server.register_tool("graph_diff", tools::graph_diff,
        "Compute semantic graph diff between working tree, commits, or index. Reports symbols added/removed/modified, call edges added/removed, and fan-in topology deltas.",
        R"J({"type":"object","properties":{"base":{"type":"string","description":"Base git ref or 'index' (default: 'HEAD')"},"target":{"type":"string","description":"Target git ref or 'working-tree' (default: 'working-tree')"},"since":{"type":"string","description":"Alias for base"},"file_pattern":{"type":"string","description":"Optional file path glob pattern to filter changes"}}})J");

    mcp_log("codetopo mcp started");
    mcp_log("lifecycle: ready elapsed=" +
        format_duration_seconds(std::chrono::steady_clock::now() - startup_started));
    mcp_log("db: " + resolved_db_path);
    mcp_log("repo: " + repo_root);
    mcp_log("schema: v" + std::to_string(version) + "  tools: " + std::to_string(server.tool_count()));

    // P2: Start filesystem watcher for auto-reindex when --watch is enabled.
    // Cross-thread contract: watcher thread -> reindex monitor thread -> atomic flag
    //   -> main thread picks up flag before next tool dispatch.
    // CRITICAL: Filter out .codetopo/ and .git/ (except .git/HEAD) events to avoid
    // infinite reindex loop (indexer writes to .codetopo/index.sqlite which the
    // watcher would see as a change, triggering another reindex, ad infinitum).
    std::unique_ptr<Watcher> watcher;
    if (watch && freshness_allows_watching(freshness)) {
        auto debounce = std::chrono::milliseconds(debounce_ms);
        watcher = std::make_unique<Watcher>(
            repo_root,
            [&](const std::vector<WatchEvent>& events) {
                // Filter: ignore .codetopo/ writes (our own DB) and .git/ internals
                // (except .git/HEAD which signals branch switch)
                std::vector<std::string> changed_paths;
                std::unordered_set<std::string> seen_paths;
                bool branch_switch = false;
                bool fallback_full = false;
                auto root_path = fs::path(repo_root).lexically_normal();

                for (const auto& ev : events) {
                    auto s = ev.path.generic_string();
                    if (s.find(".codetopo/") != std::string::npos) continue;
                    if (s.find(".codetopo\\") != std::string::npos) continue;
                    if (ev.type == FileEvent::BranchSwitch) {
                        branch_switch = true;
                        continue;
                    }
                    // Skip .git/ internals (pack files, index, refs updates, etc.)
                    if (s.find(".git/") != std::string::npos || s.find(".git\\") != std::string::npos) continue;
                    auto rel = repo_relative_watch_path(root_path, ev.path);
                    if (rel && seen_paths.insert(*rel).second) {
                        changed_paths.push_back(*rel);
                    }
                }

                if (branch_switch) {
                    auto new_head = get_git_head(repo_root);
                    if (last_known_head.empty() || new_head.empty()) {
                        fallback_full = true;
                    } else {
                        bool empty_head_diff = false;
                        append_unique_paths(changed_paths, seen_paths,
                            git_changed_paths_for_heads(repo_root, last_known_head, new_head,
                                                        &empty_head_diff));
                        if (new_head != last_known_head && empty_head_diff) {
                            fallback_full = true;
                        } else if (new_head != last_known_head && changed_paths.empty()) {
                            changed_paths.push_back(".git/HEAD");
                        }
                    }
                    if (!new_head.empty()) last_known_head = new_head;
                }

                if (!fallback_full && changed_paths.empty()) return;
                if (!ownership_established.load()) {
                    fallback_full = true;
                    changed_paths.clear();
                }

                mcp_log(fallback_full
                    ? "watcher: change detected, triggering full reindex"
                    : "watcher: change detected, triggering targeted reindex ("
                        + std::to_string(changed_paths.size()) + " paths)");
                reindex.trigger(repo_root, resolved_db_path, [&]() {
                    ownership_established = true;
                    server.request_refresh();
                    mcp_log("watcher: reindex complete, cache invalidated");
                }, changed_paths, fallback_full, ReindexReason::watcher);
            },
            debounce
        );
        watcher->start();
        mcp_log("watcher: started (" + std::to_string(debounce_ms) + "ms debounce)");
    }

    mcp_log("stdio: ready; waiting for client initialize and JSON-RPC requests "
            "(stdout is the protocol stream, diagnostics use stderr and the log file)");
    int rc = server.run();

    // Watcher::~Watcher() calls stop(), but be explicit about shutdown order.
    if (watcher) {
        watcher->stop();
        mcp_log("watcher: stopped");
    }
    reindex.stop();
    server.shutdown_jobs();
    mcp_log("codetopo mcp stopped (rc=" + std::to_string(rc) + ")");
    return rc;
} catch (const std::exception& e) {
    mcp_log("shutdown: startup_or_runtime_error " + truncate_for_log(e.what()));
    return 1;
}

// T076: Wire cmd_query — CLI wrapper for tool invocations.
inline int run_query(const std::string& db_path, const std::string& tool_name,
                     const std::string& params_json,
                     const std::string& root_hint = ".",
                     bool root_was_explicit = false) {
    namespace fs = std::filesystem;

    if (!fs::exists(db_path)) {
        mcp_log("error: database not found: " + db_path);
        return 1;
    }

    std::error_code ec;
    auto canonical_db = fs::canonical(db_path, ec);
    if (ec) {
        mcp_log("error: cannot canonicalize database: " + db_path);
        return 1;
    }
    Connection conn(canonical_db.string(), true);
    auto resolution = index_ownership::resolve_primary_root(
        conn, root_hint, root_was_explicit, canonical_db.string());
    auto repo_root = resolution.root.string();
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
        {"find_similar", tools::find_similar},
        {"code_search", tools::code_search},
        {"list_http_calls", tools::list_http_calls},
        {"ingest_traces", tools::ingest_traces},
        {"get_traces", tools::get_traces},
        {"workspace_add", tools::workspace_add},
        {"workspace_remove", tools::workspace_remove},
        {"workspace_list", tools::workspace_list},
        {"graph_quality", tools::graph_quality},
        {"quality", tools::graph_quality},
        {"get_edge_evidence", tools::get_edge_evidence},
        {"graph_diff", tools::graph_diff},
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
