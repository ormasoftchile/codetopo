#pragma once

#include "core/config.h"
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
#include "index/crashlist.h"
#include "index/subprocess_parse.h"
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
#include <set>
#include <numeric>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <yyjson.h>
#ifdef _WIN32
#include <windows.h>
#endif


namespace codetopo {

// Forward declarations for arena thread-local management
void set_thread_arena(Arena* arena);
void register_arena_allocator();

// T045-T050: Full index command implementation with parallel parse.
inline int run_index(Config config) {
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

    // --- Auto-tune arena: no longer needed, calibration runs each time ---
    std::string codetopo_dir = (fs::path(db_path).parent_path()).string();

    // Scan repository
    auto scan_start = std::chrono::steady_clock::now();
    std::cerr << "Scanning " << repo_root.string() << "...\n";
    Scanner scanner(config);
    auto scanned_files = scanner.scan();
    auto scan_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - scan_start).count();
    std::cerr << "Phase: scan " << scan_s << "s (" << scanned_files.size() << " files)\n";

    // Load exclude list from .codetopo/config.json
    std::set<std::string> exclude_set;
    {
        auto cfg_path = fs::path(codetopo_dir) / "config.json";
        if (fs::exists(cfg_path)) {
            std::ifstream f(cfg_path);
            std::ostringstream ss; ss << f.rdbuf();
            std::string json = ss.str();
            yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
            if (doc) {
                yyjson_val* root_val = yyjson_doc_get_root(doc);
                yyjson_val* arr = yyjson_obj_get(root_val, "exclude");
                if (arr && yyjson_is_arr(arr)) {
                    size_t idx, max;
                    yyjson_val* val;
                    yyjson_arr_foreach(arr, idx, max, val) {
                        if (yyjson_is_str(val))
                            exclude_set.insert(yyjson_get_str(val));
                    }
                }
                yyjson_doc_free(doc);
            }
        }
    }
    if (!exclude_set.empty()) {
        size_t before = scanned_files.size();
        scanned_files.erase(
            std::remove_if(scanned_files.begin(), scanned_files.end(),
                           [&](const ScannedFile& f) { return exclude_set.count(f.relative_path) > 0; }),
            scanned_files.end());
        std::cerr << "Excluded " << (before - scanned_files.size()) << " files (config.json)\n";
    }

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
    work_list.insert(work_list.end(), changes.new_files.begin(), changes.new_files.end());
    work_list.insert(work_list.end(), changes.changed_files.begin(), changes.changed_files.end());

    // --- Crash isolation: load crashlist and journal ---
    CrashList crashlist(codetopo_dir);

    // Separate crashed files from work list
    std::vector<ScannedFile> crashed_files;
    if (crashlist.count() > 0) {
        std::vector<ScannedFile> safe_list;
        for (auto& f : work_list) {
            if (crashlist.should_skip(f.relative_path)) {
                crashed_files.push_back(f);
            } else {
                safe_list.push_back(f);
            }
        }
        if (!crashed_files.empty()) {
            std::cerr << "Skipping " << crashed_files.size()
                      << " previously crashed files (in crashlist)\n";
        }
        work_list = std::move(safe_list);
    }

    // Check for suspects from a previous process crash (journal survived)
    std::vector<ScannedFile> suspect_files;
    if (crashlist.has_suspects()) {
        std::set<std::string> suspect_set(crashlist.suspects().begin(),
                                           crashlist.suspects().end());
        std::vector<ScannedFile> safe_list;
        for (auto& f : work_list) {
            if (suspect_set.count(f.relative_path)) {
                suspect_files.push_back(f);
            } else {
                safe_list.push_back(f);
            }
        }
        if (!suspect_files.empty()) {
            std::cerr << "Found " << suspect_files.size()
                      << " suspect files from previous crash — will use subprocess isolation\n";
        }
        work_list = std::move(safe_list);
        crashlist.journal_clear(); // We've consumed the journal
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
    register_arena_allocator();
    int thread_count = config.effective_thread_count();

    // Sort work_list by file size descending — largest files first
    // --max-files: truncate work list for testing (before sort, random sample)
    if (config.max_files > 0 && static_cast<int>(work_list.size()) > config.max_files) {
        std::cerr << "Limiting to " << config.max_files << " files (--max-files)\n";
        work_list.resize(config.max_files);
    }

    std::sort(work_list.begin(), work_list.end(),
              [](const ScannedFile& a, const ScannedFile& b) {
                  return a.size_bytes > b.size_bytes;
              });

    // Split by file size: >= 150 KB → outlier queue, < 150 KB → normal queue
    static constexpr int64_t OUTLIER_THRESHOLD = 150 * 1024; // 150 KB
    std::vector<ScannedFile> outlier_files, normal_files;
    for (auto& f : work_list) {
        if (f.size_bytes >= OUTLIER_THRESHOLD) outlier_files.push_back(f);
        else normal_files.push_back(f);
    }

    int normal_total = static_cast<int>(normal_files.size());
    int outlier_total = static_cast<int>(outlier_files.size());
    int total = normal_total + outlier_total;

    std::cerr << "Queues: " << outlier_total << " outlier (>= 150 KB, 1 thread, 768 MB arena) + "
              << normal_total << " normal (" << thread_count << " threads, "
              << config.arena_size_mb << " MB arena)\n";

    std::atomic<int> errors{0};
    std::atomic<int> persisted{0};
    auto start_time = std::chrono::steady_clock::now();

    bool bulk_mode = (total > 1000);
    if (bulk_mode) {
        // SQLite tuning for bulk insert
        conn.exec("PRAGMA synchronous = OFF");
        conn.exec("PRAGMA journal_mode = OFF");
        conn.exec("PRAGMA cache_size = -200000"); // 200 MB page cache
        conn.exec("PRAGMA page_size = 32768");    // 32 KB pages, fewer B-tree levels
        std::cerr << "Bulk mode: dropping indexes for fast insert...\n";
        schema::drop_bulk_indexes(conn);
        fts::drop_sync_triggers(conn);
    } else {
        fts::create_sync_triggers(conn);
    }

    // --- Parse parallel, then persist+resolve in single transaction ---
    // Parse threads collect results in per-thread vectors (no contention).
    // After joining, persist + resolve run in one big transaction (fastest for SQLite).

    std::cerr << "Starting outlier + " << thread_count << " normal threads...\n";

    std::vector<ParsedFile> outlier_results;
    std::thread outlier_thread([&]() {
        Arena outlier_arena(768 * 1024 * 1024);
        set_thread_arena(&outlier_arena);
        outlier_results.reserve(outlier_files.size());
        for (auto& file : outlier_files) {
            outlier_arena.reset();
            ParsedFile pf;
            pf.file = file;
            try {
                auto content = read_file_content(file.absolute_path);
                if (content.empty()) { pf.parse_status = "failed"; pf.parse_error = "empty"; pf.has_error = true; }
                else {
                    pf.content_hash = hash_string(content);
                    Parser parser;
                    if (parser.set_language(file.language)) {
                        auto tree = TreeGuard(parser.parse(content));
                        if (tree) {
                            Extractor ex(config.max_symbols_per_file, config.max_ast_depth);
                            pf.extraction = ex.extract(tree.tree, content, file.language, file.relative_path);
                            pf.parse_status = pf.extraction.truncated ? "partial" : "ok";
                        } else { pf.parse_status = "failed"; pf.parse_error = "parse null"; pf.has_error = true; }
                    } else { pf.parse_status = "skipped"; }
                }
            } catch (const std::exception& e) { pf.parse_status = "failed"; pf.parse_error = e.what(); pf.has_error = true; }
            outlier_results.push_back(std::move(pf));
        }
        set_thread_arena(nullptr);
    });

    std::vector<std::unique_ptr<Arena>> thread_arenas;
    for (int t = 0; t < thread_count; ++t)
        thread_arenas.push_back(std::make_unique<Arena>(config.arena_size_bytes()));

    std::vector<std::vector<ParsedFile>> thread_results(thread_count);
    std::atomic<int> next_tid{0};
    std::atomic<int> normal_idx{0};
    std::atomic<int> normal_done{0};

    auto normal_worker = [&]() {
        int my_id = next_tid.fetch_add(1, std::memory_order_relaxed);
        Arena* arena = thread_arenas[my_id].get();
        auto& my_results = thread_results[my_id];
        while (true) {
            int idx = normal_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= normal_total) break;
            auto& file = normal_files[idx];
            arena->reset();
            set_thread_arena(arena);
            ParsedFile result;
            result.file = file;
            try {
                auto content = read_file_content(file.absolute_path);
                if (content.empty()) { result.parse_status = "failed"; result.parse_error = "empty"; result.has_error = true; }
                else {
                    result.content_hash = hash_string(content);
                    Parser parser;
                    if (!parser.set_language(file.language)) { result.parse_status = "skipped"; }
                    else {
                        auto tree = TreeGuard(parser.parse(content));
                        if (!tree) { result.parse_status = "failed"; result.parse_error = "parse null"; result.has_error = true; }
                        else {
                            Extractor extractor(config.max_symbols_per_file, config.max_ast_depth);
                            result.extraction = extractor.extract(tree.tree, content, file.language, file.relative_path);
                            result.parse_status = result.extraction.truncated ? "partial" : "ok";
                        }
                    }
                }
            } catch (const std::exception& e) { result.parse_status = "failed"; result.parse_error = e.what(); result.has_error = true; }
            my_results.push_back(std::move(result));
            normal_done.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> normal_threads;
    for (int t = 0; t < thread_count; ++t)
        normal_threads.emplace_back(normal_worker);

    // Progress while parsing
    {
        int progress_interval = (std::max)(1, (std::min)(total / 100, 500));
        int last_reported = 0;
        while (true) {
            int nd = normal_done.load(std::memory_order_relaxed);
            if (nd >= normal_total) break;
            int done = static_cast<int>(outlier_files.size()) + nd;
            if (done - last_reported >= progress_interval) {
                auto es = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                double r = es > 0 ? static_cast<double>(done) / es : 0;
                std::cerr << "[" << done << "/" << total << "] "
                          << static_cast<int>(100.0 * done / total) << "% "
                          << es << "s " << static_cast<int>(r) << " files/s\n";
                last_reported = done;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    outlier_thread.join();
    for (auto& t : normal_threads) t.join();

    auto parse_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();
    std::cerr << "Phase: parse " << parse_s << "s (" << total << " files)\n";

    // Persist + resolve in one single transaction
    auto persist_start = std::chrono::steady_clock::now();
    persister.reserve_maps(total);
    persister.init_id_counters();
    persister.begin_batch();
    int err_count = 0;
    persister.batch_persist(outlier_results, err_count);
    for (auto& tv : thread_results) persister.batch_persist(tv, err_count);
    // DON'T commit yet — resolve will run in the same transaction
    auto persist_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - persist_start).count();
    std::cerr << "Phase: persist " << persist_s << "s (" << total << " files)\n";

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    double rate = elapsed_s > 0 ? static_cast<double>(total) / elapsed_s : 0;
    std::cerr << "Done: " << total << " files in " << elapsed_s << "s"
              << " (" << static_cast<int>(rate) << " files/s)";
    if (err_count > 0) std::cerr << " [" << err_count << " errors]";
    std::cerr << "\n";

    // --- Subprocess isolation: handle suspect files ---
    // These are files that were in-flight when a previous run crashed.
    // Parse each one in a child process to isolate tree-sitter crashes.
    if (!suspect_files.empty()) {
        std::cerr << "Parsing " << suspect_files.size()
                  << " suspect files via subprocess isolation...\n";

        // Find our own exe path for subprocess invocation
        std::string exe_path;
#ifdef _WIN32
        char exe_buf[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
        exe_path = exe_buf;
#else
        exe_path = fs::canonical("/proc/self/exe").string();
#endif

        persister.begin_batch();
        for (size_t si = 0; si < suspect_files.size(); ++si) {
            auto& file = suspect_files[si];
            std::cerr << "[suspect " << (si+1) << "/" << suspect_files.size() << "] "
                      << file.relative_path << " ... " << std::flush;

            auto sub_result = subprocess_parse(
                exe_path, file.absolute_path.string(), file.language,
                repo_root.string(),
                config.max_symbols_per_file, config.max_ast_depth);

            ParsedFile pf;
            pf.file = file;

            if (sub_result.crashed) {
                // Confirmed crash — record in crashlist
                crashlist.record_crash(file.relative_path, sub_result.parse_error);
                pf.parse_status = "failed";
                pf.parse_error = "tree-sitter crash: " + sub_result.parse_error;
                pf.has_error = true;
                std::cerr << "CRASHED (added to crashlist)\n";
            } else {
                pf.extraction = std::move(sub_result.extraction);
                pf.content_hash = sub_result.content_hash;
                pf.parse_status = sub_result.parse_status;
                pf.parse_error = sub_result.parse_error;
                // If this was previously in crashlist, remove it (it works now!)
                if (crashlist.should_skip(file.relative_path)) {
                    crashlist.remove_entry(file.relative_path);
                }
                std::cerr << "ok (" << pf.extraction.symbols.size() << " symbols)\n";
            }

            persister.persist_file(pf.file, pf.extraction, pf.content_hash,
                                   pf.parse_status, pf.parse_error);
            persister.flush_if_needed(config.batch_size);
        }
        persister.commit_batch();
    }

    // --- Rebuild indexes after bulk insert (much faster than maintaining during insert) ---
    if (bulk_mode) {
        std::cerr << "Rebuilding indexes...\n";
        auto idx_start = std::chrono::steady_clock::now();
        schema::rebuild_indexes(conn);
        auto idx_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - idx_start).count();
        std::cerr << "Phase: indexes " << idx_elapsed << "s\n";
    }

    // T039: In-memory reference resolution — refs were NOT persisted to SQLite.
    // Instead, resolve them from the in-memory ParsedFile results directly into edges.
    // Drop edge indexes before resolve — they'll be rebuilt after
    conn.exec("DROP INDEX IF EXISTS idx_edges_src");
    conn.exec("DROP INDEX IF EXISTS idx_edges_dst");

    std::cerr << "Resolving cross-file references (in-memory)...\n";
    auto resolve_start = std::chrono::steady_clock::now();
    {
        auto& sym_map = persister.sym_map_;
        auto& file_node_map = persister.file_node_map_;
        auto& file_id_map = persister.file_id_map_;
        auto& pending_refs = persister.pending_refs_;
        std::cerr << "  " << sym_map.size() << " symbols, "
                  << pending_refs.size() << " refs to resolve\n";

        sqlite3_stmt* edge_stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "INSERT INTO edges(src_id,dst_id,kind,confidence,evidence) VALUES(?,?,?,?,?)",
            -1, &edge_stmt, nullptr);

        // Resolve runs inside the same transaction as persist (no separate COMMIT)
        int total_resolved = 0;

        for (const auto& ref : pending_refs) {
            auto fnit = file_node_map.find(ref.file_path);
            if (fnit == file_node_map.end()) continue;
            auto fidit = file_id_map.find(ref.file_path);
            int64_t this_file_id = (fidit != file_id_map.end()) ? fidit->second : -1;

            auto it = sym_map.find(ref.name);
            if (it == sym_map.end()) continue;
            if (ref.kind == "call" && it->second.file_id == this_file_id) continue;

            sqlite3_reset(edge_stmt);
            sqlite3_bind_int64(edge_stmt, 1, fnit->second);
            sqlite3_bind_int64(edge_stmt, 2, it->second.id);
            const char* ek = (ref.kind == "call") ? "calls" :
                             (ref.kind == "inherit") ? "inherits" : "references";
            sqlite3_bind_text(edge_stmt, 3, ek, -1, SQLITE_STATIC);
            sqlite3_bind_double(edge_stmt, 4, 0.7);
            sqlite3_bind_text(edge_stmt, 5, "name-match", -1, SQLITE_STATIC);
            sqlite3_step(edge_stmt);
            ++total_resolved;
        }
        // Commit the entire persist+resolve transaction
        persister.commit_batch();
        sqlite3_finalize(edge_stmt);

        auto resolve_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - resolve_start).count();
        std::cerr << "Phase: resolve " << resolve_elapsed << "s (" << total_resolved << " edges)\n";
    }

    // Rebuild edge indexes after resolve
    {
        auto es = std::chrono::steady_clock::now();
        conn.exec("CREATE INDEX IF NOT EXISTS idx_edges_src ON edges(src_id, kind)");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_edges_dst ON edges(dst_id, kind)");
        auto el = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - es).count();
        std::cerr << "Phase: edge_indexes " << el << "s\n";
    }

    // Rebuild FTS5 index in one pass (much faster than per-row triggers)
    if (bulk_mode) {
        std::cerr << "Rebuilding FTS5 index...\n";
        auto fts_start = std::chrono::steady_clock::now();
        fts::rebuild(conn);
        auto fts_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - fts_start).count();
        std::cerr << "Phase: fts " << fts_elapsed << "s\n";
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
