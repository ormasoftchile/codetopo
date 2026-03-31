#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <thread>
#include <filesystem>

namespace codetopo {

enum class FreshnessPolicy { eager, normal, lazy, off };

struct Config {
    // Indexer settings
    std::filesystem::path repo_root = ".";
    std::filesystem::path db_path = "codetopo.sqlite";
    int thread_count = 0;  // 0 = auto (hardware_concurrency)
    int arena_size_mb = 128;
    int large_arena_size_mb = 0;  // 0 = disabled; separate arena for oversized files
    int large_file_threshold_kb = 0;  // 0 = auto (arena_size / 30); file size above which the large arena is used
    int batch_size = 500;
    int max_file_size_kb = 10240;  // 10 MB in KB
    int parse_timeout_s = 30;  // Per-file tree-sitter parse timeout in seconds (0=no limit)
    int max_symbols_per_file = 50000;
    int max_ast_depth = 200;
    int max_files = 0;  // 0 = unlimited; limit total files scanned (for profiling/subset runs)
    bool no_gitignore = false;
    bool turbo = false;  // Aggressive perf: synchronous=OFF, batch=1000, larger cache
    std::vector<std::string> exclude_patterns;  // Glob patterns to exclude (e.g. **/GlobalSuppressions.cs)
    bool supervised = false;   // True when running as a supervised child process
    bool safe_mode = false;    // True = commit after every file (for crash isolation)
    bool resume = false;       // True = load cached worklist instead of rescanning
    bool profile = false;      // True = print detailed per-phase profiling report
    int progress_offset = 0;   // Files already completed in prior runs (for display)
    int progress_total = 0;    // Original total file count (0 = use work_list.size())

    // MCP server settings
    int tool_timeout_s = 10;
    int idle_timeout_s = 1800;  // 30 minutes
    FreshnessPolicy freshness = FreshnessPolicy::normal;
    int debounce_ms = 1000;

    // Logging settings
    int64_t log_max_size_bytes = 50 * 1024 * 1024;  // 50 MB
    int log_max_files = 3;

    // Derived values
    int effective_thread_count() const {
        if (thread_count > 0) return thread_count;
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        return hw > 0 ? hw : 4;
    }

    size_t arena_size_bytes() const {
        return static_cast<size_t>(arena_size_mb) * 1024 * 1024;
    }

    size_t large_arena_size_bytes() const {
        return static_cast<size_t>(large_arena_size_mb) * 1024 * 1024;
    }

    size_t large_file_threshold_bytes() const {
        if (large_file_threshold_kb > 0)
            return static_cast<size_t>(large_file_threshold_kb) * 1024;
        // Auto: files needing >arena_size for parsing (tree-sitter uses ~30x file size)
        return arena_size_bytes() / 30;
    }

    size_t max_file_size_bytes() const {
        return static_cast<size_t>(max_file_size_kb) * 1024;
    }
};

} // namespace codetopo
