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
#include "util/lock.h"
#include <iostream>
#include <mutex>
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
    auto scanned_files = scanner.scan();
    std::cerr << "Found " << scanned_files.size() << " source files\n";

    // Detect changes (T029)
    ChangeDetector detector(conn);
    auto changes = detector.detect(scanned_files);

    std::cerr << "New: " << changes.new_files.size()
              << " Changed: " << changes.changed_files.size()
              << " Deleted: " << changes.deleted_paths.size() << "\n";

    // Prune deleted files (T041)
    Persister persister(conn);
    if (!changes.deleted_paths.empty()) {
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
    // Architecture: submit ALL files to thread pool up front. Workers parse
    // continuously. Main thread consumes futures in order and persists each
    // result. Workers never stall waiting for a batch boundary — they always
    // have work queued. Arena pool is 2x thread count so workers can lease
    // arenas even while main thread holds completed ParsedFile objects that
    // haven't been persisted yet (those arenas are already returned).
    int thread_count = config.effective_thread_count();
    size_t arena_size = config.arena_size_bytes();
    ArenaPool arena_pool(thread_count * 2, arena_size);

    std::atomic<int> errors{0};
    int total = static_cast<int>(work_list.size());

    auto start_time = std::chrono::steady_clock::now();

    // Progress: every 1% or every 500 files, whichever is smaller
    int progress_interval = (std::max)(1, (std::min)(total / 100, 500));

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

    // 1) Submit ALL parse tasks up front — thread pool queues them internally.
    //    Workers will process them continuously without batch stalls.
    std::vector<std::future<ParsedFile>> futures;
    futures.reserve(total);

    for (int i = 0; i < total; ++i) {
        auto& file = work_list[i];
        futures.push_back(pool.submit([&file, &arena_pool, &config]() -> ParsedFile {
            ParsedFile result;
            result.file = file;

#ifdef _WIN32
            // Install per-thread SEH translator so access violations become C++ exceptions
            _set_se_translator(seh_translator);
#endif
            try {

            // Lease arena for this thread
            ArenaLease lease(arena_pool);
            set_thread_arena(lease.get());

            // Check file size limit (T043)
            if (static_cast<size_t>(file.size_bytes) > config.max_file_size_bytes()) {
                result.content_hash = hash_file(file.absolute_path);
                result.parse_status = "partial";
                result.parse_error = "file exceeds max size";
                return result;
            }

            // Check file vs arena capacity: tree-sitter needs ~20-50x file size in arena.
            // Skip files that would overflow the arena to prevent crashes.
            size_t arena_cap = lease.get()->capacity();
            if (static_cast<size_t>(file.size_bytes) * 30 > arena_cap) {
                result.content_hash = hash_file(file.absolute_path);
                result.parse_status = "partial";
                result.parse_error = "file too large for arena (" +
                    std::to_string(file.size_bytes / (1024*1024)) + "MB file, " +
                    std::to_string(arena_cap / (1024*1024)) + "MB arena)";
                return result;
            }

            // Read file content
            auto content = read_file_content(file.absolute_path);
            if (content.empty()) {
                result.parse_status = "failed";
                result.parse_error = "could not read file";
                result.has_error = true;
                return result;
            }

            result.content_hash = hash_string(content);

            // Parse with Tree-sitter (T030)
            Parser parser;
            if (!parser.set_language(file.language)) {
                result.parse_status = "skipped";
                result.parse_error = "language grammar not available";
                return result;
            }

            auto tree = TreeGuard(parser.parse(content));
            if (!tree) {
                result.parse_status = "failed";
                result.parse_error = "tree-sitter parse failed";
                result.has_error = true;
                return result;
            }

            // Extract symbols, refs, edges (T031-T037)
            Extractor extractor(config.max_symbols_per_file, config.max_ast_depth);
            result.extraction = extractor.extract(tree.tree, content, file.language, file.relative_path);

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
            // Arena resets automatically via ArenaLease destructor
        }));
    }

    // 2) Main thread: consume futures in order, persist each result.
    //    Workers are continuously parsing ahead while we persist here.
    //    Batch transactions: commit every batch_size files instead of per-file.
    persister.begin_batch();
    for (int i = 0; i < total; ++i) {
        // Print current file BEFORE get() so we know which file crashes
        std::cerr << "\r[" << (i+1) << "/" << total << "] "
                  << work_list[i].relative_path << std::flush;

        auto result = futures[i].get();

        if (result.has_error) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }

        if (!persister.persist_file(result.file, result.extraction,
                                    result.content_hash, result.parse_status,
                                    result.parse_error)) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }

        persister.flush_if_needed(effective_batch_size);

        // Progress reporting
        int done = i + 1;
        if (done % progress_interval == 0 || done == total) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            double rate = elapsed_s > 0 ? static_cast<double>(done) / elapsed_s : 0;
            int pct = static_cast<int>(100.0 * done / total);
            std::cerr << "[" << done << "/" << total << "] "
                      << pct << "% "
                      << elapsed_s << "s "
                      << static_cast<int>(rate) << " files/s"
                      << "\n";
        }
    }
    persister.commit_batch();

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    double rate = elapsed_s > 0 ? static_cast<double>(total) / elapsed_s : 0;

    std::cerr << "Done: " << total << " files in " << elapsed_s << "s"
              << " (" << static_cast<int>(rate) << " files/s)";
    if (errors > 0) std::cerr << " [" << errors << " errors]";
    std::cerr << "\n";

    // --- Rebuild indexes after bulk insert (much faster than maintaining during insert) ---
    if (bulk_mode) {
        std::cerr << "Rebuilding indexes...\n";
        auto idx_start = std::chrono::steady_clock::now();
        schema::rebuild_indexes(conn);
        auto idx_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - idx_start).count();
        std::cerr << "Indexes rebuilt in " << idx_elapsed << "s\n";
    }

    // T039: Cross-file reference resolution (in-memory hash-based pass)
    std::cerr << "Resolving cross-file references...\n";
    auto resolve_start = std::chrono::steady_clock::now();
    auto [refs_resolved, edges_created] = persister.resolve_references();
    auto resolve_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - resolve_start).count();
    std::cerr << "Resolved " << refs_resolved << " refs, created "
              << edges_created << " edges in " << resolve_elapsed << "s\n";

    // Rebuild FTS5 index in one pass (much faster than per-row triggers)
    if (bulk_mode) {
        std::cerr << "Rebuilding FTS5 index...\n";
        auto fts_start = std::chrono::steady_clock::now();
        fts::rebuild(conn);
        auto fts_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - fts_start).count();
        std::cerr << "FTS5 rebuilt in " << fts_elapsed << "s\n";
    }

    // Re-create FTS sync triggers for future incremental updates
    fts::create_sync_triggers(conn);

    // Write metadata (T049)
    persister.write_metadata(repo_root.string());

    // WAL checkpoint (T050)
    conn.wal_checkpoint();

    return errors > 0 ? 1 : 0;
}

} // namespace codetopo
