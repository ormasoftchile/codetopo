#include <CLI/CLI.hpp>
#include <string>
#include <iostream>
#include "core/config.h"
#include "cli/cmd_index.h"
#include "cli/cmd_mcp.h"
#include "cli/cmd_doctor.h"
#include "cli/cmd_watch.h"

int main(int argc, char** argv) {
    CLI::App app{"codetopo — local code graph indexer + MCP server"};
    app.require_subcommand(1);

    // --- index subcommand ---
    auto* sub_index = app.add_subcommand("index", "Index a repository into a code graph");
    std::string index_root = ".";
    std::string index_db = "codetopo.sqlite";
    int index_threads = 0;  // 0 = auto (CPU count)
    int index_arena_size = 128;  // MB
    int index_batch_size = 100;
    int index_max_file_size = 10;  // MB
    int index_max_symbols = 50000;
    bool index_no_gitignore = false;

    sub_index->add_option("--root", index_root, "Repository root directory")->default_val(".");
    sub_index->add_option("--db", index_db, "Output database path")->default_val("codetopo.sqlite");
    sub_index->add_option("--threads", index_threads, "Worker thread count (0=auto)")->default_val(0);
    sub_index->add_option("--arena-size", index_arena_size, "Arena size in MB per thread")->default_val(128);
    sub_index->add_option("--batch-size", index_batch_size, "Files per transaction batch")->default_val(100);
    sub_index->add_option("--max-file-size", index_max_file_size, "Max file size in MB")->default_val(10);
    sub_index->add_option("--max-symbols-per-file", index_max_symbols, "Max symbols per file")->default_val(50000);
    sub_index->add_flag("--no-gitignore", index_no_gitignore, "Disable .gitignore filtering");

    // --- mcp subcommand ---
    auto* sub_mcp = app.add_subcommand("mcp", "Start MCP server over stdio");
    std::string mcp_db = "codetopo.sqlite";
    int mcp_tool_timeout = 10;  // seconds
    int mcp_idle_timeout = 1800;  // 30 minutes in seconds

    sub_mcp->add_option("--db", mcp_db, "Database path")->required();
    sub_mcp->add_option("--tool-timeout", mcp_tool_timeout, "Tool timeout in seconds")->default_val(10);
    sub_mcp->add_option("--idle-timeout", mcp_idle_timeout, "Idle timeout in seconds (0=disable)")->default_val(1800);

    // --- watch subcommand ---
    auto* sub_watch = app.add_subcommand("watch", "Watch for file changes and re-index");
    std::string watch_root = ".";
    std::string watch_db = "codetopo.sqlite";

    sub_watch->add_option("--root", watch_root, "Repository root directory")->default_val(".");
    sub_watch->add_option("--db", watch_db, "Database path")->default_val("codetopo.sqlite");

    // --- query subcommand ---
    auto* sub_query = app.add_subcommand("query", "Run an ad-hoc tool query");
    std::string query_db = "codetopo.sqlite";
    std::string query_tool;
    std::string query_params = "{}";

    sub_query->add_option("--db", query_db, "Database path")->required();
    sub_query->add_option("tool", query_tool, "Tool name")->required();
    sub_query->add_option("params", query_params, "JSON parameters")->default_val("{}");

    // --- doctor subcommand ---
    auto* sub_doctor = app.add_subcommand("doctor", "Check database health");
    std::string doctor_db = "codetopo.sqlite";

    sub_doctor->add_option("--db", doctor_db, "Database path")->required();

    // --- Parse and dispatch ---
    CLI11_PARSE(app, argc, argv);

    if (sub_index->parsed()) {
        codetopo::Config cfg;
        cfg.repo_root = index_root;
        cfg.db_path = index_db;
        cfg.thread_count = index_threads;
        cfg.arena_size_mb = index_arena_size;
        cfg.batch_size = index_batch_size;
        cfg.max_file_size_mb = index_max_file_size;
        cfg.max_symbols_per_file = index_max_symbols;
        cfg.no_gitignore = index_no_gitignore;
        try {
            return codetopo::run_index(cfg);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_mcp->parsed()) {
        try {
            return codetopo::run_mcp(mcp_db, mcp_tool_timeout, mcp_idle_timeout);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_watch->parsed()) {
        try {
            return codetopo::run_watch(watch_root, watch_db);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_query->parsed()) {
        try {
            return codetopo::run_query(query_db, query_tool, query_params);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }
    if (sub_doctor->parsed()) {
        try {
            return codetopo::run_doctor(doctor_db);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }

    return 0;
}
