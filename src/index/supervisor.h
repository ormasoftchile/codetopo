#pragma once

#include "core/config.h"
#include "db/connection.h"
#include "db/schema.h"
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
extern char** environ;
#endif

namespace codetopo {

// Get the path to the currently running executable.
inline std::string get_self_executable_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";
    // Convert wide to narrow
    int sz = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string result(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), result.data(), sz, nullptr, nullptr);
    return result;
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::string(buf);
    return "";
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    buf[len] = '\0';
    return std::string(buf);
#endif
}

// Spawn a child process and wait for it to exit. Returns exit code.
// On crash/signal, returns a non-zero code.
inline int spawn_and_wait(const std::string& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
    // Build command line
    std::string cmdline = "\"" + exe + "\"";
    for (const auto& arg : args) {
        cmdline += " ";
        // Quote arguments that contain spaces
        if (arg.find(' ') != std::string::npos) {
            cmdline += "\"" + arg + "\"";
        } else {
            cmdline += arg;
        }
    }

    // Create Job Object so child is killed if supervisor dies
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), cmdline.data(), nullptr, nullptr,
                        TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        std::cerr << "ERROR: Failed to spawn child process (error " << GetLastError() << ")\n";
        if (hJob) CloseHandle(hJob);
        return 1;
    }

    // Assign to job object before resuming
    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Wait for child
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    if (hJob) CloseHandle(hJob);

    return static_cast<int>(exit_code);

#else
    // POSIX: fork + exec
    pid_t pid;
    std::vector<const char*> argv;
    argv.push_back(exe.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    int rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr,
                         const_cast<char* const*>(argv.data()), environ);
    if (rc != 0) {
        std::cerr << "ERROR: posix_spawn failed (rc=" << rc << ")\n";
        return 1;
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);  // Convention: 128+signal
    return 1;
#endif
}

// Find the single uncommitted file — the one that's in the work list (on disk,
// changed/new) but not yet in the DB. After a safe-mode crash, at most one
// file was uncommitted because batch_size=1.
inline std::string find_uncommitted_crasher(const Config& config) {
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

    // Scan the repo to find files that should be indexed
    // We look for files that exist on disk but aren't in the DB and aren't quarantined.
    // In safe-mode after a crash, there should be exactly 1 such file (the crasher).
    // However, there may be more if the crash happened early. We return the first one
    // that the indexer would have tried next — but since we don't know the exact order,
    // we look for the file that was being processed (printed to stderr) when the crash happened.
    // As a simpler approach: if there's only 1 uncommitted new/changed file, that's our crasher.
    // If there are more, we can't pinpoint — return empty to let the supervisor retry.

    // For now, use a heuristic: the indexer processes files in scan order.
    // After safe-mode with batch_size=1, the crash happens AFTER at least one commit.
    // Change detection will show the remaining uncommitted files.
    // The first one in the list is likely the crasher (since they're processed in order).

    // Actually, the simplest reliable approach: read the last progress line we printed before crash.
    // But that's complex. Instead, just re-run change detection and if there's only a handful
    // of remaining files, quarantine them all (they'll be retried on clear).

    // Simplified: find all files not in DB and not quarantined. If <= 3, quarantine the first.
    // The design doc says safe-mode isolates to exactly 1 uncommitted file.

    // We actually need the scanner here. But to keep it simple and avoid duplicating scanner logic,
    // we use a different strategy: the supervisor stores the "last committed count" before
    // spawning the child. After crash, it compares. But that's also complex.

    // Best simple approach: re-open DB, scan repo, run change detection, return first new/changed file.
    // This is what the child would do on restart anyway.

    // For the MVP: we don't try to identify the crasher from the supervisor.
    // Instead, we rely on the child's implicit progress:
    // - In safe mode, batch_size=1 means each file is committed individually
    // - After crash, change detection shows what's left
    // - The first file in the remaining work list is the crasher
    // So we'll scan and detect changes to find it.

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

// The supervisor entry point. Spawns the indexer as a child process,
// handles crashes, enters safe mode, and quarantines bad files.
inline int run_index_supervisor(const Config& config,
                                const std::string& exe_path_override = "") {
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
