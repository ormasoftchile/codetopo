#pragma once

#include "core/config.h"
#include "index/supervisor.h"
#include "util/repo.h"
#include "util/json.h"
#include "util/process.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/workspace.h"
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <yyjson.h>
#include <sqlite3.h>

#include <cstdlib>  // std::getenv (all platforms), _dupenv_s (Windows)

namespace codetopo {

// Supported editor targets for MCP config writing.
enum class Editor { vscode, cursor, windsurf, claude, copilot };

// Parse a comma-separated editor list string into a vector.
// "auto" is returned as an empty vector (caller detects).
inline std::vector<Editor> parse_editors(const std::string& input) {
    if (input == "auto") return {};

    std::vector<Editor> result;
    std::string token;
    for (size_t i = 0; i <= input.size(); ++i) {
        if (i == input.size() || input[i] == ',') {
            // trim
            while (!token.empty() && token.front() == ' ') token.erase(token.begin());
            while (!token.empty() && token.back() == ' ') token.pop_back();
            if (token == "vscode")        result.push_back(Editor::vscode);
            else if (token == "cursor")   result.push_back(Editor::cursor);
            else if (token == "windsurf") result.push_back(Editor::windsurf);
            else if (token == "claude")   result.push_back(Editor::claude);
            else if (token == "copilot")  result.push_back(Editor::copilot);
            token.clear();
        } else {
            token += input[i];
        }
    }
    return result;
}

// Detect which editors are in use by checking for their config directories.
inline std::vector<Editor> detect_editors(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::vector<Editor> found;
    if (fs::exists(root / ".vscode"))   found.push_back(Editor::vscode);
    if (fs::exists(root / ".cursor"))   found.push_back(Editor::cursor);
    if (fs::exists(root / ".windsurf")) found.push_back(Editor::windsurf);
    // Claude Desktop uses a global config — always include if user asked for auto
    // but we don't auto-detect it (no local dir to check).

    // Copilot CLI uses a project-local .github/mcp.json — always valid to include.
    found.push_back(Editor::copilot);
    return found;
}

// Get the directory for an editor's MCP config, relative to repo root.
// Returns empty for claude (which uses a global config path).
inline std::filesystem::path editor_config_dir(Editor e, const std::filesystem::path& root) {
    switch (e) {
        case Editor::vscode:   return root / ".vscode";
        case Editor::cursor:   return root / ".cursor";
        case Editor::windsurf: return root / ".windsurf";
        case Editor::claude:   return {};
        case Editor::copilot:  return root / ".github";
    }
    return {};
}

// Get the Claude Desktop global config path (platform-specific).
inline std::filesystem::path claude_config_path() {
#ifdef _WIN32
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata) {
        auto p = std::filesystem::path(appdata) / "Claude" / "claude_desktop_config.json";
        free(appdata);
        return p;
    }
    return {};
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / "Library" / "Application Support" / "Claude" / "claude_desktop_config.json";
#else
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / ".config" / "claude" / "claude_desktop_config.json";
#endif
}

// Read an existing JSON file into a mutable document, or create a new empty object doc.
// Returns a yyjson_mut_doc* (caller owns via RAII or manual free).
inline yyjson_mut_doc* read_or_create_json(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    if (fs::exists(path)) {
        std::ifstream fin(path, std::ios::binary);
        if (fin) {
            std::string content((std::istreambuf_iterator<char>(fin)),
                                 std::istreambuf_iterator<char>());
            yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
            if (doc) {
                yyjson_mut_doc* mdoc = yyjson_doc_mut_copy(doc, nullptr);
                yyjson_doc_free(doc);
                return mdoc;
            }
        }
    }
    // Create new doc with empty root object
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_doc_set_root(mdoc, yyjson_mut_obj(mdoc));
    return mdoc;
}

// Write a yyjson_mut_doc to a file with pretty-printing.
inline bool write_json_file(const std::filesystem::path& path, yyjson_mut_doc* doc) {
    namespace fs = std::filesystem;
    // Ensure parent directory exists
    auto parent = path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        fs::create_directories(parent);
    }

    size_t len = 0;
    char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY | YYJSON_WRITE_ESCAPE_UNICODE, &len);
    if (!json) return false;

    std::ofstream fout(path, std::ios::binary);
    if (!fout) { free(json); return false; }
    fout.write(json, static_cast<std::streamsize>(len));
    free(json);
    return true;
}

// Build the codetopo MCP server args array as a mutable JSON array.
inline yyjson_mut_val* build_mcp_args(yyjson_mut_doc* doc, const std::string& root_arg,
                                       bool watch, const std::string& freshness) {
    auto* arr = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, arr, "mcp");
    yyjson_mut_arr_add_strcpy(doc, arr, root_arg.c_str());
    if (watch) {
        yyjson_mut_arr_add_str(doc, arr, "--watch");
    }
    std::string freshness_arg = "--freshness=" + freshness;
    yyjson_mut_arr_add_strcpy(doc, arr, freshness_arg.c_str());
    return arr;
}

// Write/update a workspace-local MCP config (e.g. .vscode/mcp.json).
// Format: { "servers": { "codetopo": { "command": ..., "args": [...] } } }
// Merges with existing servers if the file already exists.
inline bool write_workspace_mcp_config(const std::filesystem::path& config_path,
                                        bool watch, const std::string& freshness) {
    yyjson_mut_doc* doc = read_or_create_json(config_path);
    if (!doc) return false;

    auto* root = yyjson_mut_doc_get_root(doc);
    if (!root || !yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
    }

    // Get or create "servers" object
    auto* servers = yyjson_mut_obj_get(root, "servers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        servers = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "servers", servers);
    }

    // Remove existing codetopo entry if present
    yyjson_mut_obj_remove_key(servers, "codetopo");

    // Build codetopo server entry — use absolute path to current binary
    // so the MCP config works regardless of PATH configuration.
    auto* entry = yyjson_mut_obj(doc);
    auto exe_path = get_self_executable_path();
    yyjson_mut_obj_add_strcpy(doc, entry, "command",
        exe_path.empty() ? "codetopo" : exe_path.c_str());

    // Workspace-local configs use relative root "."
    auto* args = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, args, "mcp");
    yyjson_mut_arr_add_str(doc, args, "--root");
    yyjson_mut_arr_add_str(doc, args, ".");
    if (watch) {
        yyjson_mut_arr_add_str(doc, args, "--watch");
    }
    std::string freshness_arg = "--freshness=" + freshness;
    yyjson_mut_arr_add_strcpy(doc, args, freshness_arg.c_str());

    yyjson_mut_obj_add_val(doc, entry, "args", args);
    yyjson_mut_obj_add_val(doc, servers, "codetopo", entry);

    bool ok = write_json_file(config_path, doc);
    yyjson_mut_doc_free(doc);
    return ok;
}

// Write/update the Claude Desktop global config.
// Format: { "mcpServers": { "codetopo": { "command": ..., "args": [...] } } }
// Uses absolute path for --root since Claude runs from a different cwd.
inline bool write_claude_mcp_config(const std::filesystem::path& repo_root,
                                     bool watch, const std::string& freshness) {
    auto config_path = claude_config_path();
    if (config_path.empty()) return false;

    yyjson_mut_doc* doc = read_or_create_json(config_path);
    if (!doc) return false;

    auto* root = yyjson_mut_doc_get_root(doc);
    if (!root || !yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
    }

    // Get or create "mcpServers" object
    auto* servers = yyjson_mut_obj_get(root, "mcpServers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        servers = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "mcpServers", servers);
    }

    // Remove existing codetopo entry if present
    yyjson_mut_obj_remove_key(servers, "codetopo");

    // Build codetopo server entry — use absolute path to current binary
    auto* entry = yyjson_mut_obj(doc);
    auto exe_path = get_self_executable_path();
    yyjson_mut_obj_add_strcpy(doc, entry, "command",
        exe_path.empty() ? "codetopo" : exe_path.c_str());

    auto* args = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, args, "mcp");
    yyjson_mut_arr_add_str(doc, args, "--root");
    std::string abs_root = std::filesystem::canonical(repo_root).string();
    yyjson_mut_arr_add_strcpy(doc, args, abs_root.c_str());
    if (watch) {
        yyjson_mut_arr_add_str(doc, args, "--watch");
    }
    std::string freshness_arg = "--freshness=" + freshness;
    yyjson_mut_arr_add_strcpy(doc, args, freshness_arg.c_str());

    yyjson_mut_obj_add_val(doc, entry, "args", args);
    yyjson_mut_obj_add_val(doc, servers, "codetopo", entry);

    bool ok = write_json_file(config_path, doc);
    yyjson_mut_doc_free(doc);
    return ok;
}

// Write/update the Copilot CLI project-local MCP config at {repo_root}/.github/mcp.json.
// Format: { "mcpServers": { "codetopo": { "type": "local", "command": ..., "args": [...], "env": {}, "tools": ["*"] } } }
// Uses relative --root . since the config lives in the project alongside the workspace.
// Merges with existing mcpServers entries — does not clobber them.
inline bool write_copilot_cli_mcp_config(const std::filesystem::path& repo_root,
                                          bool watch, const std::string& freshness) {
    namespace fs = std::filesystem;
    auto github_dir = repo_root / ".github";
    if (!fs::exists(github_dir)) fs::create_directories(github_dir);
    auto config_path = github_dir / "mcp.json";

    yyjson_mut_doc* doc = read_or_create_json(config_path);
    if (!doc) return false;

    auto* root = yyjson_mut_doc_get_root(doc);
    if (!root || !yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
    }

    // Get or create "mcpServers" object, preserving any existing entries.
    auto* servers = yyjson_mut_obj_get(root, "mcpServers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        servers = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "mcpServers", servers);
    }

    // Remove existing codetopo entry so we can write a fresh one.
    yyjson_mut_obj_remove_key(servers, "codetopo");

    // Build codetopo server entry.
    auto* entry = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, entry, "type", "local");

    auto exe_path = get_self_executable_path();
    yyjson_mut_obj_add_strcpy(doc, entry, "command",
        exe_path.empty() ? "codetopo" : exe_path.c_str());

    // Use relative --root . — config lives in the project root.
    auto* args = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, args, "mcp");
    yyjson_mut_arr_add_str(doc, args, "--root");
    yyjson_mut_arr_add_str(doc, args, ".");
    if (watch) {
        yyjson_mut_arr_add_str(doc, args, "--watch");
    }
    std::string freshness_arg = "--freshness=" + freshness;
    yyjson_mut_arr_add_strcpy(doc, args, freshness_arg.c_str());

    yyjson_mut_obj_add_val(doc, entry, "args", args);
    yyjson_mut_obj_add_val(doc, entry, "env", yyjson_mut_obj(doc));

    auto* tools = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, tools, "*");
    yyjson_mut_obj_add_val(doc, entry, "tools", tools);

    yyjson_mut_obj_add_val(doc, servers, "codetopo", entry);

    bool ok = write_json_file(config_path, doc);
    yyjson_mut_doc_free(doc);
    return ok;
}

// Write .github/copilot-instructions.md telling the agent to use codetopo tools first.
// Skips if the file already exists (don't overwrite user customizations).
inline bool write_copilot_instructions(const std::filesystem::path& repo_root) {
    namespace fs = std::filesystem;
    auto github_dir = repo_root / ".github";
    if (!fs::exists(github_dir)) fs::create_directories(github_dir);
    auto path = github_dir / "copilot-instructions.md";
    if (fs::exists(path)) return true;  // never overwrite

    static const char* content =
        "# Code Intelligence\n\n"
        "This project is indexed with codetopo, a structural code intelligence MCP server.\n\n"
        "**Always use codetopo MCP tools instead of grep, glob, or reading files directly.**\n\n"
        "## Primary workflow\n\n"
        "1. `symbols_in_path(path, kind=[...], min_span_lines=0)` — list symbols under a directory in lean mode\n"
        "2. `context_by_name(name)` — get full context for a symbol by name (merges symbol_search + context_for)\n"
        "3. `dir_tree(path, depth=1)` — browse directory structure with file sizes (use instead of ls/glob)\n"
        "4. `file_overview(path)` — top-level symbols in a file without reading the body\n"
        "5. `method_fields(symbol, file)` — list a method's field accesses and outgoing calls without reading source\n"
        "6. `symbol_search(query, kind)` — find any symbol by name, get its `node_id`\n"
        "7. `context_for(node_id)` — given a `node_id`, get source + callers + callees in one call\n"
        "8. `code_search(query)` — full-text search across all source\n"
        "9. `callers_approx` / `callees_approx` — call graph traversal\n"
        "10. `impact_of` — blast radius of a change\n\n"
        "## When to use which tool\n\n"
        "| Situation | First tool |\n"
        "|-----------|------------|\n"
        "| Cold start — unknown codebase | `dir_tree('.', depth=1)` → `symbols_in_path` |\n"
        "| Known symbol name | `context_by_name(name)` |\n"
        "| Known file path | `file_overview(path)` |\n"
        "| Find all uses of a field/text | `code_search(\".fieldName\")` |\n"
        "| Understand callers/impact | `callers_approx(node_id)` → `impact_of(node_id)` |\n"
        "| Understand a method's calls | `method_fields(symbol, file)` |\n"
        "| Need exact source lines | `source_at(file, start_line, end_line)` |\n"
        "| HOF/prototype JS codebases (`callers_approx` empty) | `code_search` for call-site text |\n\n"
        "## Batch, don't bounce\n\n"
        "- Issue independent codetopo tool calls in a single response.\n"
        "- Do **not** re-run `symbol_search` for a symbol when you already have its `node_id`.\n"
        "- Do **not** re-read a `source_at(file, start_line, end_line)` range you already fetched.\n"
        "- Prefer `context_for(node_id, include_source=false)` before opening raw source.\n"
        "- If one structural query almost answers the question, refine structurally before widening to `code_search`.\n\n"
        "## Structure before source\n\n"
        "Escalate in this order:\n\n"
        "1. **Structure** — `symbol_search`, `context_for`, `file_overview`, `symbols_in_path`\n"
        "2. **Source** — `source_at` for specific lines only\n"
        "3. **Text search** — `code_search` when the target is a string, error, config value, or unresolved text artifact\n"
        "4. **Multi-root** — add another workspace root only when local-root evidence is insufficient\n\n"
        "## Token-efficient patterns\n\n"
        "**Enumeration:** `symbols_in_path` is lean by default. It returns `kind`, `name`, `signature` when present, "
        "`file`, and `start_line`; pass `include_handles=true` or explicit `fields` only when you need `node_id`/spans. "
        "Avoid `min_span_lines` for exhaustive inventories because it is lossy (metadata reports hidden counts).\n\n"
        "**Call graph without source:** `context_for(node_id, include_source=false)` returns callers/callees "
        "only (~90% fewer tokens). Follow up with `source_at(file, start_line, end_line)` for specific lines.\n\n"
        "**Constructor internals:** `method_fields(symbol)` lists `this.X` field accesses and outgoing calls "
        "for a constructor or method — faster than reading source to understand data model.\n\n"
        "**Field access tracing:** `code_search(\".fieldName\", file_pattern=\"/abs/path/**\")` finds every "
        "read and write of a field across all files including workspace roots.\n\n"
        "**Callers in HOF-heavy codebases:** `callers_approx` may return empty results when code uses HOF "
        "wrappers, prototype assignment, or `forEach` dynamic dispatch. Use `code_search` for direct "
        "call-site discovery in JS/TS libraries in those cases.\n\n"
        "**Targeted source reads:** prefer `source_at(file, start_line, end_line)` over reading whole files.\n\n"
        "**Avoid re-resolving:** `node_id`s are stable session handles. Never re-run `symbol_search` for "
        "a symbol you already have a `node_id` for.\n\n"
        "## Focused evidence\n\n"
        "When reporting findings, emit compact evidence:\n\n"
        "- `file:line` citation\n"
        "- symbol name\n"
        "- one-sentence reason it matters\n"
        "- maximum 5 citations unless asked for more\n"
        "- no long prose unless the user asks for explanation\n\n"
        "## Symbol kinds\n\n"
        "codetopo indexes these kinds: `function`, `class`, `method`, `interface`, `enum`, `type`, "
        "`type_alias`, `macro`, `field`, `variable`, `namespace`, `constructor_fn`.\n\n"
        "**`type_alias`** — TypeScript `type X = ...` declarations. Use `symbols_in_path(kind=[\"type_alias\"])` "
        "to find all type aliases. Note: `type` and `type_alias` are separate kinds — query both when needed.\n\n"
        "**`constructor_fn`** — pre-ES6 JavaScript factory pattern: `let Foo = function() { this.x = ...; }`. "
        "Use `symbols_in_path(kind=[\"constructor_fn\"], compact=true)` to find all factory classes in a JS codebase. "
        "Use `method_fields` to inspect their `this.X` surface.\n\n"
        "## Multi-root workspace\n\n"
        "Extra reference repositories may be indexed alongside this project via "
        "`codetopo workspace add <path>`. Their files appear in results with absolute paths. "
        "`dir_tree('.')` lists all indexed roots.\n";

    std::ofstream f(path);
    if (!f) return false;
    f << content;
    return f.good();
}
struct IndexStats {
    int64_t file_count = 0;
    int64_t symbol_count = 0;
    int64_t edge_count = 0;
};

inline IndexStats query_index_stats(const std::string& db_path) {
    IndexStats stats;
    Connection conn(db_path, true);  // read-only
    auto count = [&](const char* table) -> int64_t {
        sqlite3_stmt* stmt = nullptr;
        std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
        sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr);
        int64_t n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    };
    stats.file_count = count("files");
    stats.symbol_count = count("nodes");
    stats.edge_count = count("edges");
    return stats;
}

// Editor name for display.
inline const char* editor_name(Editor e) {
    switch (e) {
        case Editor::vscode:   return "VS Code";
        case Editor::cursor:   return "Cursor";
        case Editor::windsurf: return "Windsurf";
        case Editor::claude:   return "Claude Desktop";
        case Editor::copilot:  return "GitHub Copilot";
    }
    return "Unknown";
}

// Config file path for display.
inline std::string editor_config_display(Editor e, const std::filesystem::path& root) {
    switch (e) {
        case Editor::vscode:   return ".vscode/mcp.json";
        case Editor::cursor:   return ".cursor/mcp.json";
        case Editor::windsurf: return ".windsurf/mcp.json";
        case Editor::claude:   return claude_config_path().string();
        case Editor::copilot:  return ".github/mcp.json";
    }
    return "";
}

// The init command entry point.
inline int run_init(const std::string& root_str,
                    const std::string& editors_str,
                    int threads, int arena_size, int large_arena_size,
                    int large_file_threshold,
                    int max_file_size,
                    int parse_timeout,
                    bool turbo,
                    bool force_reindex,
                    const std::vector<std::string>& exclude_patterns,
                    bool watch, const std::string& freshness,
                    const std::vector<std::string>& extra_roots = {}) {
    namespace fs = std::filesystem;

    // 1. Resolve repo root
    auto repo_root = fs::canonical(root_str);
    std::string db_path = default_db(repo_root.string());

    // 2-3. Create .codetopo/ and add to .gitignore
    bool gitignore_modified = ensure_codetopo_dir(repo_root.string());

    // 4. Run the full index via supervisor (with crash protection)
    codetopo::Config cfg;
    cfg.repo_root = repo_root;
    cfg.db_path = db_path;
    cfg.thread_count = threads;
    cfg.arena_size_mb = arena_size;
    cfg.large_arena_size_mb = large_arena_size;
    cfg.large_file_threshold_kb = large_file_threshold;
    cfg.max_file_size_kb = max_file_size;
    cfg.parse_timeout_s = parse_timeout;
    cfg.turbo = turbo;
    cfg.force_reindex = force_reindex;
    cfg.profile = false;
    cfg.max_files = 0;
    cfg.exclude_patterns = exclude_patterns;

    // Fresh init: clear stale quarantine from previous runs
    {
        Connection conn(db_path);
        schema::ensure_quarantine_table(conn);
        int old_q = schema::quarantine_count(conn);
        if (old_q > 0) {
            schema::clear_quarantine(conn);
            std::cerr << "Cleared " << old_q << " stale quarantine entries from previous run\n";
        }
    }

    int index_rc = run_index_supervisor(cfg);
    if (index_rc != 0 && index_rc != 1) {
        std::cerr << "ERROR: Indexing failed (exit code " << index_rc << ")\n";
        return index_rc;
    }
    if (index_rc == 1) {
        std::cerr << "WARNING: Indexing completed with some parse errors (non-fatal)\n";
    }

    // Query stats from the finished index
    auto stats = query_index_stats(db_path);

    // 5. Determine which editors to configure
    std::vector<Editor> editors = parse_editors(editors_str);
    if (editors.empty()) {
        // auto-detect
        editors = detect_editors(repo_root);
        if (editors.empty()) {
            // Default to VS Code if nothing detected
            editors.push_back(Editor::vscode);
        }
    }

    // Write MCP configs
    std::vector<std::string> written_configs;
    for (auto e : editors) {
        if (e == Editor::claude) {
            if (write_claude_mcp_config(repo_root, watch, freshness)) {
                written_configs.push_back(editor_config_display(e, repo_root));
            } else {
                std::cerr << "WARNING: Could not write Claude Desktop config\n";
            }
        } else if (e == Editor::copilot) {
            if (write_copilot_cli_mcp_config(repo_root, watch, freshness)) {
                written_configs.push_back(editor_config_display(e, repo_root));
                write_copilot_instructions(repo_root);  // best-effort, never overwrites
            } else {
                std::cerr << "WARNING: Could not write Copilot CLI MCP config\n";
            }
        } else {
            auto dir = editor_config_dir(e, repo_root);
            if (!fs::exists(dir)) fs::create_directories(dir);
            auto config_path = dir / "mcp.json";
            if (write_workspace_mcp_config(config_path, watch, freshness)) {
                written_configs.push_back(editor_config_display(e, repo_root));
            } else {
                std::cerr << "WARNING: Could not write " << editor_config_display(e, repo_root) << "\n";
            }
        }
    }

    // 5b. Process extra roots into workspace.sqlite
    if (!extra_roots.empty()) {
        auto ws_path = workspace_db_path(repo_root.string());
        try {
            WorkspaceDB ws(ws_path);
            // Also add the main root to the workspace
            ws.add_root(repo_root.string(), cfg);
            for (const auto& extra : extra_roots) {
                std::cerr << "Adding workspace root: " << extra << "\n";
                auto result = ws.add_root(extra, cfg);
                std::cerr << "  -> " << result.files << " files, "
                          << result.symbols << " symbols, "
                          << result.edges << " edges\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "WARNING: workspace setup failed: " << e.what() << "\n";
        }
    }

    // 6. Print summary
    std::cout << "\n";
    std::cout << "  \xe2\x9c\x93 Indexed " << stats.file_count << " files ("
              << stats.symbol_count << " symbols, "
              << stats.edge_count << " edges)\n";
    for (const auto& path : written_configs) {
        std::cout << "  \xe2\x9c\x93 Wrote " << path << "\n";
    }
    if (gitignore_modified) {
        std::cout << "  \xe2\x9c\x93 Added .codetopo/ to .gitignore\n";
    }
    std::cout << "\n";

    return 0;
}

} // namespace codetopo
