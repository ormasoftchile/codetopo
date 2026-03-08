#pragma once

#include <string>
#include <vector>

namespace codetopo {

// Get the path to the currently running executable.
std::string get_self_executable_path();

// Spawn a child process and wait for it to exit. Returns exit code.
// On crash/signal, returns a non-zero code.
int spawn_and_wait(const std::string& exe, const std::vector<std::string>& args);

} // namespace codetopo
