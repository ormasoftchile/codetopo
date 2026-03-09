#pragma once

#include "db/connection.h"
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <stdexcept>

namespace codetopo {

// T018: Prepared statement cache for read queries.
// Avoids re-preparing the same SQL for repeated tool calls.
class QueryCache {
public:
    explicit QueryCache(Connection& conn) : conn_(conn) {}

    ~QueryCache() {
        for (auto& [key, stmt] : cache_) {
            sqlite3_finalize(stmt);
        }
    }

    QueryCache(const QueryCache&) = delete;
    QueryCache& operator=(const QueryCache&) = delete;

    // Invalidate all cached prepared statements (e.g. after re-index).
    void clear() {
        for (auto& [key, stmt] : cache_) {
            sqlite3_finalize(stmt);
        }
        cache_.clear();
    }

    // Get or prepare a statement. Resets it if already prepared.
    sqlite3_stmt* get(const std::string& name, const std::string& sql) {
        auto it = cache_.find(name);
        if (it != cache_.end()) {
            sqlite3_reset(it->second);
            sqlite3_clear_bindings(it->second);
            return it->second;
        }

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(conn_.raw(), sql.c_str(),
                                     static_cast<int>(sql.size()), &stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare SQL '" + name + "': " +
                                     sqlite3_errmsg(conn_.raw()));
        }
        cache_[name] = stmt;
        return stmt;
    }

private:
    Connection& conn_;
    std::unordered_map<std::string, sqlite3_stmt*> cache_;
};

} // namespace codetopo
