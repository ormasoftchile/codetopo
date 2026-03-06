#pragma once

#include "db/connection.h"
#include "util/hash.h"
#include "index/scanner.h"
#include <sqlite3.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace codetopo {

// T029: Incremental change detection — mtime+size fast path, xxHash64 on change.

struct FileRecord {
    int64_t id;
    std::string path;
    int64_t mtime_ns;
    int64_t size_bytes;
    std::string content_hash;
};

class ChangeDetector {
public:
    explicit ChangeDetector(Connection& conn) : conn_(conn) {
        load_existing();
    }

    struct ChangeResult {
        std::vector<ScannedFile> new_files;
        std::vector<ScannedFile> changed_files;
        std::vector<std::string> deleted_paths;  // paths in DB but not on disk
    };

    ChangeResult detect(const std::vector<ScannedFile>& scanned) {
        ChangeResult result;
        std::unordered_set<std::string> seen_paths;

        for (const auto& file : scanned) {
            seen_paths.insert(file.relative_path);

            auto it = existing_.find(file.relative_path);
            if (it == existing_.end()) {
                result.new_files.push_back(file);
                continue;
            }

            const auto& record = it->second;

            // Fast path: mtime + size unchanged → skip
            if (record.mtime_ns == file.mtime_ns && record.size_bytes == file.size_bytes) {
                continue;
            }

            // mtime or size changed → compute hash
            auto new_hash = hash_file(file.absolute_path);
            if (new_hash != record.content_hash) {
                result.changed_files.push_back(file);
            }
            // If hash matches despite mtime change, still skip (touch without edit)
        }

        // Detect deleted files
        for (const auto& [path, record] : existing_) {
            if (!seen_paths.count(path)) {
                result.deleted_paths.push_back(path);
            }
        }

        return result;
    }

private:
    Connection& conn_;
    std::unordered_map<std::string, FileRecord> existing_;

    void load_existing() {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(conn_.raw(),
            "SELECT id, path, mtime_ns, size_bytes, content_hash FROM files",
            -1, &stmt, nullptr);
        if (rc != SQLITE_OK) return;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FileRecord r;
            r.id = sqlite3_column_int64(stmt, 0);
            r.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            r.mtime_ns = sqlite3_column_int64(stmt, 2);
            r.size_bytes = sqlite3_column_int64(stmt, 3);
            r.content_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            existing_[r.path] = r;
        }
        sqlite3_finalize(stmt);
    }
};

} // namespace codetopo
