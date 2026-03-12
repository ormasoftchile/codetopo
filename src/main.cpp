#include <CLI/CLI.hpp>
#include <string>
#include <iostream>
#include <filesystem>
#include <fstream>
#include "core/config.h"
#include "cli/cmd_index.h"
#include "cli/cmd_init.h"
#include "cli/cmd_mcp.h"
#include "cli/cmd_doctor.h"
#include "cli/cmd_watch.h"
#include "index/supervisor.h"
#include "util/repo.h"

int main(int argc, char** argv) {
    CLI::App app{"codetopo — local code graph indexer + MCP server"};
    app.require_subcommand(1);

    // --- index subcommand ---
    auto* sub_index = app.add_subcommand("index", "Index a repository into a code graph");
    std::string index_root = ".";
    std::string index_db;
    int index_threads = 0;
    int index_arena_size = 128;
    int index_batch_size = 100;
    int index_max_file_size = 10;
    int index_max_symbols = 50000;
    bool index_no_gitignore = false;
    bool index_supervised = false;
    bool index_safe_mode = false;

    sub_index->add_option("--root", index_root, "Repository root directory")->default_val(".");
    sub_index->add_option("--db", index_db, "Database path (default: <root>/.codetopo/index.sqlite)");
    sub_index->add_option("--threads", index_threads, "Worker thread count (0=auto)")->default_val(0);
    sub_index->add_option("--arena-size", index_arena_size, "Arena size in MB per thread")->default_val(128);
    sub_index->add_option("--batch-size", index_batch_size, "Files per transaction batch")->default_val(100);
    sub_index->add_option("--max-file-size", index_max_file_size, "Max file size in MB")->default_val(10);
    sub_index->add_option("--max-symbols-per-file", index_max_symbols, "Max symbols per file")->default_val(50000);
    sub_index->add_flag("--no-gitignore", index_no_gitignore, "Disable .gitignore filtering");
    sub_index->add_flag("--supervised", index_supervised, "Run as supervised child (internal)")->group("");
    sub_index->add_flag("--safe-mode", index_safe_mode, "Commit after every file (internal)")->group("");

    // --- init subcommand ---
    auto* sub_init = app.add_subcommand("init", "Index a repository and configure editors for MCP");
    std::string init_root = ".";
    std::string init_editors = "auto";
    int init_threads = 0;
    int init_arena_size = 128;
    int init_max_file_size = 10;
    bool init_watch = true;
    std::string init_freshness = "normal";

    sub_init->add_option("--root", init_root, "Repository root directory")->default_val(".");
    sub_init->add_option("--editors", init_editors,
        "Comma-separated editors: vscode,cursor,windsurf,claude,auto")->default_val("auto");
    sub_init->add_option("--threads", init_threads, "Worker thread count (0=auto)")->default_val(0);
    sub_init->add_option("--arena-size", init_arena_size, "Arena size in MB per thread")->default_val(128);
    sub_init->add_option("--max-file-size", init_max_file_size, "Max file size in MB")->default_val(10);
    sub_init->add_flag("--watch,!--no-watch", init_watch,
        "Include --watch in MCP config (default: true)")->default_val(true);
    sub_init->add_option("--freshness", init_freshness,
        "Freshness policy for MCP config: eager|normal|lazy|off")->default_val("normal");

    // --- mcp subcommand ---
    auto* sub_mcp = app.add_subcommand("mcp", "Start MCP server over stdio");
    std::string mcp_root = ".";
    std::string mcp_db;
    int mcp_tool_timeout = 10;
    int mcp_idle_timeout = 1800;

    sub_mcp->add_option("--root", mcp_root, "Repository root directory")->default_val(".");
    sub_mcp->add_option("--db", mcp_db, "Database path (default: <root>/.codetopo/index.sqlite)");
    sub_mcp->add_option("--tool-timeout", mcp_tool_timeout, "Tool timeout in seconds")->default_val(10);
    sub_mcp->add_option("--idle-timeout", mcp_idle_timeout, "Idle timeout in seconds (0=disable)")->default_val(1800);

    // R9: Freshness policy and debounce tuning
    std::string mcp_freshness = "normal";
    int mcp_debounce = 1000;
    bool mcp_watch = true;
    sub_mcp->add_option("--freshness", mcp_freshness,
        "Index freshness policy: eager|normal|lazy|off")->default_val("normal");
    sub_mcp->add_option("--debounce", mcp_debounce,
        "Watcher debounce in ms")->default_val(1000);
    sub_mcp->add_flag("--watch,!--no-watch", mcp_watch,
        "Enable filesystem watcher for auto-reindex (default: on)");
    bool mcp_verbose = false;
    sub_mcp->add_flag("--verbose", mcp_verbose,
        "Enable verbose logging for watcher and reindex diagnostics");

    // --- watch subcommand ---
    auto* sub_watch = app.add_subcommand("watch", "Watch for file changes and re-index");
    std::string watch_root = ".";
    std::string watch_db;

    sub_watch->add_option("--root", watch_root, "Repository root directory")->default_val(".");
    sub_watch->add_option("--db", watch_db, "Database path (default: <root>/.codetopo/index.sqlite)");

    // --- query subcommand ---
    auto* sub_query = app.add_subcommand("query", "Run an ad-hoc tool query");
    std::string query_root = ".";
    std::string query_db;
    std::string query_tool;
    std::string query_params = "{}";

    sub_query->add_option("--root", query_root, "Repository root directory")->default_val(".");
    sub_query->add_option("--db", query_db, "Database path (default: <root>/.codetopo/index.sqlite)");
    sub_query->add_option("tool", query_tool, "Tool name")->required();
    sub_query->add_option("params", query_params, "JSON parameters")->default_val("{}");

    // --- doctor subcommand ---
    auto* sub_doctor = app.add_subcommand("doctor", "Check database health");
    std::string doctor_root = ".";
    std::string doctor_db;

    sub_doctor->add_option("--root", doctor_root, "Repository root directory")->default_val(".");
    sub_doctor->add_option("--db", doctor_db, "Database path (default: <root>/.codetopo/index.sqlite)");

    // --- Parse and dispatch ---
    CLI11_PARSE(app, argc, argv);

    if (sub_index->parsed()) {
        if (index_db.empty()) index_db = codetopo::default_db(index_root);
        codetopo::ensure_codetopo_dir(index_root);
        codetopo::Config cfg;
        cfg.repo_root = index_root;
        cfg.db_path = index_db;
        cfg.thread_count = index_threads;
        cfg.arena_size_mb = index_arena_size;
        cfg.batch_size = index_batch_size;
        cfg.max_file_size_mb = index_max_file_size;
        cfg.max_symbols_per_file = index_max_symbols;
        cfg.no_gitignore = index_no_gitignore;
        cfg.supervised = index_supervised;
        cfg.safe_mode = index_safe_mode;
        try {
            if (cfg.supervised) {
                // Running as a supervised child — index directly
                return codetopo::run_index(cfg);
            } else {
                // Running as top-level — become supervisor
                return codetopo::run_index_supervisor(cfg);
            }
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_init->parsed()) {
        try {
            return codetopo::run_init(init_root, init_editors,
                                      init_threads, init_arena_size,
                                      init_max_file_size,
                                      init_watch, init_freshness);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_mcp->parsed()) {
        if (mcp_db.empty()) mcp_db = codetopo::default_db(mcp_root);

        // R9: Parse freshness policy string
        codetopo::FreshnessPolicy freshness = codetopo::FreshnessPolicy::normal;
        if (mcp_freshness == "eager") freshness = codetopo::FreshnessPolicy::eager;
        else if (mcp_freshness == "lazy") freshness = codetopo::FreshnessPolicy::lazy;
        else if (mcp_freshness == "off") freshness = codetopo::FreshnessPolicy::off;
        // else: remains "normal" (the default)

        try {
            return codetopo::run_mcp(mcp_db, mcp_root, mcp_tool_timeout,
                                     mcp_idle_timeout, freshness, mcp_debounce,
                                     mcp_watch, mcp_verbose);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_watch->parsed()) {
        if (watch_db.empty()) watch_db = codetopo::default_db(watch_root);
        try {
            return codetopo::run_watch(watch_root, watch_db);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_query->parsed()) {
        if (query_db.empty()) query_db = codetopo::default_db(query_root);
        try {
            return codetopo::run_query(query_db, query_tool, query_params);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_doctor->parsed()) {
        if (doctor_db.empty()) doctor_db = codetopo::default_db(doctor_root);
        try {
            return codetopo::run_doctor(doctor_db);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }

    return 0;
}
