#pragma once

#include "db/connection.h"
#include "db/schema.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace codetopo {
namespace index_ownership {

namespace fs = std::filesystem;

struct RootResolution {
    fs::path root;
    bool metadata_present = false;
};

struct MetadataStatus {
    std::string ownership;
    std::string index;
    std::string persisted_state;
    std::string last_index_time;
};

inline fs::path canonical_directory(const fs::path& path, const std::string& label) {
    std::error_code ec;
    auto canonical = fs::canonical(path, ec);
    if (ec || !fs::is_directory(canonical, ec) || ec) {
        throw std::runtime_error(
            label + " is not an existing directory: " + path.string());
    }
    return canonical.lexically_normal();
}

inline std::string comparison_key(const fs::path& path) {
    auto value = path.lexically_normal().generic_string();
    while (value.size() > 1 && value.back() == '/') value.pop_back();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return value;
}

inline bool same_root(const fs::path& left, const fs::path& right) {
    std::error_code ec;
    if (fs::equivalent(left, right, ec) && !ec) return true;
    return comparison_key(left) == comparison_key(right);
}

inline RootResolution resolve_primary_root(
    Connection& conn,
    const fs::path& root_hint,
    bool root_was_explicit,
    const std::string& db_path) {
    auto stored = schema::get_kv(conn, "repo_root", "");

    if (!root_was_explicit && stored.empty()) {
        throw std::runtime_error(
            "Index root identity is missing for database '" + db_path +
            "'. Automatic indexing and watching will not use the client's current "
            "directory. Restart with an explicit --root <primary-repository> and "
            "--db <index.sqlite>; the first complete full index will establish ownership "
            "without deletion pruning.");
    }
    if (!stored.empty() && !fs::path(stored).is_absolute()) {
        throw std::runtime_error(
            "Database '" + db_path + "' has invalid relative repo_root metadata '" +
            stored + "'. Refusing to resolve it against the client working directory.");
    }

    auto selected = canonical_directory(
        root_was_explicit ? root_hint : fs::path(stored),
        root_was_explicit ? "Configured --root" : "Database repo_root metadata");

    if (!stored.empty()) {
        auto owner = canonical_directory(stored, "Database repo_root metadata");
        if (!same_root(selected, owner)) {
            throw std::runtime_error(
                "Database ownership conflict: '" + db_path + "' belongs to '" +
                owner.string() + "', but --root resolves to '" + selected.string() +
                "'. Use the database owned by that root, or restore the correct database "
                "metadata after verifying the index. No indexing was started.");
        }
        selected = owner;
    }

    return {selected, !stored.empty()};
}

inline MetadataStatus inspect_metadata(Connection& conn, bool writer_active) {
    MetadataStatus result;
    result.ownership =
        schema::get_kv(conn, "repo_root", "").empty() ? "missing" : "verified";
    result.persisted_state = schema::get_kv(conn, "index_state", "");
    result.last_index_time = schema::get_kv(conn, "last_index_time", "");

    if (writer_active) {
        result.index = "indexing";
    } else if (result.persisted_state == "indexing") {
        result.index = "interrupted";
    } else if (result.ownership == "missing") {
        result.index = "missing_metadata";
    } else if (result.persisted_state == "needs_reconciliation") {
        result.index = "needs_reconciliation";
    } else if (result.persisted_state == "incomplete") {
        result.index = "incomplete";
    } else if (result.last_index_time.empty()) {
        result.index = "incomplete";
    } else {
        result.index = "current";
    }
    return result;
}

inline bool metadata_allows_deletion(const RootResolution& resolution) {
    return resolution.metadata_present;
}

} // namespace index_ownership
} // namespace codetopo
