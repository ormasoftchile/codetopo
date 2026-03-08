#pragma once

#include "core/config.h"
#include <string>

namespace codetopo {

// Find the single uncommitted file after a safe-mode crash.
// Returns empty string if none can be identified.
std::string find_uncommitted_crasher(const Config& config);

// The supervisor entry point. Spawns the indexer as a child process,
// handles crashes, enters safe mode, and quarantines bad files.
int run_index_supervisor(const Config& config,
                         const std::string& exe_path_override = "");

} // namespace codetopo
