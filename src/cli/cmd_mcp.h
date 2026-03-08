#pragma once

#include "core/config.h"
#include "db/connection.h"
#include "db/schema.h"
#include "mcp/server.h"
#include "mcp/tools.h"
#include <iostream>
#include <filesystem>

namespace codetopo {

// T075: Wire cmd_mcp — start MCP server over stdio.
inline int run_mcp(const std::string& db_path, int tool_timeout, int idle_timeout) {
    namespace fs = std::filesystem;

    if (!fs::exists(db_path)) {
        std::cerr << "ERROR: Database not found: " << db_path << "\n";
        return 1;
    }

    Connection conn(db_path, true);  // read-only

    // Schema version check
    int version = schema::get_schema_version(conn);
    if (version != CURRENT_SCHEMA_VERSION) {
        std::cerr << "ERROR: Schema version mismatch (db=" << version
                  << " expected=" << CURRENT_SCHEMA_VERSION << ")\n";
        return 3;
    }

    auto repo_root = schema::get_kv(conn, "repo_root", ".");

    McpServer server(conn, repo_root, tool_timeout, idle_timeout);

    // Register all tools with descriptions and parameter schemas
    server.register_tool("server_info", tools::server_info,
        "Get server capabilities, schema version, database path, and uptime.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("repo_stats", tools::repo_stats,
        "Get repository statistics: file count, symbol count, edge count, last index time.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("symbol_search", tools::symbol_search,
        "Search for symbols (functions, classes, macros, variables) by name. Use query='*' with kind filter to list all symbols of a kind without FTS. Returns node_id, kind, name, file_path, span. Use the node_id in other tools like context_for, callers_approx, impact_of.",
        R"J({"type":"object","properties":{"query":{"type":"string","description":"Symbol name or prefix to search for. Use '*' to list all (combine with kind filter)"},"kind":{"type":"string","description":"Filter by symbol kind: function, class, struct, variable, field, etc."},"limit":{"type":"integer","description":"Max results (default 50, max 500)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":["query"]})J");

    server.register_tool("symbol_list", tools::symbol_list,
        "List and filter symbols without full-text search. Supports filtering by kind (class, struct, function, variable, etc.), file_path, and name_glob (SQLite GLOB pattern). Ideal for enumerating all classes, all structs, or all symbols in a file.",
        R"J({"type":"object","properties":{"kind":{"type":"string","description":"Filter by symbol kind: function, class, struct, variable, field, macro, etc."},"file_path":{"type":"string","description":"Filter to symbols in this file (relative path)"},"name_glob":{"type":"string","description":"SQLite GLOB pattern for symbol name (e.g. 'Get*', '*Handler')"},"limit":{"type":"integer","description":"Max results (default 200, max 2000)"},"offset":{"type":"integer","description":"Pagination offset (default 0)"}},"required":[]})J");

    server.register_tool("symbol_get", tools::symbol_get,
        "Get detailed information about a specific symbol by node_id, including source code snippet.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id from symbol_search results"}},"required":["node_id"])J");

    server.register_tool("symbol_get_batch", tools::symbol_get_batch,
        "Get details for multiple symbols at once by their node_ids.",
        R"J({"type":"object","properties":{"node_ids":{"type":"array","items":{"type":"integer"},"description":"Array of node_ids to fetch"}},"required":["node_ids"])J");

    server.register_tool("callers_approx", tools::callers_approx,
        "Find all functions/files that call or reference the given symbol. Returns caller names, file paths, and confidence scores.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol to find callers for"}},"required":["node_id"])J");

    server.register_tool("callees_approx", tools::callees_approx,
        "Find all functions/symbols that the given symbol calls or references.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol to find callees for"}},"required":["node_id"])J");

    server.register_tool("references", tools::references,
        "Find all references to a symbol across the codebase.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol to find references for"}},"required":["node_id"])J");

    server.register_tool("file_summary", tools::file_summary,
        "List all symbols defined in a file: functions, classes, structs, macros, variables.",
        R"J({"type":"object","properties":{"file_path":{"type":"string","description":"Relative file path within the repository"}},"required":["file_path"])J");

    server.register_tool("context_for", tools::context_for,
        "Get the full structural context of a symbol: its definition, source snippet, callers, and callees. Best for understanding what a symbol does and how it connects to the codebase.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol"}},"required":["node_id"])J");

    server.register_tool("entrypoints", tools::entrypoints,
        "Find entry point functions (main, DllMain, etc.) in the codebase.",
        R"J({"type":"object","properties":{}})J");

    server.register_tool("impact_of", tools::impact_of,
        "Compute the blast radius of changing a symbol: all transitive dependents up to a given depth.",
        R"J({"type":"object","properties":{"node_id":{"type":"integer","description":"The node_id of the symbol to analyze"},"depth":{"type":"integer","description":"How many levels of transitive callers to follow (default 2)"}},"required":["node_id"])J");

    server.register_tool("file_deps", tools::file_deps,
        "Get file-level dependencies: which files does a file include/import, and which files include it.",
        R"J({"type":"object","properties":{"file_path":{"type":"string","description":"Relative file path"}},"required":["file_path"])J");

    server.register_tool("subgraph", tools::subgraph,
        "Extract a local dependency neighborhood graph around one or more seed symbols.",
        R"J({"type":"object","properties":{"seed_symbols":{"type":"array","items":{"type":"integer"},"description":"Array of node_ids to use as seeds"},"depth":{"type":"integer","description":"Graph traversal depth (default 2)"}},"required":["seed_symbols"])J");

    server.register_tool("shortest_path", tools::shortest_path,
        "Find the shortest dependency path between two symbols in the code graph.",
        R"J({"type":"object","properties":{"src_id":{"type":"integer","description":"Source node_id"},"dst_id":{"type":"integer","description":"Destination node_id"}},"required":["src_id","dst_id"])J");

    std::cerr << "MCP server started (db=" << db_path << " repo=" << repo_root << ")\n";
    return server.run();
}

// T076: Wire cmd_query — CLI wrapper for tool invocations.
inline int run_query(const std::string& db_path, const std::string& tool_name,
                      const std::string& params_json) {
    namespace fs = std::filesystem;

    if (!fs::exists(db_path)) {
        std::cerr << "ERROR: Database not found: " << db_path << "\n";
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
        {"symbol_search", tools::symbol_search},
        {"symbol_list", tools::symbol_list},
        {"symbol_get", tools::symbol_get},
        {"symbol_get_batch", tools::symbol_get_batch},
        {"callers_approx", tools::callers_approx},
        {"callees_approx", tools::callees_approx},
        {"references", tools::references},
        {"file_summary", tools::file_summary},
        {"context_for", tools::context_for},
        {"entrypoints", tools::entrypoints},
        {"impact_of", tools::impact_of},
        {"file_deps", tools::file_deps},
        {"subgraph", tools::subgraph},
        {"shortest_path", tools::shortest_path},
    };

    auto it = all_tools.find(tool_name);
    if (it == all_tools.end()) {
        std::cerr << "ERROR: Unknown tool: " << tool_name << "\n";
        return 2;
    }

    try {
        std::string result = it->second(params, conn, cache, repo_root);
        std::cout << result << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}

} // namespace codetopo
