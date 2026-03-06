#pragma once

#include "db/connection.h"
#include "db/schema.h"
#include <iostream>
#include <filesystem>
#include <sqlite3.h>

namespace codetopo {

// T100-T101: Doctor command — database integrity check and diagnostics.
inline int run_doctor(const std::string& db_path) {
    namespace fs = std::filesystem;

    if (!fs::exists(db_path)) {
        std::cerr << "ERROR: Database not found: " << db_path << "\n";
        return 1;
    }

    Connection conn(db_path, true);

    // Schema version check
    int version = schema::get_schema_version(conn);

    if (version == 0) {
        std::cout << "status: empty\n";
        std::cout << "message: Database has no schema (not yet indexed)\n";
        return 1;
    }

    if (version > CURRENT_SCHEMA_VERSION) {
        std::cout << "status: version_mismatch\n";
        std::cout << "schema_version: " << version << "\n";
        std::cout << "expected_version: " << CURRENT_SCHEMA_VERSION << "\n";
        std::cout << "message: Database was created by a newer version of codetopo. Please upgrade.\n";
        return 3;
    }

    // Integrity check
    auto integrity = conn.integrity_check();
    int fk_violations = conn.foreign_key_check();

    // Counts
    auto count = [&](const char* table) -> int64_t {
        sqlite3_stmt* stmt = nullptr;
        std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
        sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr);
        int64_t n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    };

    auto idx_version = schema::get_kv(const_cast<Connection&>(conn), "indexer_version", "unknown");
    auto last_index = schema::get_kv(const_cast<Connection&>(conn), "last_index_time", "never");
    auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root", "unknown");

    bool healthy = (integrity == "ok" && fk_violations == 0);

    std::cout << "status: " << (healthy ? "ok" : "error") << "\n";
    std::cout << "schema_version: " << version << "\n";
    std::cout << "indexer_version: " << idx_version << "\n";
    std::cout << "repo_root: " << repo_root << "\n";
    std::cout << "last_index_time: " << last_index << "\n";
    std::cout << "integrity_check: " << integrity << "\n";
    std::cout << "foreign_key_violations: " << fk_violations << "\n";
    std::cout << "files: " << count("files") << "\n";
    std::cout << "symbols: " << count("nodes") << "\n";
    std::cout << "edges: " << count("edges") << "\n";
    std::cout << "refs: " << count("refs") << "\n";

    if (!healthy) {
        if (integrity != "ok") std::cout << "PROBLEM: integrity check failed: " << integrity << "\n";
        if (fk_violations > 0) std::cout << "PROBLEM: " << fk_violations << " foreign key violations\n";
        return 1;
    }

    return 0;
}

} // namespace codetopo
