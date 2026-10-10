#pragma once

#include "util/json.h"
#include "util/git.h"
#include "mcp/error.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "index/ownership.h"
#include "util/log.h"
#include "mcp/workspace_jobs.h"
#include "mcp/stdio_input.h"
#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <iostream>
#include <fstream>
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
              int tool_timeout_s = 10, int idle_timeout_s = 0,
              std::mutex* writer_gate = nullptr)
        : conn_(conn), repo_root_(repo_root)
        , tool_timeout_s_(tool_timeout_s), idle_timeout_s_(idle_timeout_s)
        , cache_(conn), start_time_(std::chrono::steady_clock::now())
        , writer_gate_(writer_gate ? *writer_gate : owned_gate_), jobs_(writer_gate_) {}

    void shutdown_jobs() { jobs_.shutdown(); }
    void set_indexing_flag(const std::atomic<bool>* flag) { indexing_ = flag; }

    // Optional trajectory log: when set, each tool call + result is appended as JSONL.
    void set_trajectory_log(const std::string& path) {
        traj_path_ = path;
        traj_file_.open(path, std::ios::app);
        if (!traj_file_) mcp_log("warn: could not open trajectory log: " + path);
    }

    // Only the stdio owner ever touches conn_ or cache_. Workers signal atomically.
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
        struct LogSession {
            LogSession() {
                mcp_notify_active().store(false, std::memory_order_relaxed);
                mcp_log_level().store(1, std::memory_order_relaxed);
            }
            ~LogSession() { mcp_notify_active().store(false, std::memory_order_relaxed); }
        } log_session;
        StdioInput input;

        while (true) {
            std::string line;
            StdioInput::Result input_result;
            try {
                input_result = input.next(line, idle_timeout_s_);
            } catch (const std::exception& e) {
                mcp_log("shutdown: transport_error " + std::string(e.what()));
                return 1;
            }
            if (input_result == StdioInput::Result::idle) {
                mcp_log("shutdown: idle timeout (" + std::to_string(idle_timeout_s_) + "s)");
                return 0;
            }
            if (input_result == StdioInput::Result::eof) {
                mcp_log("shutdown: stdin EOF");
                return 0;
            }
            if (line.empty()) continue;

            auto doc = json_parse(line);
            if (!doc) {
                write_error(nullptr, -32700, "parse_error", "Failed to parse JSON");
                continue;
            }

            auto* root = doc.root();
            auto* method_val = yyjson_obj_get(root, "method");
            auto* id_val = yyjson_obj_get(root, "id");
            auto* params_val = yyjson_obj_get(root, "params");

            auto* id = id_val;
            const char* method = method_val ? yyjson_get_str(method_val) : nullptr;

            if (!method) {
                write_error(id, -32600, "invalid_input", "Missing 'method' field");
                continue;
            }

            std::string method_str(method);

            // T058: Initialization handshake
            if (method_str == "initialize") {
                mcp_log("stdio: initialize received");
                handle_initialize(id);
                continue;
            }

            if (method_str == "notifications/initialized") {
                // No response needed for notifications
                initialized_ = true;
                mcp_notify_active().store(true, std::memory_order_relaxed);
                mcp_log("logging: client initialized, live logs enabled");
                continue;
            }
            if (method_str == "logging/setLevel") {
                auto* level_val = params_val ? yyjson_obj_get(params_val, "level") : nullptr;
                const char* level = level_val ? yyjson_get_str(level_val) : nullptr;
                auto parsed = mcp_log_level_value(level ? level : "");
                if (!parsed) {
                    write_error(id, -32602, "invalid_input", "Invalid or missing logging level");
                    continue;
                }
                mcp_log_level().store(*parsed, std::memory_order_relaxed);
                JsonMutDoc response;
                auto* root = response.new_obj();
                response.set_root(root);
                yyjson_mut_obj_add_str(response.doc, root, "jsonrpc", "2.0");
                add_response_id(response, root, id);
                yyjson_mut_obj_add_val(response.doc, root, "result", response.new_obj());
                json_write_line(response.to_string());
                continue;
            }
            if (method_str == "ping") {
                JsonMutDoc response;
                auto* root = response.new_obj();
                response.set_root(root);
                yyjson_mut_obj_add_str(response.doc, root, "jsonrpc", "2.0");
                add_response_id(response, root, id);
                yyjson_mut_obj_add_val(response.doc, root, "result", response.new_obj());
                json_write_line(response.to_string());
                continue;
            }
            if (method_str == "notifications/cancelled") {
                // A request timeout is not cancellation of an accepted workspace job.
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

                const std::string name(tool_name);
                const bool job_control = name == "workspace_job_status" || name == "workspace_job_cancel";
                const bool workspace_mutation = name == "workspace_add" ||
                    name == "workspace_refresh" || name == "workspace_remove";
                if (job_control || workspace_mutation || name == "server_health") {
                    try {
                        std::string result;
                        if (job_control) {
                            auto* job_id = tool_params ? json_get_str(tool_params, "job_id") : nullptr;
                            result = jobs_.status(job_id ? job_id : "", name == "workspace_job_cancel");
                        } else if (workspace_mutation) {
                            // Admission-free requests must not touch the query cache.
                            // Graph dispatch owns its independent committed read snapshot.
                            auto* path = tool_params ? json_get_str(tool_params, "path") : nullptr;
                            auto* reparse = tool_params ? yyjson_obj_get(tool_params, "reparse") : nullptr;
                            if (reparse && !yyjson_is_bool(reparse))
                                throw std::runtime_error("reparse must be a boolean");
                            result = jobs_.start(name == "workspace_remove" ? "remove" :
                                name == "workspace_refresh" ? "refresh" : "add",
                                path ? path : "", conn_.db_path(), repo_root_,
                                reparse && yyjson_get_bool(reparse));
                        } else {
                            std::unique_lock<std::mutex> gate(writer_gate_, std::try_to_lock);
                            FileLock probe(conn_.db_path() + ".lock");
                            bool busy = jobs_.active() ||
                                (indexing_ && indexing_->load()) ||
                                !gate.owns_lock() || probe.held_by_live_process();
                            auto metadata = index_ownership::inspect_metadata(conn_, busy);
                            JsonMutDoc health;
                            auto* out = health.new_obj();
                            health.set_root(out);
                            if (busy) {
                                yyjson_mut_obj_add_str(health.doc, out, "status", "busy");
                                yyjson_mut_obj_add_bool(health.doc, out, "retryable", true);
                                yyjson_mut_obj_add_str(
                                    health.doc, out, "reason", "indexing_or_workspace_job");
                            } else if (metadata.index == "missing_metadata" ||
                                       metadata.index == "interrupted" ||
                                       metadata.index == "incomplete" ||
                                       metadata.index == "needs_reconciliation") {
                                yyjson_mut_obj_add_str(health.doc, out, "status", "degraded");
                                yyjson_mut_obj_add_bool(health.doc, out, "retryable", false);
                                yyjson_mut_obj_add_strcpy(
                                    health.doc, out, "reason", metadata.index.c_str());
                            } else {
                                yyjson_mut_obj_add_str(health.doc, out, "status", "ready");
                            }
                            yyjson_mut_obj_add_strcpy(
                                health.doc, out, "ownership_status", metadata.ownership.c_str());
                            yyjson_mut_obj_add_strcpy(
                                health.doc, out, "index_status", metadata.index.c_str());
                            yyjson_mut_obj_add_str(
                                health.doc, out, "health_check", "metadata_only");
                            yyjson_mut_obj_add_bool(
                                health.doc, out, "counts_checked", false);
                            result = health.to_string();
                        }
                        write_result(id, result);
                    } catch (const std::exception& e) {
                        write_error(id, -32602, "invalid_input", e.what(), false);
                    }
                    continue;
                }
                auto it = tools_.find(tool_name);
                if (it == tools_.end()) {
                    write_error(id, -32601, "invalid_input", std::string("Unknown tool: ") + tool_name);
                    continue;
                }

                auto started = std::chrono::steady_clock::now();
                mcp_log("tool: " + std::string(tool_name) + " " + json_for_log(tool_params));

                try {
                    // Trace ingestion opens a write connection. It alone needs writer
                    // admission; ordinary reads never contend on this gate or FileLock.
                    std::unique_lock<std::mutex> gate(writer_gate_, std::defer_lock);
                    FileLock writer_probe(conn_.db_path() + ".lock");
                    if (name == "ingest_traces" &&
                        (!gate.try_lock() || !writer_probe.acquire())) {
                        write_error(id, -32603, "busy", "Another index writer is active", true);
                        continue;
                    }
                    refresh_read_cache();
                    check_staleness();
                    std::string result;
                    {
                        ReadSnapshot snapshot(conn_);
                        try {
                            result = it->second(tool_params, conn_, cache_, repo_root_);
                        } catch (...) {
                            cache_.reset_all();
                            throw;
                        }
                        cache_.reset_all();
                    } // Release the snapshot before stdout can block or a worker checkpoints.
                    auto elapsed = std::chrono::steady_clock::now() - started;
                    auto result_count = json_result_count(result);

                    // T070: Response size cap (512 KB)
                    if (result.size() > 512 * 1024) {
                        result = truncate_response(tool_name, result);
                    }

                    std::string done = "done: " + std::string(tool_name) + " (" + format_duration_ms(elapsed);
                    if (result_count) {
                        done += ", " + std::to_string(*result_count) + " results";
                    }
                    done += ")";
                    mcp_log(done);
                    write_result(id, result, staleness_.stale ? &staleness_ : nullptr);

                    // Trajectory logging: append {tool, arguments, result} as JSONL
                    if (traj_file_.is_open()) {
                        JsonMutDoc entry;
                        auto* obj = entry.new_obj();
                        entry.set_root(obj);
                        yyjson_mut_obj_add_str(entry.doc, obj, "tool", tool_name);
                        if (tool_params) {
                            auto* args_copy = yyjson_val_mut_copy(entry.doc, tool_params);
                            if (args_copy) yyjson_mut_obj_add_val(entry.doc, obj, "arguments", args_copy);
                        }
                        // Parse result JSON and embed it (fall back to raw string)
                        auto result_doc = json_parse(result);
                        if (result_doc) {
                            auto* result_copy = yyjson_val_mut_copy(entry.doc, result_doc.root());
                            if (result_copy) yyjson_mut_obj_add_val(entry.doc, obj, "result", result_copy);
                        } else {
                            yyjson_mut_obj_add_strcpy(entry.doc, obj, "result", result.c_str());
                        }
                        traj_file_ << entry.to_string() << "\n";
                        traj_file_.flush();
                    }

                } catch (const std::exception& e) {
                    auto elapsed = std::chrono::steady_clock::now() - started;
                    mcp_log("error: " + std::string(tool_name) + " (" + format_duration_ms(elapsed) + "): "
                            + truncate_for_log(e.what()));
                    auto* sqlite_error = dynamic_cast<const SqliteError*>(&e);
                    auto rc = sqlite_error ? sqlite_error->code() : sqlite3_errcode(conn_.raw());
                    bool busy = rc == SQLITE_BUSY || rc == SQLITE_LOCKED;
                    write_error(id, -32603, busy ? "busy" : "db_error", e.what(), busy);
                }
                cache_.reset_all();
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

    size_t tool_count() const {
        return tools_.size();
    }

private:
    Connection& conn_;
    std::string repo_root_;
    [[maybe_unused]] int tool_timeout_s_;
    int idle_timeout_s_;
    QueryCache cache_;
    std::chrono::steady_clock::time_point start_time_;
    std::unordered_map<std::string, ToolHandler> tools_;
    std::unordered_map<std::string, std::string> tool_descriptions_;
    std::unordered_map<std::string, std::string> tool_schemas_;
    bool initialized_ = false;
    StalenessState staleness_;
    std::atomic<bool> needs_refresh_{false};
    std::mutex owned_gate_;
    std::mutex& writer_gate_;
    WorkspaceJobs jobs_;
    const std::atomic<bool>* indexing_ = nullptr;
    int64_t data_version_ = -1;

    void refresh_read_cache() {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(conn_.raw(), "PRAGMA data_version", -1, &stmt, nullptr);
        if (rc != SQLITE_OK) throw SqliteError(rc, sqlite3_errmsg(conn_.raw()));
        rc = sqlite3_step(stmt);
        int64_t version = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
        std::string error = rc == SQLITE_ROW ? "" : sqlite3_errmsg(conn_.raw());
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW) throw SqliteError(rc, error);
        // Detect workspace commits and independent CLI writers too. Preparing with
        // v2 also reparses statements if a schema commit races this version check.
        if (needs_refresh_.exchange(false) || version != data_version_) {
            cache_.clear();
            staleness_.last_head_mtime = {};
        }
        data_version_ = version;
    }

    static void add_response_id(JsonMutDoc& doc, yyjson_mut_val* root, yyjson_val* id) {
        // JSON-RPC IDs can be strings; coercing them to integers strands the
        // client's pending initialize request even though the server is ready.
        yyjson_mut_obj_add_val(doc.doc, root, "id",
            id ? yyjson_val_mut_copy(doc.doc, id) : doc.new_null());
    }

    void handle_initialize(yyjson_val* id) {
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);

        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        add_response_id(doc, root, id);

        auto* result = doc.new_obj();
        yyjson_mut_obj_add_str(doc.doc, result, "protocolVersion", "2024-11-05");

        auto* caps = doc.new_obj();
        auto* tools_cap = doc.new_obj();
        yyjson_mut_obj_add_bool(doc.doc, tools_cap, "listChanged", false);
        yyjson_mut_obj_add_val(doc.doc, caps, "tools", tools_cap);
        yyjson_mut_obj_add_val(doc.doc, caps, "logging", doc.new_obj());
        yyjson_mut_obj_add_val(doc.doc, result, "capabilities", caps);

        auto* server_info = doc.new_obj();
        yyjson_mut_obj_add_str(doc.doc, server_info, "name", "codetopo");
        yyjson_mut_obj_add_str(doc.doc, server_info, "version", INDEXER_VERSION);
        yyjson_mut_obj_add_val(doc.doc, result, "serverInfo", server_info);

        // MCP instructions — guides the LLM on when to prefer these tools
        yyjson_mut_obj_add_str(doc.doc, result, "instructions",
            "codetopo provides structural code intelligence for large codebases. "
            "It pre-indexes all source files into a symbol graph with call relationships.\n\n"
            "PRIMARY WORKFLOW — always start here:\n"
            "1. symbol_search: find a symbol by name → get its stable_key (preferred) and node_id\n"
            "2. context_for: pass that stable_key → get source code + callers + callees in one call\n"
            "Never skip symbol_search. Never guess node_id values — always obtain handles from symbol_search results.\n"
            "node_id is a SESSION-SCOPED handle: it is a rowid that gets reassigned when the index is "
            "rebuilt, so a node_id cached from an earlier session may point to a DIFFERENT symbol after a "
            "reindex. For durable references across turns, cache the 'stable_key' returned by symbol_search/context_for "
            "and pass it back (context_for/references/etc. accept stable_key in place of node_id). If a "
            "result looks wrong, re-resolve the symbol with symbol_search.\n\n"
            "WHEN TO USE THESE TOOLS (prefer over reading files or grep/regex):\n"
            "- When the user asks about code structure, classes, functions, or symbols\n"
            "- When the user asks to refactor, split, decompose, or reorganize code\n"
            "- When you need to understand callers, callees, or impact of a change\n"
            "- When files are large (>500 lines) — use symbol_search + context_for instead of reading\n"
            "- When you need to find all references or implementations of a type\n"
            "- When you need to browse a directory or understand project layout\n\n"
            "NEVER read source files or directories directly. ALL indexed data is available through these tools:\n"
            "- dir_list: browse directories (files + subdirectories) — use INSTEAD of built-in Read/list_dir\n"
            "- file_summary: list symbols in a file — use INSTEAD of reading the file\n"
            "- file_search: find files by glob pattern — use INSTEAD of find/grep on filenames\n"
            "- symbol_search: find symbols by name (FTS) — PRIMARY entry point for code exploration\n"
            "- context_for: get a symbol's source code + callers + callees in one call\n"
            "- code_search: full-text trigram search — NOTE: requires a content index which may NOT be "
            "available in workspace mode. Prefer symbol_search over code_search when searching for "
            "symbols by name.\n\n"
            "MULTI-ROOT WORKSPACE (when extra roots have been added via 'codetopo workspace add'):\n"
            "- All roots are indexed together in one database — results span all roots transparently\n"
            "- Extra-root file paths are ABSOLUTE (e.g. /Volumes/Projects/elkjs/src/foo.ts)\n"
            "- Use file_search with absolute glob patterns to target a specific root, e.g. /Volumes/Projects/elkjs/**/*.ts\n"
            "- dir_list('.') lists all workspace roots — use those as starting points\n"
            "- code_search may be unavailable for extra roots; use symbol_search instead\n"
            "- workspace_list includes the primary repository as root_id=0, role=primary, "
            "followed by added roots with role=additional. The database roots table still stores "
            "only additional roots; primary files have root_id IS NULL. Primary graph totals are "
            "null (not computed), not zero or a full-graph scan.\n\n"
            "KEY WORKFLOW: symbol_search (find symbol, get node_id) → context_for (source + callers + callees) "
            "→ impact_of (blast radius). "
            "For browsing: dir_list (orient) → file_summary (list symbols in file) → symbol_search + context_for. "
            "For refactoring: dependency_cluster (find coupled methods) → "
            "method_fields (classify field access) → context_for (get method bodies to extract).");

        yyjson_mut_obj_add_val(doc.doc, root, "result", result);

        json_write_line(doc.to_string());
    }

    void handle_tools_list(yyjson_val* id) {
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);

        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        add_response_id(doc, root, id);

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

    void write_result(yyjson_val* id, const std::string& content_json,
                       const StalenessState* staleness = nullptr) {
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);

        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        add_response_id(doc, root, id);

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

    void write_error(yyjson_val* id, int code, const std::string& error_code,
                     const std::string& message, bool log_error = true) {
        if (log_error) {
            mcp_log("error: " + error_code + " (" + std::to_string(code) + "): "
                    + truncate_for_log(message));
        }
        McpError err;
        err.json_rpc_code = code;
        err.error_code = error_code;
        err.message = message;
        json_write_line(err.to_json_rpc_id(id));
    }

    static std::string truncate_response(std::string_view tool_name, const std::string& json) {
        auto doc = json_parse(json);
        if (!doc || !yyjson_is_obj(doc.root())) {
            mcp_log("warn: oversized response from " + std::string(tool_name)
                    + " (" + std::to_string(json.size())
                    + " bytes) could not be annotated");
            return json;
        }

        auto* root = doc.root();
        auto* has_more = yyjson_obj_get(root, "has_more");
        auto* offset = yyjson_obj_get(root, "offset");
        if (has_more && offset) {
            mcp_log("warn: oversized paginated response from " + std::string(tool_name)
                    + " (" + std::to_string(json.size())
                    + " bytes) left intact for caller pagination");
            return json;
        }

        JsonMutDoc out;
        auto* root_copy = yyjson_val_mut_copy(out.doc, root);
        out.set_root(root_copy);
        yyjson_mut_obj_add_bool(out.doc, root_copy, "_truncated", true);
        yyjson_mut_obj_add_int(out.doc, root_copy, "_truncated_bytes",
                               static_cast<int64_t>(json.size()));
        mcp_log("warn: oversized response from " + std::string(tool_name)
                + " (" + std::to_string(json.size())
                + " bytes) annotated with _truncated metadata");
        return out.to_string();
    }

    // R2: Stat HEAD to cheaply detect branch switches / new commits.
    // Handles standard Git repositories as well as linked Git worktrees.
    // Only spawns git processes when mtime has actually changed.
    void check_staleness() {
        namespace fs = std::filesystem;
        auto head_path = get_git_head_path(repo_root_);
        if (head_path.empty()) return;

        std::error_code ec;
        auto mtime = fs::last_write_time(head_path, ec);
        if (ec) return;  // cannot stat HEAD — skip

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

    std::string traj_path_;
    std::ofstream traj_file_;
};

} // namespace codetopo
