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
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <future>
#include <queue>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <fstream>
#ifdef _WIN32
#include <process.h>  // _beginthreadex
#include <windows.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <eh.h>
#include <malloc.h>  // _resetstkoflw

// C++ exception wrapper for SEH exceptions
struct SehException : std::exception {
    DWORD code;
    SehException(DWORD c) : code(c) {}
    const char* what() const noexcept override { return "SEH exception"; }
};

// Per-thread SEH translator — converts SEH to C++ exception.
// On stack overflow, must call _resetstkoflw() to restore the guard page.
// Without it, the NEXT stack overflow triggers __fastfail (0xC0000409)
// which is uncatchable.
static void seh_translator(unsigned int code, EXCEPTION_POINTERS*) {
    if (code == EXCEPTION_STACK_OVERFLOW) {
        _resetstkoflw();
    }
    throw SehException(static_cast<DWORD>(code));
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
};

// T045-T050: Full index command implementation with parallel parse.
int run_index(const Config& config) {
    namespace fs = std::filesystem;

    Profiler profiler;
    profiler.enabled = config.profile;

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

    // Worklist cache: on --resume, load cached work list to avoid rescanning.
    auto worklist_path = db_path;
    worklist_path += ".worklist";

    Persister persister(conn);
    std::vector<ScannedFile> work_list;

    bool resumed = false;
    if (config.resume && fs::exists(worklist_path)) {
        // Fast path: load cached worklist from previous crash
        std::ifstream wf(worklist_path);
        std::string line;
        while (std::getline(wf, line)) {
            // Format: relative_path \t absolute_path \t language \t size_bytes \t mtime_ns
            auto t1 = line.find('\t');
            auto t2 = line.find('\t', t1 + 1);
            auto t3 = line.find('\t', t2 + 1);
            auto t4 = line.find('\t', t3 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos ||
                t3 == std::string::npos || t4 == std::string::npos)
                continue;
            ScannedFile f;
            f.relative_path = line.substr(0, t1);
            f.absolute_path = fs::path(line.substr(t1 + 1, t2 - t1 - 1));
            f.language = line.substr(t2 + 1, t3 - t2 - 1);
            f.size_bytes = std::stoll(line.substr(t3 + 1, t4 - t3 - 1));
            f.mtime_ns = std::stoll(line.substr(t4 + 1));
            // Skip quarantined files (new quarantine entries since cache was written)
            if (!quarantined.count(f.relative_path))
                work_list.push_back(std::move(f));
        }
        if (!work_list.empty()) {
            resumed = true;
            std::cerr << "Resumed: " << work_list.size() << " files from cached worklist\n";
        }
    }

    if (!resumed) {
        // Normal path: full scan + change detection
        std::cerr << "Scanning " << repo_root.string() << "...\n";
        Scanner scanner(config);
        auto scanned_files = scanner.scan();
        std::cerr << "Found " << scanned_files.size() << " source files\n";

        ChangeDetector detector(conn);
        auto changes = detector.detect(scanned_files);

        std::cerr << "New: " << changes.new_files.size()
                  << " Changed: " << changes.changed_files.size()
                  << " Deleted: " << changes.deleted_paths.size() << "\n";

        // R7: Quarantine rehab on branch switch
        auto old_head = schema::get_kv(conn, "git_head", "");
        auto new_head = get_git_head(repo_root.string());
        bool head_changed = (!old_head.empty() && !new_head.empty() && old_head != new_head);

        if (head_changed) {
            int rehabbed = schema::rehab_quarantine(conn, scanned_files);
            if (rehabbed > 0) {
                std::cerr << "Quarantine rehab: " << rehabbed
                          << " file(s) given another chance after branch switch\n";
                quarantined = schema::load_quarantine(conn);
            }
        }

        // Prune deleted files (T041)
        if (!changes.deleted_paths.empty()) {
            int pruned = persister.prune_deleted(changes.deleted_paths);
            std::cerr << "Pruned " << pruned << " deleted files\n";
        }

        // Merge new + changed into work list
        work_list.reserve(changes.new_files.size() + changes.changed_files.size());
        for (const auto& f : changes.new_files) {
            if (!quarantined.count(f.relative_path))
                work_list.push_back(f);
        }
        for (const auto& f : changes.changed_files) {
            if (!quarantined.count(f.relative_path))
                work_list.push_back(f);
        }
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
        fs::remove(worklist_path);  // Clean up stale worklist
        return 0;
    }

    // Sort largest-first: fail-fast — hit problematic big files immediately
    // so crashes surface early and iteration is fast.
    std::sort(work_list.begin(), work_list.end(),
              [](const ScannedFile& a, const ScannedFile& b) {
                  return a.size_bytes > b.size_bytes;
              });

    // Save worklist so restarted children can skip scanning
    if (config.supervised && !resumed) {
        std::ofstream wf(worklist_path, std::ios::trunc);
        for (const auto& f : work_list) {
            wf << f.relative_path << '\t'
               << f.absolute_path.string() << '\t'
               << f.language << '\t'
               << f.size_bytes << '\t'
               << f.mtime_ns << '\n';
        }
    }

    // --- T045: Parallel parse, pipelined persist ---
    // Architecture: submit ALL files to thread pool up front. Workers parse
    // continuously. Main thread consumes futures in order and persists each
    // result. Workers never stall waiting for a batch boundary — they always
    // have work queued. Arena pool is 2x thread count so workers can lease
    // arenas even while main thread holds completed ParsedFile objects that
    // haven't been persisted yet (those arenas are already returned).
    int thread_count = config.effective_thread_count();
    size_t arena_size = config.arena_size_bytes();
    ArenaPool arena_pool(thread_count * 2, arena_size);

    // Optional large-file arena pool. Use multiple arenas (thread_count/4,
    // min 2) to allow parallel parsing of large files. Each arena is
    // large_arena_size_mb, so total = (thread_count/4) * large_arena_size_mb.
    bool has_large_pool = (config.large_arena_size_mb > 0
                           && config.large_arena_size_bytes() > arena_size);
    std::unique_ptr<ArenaPool> large_pool;
    if (has_large_pool) {
        int large_arena_count = (std::max)(2, thread_count / 4);
        large_pool = std::make_unique<ArenaPool>(large_arena_count, config.large_arena_size_bytes());
    }

    std::atomic<int> errors{0};
    int total = static_cast<int>(work_list.size());
    int display_offset = config.progress_offset;
    int display_total = config.progress_total > 0 ? config.progress_total : total;

    auto start_time = std::chrono::steady_clock::now();

    int last_pct = -1;
    int64_t last_display_time = -10; // Force first display

    std::cerr << "Indexing " << total << " files with " << thread_count
              << " threads (arena " << config.arena_size_mb << " MB";
    if (has_large_pool) {
        std::cerr << ", large-file arena " << config.large_arena_size_mb << " MB"
                  << ", large-file threshold " << config.large_file_threshold_bytes() / 1024 << " KB";
    }
    std::cerr << ", max-file-size " << config.max_file_size_kb << " KB)\n";

    // Safe mode: commit after every file to isolate crashers.
    int effective_batch_size = config.batch_size;
    if (config.safe_mode) {
        effective_batch_size = 1;
        std::cerr << "SAFE MODE: commit after every file\n";
    }
    if (config.turbo) {
        effective_batch_size = (std::max)(effective_batch_size, 1000);
        conn.enable_turbo();
    }
    // On resume with a small remaining worklist, cap batch size so progress
    // gets written before a potential crash.  Without this, a crash in a
    // 2500-file worklist with batch=1000 can happen before any commit,
    // leaving no progress file and causing an infinite restart loop.
    if (config.resume && total > 0 && total < effective_batch_size * 4) {
        effective_batch_size = (std::max)(total / 4, 1);
    }
    if (config.turbo) {
        std::cerr << "TURBO: synchronous=OFF, batch=" << effective_batch_size << "\n";
    }

    // Progress file: child writes the relative path of the last committed
    // file to .progress after each batch commit.  On crash, the supervisor
    // reads it to identify committed vs. in-flight files.
    auto progress_path = db_path;
    progress_path += ".progress";

    // --- Bulk load optimization: drop indexes and FTS triggers ---
    // Building indexes on a populated table is 10-50x faster than maintaining
    // them per-insert. FTS rebuild is similarly much faster in one pass.
    // Uses IF EXISTS / IF NOT EXISTS — safe to call on resume after crash.
    bool bulk_mode = (total > 1000) && !config.safe_mode;
    if (bulk_mode) {
        std::cerr << "Bulk mode: dropping indexes for fast insert...\n";
        schema::drop_bulk_indexes(conn);
        fts::drop_sync_triggers(conn);
    } else {
        fts::create_sync_triggers(conn);
    }

    // No fixed thread pool — use detached threads per task.
    // Stuck threads are leaked harmlessly; window_size caps concurrency.
    // window_size = thread_count + 2: slight over-subscription hides latency
    // (main thread persist time) without starving CPU-bound parsers.
    const int window_size = thread_count + 2;

    // Concurrency is bounded purely by window_size (thread_count + 2).
    // The start_epoch_ms is set inside the worker AFTER arena acquisition,
    // so arena queue wait time is not counted as parse time by the watchdog.

    // --- Per-slot cancellation flags ---
    // Each worker slot has a plain size_t flag shared with tree-sitter's
    // cancellation API (ts_parser_set_cancellation_flag takes const size_t*).
    // A watchdog thread monitors timestamps and sets the flag to cancel
    // stuck parses.
    struct SlotState {
        size_t cancel_flag{0};           // written by watchdog, read by tree-sitter
        std::atomic<int64_t> start_epoch_ms{0}; // 0 = idle
        std::atomic<bool> dead{false};   // watchdog declared this slot unrecoverable
        std::atomic<bool> available{true}; // main thread: slot free for reuse
        std::atomic<int> generation{0};  // prevents stale thread interference
        std::atomic<int> task_index{-1}; // index in work_list being processed
        std::string file_path; // for logging only
        int64_t file_size_bytes{0};      // for per-file timeout scaling
    };
    const int num_slots = window_size;
    std::vector<SlotState> slots(num_slots);

    // Parse lambda: shared by windowed submission below.
    auto make_parse_task = [&arena_pool, &large_pool, has_large_pool, arena_size, &config, &slots, &profiler]
        (const ScannedFile& file, int slot) -> ParsedFile {
        ParsedFile result;
        result.file = file;

#ifdef _WIN32
        _set_se_translator(seh_translator);
#endif
        try {

        if (static_cast<size_t>(file.size_bytes) > config.max_file_size_bytes()) {
            result.content_hash = hash_file(file.absolute_path);
            result.parse_status = "partial";
            result.parse_error = "file exceeds max size";
            return result;
        }

        // Route to large arena ONLY when the file won't fit in a normal arena.
        // Tree-sitter uses ~30x file size in memory, so the threshold is:
        //   file_size * 30 > normal_arena_capacity
        // This keeps all smaller files (even 500KB) on the normal pool
        // which has thread_count*2 arenas — full parallelism.
        // The old threshold-based routing sent ALL >180KB files to 4 large
        // arenas, capping parallelism at 4 instead of 16.
        bool actually_needs_large = has_large_pool
            && (static_cast<size_t>(file.size_bytes) * 30 > arena_size);

        std::unique_ptr<ArenaLease> lease;
        {
        ScopedPhase _al(profiler.arena_lease);
        if (actually_needs_large) {
            // Truly huge file — must use large arena. Try non-blocking first.
            Arena* a = large_pool->try_lease();
            if (a) {
                lease = std::make_unique<ArenaLease>(*large_pool, a);
            } else {
                lease = std::make_unique<ArenaLease>(*large_pool); // block
            }
        } else {
            // Normal file — use normal pool (32 arenas, plenty for 18 slots).
            // Fall back to large pool only if normal is transiently full.
            Arena* a = arena_pool.try_lease();
            if (a) {
                lease = std::make_unique<ArenaLease>(arena_pool, a);
            } else if (has_large_pool) {
                Arena* la = large_pool->try_lease();
                if (la) {
                    lease = std::make_unique<ArenaLease>(*large_pool, la);
                } else {
                    lease = std::make_unique<ArenaLease>(arena_pool); // block on normal
                }
            } else {
                lease = std::make_unique<ArenaLease>(arena_pool); // block
            }
        }
        } // ScopedPhase arena_lease
        set_thread_arena(lease->get());

        size_t arena_cap = lease->get()->capacity();
        if (static_cast<size_t>(file.size_bytes) * 30 > arena_cap) {
            result.content_hash = hash_file(file.absolute_path);
            result.parse_status = "partial";
            result.parse_error = "file too large for arena (" +
                std::to_string(file.size_bytes / (1024*1024)) + "MB file, " +
                std::to_string(arena_cap / (1024*1024)) + "MB arena)";
            return result;
        }

        std::string content;
        {
            ScopedPhase _fr(profiler.file_read);
            content = read_file_content(file.absolute_path);
        }
        if (content.empty()) {
            result.parse_status = "failed";
            result.parse_error = "could not read file";
            result.has_error = true;
            return result;
        }

        {
            ScopedPhase _h(profiler.hash);
            result.content_hash = hash_string(content);
        }

        // Set up cancellation flag for this slot — watchdog sets it
        // if this parse exceeds the timeout.
        slots[slot].cancel_flag = 0;
        slots[slot].file_path = file.relative_path;
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        slots[slot].start_epoch_ms.store(now_ms, std::memory_order_release);

        Parser parser;
        if (!parser.set_language(file.language)) {
            slots[slot].start_epoch_ms.store(0, std::memory_order_relaxed);
            result.parse_status = "skipped";
            result.parse_error = "language grammar not available";
            return result;
        }
        if (config.parse_timeout_s > 0) {
            parser.set_timeout(static_cast<uint64_t>(config.parse_timeout_s) * 1'000'000);
        }
        parser.set_cancellation_flag(&slots[slot].cancel_flag);

        TreeGuard tree(nullptr);
        {
            ScopedPhase _p(profiler.parse);
            tree = TreeGuard(parser.parse(content));
        }
        // Do NOT reset start_epoch_ms here — watchdog must cover extraction too.

        if (!tree || slots[slot].cancel_flag != 0) {
            slots[slot].start_epoch_ms.store(0, std::memory_order_relaxed);
            result.parse_status = "failed";
            result.parse_error = "parse cancelled by watchdog (exceeded timeout)";
            result.has_error = true;
            return result;
        }

        {
            ScopedPhase _e(profiler.extract);
            Extractor extractor(config.max_symbols_per_file, config.max_ast_depth,
                                config.extraction_timeout_s, &slots[slot].cancel_flag);
            result.extraction = extractor.extract(tree.tree, content, file.language, file.relative_path);
        }

        // Mark slot idle now that both parse + extract are done.
        slots[slot].start_epoch_ms.store(0, std::memory_order_relaxed);

        if (slots[slot].cancel_flag != 0) {
            result.parse_status = "failed";
            result.parse_error = "extraction cancelled by watchdog (exceeded timeout)";
            result.has_error = true;
            return result;
        }

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

    // Concurrent result queue: workers push results, main thread pops.
    // No polling, no busy-wait — uses condition variable.
    // Watchdog can also inject failed results for dead slots.
    struct IndexedResult {
        ParsedFile parsed;
        int index;
        int slot;
    };
    std::mutex result_mutex;
    std::condition_variable result_cv;
    std::queue<IndexedResult> result_queue;

    // Watchdog: checks every second, cancels slots exceeding timeout.
    // After 2x timeout with no response, injects a failed result and marks slot dead.
    std::atomic<bool> watchdog_stop{false};
    // Per-file timeout: base + scaled by file size.
    // base_timeout_ms: minimum timeout for any file (default 30s)
    // Files get extra time proportional to size: +1s per 10 KB.
    int64_t base_timeout_ms = config.parse_timeout_s > 0
        ? static_cast<int64_t>(config.parse_timeout_s) * 1000
        : 30000;
    auto slot_timeout_ms = [base_timeout_ms](int64_t file_size_bytes) -> int64_t {
        int64_t size_bonus_ms = (file_size_bytes / (10 * 1024)) * 1000; // +1s per 10KB
        return base_timeout_ms + size_bonus_ms;
    };
    // stderr mutex: prevents garbled output between watchdog and main thread
    std::thread watchdog([&slots, num_slots, &watchdog_stop, &slot_timeout_ms,
                          &result_mutex, &result_cv, &result_queue, &work_list]() {
        while (!watchdog_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            for (int s = 0; s < num_slots; ++s) {
                if (slots[s].dead.load(std::memory_order_relaxed)) continue;
                if (slots[s].available.load(std::memory_order_relaxed)) continue;
                int64_t start = slots[s].start_epoch_ms.load(std::memory_order_relaxed);
                if (start <= 0) continue;
                int64_t elapsed = now_ms - start;
                int64_t file_timeout = slot_timeout_ms(slots[s].file_size_bytes);
                int64_t file_kill = file_timeout + file_timeout / 2; // 1.5x
                if (elapsed > file_kill) {
                    slots[s].dead.store(true, std::memory_order_relaxed);
                    slots[s].generation.fetch_add(1, std::memory_order_relaxed);
                    slots[s].start_epoch_ms.store(0, std::memory_order_relaxed);
                    int idx = slots[s].task_index.load(std::memory_order_relaxed);
                    std::cerr << "\r\033[KWATCHDOG: killing slot " << s
                              << " after " << (elapsed / 1000) << "s (" 
                              << slots[s].file_path << ")\n" << std::flush;
                    if (idx >= 0) {
                        ParsedFile failed;
                        failed.file = work_list[idx];
                        failed.parse_status = "failed";
                        failed.parse_error = "killed by watchdog (unresponsive to cancellation)";
                        failed.has_error = true;
                        {
                            std::lock_guard<std::mutex> lk(result_mutex);
                            result_queue.push({std::move(failed), idx, s});
                        }
                        result_cv.notify_one();
                    }
                } else if (elapsed > file_timeout) {
                    slots[s].cancel_flag = 1;
                }
            }
        }
    });

    int next_submit = 0;
    int next_persist = 0;
    int in_flight = 0;
    int next_slot = 0;

    // Persistent thread pool with 64MB stacks — reuses threads across all files.
    // Eliminates ~162K thread create/destroy operations (saves 80-160s overhead).
    ThreadPool worker_pool(window_size, 64 * 1024 * 1024);

    // try_submit_one: submits next file if budget allows.
    // Returns true if submitted (or injected failure), false if deferred.
    auto try_submit_one = [&]() -> bool {
        if (next_submit >= total) return false;
        int idx = next_submit;
        next_submit++;

        // Find a live, available slot
        int slot = -1;
        for (int attempts = 0; attempts < num_slots; ++attempts) {
            int candidate = next_slot++ % num_slots;
            if (slots[candidate].dead.load(std::memory_order_relaxed)) continue;
            if (!slots[candidate].available.load(std::memory_order_relaxed)) continue;
            slot = candidate;
            break;
        }
        if (slot < 0) {
            // All slots busy or dead — inject failure directly
            ParsedFile failed;
            failed.file = work_list[idx];
            failed.parse_status = "failed";
            failed.parse_error = "no live worker slots available";
            failed.has_error = true;
            {
                std::lock_guard<std::mutex> lk(result_mutex);
                result_queue.push({std::move(failed), idx, -1});
            }
            result_cv.notify_one();
            in_flight++;
            return true;
        }
        int gen = slots[slot].generation.fetch_add(1, std::memory_order_relaxed) + 1;
        slots[slot].available.store(false, std::memory_order_relaxed);
        slots[slot].task_index.store(idx, std::memory_order_relaxed);
        slots[slot].file_path = work_list[idx].relative_path;
        slots[slot].file_size_bytes = work_list[idx].size_bytes;
        slots[slot].cancel_flag = 0;
        // Do NOT set start_epoch_ms here. The worker sets it AFTER
        // acquiring the arena — otherwise queue time on the large arena
        // pool (1 arena) is counted as parse time by the watchdog,
        // causing false kills on files that haven't even started parsing.
        in_flight++;
        // Worker lambda — shared between both thread creation paths
        auto worker_fn = [&make_parse_task, &work_list, &result_mutex, &result_cv, &result_queue, &slots, idx, slot, gen]() {
            auto parsed = make_parse_task(work_list[idx], slot);
            // Only push if slot generation still matches (not killed by watchdog)
            if (slots[slot].generation.load(std::memory_order_relaxed) == gen) {
                {
                    std::lock_guard<std::mutex> lk(result_mutex);
                    result_queue.push({std::move(parsed), idx, slot});
                }
                result_cv.notify_one();
            }
        };
        worker_pool.submit_detached(std::move(worker_fn));
        return true;
    };

    persister.begin_batch();

    // Fill initial window (up to budget)
    while (next_submit < total && in_flight < window_size) {
        if (!try_submit_one()) break;
    }

    // Collect results as they arrive (any order), persist, refill
    while (in_flight > 0) {
        IndexedResult item;
        {
            ScopedPhase _ct(profiler.contention);
            std::unique_lock<std::mutex> lk(result_mutex);
            if (!result_cv.wait_for(lk, std::chrono::seconds(5),
                    [&] { return !result_queue.empty(); })) {
                continue; // watchdog handles stuck slots
            }
            item = std::move(result_queue.front());
            result_queue.pop();
        }
        in_flight--;

        // Revive slot for reuse. Dead slots (watchdog-killed) are safe to
        // reuse because the generation counter prevents stale threads from
        // pushing results into the revived slot.
        if (item.slot >= 0) {
            slots[item.slot].dead.store(false, std::memory_order_relaxed);
            slots[item.slot].available.store(true, std::memory_order_relaxed);
        }

        // Refill — budget may now allow more submissions
        while (next_submit < total && in_flight < window_size) {
            if (!try_submit_one()) break;
        }

        int i = item.index;
        auto& result = item.parsed;



        if (result.has_error) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }

        {
            ScopedPhase _ps(profiler.persist);
            if (!persister.persist_file(result.file, result.extraction,
                                        result.content_hash, result.parse_status,
                                        result.parse_error)) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }

        next_persist++;
        bool committed = false;
        {
            ScopedPhase _fl(profiler.flush);
            committed = persister.flush_if_needed(effective_batch_size);
        }

        if (config.supervised && committed) {
            std::ofstream pf(progress_path, std::ios::trunc);
            pf << work_list[i].relative_path << '\n';
            pf.flush();
        }

        {
            int pct = static_cast<int>(100.0 * (display_offset + next_persist) / display_total);
            auto now = std::chrono::steady_clock::now();
            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            // Update on percentage change OR every 10 seconds
            bool time_update = (elapsed_s / 10) > (last_display_time / 10);
            if (pct > last_pct || time_update || next_persist == total) {
                last_pct = pct;
                last_display_time = elapsed_s;
                double rate = elapsed_s > 0 ? static_cast<double>(next_persist) / elapsed_s : 0;
                std::cerr << "\r\033[K[" << (display_offset + next_persist) << "/" << display_total << "] "
                          << pct << "% "
                          << elapsed_s << "s "
                          << static_cast<int>(rate) << " files/s" << std::flush;
            }
        }
    }
    persister.commit_batch();

    // Stop watchdog thread
    watchdog_stop.store(true, std::memory_order_relaxed);
    if (watchdog.joinable()) watchdog.join();

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    double rate = elapsed_s > 0 ? static_cast<double>(total) / elapsed_s : 0;

    std::cerr << "Done: " << total << " files in " << elapsed_s << "s"
              << " (" << static_cast<int>(rate) << " files/s)";
    if (errors > 0) std::cerr << " [" << errors << " errors]";
    std::cerr << "\n";

    // --- Rebuild read-path indexes only (nodes, files) for resolve_references lookups ---
    // Edge and refs write-path indexes are deferred until AFTER resolve_references
    // to avoid maintaining them during 2.8M+ random inserts/updates.
    if (bulk_mode) {
        std::cerr << "Rebuilding read-path indexes...\n";
        ScopedPhase _ir(profiler.idx_read);
        auto idx_start = std::chrono::steady_clock::now();
        conn.exec("CREATE INDEX IF NOT EXISTS idx_files_content_hash ON files(content_hash)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_file_id ON nodes(file_id)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_type_kind_name ON nodes(node_type, kind, name)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_qualname ON nodes(qualname)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_name_type ON nodes(name, node_type)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_file_id ON refs(file_id)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_kind_name ON refs(kind, name)");
        auto idx_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - idx_start).count();
        std::cerr << "Read-path indexes rebuilt in " << idx_elapsed << "s\n";
    }

    // T039: Cross-file reference resolution (in-memory hash-based pass)
    // Disable FK checks — all IDs come from the DB itself, FK validation is pure overhead.
    std::cerr << "Resolving cross-file references...\n";
    {
        ScopedPhase _rr(profiler.resolve_refs);
        auto resolve_start = std::chrono::steady_clock::now();
        conn.exec("PRAGMA foreign_keys=OFF");
        auto [refs_resolved, edges_created] = persister.resolve_references();
        conn.exec("PRAGMA foreign_keys=ON");
        auto resolve_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - resolve_start).count();
        std::cerr << "Resolved " << refs_resolved << " refs, created "
                  << edges_created << " edges in " << resolve_elapsed << "s\n";
    }

    // --- Rebuild remaining write-path indexes (deferred from above) ---
    if (bulk_mode) {
        std::cerr << "Rebuilding write-path indexes...\n";
        ScopedPhase _iw(profiler.idx_write);
        auto idx_start = std::chrono::steady_clock::now();
        conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_resolved ON refs(resolved_node_id)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_edges_src ON edges(src_id, kind)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_edges_dst ON edges(dst_id, kind)");
        auto idx_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - idx_start).count();
        std::cerr << "Write-path indexes rebuilt in " << idx_elapsed << "s\n";
    }

    // Rebuild FTS5 index in one pass (much faster than per-row triggers)
    if (bulk_mode) {
        std::cerr << "Rebuilding FTS5 index...\n";
        ScopedPhase _fts(profiler.fts_rebuild);
        auto fts_start = std::chrono::steady_clock::now();
        fts::rebuild(conn);
        auto fts_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - fts_start).count();
        std::cerr << "FTS5 rebuilt in " << fts_elapsed << "s\n";
    }

    // Re-create FTS sync triggers for future incremental updates
    fts::create_sync_triggers(conn);

    // Write metadata (T049)
    {
        ScopedPhase _md(profiler.metadata);
        persister.write_metadata(repo_root.string());
    }

    // WAL checkpoint (T050)
    {
        ScopedPhase _wc(profiler.wal_ckpt);
        conn.wal_checkpoint();
    }

    // Remove worklist and progress files on success
    fs::remove(worklist_path);
    fs::remove(progress_path);

    // Print profiling report
    auto total_elapsed = std::chrono::steady_clock::now() - start_time;
    auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_elapsed).count();
    profiler.print_report(total_us, total, thread_count);

    return errors > 0 ? 1 : 0;
}

} // namespace codetopo
