#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstddef>

namespace codetopo {

unsigned long get_current_process_id();

// Get the path to the currently running executable.
std::string get_self_executable_path();

// Spawn a child process and wait for it to exit. Returns exit code.
// On crash/signal, returns a non-zero code.
int spawn_and_wait(const std::string& exe, const std::vector<std::string>& args);

// Spawn a child process and wait with progress-based timeout.
// progress_path: file the child writes to on each batch commit.
// stall_timeout_s: seconds without progress update before killing the child.
// Returns exit code, or 128+SIGKILL if killed due to stall.
int spawn_and_wait_with_stall_timeout(
    const std::string& exe,
    const std::vector<std::string>& args,
    const std::string& progress_path,
    int stall_timeout_s);

// Spawn a child process, read stdout line-by-line via callback. Returns exit code.
int spawn_and_read_stdout(const std::string& exe,
                          const std::vector<std::string>& args,
                          const std::function<void(const std::string&)>& on_line);

struct CapturedProcessOutput {
    int exit_code = 1;
    std::string output;
    bool truncated = false;
};

CapturedProcessOutput capture_process_stdout(
    const std::string& exe, const std::vector<std::string>& args,
    size_t max_bytes = 64 * 1024 * 1024);

} // namespace codetopo
