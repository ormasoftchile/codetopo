// T045-T050: Full index command implementation with parallel parse.
// Split from cmd_index.h — definition moved here to reduce compile times.

#include "cli/cmd_index.h"

#include "core/arena.h"
#include "core/arena_pool.h"
#include "core/thread_pool.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/fts.h"
#include "index/scanner.h"
#include "index/change_detector.h"
#include "index/parser.h"
#include "index/extractor.h"
#include "index/persister.h"
#include "util/hash.h"
#include "util/git.h"
#include "util/lock.h"
#include "util/profiler.h"
#include <iostream>
#include <iomanip>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <thread>
#include <vector>
#include <future>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <eh.h>

// C++ exception wrapper for SEH exceptions
struct SehException : std::exception {
    DWORD code;
    SehException(DWORD c) : code(c) {}
    const char* what() const noexcept override { return "SEH exception"; }
};

// Per-thread SEH translator — converts SEH to C++ exception
static void seh_translator(unsigned int code, EXCEPTION_POINTERS*) {
    throw SehException(static_cast<DWORD>(code));
}

// Process-wide vectored exception handler for stack overflow recovery
static LONG WINAPI vectored_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        // Can't do much here — just let it pass to SEH
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

namespace codetopo {

// Forward declarations for arena thread-local management
void set_thread_arena(Arena* arena);
void register_arena_allocator();

// Result of parsing + extracting a single file (produced on worker threads).
struct ParsedFile {
    ScannedFile file;
    ExtractionResult extraction;
    std::string content_hash;
    std::string parse_status;   // ok, partial, failed, skipped
    std::string parse_error;
    bool has_error = false;
    int64_t extract_us = 0;    // per-file extraction time for slow-file logging (R4)
};

// T045-T050: Full index command implementation with parallel parse.
int run_index(const Config& config) {
    namespace fs = std::filesystem;

    auto repo_root = fs::canonical(config.repo_root);
    auto db_path = config.db_path;

    // Acquire lock (T014/FR-036)
    auto lock_path = db_path;
    lock_path += ".lock";
    FileLock lock(lock_path);
    if (!lock.acquire()) {
        std::cerr << "ERROR: Another indexer is running (PID " << lock.holder_pid() << ")\n";
        return 1;
    }
    if (lock.was_stale_broken()) {
        std::cerr << "WARN: Broke stale lock from dead process\n";
    }

    // Register arena allocator with Tree-sitter (T008)
    register_arena_allocator();

    // Profiler — activated by --profile flag
    Profiler profiler;
    profiler.enabled = config.profile;
    auto pipeline_start = std::chrono::steady_clock::now();

    // Open DB and ensure schema (T010, T011)
    Connection conn(db_path);
    int schema_rc = schema::ensure_schema(conn);
    if (schema_rc != 0) {
        std::cerr << "ERROR: Schema version mismatch (exit code 3)\n";
        return 3;
    }
    // FTS triggers created later — after bulk inserts for performance

    // Ensure quarantine table exists (additive migration)
    schema::ensure_quarantine_table(conn);

    // Load quarantined file paths
    auto quarantined = schema::load_quarantine(conn);
    if (!quarantined.empty()) {
        std::cerr << "Quarantine: " << quarantined.size() << " file(s) will be skipped\n";
    }

    // Scan repository (T025-T028)
    std::cerr << "Scanning " << repo_root.string() << "...\n";
    Scanner scanner(config);
    std::vector<ScannedFile> scanned_files;
    {
        ScopedPhase _sp(profiler.scan);
        scanned_files = scanner.scan();
    }
    std::cerr << "Found " << scanned_files.size() << " source files\n";

    // Detect changes (T029)
    ChangeDetector detector(conn);
    ChangeDetector::ChangeResult changes;
    {
        ScopedPhase _sp(profiler.change_detect);
        changes = detector.detect(scanned_files);
    }

    std::cerr << "New: " << changes.new_files.size()
              << " Changed: " << changes.changed_files.size()
              << " Deleted: " << changes.deleted_paths.size() << "\n";

    // R7: Quarantine rehab on branch switch — give quarantined files another
    // chance if the git HEAD changed (content may differ on the new branch).
    auto old_head = schema::get_kv(conn, "git_head", "");
    auto new_head = get_git_head(repo_root.string());
    bool head_changed = (!old_head.empty() && !new_head.empty() && old_head != new_head);

    if (head_changed) {
        int rehabbed = schema::rehab_quarantine(conn, scanned_files);
        if (rehabbed > 0) {
            std::cerr << "Quarantine rehab: " << rehabbed
                      << " file(s) given another chance after branch switch\n";
            quarantined = schema::load_quarantine(conn);  // Refresh after rehab
        }
    }

    // Prune deleted files (T041)
    Persister persister(conn);
    if (!changes.deleted_paths.empty()) {
        ScopedPhase _sp(profiler.prune);
        int pruned = persister.prune_deleted(changes.deleted_paths);
        std::cerr << "Pruned " << pruned << " deleted files\n";
    }

    // Merge new + changed into a single work list
    std::vector<ScannedFile> work_list;
    work_list.reserve(changes.new_files.size() + changes.changed_files.size());
    for (const auto& f : changes.new_files) {
        if (!quarantined.count(f.relative_path))
            work_list.push_back(f);
    }
    for (const auto& f : changes.changed_files) {
        if (!quarantined.count(f.relative_path))
            work_list.push_back(f);
    }

    if (work_list.empty()) {
        std::cerr << "Nothing to index. Database is up to date.\n";

        // Still run resolver if there are unresolved refs
        std::cerr << "Resolving cross-file references...\n";
        auto [refs_resolved, edges_created] = persister.resolve_references();
        if (refs_resolved > 0 || edges_created > 0) {
            std::cerr << "Resolved " << refs_resolved << " refs, created "
                      << edges_created << " edges\n";
        } else {
            std::cerr << "All refs already resolved.\n";
        }

        // Ensure FTS triggers are active for future incremental updates
        fts::create_sync_triggers(conn);
        persister.write_metadata(repo_root.string());
        conn.wal_checkpoint();
        return 0;
    }

    // --- T045: Parallel parse, pipelined persist ---
    // Architecture: workers push results to a queue as they complete.
    // Main thread drains the queue and persists in completion order.
    // No head-of-line blocking: a stuck file only wastes one thread,
    // not the entire pipeline.
    int thread_count = config.effective_thread_count();
    size_t arena_size = config.arena_size_bytes();
    ArenaPool arena_pool(thread_count * 2, arena_size);

    std::atomic<int> errors{0};
    int total = static_cast<int>(work_list.size());

    auto start_time = std::chrono::steady_clock::now();

    // Progress: every 1% of total files
    int progress_interval = (std::max)(1, total / 100);

    std::cerr << "Indexing " << total << " files with " << thread_count
              << " threads (arena " << config.arena_size_mb << " MB)\n";

    // Safe mode: commit after every file to isolate crashers
    int effective_batch_size = config.batch_size;
    if (config.safe_mode) {
        effective_batch_size = 1;
        std::cerr << "SAFE MODE: commit after every file\n";
    }

    // --- Bulk load optimization: drop indexes and FTS triggers ---
    // Building indexes on a populated table is 10-50x faster than maintaining
    // them per-insert. FTS rebuild is similarly much faster in one pass.
    bool bulk_mode = (total > 1000) && !config.safe_mode;
    if (bulk_mode) {
        std::cerr << "Bulk mode: dropping indexes for fast insert...\n";
        schema::drop_bulk_indexes(conn);
        fts::drop_sync_triggers(conn);
    } else {
        fts::create_sync_triggers(conn);
    }

    ThreadPool pool(thread_count);

    // Result queue: workers push here, main thread drains.
    std::mutex result_mutex;
    std::condition_variable result_cv;
    std::queue<ParsedFile> result_queue;

    // Parse one file — pure function, returns result.
    // Uses profiler phase accumulators for cross-thread timing.
    auto parse_one = [&arena_pool, &config, &profiler]
        (const ScannedFile& file) -> ParsedFile {
        using clock = std::chrono::steady_clock;
        ParsedFile result;
        result.file = file;

#ifdef _WIN32
        _set_se_translator(seh_translator);
#endif
        try {

        auto t0 = clock::now();
        ArenaLease lease(arena_pool);
        set_thread_arena(lease.get());
        auto t1 = clock::now();
        profiler.arena_lease.add(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

        if (static_cast<size_t>(file.size_bytes) > config.max_file_size_bytes()) {
            result.content_hash = hash_file(file.absolute_path);
            result.parse_status = "partial";
            result.parse_error = "file exceeds max size";
            return result;
        }

        size_t arena_cap = lease.get()->capacity();
        if (static_cast<size_t>(file.size_bytes) * 30 > arena_cap) {
            result.content_hash = hash_file(file.absolute_path);
            result.parse_status = "partial";
            result.parse_error = "file too large for arena (" +
                std::to_string(file.size_bytes / (1024*1024)) + "MB file, " +
                std::to_string(arena_cap / (1024*1024)) + "MB arena)";
            return result;
        }

        auto t_rd0 = clock::now();
        auto content = read_file_content(file.absolute_path);
        auto t_rd1 = clock::now();
        profiler.file_read.add(std::chrono::duration_cast<std::chrono::microseconds>(t_rd1 - t_rd0).count());
        if (content.empty()) {
            result.parse_status = "failed";
            result.parse_error = "could not read file";
            result.has_error = true;
            return result;
        }

        auto t_h0 = clock::now();
        result.content_hash = hash_string(content);
        auto t_h1 = clock::now();
        profiler.hash.add(std::chrono::duration_cast<std::chrono::microseconds>(t_h1 - t_h0).count());

        Parser parser;
        if (!parser.set_language(file.language)) {
            result.parse_status = "skipped";
            result.parse_error = "language grammar not available";
            return result;
        }

        // DEC-028 R3: tree-sitter set_timeout removed — parse phase is fast
        // (1.8ms/file avg). Extraction timeout (R1) is the real defense against
        // tail-latency from large-AST files.

        auto t_p0 = clock::now();
        auto tree = TreeGuard(parser.parse(content));
        auto t_p1 = clock::now();
        profiler.parse.add(std::chrono::duration_cast<std::chrono::microseconds>(t_p1 - t_p0).count());
        if (!tree) {
            result.parse_status = "failed";
            result.parse_error = config.parse_timeout_s > 0
                ? "tree-sitter parse failed (possible timeout)"
                : "tree-sitter parse failed";
            result.has_error = true;
            return result;
        }

        Extractor extractor(config.max_symbols_per_file, config.max_ast_depth,
                            config.extraction_timeout_s);
        auto t_e0 = clock::now();
        result.extraction = extractor.extract(tree.tree, content, file.language, file.relative_path);
        auto t_e1 = clock::now();
        auto extract_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(t_e1 - t_e0).count();
        profiler.extract.add(extract_elapsed);
        result.extract_us = extract_elapsed;

        result.parse_status = result.extraction.truncated ? "partial" : "ok";
        result.parse_error = result.extraction.truncated ? result.extraction.truncation_reason : "";

#ifdef _WIN32
        } catch (const SehException& e) {
            char buf[64]; snprintf(buf, sizeof(buf), "SEH 0x%08X during parse/extract", e.code);
            result.parse_status = "failed";
            result.parse_error = buf;
            result.has_error = true;
#endif
        } catch (const std::exception& e) {
            result.parse_status = "failed";
            result.parse_error = std::string("exception: ") + e.what();
            result.has_error = true;
        }

        return result;
    };

    // 1) Submit ALL parse tasks. Workers push results to queue as they complete.
    for (int i = 0; i < total; ++i) {
        auto& file = work_list[i];
        pool.submit([&file, &parse_one, &result_mutex, &result_cv, &result_queue]() -> ParsedFile {
            auto result = parse_one(file);
            {
                std::lock_guard<std::mutex> lk(result_mutex);
                result_queue.push(std::move(result));
            }
            result_cv.notify_one();
            return ParsedFile{};
        });
    }

    // 2) Main thread: drain results as they complete (no ordering).
    //    R3: Batch drain — dequeue ALL available results under a single lock,
    //    reducing lock/unlock cycles and condition variable overhead.
    //    R4: Cold index — skip DELETE when DB has no existing files.
    persister.enable_cold_index_if_empty();
    if (persister.is_cold_index()) {
        std::cerr << "Cold index: skipping DELETE phase\n";
    }
    persister.begin_batch();
    int done = 0;
    double total_wait_ms = 0;
    int wait_count = 0;       // times main thread had to wait (queue was empty)
    int timeouts = 0;         // files that hit extraction timeout
    int slow_files = 0;       // R4: files that took >2s to extract
    constexpr int64_t slow_threshold_us = 2000000;  // 2 seconds

    std::vector<ParsedFile> drain_batch;
    drain_batch.reserve((std::min)(total, thread_count * 4));

    while (done < total) {
        // R3: Drain all available results under a single lock
        {
            auto wait_start = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lk(result_mutex);
            result_cv.wait(lk, [&] { return !result_queue.empty(); });
            auto wait_end = std::chrono::steady_clock::now();
            auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(wait_end - wait_start).count();
            double wait_ms = static_cast<double>(wait_us) / 1000.0;
            total_wait_ms += wait_ms;
            if (wait_ms > 1.0) {
                wait_count++;
                profiler.contention.add(wait_us);
            }
            while (!result_queue.empty()) {
                drain_batch.push_back(std::move(result_queue.front()));
                result_queue.pop();
            }
        }

        // Process the entire batch outside the lock
        for (auto& result : drain_batch) {
            if (result.has_error) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            if (result.parse_error.find("timeout") != std::string::npos) {
                timeouts++;
            }

            // DEC-028 R4: Log slow files so users can identify pathological files
            if (result.extract_us > slow_threshold_us) {
                slow_files++;
                std::cerr << "\n  [SLOW] " << result.file.relative_path
                          << " extract=" << (result.extract_us / 1000) << "ms"
                          << " (" << result.extraction.symbols.size() << " symbols"
                          << (result.extraction.truncated ? ", truncated" : "")
                          << ")\n";
            }

            {
                ScopedPhase _sp(profiler.persist);
                if (!persister.persist_file(result.file, result.extraction,
                                            result.content_hash, result.parse_status,
                                            result.parse_error)) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }

            {
                ScopedPhase _sp(profiler.flush);
                persister.flush_if_needed(effective_batch_size);
            }
            done++;

            // Progress reporting
            if (done % progress_interval == 0 || done == total) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                double rate = elapsed_s > 0 ? static_cast<double>(done) / elapsed_s : 0;
                int pct = static_cast<int>(100.0 * done / total);
                std::cerr << "\r\033[K[" << done << "/" << total << "] "
                          << pct << "% "
                          << elapsed_s << "s "
                          << static_cast<int>(rate) << " files/s"
                          << std::flush;
            }
        }
        drain_batch.clear();
    }
    persister.commit_batch();

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    double rate = elapsed_s > 0 ? static_cast<double>(total) / elapsed_s : 0;

    std::cerr << "\nDone: " << total << " files in " << elapsed_s << "s"
              << " (" << static_cast<int>(rate) << " files/s)";
    if (errors > 0) std::cerr << " [" << errors << " errors]";
    if (timeouts > 0) std::cerr << " [" << timeouts << " timeouts]";
    if (slow_files > 0) std::cerr << " [" << slow_files << " slow]";
    std::cerr << "\n";
    std::cerr << "Contention: main thread waited "
              << std::fixed << std::setprecision(1) << (total_wait_ms / 1000.0)
              << "s across " << wait_count << " stalls\n";

    // Per-phase profiling report (always printed — lightweight summary)
    int64_t pf = profiler.extract.load_count();
    if (pf > 0) {
        auto us_to_s = [](int64_t us) { return static_cast<double>(us) / 1000000.0; };
        auto avg_us = [pf](int64_t us) { return static_cast<double>(us) / pf; };
        std::cerr << "Profile (" << pf << " files): "
                  << "arena=" << std::setprecision(1) << us_to_s(profiler.arena_lease.load_us()) << "s "
                  << "(" << std::setprecision(0) << avg_us(profiler.arena_lease.load_us()) << "us/f) "
                  << "read=" << std::setprecision(1) << us_to_s(profiler.file_read.load_us()) << "s "
                  << "(" << std::setprecision(0) << avg_us(profiler.file_read.load_us()) << "us/f) "
                  << "hash=" << std::setprecision(1) << us_to_s(profiler.hash.load_us()) << "s "
                  << "(" << std::setprecision(0) << avg_us(profiler.hash.load_us()) << "us/f) "
                  << "parse=" << std::setprecision(1) << us_to_s(profiler.parse.load_us()) << "s "
                  << "(" << std::setprecision(0) << avg_us(profiler.parse.load_us()) << "us/f) "
                  << "extract=" << std::setprecision(1) << us_to_s(profiler.extract.load_us()) << "s "
                  << "(" << std::setprecision(0) << avg_us(profiler.extract.load_us()) << "us/f) "
                  << "persist=" << std::setprecision(1) << us_to_s(profiler.persist.load_us()) << "s "
                  << "(" << std::setprecision(0) << avg_us(profiler.persist.load_us()) << "us/f)\n";
    }

    // --- Rebuild read-path indexes (needed by resolve_references) ---
    if (bulk_mode) {
        std::cerr << "Rebuilding read-path indexes...\n";
        auto idx_start = std::chrono::steady_clock::now();
        {
            ScopedPhase _sp(profiler.idx_read);
            schema::rebuild_read_indexes(conn);
        }
        auto idx_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - idx_start).count();
        std::cerr << "Read indexes rebuilt in " << idx_elapsed << "s\n";
    }

    // T039: Cross-file reference resolution (in-memory hash-based pass)
    std::cerr << "Resolving cross-file references...\n";
    auto resolve_start = std::chrono::steady_clock::now();
    std::pair<int,int> resolve_result;
    {
        ScopedPhase _sp(profiler.resolve_refs);
        resolve_result = persister.resolve_references();
    }
    auto [refs_resolved, edges_created] = resolve_result;
    auto resolve_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - resolve_start).count();
    std::cerr << "Resolved " << refs_resolved << " refs, created "
              << edges_created << " edges in " << resolve_elapsed << "s\n";

    // --- Rebuild write-path indexes (after resolver — avoids per-row maintenance cost) ---
    if (bulk_mode) {
        std::cerr << "Rebuilding write-path indexes...\n";
        auto widx_start = std::chrono::steady_clock::now();
        {
            ScopedPhase _sp(profiler.idx_write);
            schema::rebuild_write_indexes(conn);
        }
        auto widx_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - widx_start).count();
        std::cerr << "Write indexes rebuilt in " << widx_elapsed << "s\n";
    }

    // Rebuild FTS5 index in one pass (much faster than per-row triggers)
    if (bulk_mode) {
        std::cerr << "Rebuilding FTS5 index...\n";
        auto fts_start = std::chrono::steady_clock::now();
        {
            ScopedPhase _sp(profiler.fts_rebuild);
            fts::rebuild(conn);
        }
        auto fts_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - fts_start).count();
        std::cerr << "FTS5 rebuilt in " << fts_elapsed << "s\n";
    }

    // Re-create FTS sync triggers for future incremental updates
    fts::create_sync_triggers(conn);

    // Write metadata (T049)
    {
        ScopedPhase _sp(profiler.metadata);
        persister.write_metadata(repo_root.string());
    }

    // WAL checkpoint (T050)
    {
        ScopedPhase _sp(profiler.wal_ckpt);
        conn.wal_checkpoint();
    }

    // Comprehensive profiling report (only with --profile)
    auto pipeline_end = std::chrono::steady_clock::now();
    auto pipeline_us = std::chrono::duration_cast<std::chrono::microseconds>(
        pipeline_end - pipeline_start).count();
    profiler.print_report(pipeline_us, total, thread_count);

    return errors > 0 ? 1 : 0;
}

} // namespace codetopo
