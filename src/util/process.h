#pragma once

#include <string>
#include <vector>
#include <functional>

namespace codetopo {

// Get the path to the currently running executable.
std::string get_self_executable_path();

// Spawn a child process and wait for it to exit. Returns exit code.
// On crash/signal, returns a non-zero code.
int spawn_and_wait(const std::string& exe, const std::vector<std::string>& args);

// Spawn a child process, read stdout line-by-line via callback. Returns exit code.
int spawn_and_read_stdout(const std::string& exe,
                          const std::vector<std::string>& args,
                          const std::function<void(const std::string&)>& on_line);

} // namespace codetopo
