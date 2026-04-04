#pragma once

#include <sqlite3.h>
#include <string>
#include <filesystem>
#include <stdexcept>
#include <memory>

namespace codetopo {

// T010: SQLite connection manager with WAL, FK pragmas, busy_timeout,
// and SQLITE_CONFIG_HEAP/PAGECACHE pre-allocation.
class Connection {
public:
    // Pre-allocate SQLite heap before ANY sqlite3 API call.
    // Must be called once at process startup, before open().
    static void configure_heap(size_t heap_size, size_t page_cache_size) {
        static std::unique_ptr<uint8_t[]> heap_buf;
        static std::unique_ptr<uint8_t[]> page_buf;

        heap_buf = std::make_unique<uint8_t[]>(heap_size);
        page_buf = std::make_unique<uint8_t[]>(page_cache_size);

        sqlite3_config(SQLITE_CONFIG_HEAP, heap_buf.get(),
                       static_cast<int>(heap_size), 64);
        sqlite3_config(SQLITE_CONFIG_PAGECACHE, page_buf.get(),
                       4096, static_cast<int>(page_cache_size / 4096));
    }

    explicit Connection(const std::filesystem::path& db_path, bool readonly = false) {
        int flags = readonly
            ? (SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX)
            : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX);

        bool is_memory = (db_path.string() == ":memory:");

        int rc = sqlite3_open_v2(
            is_memory ? ":memory:" : db_path.string().c_str(),
            &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "failed to open database";
            if (db_) sqlite3_close(db_);
            throw std::runtime_error("SQLite open failed: " + msg);
        }

        // PRAGMA setup — skip WAL and mmap for in-memory databases
        if (!is_memory) {
            exec("PRAGMA journal_mode=WAL");
            exec("PRAGMA mmap_size=4294967296");  // 4 GB memory-mapped I/O
        }
        exec("PRAGMA foreign_keys=ON");
        exec("PRAGMA busy_timeout=5000");
        exec("PRAGMA synchronous=NORMAL");
        exec("PRAGMA cache_size=-65536");    // 64 MB page cache
    }

    ~Connection() {
        if (db_) sqlite3_close(db_);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept : db_(other.db_) {
        other.db_ = nullptr;
    }

    sqlite3* raw() { return db_; }

    // Enable turbo mode for maximum write throughput.
    // - synchronous=OFF: skip WAL fsync (safe against app crash, not OS crash)
    // - wal_autocheckpoint=0: no checkpoints during bulk insert (one at end)
    // - temp_store=MEMORY: temp tables in RAM
    // - cache_size=512 MB: larger page cache
    void enable_turbo() {
        exec("PRAGMA synchronous=OFF");
        exec("PRAGMA wal_autocheckpoint=0");
        exec("PRAGMA temp_store=MEMORY");
        exec("PRAGMA cache_size=-524288");  // 512 MB page cache (covers UNIQUE B-tree for 3M+ rows)
    }

    void exec(const char* sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error("SQLite exec failed: " + msg);
        }
    }

    void exec(const std::string& sql) { exec(sql.c_str()); }

    // Run PRAGMA wal_checkpoint(PASSIVE)
    void wal_checkpoint() {
        exec("PRAGMA wal_checkpoint(PASSIVE)");
    }

    // Backup this in-memory (or any) database to a file on disk using sqlite3_backup API.
    bool backup_to(const std::filesystem::path& dest_path) {
        sqlite3* dest_db = nullptr;
        int rc = sqlite3_open_v2(dest_path.string().c_str(), &dest_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK) {
            if (dest_db) sqlite3_close(dest_db);
            return false;
        }

        sqlite3_backup* backup = sqlite3_backup_init(dest_db, "main", db_, "main");
        if (!backup) {
            sqlite3_close(dest_db);
            return false;
        }

        rc = sqlite3_backup_step(backup, -1);  // copy all pages in one step
        sqlite3_backup_finish(backup);
        sqlite3_close(dest_db);

        return (rc == SQLITE_DONE);
    }

    // Run PRAGMA integrity_check (SLOW — scans every B-tree page, avoid in hot paths)
    std::string integrity_check() {
        std::string result;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "PRAGMA integrity_check", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        return result;
    }

    // Run PRAGMA quick_check — fast subset of integrity_check (no index verification)
    std::string quick_check() {
        std::string result;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "PRAGMA quick_check", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        return result;
    }

    // Run PRAGMA foreign_key_check — returns count of violations
    int foreign_key_check() {
        int count = 0;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "PRAGMA foreign_key_check", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ++count;
        }
        sqlite3_finalize(stmt);
        return count;
    }

private:
    sqlite3* db_ = nullptr;
};

} // namespace codetopo
