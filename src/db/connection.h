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

        int rc = sqlite3_open_v2(db_path.string().c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "failed to open database";
            if (db_) sqlite3_close(db_);
            throw std::runtime_error("SQLite open failed: " + msg);
        }

        // PRAGMA setup
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA foreign_keys=ON");
        exec("PRAGMA busy_timeout=5000");
        exec("PRAGMA synchronous=NORMAL");
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

    // Run PRAGMA integrity_check
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
