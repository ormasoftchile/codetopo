#pragma once

#include "core/config.h"
#include "db/connection.h"
#include "db/schema.h"
#include "index/ownership.h"
#include "mcp/reindex.h"
#include "watch/watcher.h"
#include <iostream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

namespace codetopo {

// Set by SIGINT/SIGTERM to stop the watch loop. Using a signal (not stdin)
// means `codetopo watch` keeps running when backgrounded or launched with a
// closed/redirected stdin — a bare `std::getline(std::cin, …)` returns EOF
// immediately in those cases and the process would exit at once.
namespace {
volatile std::sig_atomic_t g_watch_stop = 0;
void watch_signal_handler(int) { g_watch_stop = 1; }
}

// T096: cmd_watch — starts watcher and triggers incremental indexing.
inline int run_watch(const std::string& root_str, const std::string& db_path_str,
                     bool root_was_explicit = true) {
    namespace fs = std::filesystem;

    if (!fs::exists(db_path_str)) {
        std::cerr << "ERROR: Database does not exist at " << db_path_str << "\n";
        return 1;
    }
    auto db_path = fs::canonical(db_path_str);
    index_ownership::RootResolution resolution;
    {
        Connection probe(db_path.string(), true);
        resolution = index_ownership::resolve_primary_root(
            probe, root_str, root_was_explicit, db_path.string());
    }
    auto repo_root = resolution.root;

    std::cerr << "Watching " << repo_root.string() << " for changes...\n";
    if (!resolution.metadata_present) {
        std::cerr << "WARN: repo_root metadata is missing; the first successful full "
                     "reconciliation will establish ownership without deletion pruning\n";
    }

    ReindexState reindex_state;
    auto reindex_callback = [&](const std::vector<WatchEvent>& events) {
        std::cerr << "Detected " << events.size() << " change(s), re-indexing...\n";
        reindex_state.trigger(
            repo_root.string(), db_path.string(), [] {}, {}, true,
            ReindexReason::watcher);
    };

    Watcher watcher(repo_root, reindex_callback);
    watcher.start();

    // Block until interrupted (Ctrl+C) or terminated. Signal-based rather than
    // stdin-based so the watcher survives being backgrounded / stdin-less.
    std::signal(SIGINT, watch_signal_handler);
    std::signal(SIGTERM, watch_signal_handler);
    g_watch_stop = 0;
    std::cerr << "Press Ctrl+C to stop watching.\n";
    while (g_watch_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cerr << "Stopping watcher...\n";

    return 0;
}

} // namespace codetopo
