#pragma once

#include <string>
#include <cstdint>
#include <thread>
#include <filesystem>

namespace codetopo {

struct Config {
    // Indexer settings
    std::filesystem::path repo_root = ".";
    std::filesystem::path db_path = "codetopo.sqlite";
    int thread_count = 0;  // 0 = auto (hardware_concurrency)
    int arena_size_mb = 128;
    int batch_size = 100;
    int max_file_size_mb = 10;
    int max_symbols_per_file = 50000;
    int max_ast_depth = 500;
    bool no_gitignore = false;
    bool supervised = false;   // True when running as a supervised child process
    bool safe_mode = false;    // True = commit after every file (for crash isolation)

    // MCP server settings
    int tool_timeout_s = 10;
    int idle_timeout_s = 1800;  // 30 minutes

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

    size_t max_file_size_bytes() const {
        return static_cast<size_t>(max_file_size_mb) * 1024 * 1024;
    }
};

} // namespace codetopo
