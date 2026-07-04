#pragma once

#include "core/config.h"
#include "core/arena.h"
#include "core/arena_pool.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/fts.h"
#include "index/scanner.h"
#include "index/change_detector.h"
#include "index/parser.h"
#include "index/extractor.h"
#include "index/persister.h"
#include "util/hash.h"
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
inline int run_watch(const std::string& root_str, const std::string& db_path_str) {
    namespace fs = std::filesystem;

    auto repo_root = fs::canonical(root_str);
    fs::path db_path = db_path_str;

    register_arena_allocator();

    std::cerr << "Watching " << repo_root.string() << " for changes...\n";

    ArenaPool arena_pool(1, 128 * 1024 * 1024);

    auto reindex = [&](const std::vector<WatchEvent>& events) {
        std::cerr << "Detected " << events.size() << " change(s), re-indexing...\n";

        try {
            Connection conn(db_path);
            schema::ensure_schema(conn);
            fts::create_sync_triggers(conn);

            Config cfg;
            cfg.repo_root = repo_root;
            cfg.db_path = db_path;

            Scanner scanner(cfg);
            auto scanned = scanner.scan();

            ChangeDetector detector(conn);
            auto changes = detector.detect(scanned);

            Persister persister(conn);
            persister.prune_deleted(changes.deleted_paths);

            auto work = changes.new_files;
            work.insert(work.end(), changes.changed_files.begin(), changes.changed_files.end());

            for (auto& file : work) {
                auto lease = ArenaLease(arena_pool);
                set_thread_arena(lease.get());

                auto content = read_file_content(file.absolute_path);
                if (content.empty()) continue;

                auto hash = hash_string(content);

                Parser parser;
                if (!parser.set_language(file.language)) {
                    persister.persist_file(file, ExtractionResult{}, hash, "skipped");
                    continue;
                }

                auto tree = TreeGuard(parser.parse(content));
                if (!tree) {
                    persister.persist_file(file, ExtractionResult{}, hash, "failed");
                    continue;
                }

                Extractor extractor(cfg.max_symbols_per_file, cfg.max_ast_depth);
                auto result = extractor.extract(tree.tree, content, file.language, file.relative_path);
                persister.persist_file(file, result, hash, result.truncated ? "partial" : "ok",
                                       result.truncated ? result.truncation_reason : "");
            }

            persister.write_metadata(repo_root.string());
            conn.wal_checkpoint();

            std::cerr << "Re-indexed " << work.size() << " file(s), pruned "
                      << changes.deleted_paths.size() << " file(s)\n";
        } catch (const std::exception& e) {
            std::cerr << "Re-index error: " << e.what() << "\n";
        }
    };

    Watcher watcher(repo_root, reindex);
    watcher.start();

    // Block until interrupted (Ctrl+C) or terminated. Signal-based rather than
    // stdin-based so the watcher survives being backgrounded / stdin-less.
    std::signal(SIGINT, watch_signal_handler);
    std::signal(SIGTERM, watch_signal_handler);
    std::cerr << "Press Ctrl+C to stop watching.\n";
    while (g_watch_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cerr << "Stopping watcher...\n";

    return 0;
}

} // namespace codetopo
