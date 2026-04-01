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
#include "cli/cmd_skills.h"
#include "cli/cmd_parse_file.h"
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
    int index_large_arena_size = 0;
    int index_large_file_threshold = 0;
    int index_batch_size = 500;
    int index_max_file_size = 10240;
    int index_max_symbols = 50000;
    bool index_no_gitignore = false;
    bool index_turbo = false;
    std::vector<std::string> index_exclude;
    bool index_supervised = false;
    bool index_safe_mode = false;
    bool index_resume = false;

    sub_index->add_option("--root", index_root, "Repository root directory")->default_val(".");
    sub_index->add_option("--db", index_db, "Database path (default: <root>/.codetopo/index.sqlite)");
    sub_index->add_option("--threads", index_threads, "Worker thread count (0=auto)")->default_val(0);
    sub_index->add_option("--arena-size", index_arena_size, "Arena size in MB per thread")->default_val(128);
    sub_index->add_option("--large-arena-size", index_large_arena_size, "Large arena size in MB for oversized files (0=disabled)")->default_val(0);
    sub_index->add_option("--large-file-threshold", index_large_file_threshold, "File size in KB above which the large arena is used (0=auto: arena/30)")->default_val(0);
    sub_index->add_option("--batch-size", index_batch_size, "Files per transaction batch")->default_val(500);
    sub_index->add_flag("--turbo", index_turbo, "Aggressive perf: synchronous=OFF, batch=1000, larger cache");
    int index_parse_timeout = 30;
    sub_index->add_option("--max-file-size", index_max_file_size, "Max file size in KB")->default_val(10240);
    sub_index->add_option("--parse-timeout", index_parse_timeout, "Per-file parse timeout in seconds (0=no limit)")->default_val(30);
    sub_index->add_option("--max-symbols-per-file", index_max_symbols, "Max symbols per file")->default_val(50000);
    sub_index->add_flag("--no-gitignore", index_no_gitignore, "Disable .gitignore filtering");
    sub_index->add_option("--exclude", index_exclude, "Glob patterns to exclude (repeatable, e.g. **/GlobalSuppressions.cs)");
    sub_index->add_flag("--supervised", index_supervised, "Run as supervised child (internal)")->group("");
    sub_index->add_flag("--safe-mode", index_safe_mode, "Commit after every file (internal)")->group("");
    sub_index->add_flag("--resume", index_resume, "Resume from cached worklist (internal)")->group("");
    int index_progress_offset = 0;
    int index_progress_total = 0;
    int index_max_files = 0;
    int index_extract_timeout = 10;
    bool index_profile = false;
    sub_index->add_option("--progress-offset", index_progress_offset, "Files already done (internal)")->group("");
    sub_index->add_option("--progress-total", index_progress_total, "Original total (internal)")->group("");
    sub_index->add_option("--max-files", index_max_files, "Max files to index (0=unlimited, for profiling)")->default_val(0);
    sub_index->add_option("--extract-timeout", index_extract_timeout, "Per-file extraction timeout in seconds (0=no limit)")->default_val(10);
    sub_index->add_flag("--profile", index_profile, "Enable per-phase profiling output");

    // --- init subcommand ---
    auto* sub_init = app.add_subcommand("init", "Index a repository and configure editors for MCP");
    std::string init_root = ".";
    std::string init_editors = "auto";
    int init_threads = 0;
    int init_arena_size = 128;
    int init_large_arena_size = 0;
    int init_large_file_threshold = 0;
    int init_max_file_size = 10240;
    bool init_watch = true;
    bool init_turbo = false;
    std::vector<std::string> init_exclude;
    std::string init_freshness = "normal";

    sub_init->add_option("--root", init_root, "Repository root directory")->default_val(".");
    sub_init->add_option("--editors", init_editors,
        "Comma-separated editors: vscode,cursor,windsurf,claude,auto")->default_val("auto");
    sub_init->add_option("--threads", init_threads, "Worker thread count (0=auto)")->default_val(0);
    sub_init->add_option("--arena-size", init_arena_size, "Arena size in MB per thread")->default_val(128);
    sub_init->add_option("--large-arena-size", init_large_arena_size, "Large arena size in MB for oversized files (0=disabled)")->default_val(0);
    sub_init->add_option("--large-file-threshold", init_large_file_threshold, "File size in KB above which the large arena is used (0=auto: arena/30)")->default_val(0);
    int init_parse_timeout = 30;
    sub_init->add_option("--max-file-size", init_max_file_size, "Max file size in KB")->default_val(10240);
    sub_init->add_option("--parse-timeout", init_parse_timeout, "Per-file parse timeout in seconds (0=no limit)")->default_val(30);
    sub_init->add_flag("--turbo", init_turbo, "Aggressive perf: synchronous=OFF, batch=1000, larger cache");
    sub_init->add_option("--exclude", init_exclude, "Glob patterns to exclude (repeatable, e.g. **/GlobalSuppressions.cs)");
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
    bool mcp_watch = false;
    sub_mcp->add_option("--freshness", mcp_freshness,
        "Index freshness policy: eager|normal|lazy|off")->default_val("normal");
    sub_mcp->add_option("--debounce", mcp_debounce,
        "Watcher debounce in ms")->default_val(1000);
    sub_mcp->add_flag("--watch", mcp_watch,
        "Enable filesystem watcher for auto-reindex");

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

    // --- skills subcommand ---
    auto* sub_skills = app.add_subcommand("skills", "Install agent skill files into a repository");
    std::string skills_action;
    std::string skills_name;
    std::string skills_root = ".";

    sub_skills->add_option("action", skills_action, "Action: list or install")->required();
    sub_skills->add_option("name", skills_name, "Skill name (or 'all')");
    sub_skills->add_option("--root", skills_root, "Repository root directory")->default_val(".");

    // --- parse-file subcommand ---
    auto* sub_parse_file = app.add_subcommand("parse-file", "Parse a single file and show detailed diagnostics");
    std::string pf_file;
    int pf_arena_size = 1024;
    int pf_timeout = 0;
    int pf_max_symbols = 50000;
    bool pf_symbols = false;
    bool pf_refs = false;
    bool pf_edges = false;

    sub_parse_file->add_option("file", pf_file, "File path to parse")->required();
    sub_parse_file->add_option("--arena-size", pf_arena_size, "Arena size in MB")->default_val(1024);
    sub_parse_file->add_option("--parse-timeout", pf_timeout, "Parse timeout in seconds (0=no limit)")->default_val(0);
    sub_parse_file->add_option("--max-symbols", pf_max_symbols, "Max symbols to extract")->default_val(50000);
    sub_parse_file->add_flag("--symbols", pf_symbols, "Show all extracted symbols");
    sub_parse_file->add_flag("--refs", pf_refs, "Show all extracted references");
    sub_parse_file->add_flag("--edges", pf_edges, "Show all extracted edges");

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
        cfg.large_arena_size_mb = index_large_arena_size;
        cfg.large_file_threshold_kb = index_large_file_threshold;
        cfg.batch_size = index_batch_size;
        cfg.max_file_size_kb = index_max_file_size;
        cfg.parse_timeout_s = index_parse_timeout;
        cfg.max_symbols_per_file = index_max_symbols;
        cfg.no_gitignore = index_no_gitignore;
        cfg.turbo = index_turbo;
        cfg.exclude_patterns = index_exclude;
        cfg.supervised = index_supervised;
        cfg.safe_mode = index_safe_mode;
        cfg.resume = index_resume;
        cfg.progress_offset = index_progress_offset;
        cfg.progress_total = index_progress_total;
        cfg.max_files = index_max_files;
        cfg.extraction_timeout_s = index_extract_timeout;
        cfg.profile = index_profile;
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
                                      init_large_arena_size,
                                      init_large_file_threshold,
                                      init_max_file_size,
                                      init_parse_timeout,
                                      init_turbo,
                                      init_exclude,
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
                                     mcp_watch);
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
    if (sub_skills->parsed()) {
        return codetopo::run_skills(skills_action, skills_name, skills_root);
    }
    if (sub_parse_file->parsed()) {
        codetopo::ParseFileConfig pf_cfg;
        pf_cfg.file_path = pf_file;
        pf_cfg.arena_size_mb = pf_arena_size;
        pf_cfg.parse_timeout_s = pf_timeout;
        pf_cfg.max_symbols = pf_max_symbols;
        pf_cfg.show_symbols = pf_symbols;
        pf_cfg.show_refs = pf_refs;
        pf_cfg.show_edges = pf_edges;
        try {
            return codetopo::run_parse_file(pf_cfg);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: " << e.what() << "\n";
            return 1;
        }
    }

    return 0;
}
