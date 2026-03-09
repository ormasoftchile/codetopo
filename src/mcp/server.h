#pragma once

#include "util/json.h"
#include "util/git.h"
#include "mcp/error.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include <string>
#include <functional>
#include <unordered_map>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <atomic>

namespace codetopo {

// T057-T059: MCP stdio server — NDJSON JSON-RPC 2.0, tool dispatch, handshake.

using ToolHandler = std::function<std::string(yyjson_val* params, Connection& conn,
                                               QueryCache& cache, const std::string& repo_root)>;

// R2: Lightweight staleness state — stat() only on hot path, git process only on mtime change.
struct StalenessState {
    std::filesystem::file_time_type last_head_mtime{};
    std::string indexed_head;
    std::string indexed_branch;
    bool stale = false;
    std::string current_branch;
};

class McpServer {
public:
    McpServer(Connection& conn, const std::string& repo_root,
              int tool_timeout_s = 10, int idle_timeout_s = 1800)
        : conn_(conn), repo_root_(repo_root)
        , tool_timeout_s_(tool_timeout_s), idle_timeout_s_(idle_timeout_s)
        , cache_(conn), start_time_(std::chrono::steady_clock::now()) {}

    // P2: Thread-safe flag — set from watcher's reindex-complete callback,
    // consumed on the main stdio thread before each tool dispatch.
    void request_refresh() {
        needs_refresh_.store(true);
    }

    void register_tool(const std::string& name, ToolHandler handler,
                       const std::string& description = "",
                       const std::string& params_schema_json = "") {
        tools_[name] = std::move(handler);
        tool_descriptions_[name] = description;
        tool_schemas_[name] = params_schema_json;
    }

    // T057: Main stdio loop — read NDJSON, dispatch, write response.
    int run() {
        auto last_activity = std::chrono::steady_clock::now();

        while (true) {
            // Check idle timeout
            if (idle_timeout_s_ > 0) {
                auto idle = std::chrono::steady_clock::now() - last_activity;
                if (std::chrono::duration_cast<std::chrono::seconds>(idle).count() > idle_timeout_s_) {
                    std::cerr << "MCP server idle timeout (" << idle_timeout_s_ << "s). Exiting.\n";
                    return 0;
                }
            }

            std::string line = json_read_line();
            if (line.empty()) {
                if (std::cin.eof()) return 0;  // Client closed connection
                continue;
            }

            last_activity = std::chrono::steady_clock::now();

            auto doc = json_parse(line);
            if (!doc) {
                write_error(-1, -32700, "parse_error", "Failed to parse JSON");
                continue;
            }

            auto* root = doc.root();
            auto* method_val = yyjson_obj_get(root, "method");
            auto* id_val = yyjson_obj_get(root, "id");
            auto* params_val = yyjson_obj_get(root, "params");

            int64_t id = id_val ? yyjson_get_sint(id_val) : -1;
            const char* method = method_val ? yyjson_get_str(method_val) : nullptr;

            if (!method) {
                write_error(id, -32600, "invalid_input", "Missing 'method' field");
                continue;
            }

            std::string method_str(method);

            // T058: Initialization handshake
            if (method_str == "initialize") {
                handle_initialize(id);
                continue;
            }

            if (method_str == "notifications/initialized") {
                // No response needed for notifications
                initialized_ = true;
                continue;
            }

            // T059: Tool dispatch
            if (method_str == "tools/call") {
                if (!initialized_) {
                    write_error(id, -32600, "invalid_input", "Server not initialized");
                    continue;
                }

                const char* tool_name = nullptr;
                yyjson_val* tool_params = nullptr;

                if (params_val) {
                    auto* name_val = yyjson_obj_get(params_val, "name");
                    tool_name = name_val ? yyjson_get_str(name_val) : nullptr;
                    tool_params = yyjson_obj_get(params_val, "arguments");
                }

                if (!tool_name) {
                    write_error(id, -32602, "invalid_input", "Missing tool name in params");
                    continue;
                }

                // P2: If watcher-triggered reindex completed, clear cached state
                if (needs_refresh_.exchange(false)) {
                    cache_.clear();
                    staleness_.last_head_mtime = {};  // force re-read of git state
                }

                // R2: Per-request staleness check (cheap stat)
                check_staleness();

                auto it = tools_.find(tool_name);
                if (it == tools_.end()) {
                    write_error(id, -32601, "invalid_input", std::string("Unknown tool: ") + tool_name);
                    continue;
                }

                try {
                    std::string result = it->second(tool_params, conn_, cache_, repo_root_);

                    // T070: Response size cap (512 KB)
                    if (result.size() > 512 * 1024) {
                        result = truncate_response(result);
                    }

                    write_result(id, result, staleness_.stale ? &staleness_ : nullptr);
                } catch (const std::exception& e) {
                    write_error(id, -32603, "db_error", e.what());
                }
                continue;
            }

            // tools/list — return available tools
            if (method_str == "tools/list") {
                handle_tools_list(id);
                continue;
            }

            write_error(id, -32601, "invalid_input", std::string("Unknown method: ") + method_str);
        }
    }

    int uptime_seconds() const {
        auto now = std::chrono::steady_clock::now();
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
    }

private:
    Connection& conn_;
    std::string repo_root_;
    int tool_timeout_s_;
    int idle_timeout_s_;
    QueryCache cache_;
    std::chrono::steady_clock::time_point start_time_;
    std::unordered_map<std::string, ToolHandler> tools_;
    std::unordered_map<std::string, std::string> tool_descriptions_;
    std::unordered_map<std::string, std::string> tool_schemas_;
    bool initialized_ = false;
    StalenessState staleness_;
    std::atomic<bool> needs_refresh_{false};

    void handle_initialize(int64_t id) {
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);

        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_int(doc.doc, root, "id", id);

        auto* result = doc.new_obj();
        yyjson_mut_obj_add_str(doc.doc, result, "protocolVersion", "2024-11-05");

        auto* caps = doc.new_obj();
        auto* tools_cap = doc.new_obj();
        yyjson_mut_obj_add_bool(doc.doc, tools_cap, "listChanged", false);
        yyjson_mut_obj_add_val(doc.doc, caps, "tools", tools_cap);
        yyjson_mut_obj_add_val(doc.doc, result, "capabilities", caps);

        auto* server_info = doc.new_obj();
        yyjson_mut_obj_add_str(doc.doc, server_info, "name", "codetopo");
        yyjson_mut_obj_add_str(doc.doc, server_info, "version", INDEXER_VERSION);
        yyjson_mut_obj_add_val(doc.doc, result, "serverInfo", server_info);

        yyjson_mut_obj_add_val(doc.doc, root, "result", result);

        json_write_line(doc.to_string());
    }

    void handle_tools_list(int64_t id) {
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);

        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_int(doc.doc, root, "id", id);

        auto* result = doc.new_obj();
        auto* tools_arr = doc.new_arr();

        for (auto& [name, _] : tools_) {
            auto* tool = doc.new_obj();
            yyjson_mut_obj_add_strcpy(doc.doc, tool, "name", name.c_str());

            // Add description if available
            auto desc_it = tool_descriptions_.find(name);
            if (desc_it != tool_descriptions_.end() && !desc_it->second.empty()) {
                yyjson_mut_obj_add_strcpy(doc.doc, tool, "description", desc_it->second.c_str());
            }

            // Add typed inputSchema if available, else fallback to empty object
            auto schema_it = tool_schemas_.find(name);
            if (schema_it != tool_schemas_.end() && !schema_it->second.empty()) {
                auto schema_doc = json_parse(schema_it->second);
                if (schema_doc) {
                    auto* schema_copy = yyjson_val_mut_copy(doc.doc, schema_doc.root());
                    yyjson_mut_obj_add_val(doc.doc, tool, "inputSchema", schema_copy);
                } else {
                    auto* schema = doc.new_obj();
                    yyjson_mut_obj_add_str(doc.doc, schema, "type", "object");
                    yyjson_mut_obj_add_val(doc.doc, tool, "inputSchema", schema);
                }
            } else {
                auto* schema = doc.new_obj();
                yyjson_mut_obj_add_str(doc.doc, schema, "type", "object");
                yyjson_mut_obj_add_val(doc.doc, tool, "inputSchema", schema);
            }

            yyjson_mut_arr_append(tools_arr, tool);
        }

        yyjson_mut_obj_add_val(doc.doc, result, "tools", tools_arr);
        yyjson_mut_obj_add_val(doc.doc, root, "result", result);

        json_write_line(doc.to_string());
    }

    void write_result(int64_t id, const std::string& content_json,
                       const StalenessState* staleness = nullptr) {
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);

        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_int(doc.doc, root, "id", id);

        // MCP tool results are wrapped in content array
        auto* result = doc.new_obj();
        auto* content_arr = doc.new_arr();
        auto* text_item = doc.new_obj();
        yyjson_mut_obj_add_str(doc.doc, text_item, "type", "text");
        yyjson_mut_obj_add_strcpy(doc.doc, text_item, "text", content_json.c_str());
        yyjson_mut_arr_append(content_arr, text_item);
        yyjson_mut_obj_add_val(doc.doc, result, "content", content_arr);

        // R2: Inject _meta with staleness info when index is stale
        if (staleness) {
            auto* meta = doc.new_obj();
            yyjson_mut_obj_add_bool(doc.doc, meta, "stale", true);
            yyjson_mut_obj_add_strcpy(doc.doc, meta, "indexed_branch",
                                     staleness->indexed_branch.c_str());
            yyjson_mut_obj_add_strcpy(doc.doc, meta, "indexed_commit",
                                     staleness->indexed_head.c_str());
            yyjson_mut_obj_add_strcpy(doc.doc, meta, "current_branch",
                                     staleness->current_branch.c_str());
            yyjson_mut_obj_add_val(doc.doc, result, "_meta", meta);
        }

        yyjson_mut_obj_add_val(doc.doc, root, "result", result);

        json_write_line(doc.to_string());
    }

    void write_error(int64_t id, int code, const std::string& error_code,
                     const std::string& message) {
        McpError err;
        err.json_rpc_code = code;
        err.error_code = error_code;
        err.message = message;
        json_write_line(err.to_json_rpc(id));
    }

    static std::string truncate_response(const std::string& json) {
        // Simple truncation: wrap in a truncation notice
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);
        yyjson_mut_obj_add_bool(doc.doc, root, "truncated", true);
        yyjson_mut_obj_add_str(doc.doc, root, "truncated_reason", "response_size");
        yyjson_mut_obj_add_str(doc.doc, root, "message", "Response exceeded 512 KB limit");
        return doc.to_string();
    }

    // R2: Stat .git/HEAD to cheaply detect branch switches / new commits.
    // Only spawns git processes when mtime has actually changed.
    void check_staleness() {
        namespace fs = std::filesystem;
        auto head_path = fs::path(repo_root_) / ".git" / "HEAD";

        std::error_code ec;
        auto mtime = fs::last_write_time(head_path, ec);
        if (ec) return;  // no .git/HEAD — not a git repo, skip

        // Fast path: mtime unchanged since last check
        if (mtime == staleness_.last_head_mtime) return;
        staleness_.last_head_mtime = mtime;

        // Mtime changed — read current git state (spawns git processes)
        auto current_head = get_git_head(repo_root_);
        staleness_.current_branch = get_git_branch(repo_root_);

        // Read indexed state from DB
        staleness_.indexed_head = schema::get_kv(conn_, "git_head", "");
        staleness_.indexed_branch = schema::get_kv(conn_, "git_branch", "");

        // Stale if indexed head is known and differs from current
        staleness_.stale = !staleness_.indexed_head.empty()
                        && staleness_.indexed_head != current_head;
    }
};

} // namespace codetopo
