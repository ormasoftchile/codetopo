#include "index/supervisor.h"
#include "db/connection.h"
#include "db/schema.h"
#include "util/process.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <algorithm>

namespace codetopo {

// Helper: determine if an exit code indicates a genuine process crash
// (signal death, access violation) vs. a deliberate error return.
// - Exit 0 = success (handled separately)
// - Exit 3 = schema mismatch (handled separately)
// - Exit 1-2 = normal error (parse errors, lock failure) — NOT a crash
// - Negative = Windows NTSTATUS exception code (e.g. 0xC0000005 → -1073741819)
// - > 127 = POSIX signal death (convention: 128 + signal_number)
static bool is_crash_exit_code(int code) {
    if (code < 0) return true;
    if (code > 127) return true;
    return false;
}

// Helper: read worklist file. Each line has tab-separated fields;
// first field is the relative path.
static std::vector<std::string> read_worklist_paths(
    const std::filesystem::path& worklist_path) {
    std::vector<std::string> paths;
    std::ifstream wf(worklist_path);
    std::string line;
    while (std::getline(wf, line)) {
        if (line.empty()) continue;
        auto tab = line.find('\t');
        paths.push_back(tab != std::string::npos ? line.substr(0, tab) : line);
    }
    return paths;
}

// Helper: read the full worklist lines (for rewriting).
static std::vector<std::string> read_worklist_lines(
    const std::filesystem::path& worklist_path) {
    std::vector<std::string> lines;
    std::ifstream wf(worklist_path);
    std::string line;
    while (std::getline(wf, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

int run_index_supervisor(const Config& config,
                         const std::string& exe_path_override) {
    std::string self = exe_path_override.empty() ? get_self_executable_path() : exe_path_override;
    if (self.empty()) {
        std::cerr << "ERROR: Could not determine own executable path\n";
        return 1;
    }

    auto worklist_path = config.db_path;
    worklist_path += ".worklist";
    auto progress_path = config.db_path;
    progress_path += ".progress";

    constexpr int MAX_RESTARTS = 10;
    int consecutive_crashes = 0;
    int no_progress_crashes = 0;  // consecutive crashes with no progress

    int progress_offset = 0;  // cumulative files completed before current attempt
    int original_total = 0;    // total from very first worklist

    for (int attempt = 0; attempt < MAX_RESTARTS; ++attempt) {
        // Build child arguments
        std::vector<std::string> args;
        args.push_back("index");
        args.push_back("--root");
        args.push_back(config.repo_root.string());
        args.push_back("--db");
        args.push_back(config.db_path.string());

        // After 2+ consecutive no-progress crashes, force single thread
        // so the crasher is the only file in-flight and gets quarantined.
        int child_threads = config.thread_count;
        if (no_progress_crashes >= 2) {
            child_threads = 1;
            std::cerr << "Forcing single-thread mode to isolate crasher file...\n";
        }
        args.push_back("--threads");
        args.push_back(std::to_string(child_threads));
        args.push_back("--arena-size");
        args.push_back(std::to_string(config.arena_size_mb));
        args.push_back("--batch-size");
        args.push_back(std::to_string(config.batch_size));
        args.push_back("--max-file-size");
        args.push_back(std::to_string(config.max_file_size_kb));
        if (config.large_arena_size_mb > 0) {
            args.push_back("--large-arena-size");
            args.push_back(std::to_string(config.large_arena_size_mb));
        }
        if (config.large_file_threshold_kb > 0) {
            args.push_back("--large-file-threshold");
            args.push_back(std::to_string(config.large_file_threshold_kb));
        }
        args.push_back("--max-symbols-per-file");
        args.push_back(std::to_string(config.max_symbols_per_file));
        if (config.parse_timeout_s > 0) {
            args.push_back("--parse-timeout");
            args.push_back(std::to_string(config.parse_timeout_s));
        }
        if (config.no_gitignore) args.push_back("--no-gitignore");
        if (config.turbo) args.push_back("--turbo");
        if (config.profile) args.push_back("--profile");
        if (config.max_files > 0) {
            args.push_back("--max-files");
            args.push_back(std::to_string(config.max_files));
        }
        if (config.extraction_timeout_s > 0) {
            args.push_back("--extract-timeout");
            args.push_back(std::to_string(config.extraction_timeout_s));
        }
        for (const auto& pat : config.exclude_patterns) {
            args.push_back("--exclude");
            args.push_back(pat);
        }
        args.push_back("--supervised");
        if (config.safe_mode) args.push_back("--safe-mode");
        if (attempt > 0) {
            args.push_back("--resume");
            if (original_total > 0) {
                args.push_back("--progress-offset");
                args.push_back(std::to_string(progress_offset));
                args.push_back("--progress-total");
                args.push_back(std::to_string(original_total));
            }
        }

        if (attempt > 0) {
            std::cerr << "Restarting indexer (attempt " << (attempt + 1)
                      << ", safe_mode=" << (config.safe_mode ? "true" : "false") << ")...\n";
        }

        // Remove stale .progress before each child run so we don't
        // confuse it with the new child's progress.
        std::filesystem::remove(progress_path);

        int exit_code = spawn_and_wait(self, args);

        if (exit_code == 0) {
            // Success — clean up temp files
            std::filesystem::remove(worklist_path);
            std::filesystem::remove(progress_path);
            return 0;
        }
        if (exit_code == 3) return 3;  // Schema mismatch — don't retry

        // Normal error exit (code 1-2): child completed its work but had
        // non-fatal errors (e.g. parse failures). Do NOT trigger crash
        // recovery — no quarantine, no worklist trimming, no restart.
        // Real crashes produce negative codes (Windows NTSTATUS) or >127 (POSIX signals).
        if (!is_crash_exit_code(exit_code)) {
            std::cerr << "Indexer exited with code " << exit_code
                      << " (normal error, not a crash — skipping recovery)\n";
            std::filesystem::remove(worklist_path);
            std::filesystem::remove(progress_path);
            return exit_code;
        }

        consecutive_crashes++;
        std::cerr << "Indexer exited with code " << exit_code
                  << " (crash #" << consecutive_crashes << ")\n";

        // --- Crash recovery ---
        // The child writes the relative path of the last committed file to
        // .progress after each batch commit. Read it to determine what was
        // committed vs. what was in-flight when the crash happened.
        std::string last_committed_path;
        if (std::filesystem::exists(progress_path)) {
            std::ifstream pf(progress_path);
            std::getline(pf, last_committed_path);
        }

        if (!last_committed_path.empty() && std::filesystem::exists(worklist_path)) {
            auto paths = read_worklist_paths(worklist_path);
            auto lines = read_worklist_lines(worklist_path);

            // Set original_total on first crash (before any trimming)
            if (original_total == 0)
                original_total = static_cast<int>(paths.size());

            // Find the last committed file in the worklist
            int committed_idx = -1;
            for (int j = 0; j < static_cast<int>(paths.size()); ++j) {
                if (paths[j] == last_committed_path) {
                    committed_idx = j;
                    break;
                }
            }

            if (committed_idx >= 0) {
                int tc = config.thread_count > 0 ? config.thread_count
                         : static_cast<int>(std::thread::hardware_concurrency());

                // Quarantine the next window_size files after last committed
                // (they were potentially being parsed when crash happened)
                int window = tc + 2;
                int q_start = committed_idx + 1;
                int q_end = (std::min)(q_start + window, static_cast<int>(paths.size()));

                Connection conn(config.db_path);
                schema::ensure_quarantine_table(conn);
                int quarantined = 0;
                for (int j = q_start; j < q_end; ++j) {
                    schema::quarantine_file(conn, paths[j],
                        "Process crashed during parse/extract (exit code " +
                        std::to_string(exit_code) + ")");
                    quarantined++;
                }
                std::cerr << "Quarantine: " << quarantined
                          << " file(s) will be skipped\n";

                // Trim worklist: remove committed + quarantined entries.
                // The child on resume will load only the remaining files.
                int new_start = q_end;
                int remaining = static_cast<int>(lines.size()) - new_start;
                if (remaining > 0) {
                    std::ofstream wf(worklist_path, std::ios::trunc);
                    for (int j = new_start; j < static_cast<int>(lines.size()); ++j) {
                        wf << lines[j] << '\n';
                    }
                    progress_offset = original_total - remaining;
                    std::cerr << "Trimmed worklist: " << remaining
                              << " files remaining\n";
                } else {
                    // All files processed or quarantined
                    std::filesystem::remove(worklist_path);
                    progress_offset = original_total;
                }
                consecutive_crashes = 0;
                no_progress_crashes = 0;
            } else {
                std::cerr << "WARN: Last committed file not found in worklist. Restarting...\n";
            }
        } else if (std::filesystem::exists(worklist_path)) {
            // No progress file — crash happened before the first batch commit.
            // Quarantine window_size files (= thread_count + 2) since that's
            // how many files are actually in-flight at any time.
            no_progress_crashes++;

            auto paths = read_worklist_paths(worklist_path);
            auto lines = read_worklist_lines(worklist_path);

            if (original_total == 0)
                original_total = static_cast<int>(paths.size());

            int tc = config.thread_count > 0 ? config.thread_count
                     : static_cast<int>(std::thread::hardware_concurrency());
            // After forced single-thread, quarantine window_size=3
            if (no_progress_crashes >= 3) tc = 1;
            int window_size = tc + 2;
            int q_end = (std::min)(window_size, static_cast<int>(paths.size()));

            Connection conn(config.db_path);
            schema::ensure_quarantine_table(conn);
            for (int j = 0; j < q_end; ++j) {
                schema::quarantine_file(conn, paths[j],
                    "Process crashed before first commit (exit code " +
                    std::to_string(exit_code) + ")");
            }
            std::cerr << "Quarantine: " << q_end
                      << " file(s) will be skipped (no progress)\n";

            int remaining = static_cast<int>(lines.size()) - q_end;
            if (remaining > 0) {
                std::ofstream wf(worklist_path, std::ios::trunc);
                for (int j = q_end; j < static_cast<int>(lines.size()); ++j) {
                    wf << lines[j] << '\n';
                }
                progress_offset = original_total - remaining;
                std::cerr << "Trimmed worklist: " << remaining
                          << " files remaining\n";
            } else {
                std::filesystem::remove(worklist_path);
                progress_offset = original_total;
            }
            // Don't reset consecutive_crashes — still failing
        } else {
            std::cerr << "WARN: No progress file or worklist found. Restarting...\n";
        }
    }

    std::cerr << "ERROR: Max restarts (" << MAX_RESTARTS << ") reached. Aborting.\n";
    return 1;
}

} // namespace codetopo
