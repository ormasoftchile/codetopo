#pragma once

#include "db/bind.h"
#include "db/connection.h"
#include "index/call_binding.h"
#include <memory>
#include <unordered_set>

namespace codetopo::call_binding {

struct Scopes {
    std::unordered_set<std::string> classes;
    std::unordered_set<std::string> namespaces;
};

inline Scopes load_scopes(
    Connection& conn, const std::unordered_set<std::string>& owner_names) {
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS __ct_call_owners(name TEXT PRIMARY KEY)");
    conn.exec("DELETE FROM temp.__ct_call_owners");
    auto prepare = [&](const char* sql) {
        sqlite3_stmt* raw = nullptr;
        int rc = sqlite3_prepare_v2(conn.raw(), sql, -1, &raw, nullptr);
        if (rc != SQLITE_OK)
            throw SqliteError(rc, "Call owner lookup failed: " + std::string(sqlite3_errmsg(conn.raw())));
        return std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(raw, sqlite3_finalize);
    };
    auto insert = prepare("INSERT OR IGNORE INTO temp.__ct_call_owners(name) VALUES(?)");
    for (const auto& name : owner_names) {
        for (const auto& lookup : {name, bare_name(name)}) {
            db::bind(insert.get(), lookup);
            int rc = sqlite3_step(insert.get());
            if (rc != SQLITE_DONE)
                throw SqliteError(rc, "Call owner collection failed: " + std::string(sqlite3_errmsg(conn.raw())));
        }
    }
    Scopes scopes;
    for (const auto& probe : {
        "CROSS JOIN nodes n INDEXED BY idx_nodes_name_type ON n.name=o.name ",
        "CROSS JOIN nodes n INDEXED BY idx_nodes_qualname ON n.qualname=o.name "}) {
        auto sql = "SELECT COALESCE(NULLIF(n.qualname,''),n.name),n.kind "
                   "FROM temp.__ct_call_owners o " + std::string(probe) +
                   "WHERE n.node_type='symbol' AND n.kind IN ('class','struct','interface','namespace')";
        auto select = prepare(sql.c_str());
        int rc;
        while ((rc = sqlite3_step(select.get())) == SQLITE_ROW) {
            auto text = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 0));
            auto kind = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 1));
            if (!text || !kind) continue;
            (std::string_view(kind) == "namespace" ? scopes.namespaces : scopes.classes).insert(without_templates(text));
        }
        if (rc != SQLITE_DONE)
            throw SqliteError(rc, "Call owner lookup failed: " + std::string(sqlite3_errmsg(conn.raw())));
    }
    return scopes;
}

} // namespace codetopo::call_binding
