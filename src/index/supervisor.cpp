#include "index/supervisor.h"
#include "index/scanner.h"
#include "index/change_detector.h"
#include "db/schema.h"
#include "util/process.h"
#include <iostream>

namespace codetopo {

std::string find_uncommitted_crasher(const Config& config) {
    namespace fs = std::filesystem;

    // Open DB read-only to find what's NOT yet persisted
    Connection conn(config.db_path);

    // Load quarantine to exclude already-quarantined files
    auto quarantined = schema::load_quarantine(conn);

    // Load all known file paths from DB
    std::unordered_set<std::string> db_paths;
    {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(conn.raw(),
            "SELECT path FROM files", -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                db_paths.insert(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }

    Scanner scanner(config);
    auto scanned = scanner.scan();

    ChangeDetector detector(conn);
    auto changes = detector.detect(scanned);

    // Merge new + changed, excluding quarantined
    std::vector<std::string> candidates;
    for (const auto& f : changes.new_files) {
        if (!quarantined.count(f.relative_path))
            candidates.push_back(f.relative_path);
    }
    for (const auto& f : changes.changed_files) {
        if (!quarantined.count(f.relative_path))
            candidates.push_back(f.relative_path);
    }

    if (candidates.empty()) return "";
    // In safe mode (batch_size=1), the first uncommitted file is the crasher
    return candidates.front();
}

int run_index_supervisor(const Config& config,
                         const std::string& exe_path_override) {
    std::string self = exe_path_override.empty() ? get_self_executable_path() : exe_path_override;
    if (self.empty()) {
        std::cerr << "ERROR: Could not determine own executable path\n";
        return 1;
    }

    constexpr int MAX_RESTARTS = 10;
    int consecutive_crashes = 0;
    bool safe_mode = false;

    for (int attempt = 0; attempt < MAX_RESTARTS; ++attempt) {
        // Build child arguments
        std::vector<std::string> args;
        args.push_back("index");
        args.push_back("--root");
        args.push_back(config.repo_root.string());
        args.push_back("--db");
        args.push_back(config.db_path.string());
        args.push_back("--threads");
        args.push_back(std::to_string(config.thread_count));
        args.push_back("--arena-size");
        args.push_back(std::to_string(config.arena_size_mb));
        args.push_back("--batch-size");
        args.push_back(std::to_string(config.batch_size));
        args.push_back("--max-file-size");
        args.push_back(std::to_string(config.max_file_size_mb));
        args.push_back("--max-symbols-per-file");
        args.push_back(std::to_string(config.max_symbols_per_file));
        if (config.no_gitignore) args.push_back("--no-gitignore");
        args.push_back("--supervised");
        if (safe_mode) args.push_back("--safe-mode");

        if (attempt > 0) {
            std::cerr << "Restarting indexer (attempt " << (attempt + 1)
                      << ", safe_mode=" << (safe_mode ? "true" : "false") << ")...\n";
        }

        int exit_code = spawn_and_wait(self, args);

        if (exit_code == 0) return 0;  // Success
        if (exit_code == 3) return 3;  // Schema mismatch — don't retry

        // Child crashed or failed abnormally
        consecutive_crashes++;
        std::cerr << "Indexer exited with code " << exit_code
                  << " (crash #" << consecutive_crashes << ")\n";

        if (safe_mode) {
            // Safe mode (batch_size=1) still crashed → isolate the crasher
            std::string crasher = find_uncommitted_crasher(config);
            if (!crasher.empty()) {
                // Quarantine the file
                Connection conn(config.db_path);
                schema::ensure_quarantine_table(conn);
                schema::quarantine_file(conn, crasher,
                    "Process crashed during parse/extract (exit code " +
                    std::to_string(exit_code) + ")");
                std::cerr << "Quarantined: " << crasher << "\n";
            } else {
                std::cerr << "WARN: Could not identify crasher file\n";
            }
            safe_mode = false;
            consecutive_crashes = 0;
        } else if (consecutive_crashes >= 2) {
            // Two crashes at normal batch size → enter safe mode
            safe_mode = true;
            std::cerr << "Entering safe mode (per-file commit) to isolate crasher\n";
        }
    }

    std::cerr << "ERROR: Max restarts (" << MAX_RESTARTS << ") reached. Aborting.\n";
    return 1;
}

} // namespace codetopo
