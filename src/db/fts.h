#pragma once

#include "db/connection.h"

namespace codetopo {

// T019: FTS5 index management — sync triggers and rebuild.
namespace fts {

inline void create_sync_triggers(Connection& conn) {
    // Trigger to keep nodes_fts in sync on INSERT
    conn.exec(R"SQL(
        CREATE TRIGGER IF NOT EXISTS nodes_fts_insert AFTER INSERT ON nodes BEGIN
            INSERT INTO nodes_fts(rowid, name, qualname, signature, doc)
            VALUES (new.id, new.name, new.qualname, new.signature, new.doc);
        END;
    )SQL");

    // Trigger to keep nodes_fts in sync on DELETE
    conn.exec(R"SQL(
        CREATE TRIGGER IF NOT EXISTS nodes_fts_delete AFTER DELETE ON nodes BEGIN
            INSERT INTO nodes_fts(nodes_fts, rowid, name, qualname, signature, doc)
            VALUES ('delete', old.id, old.name, old.qualname, old.signature, old.doc);
        END;
    )SQL");

    // Trigger for UPDATE
    conn.exec(R"SQL(
        CREATE TRIGGER IF NOT EXISTS nodes_fts_update AFTER UPDATE ON nodes BEGIN
            INSERT INTO nodes_fts(nodes_fts, rowid, name, qualname, signature, doc)
            VALUES ('delete', old.id, old.name, old.qualname, old.signature, old.doc);
            INSERT INTO nodes_fts(rowid, name, qualname, signature, doc)
            VALUES (new.id, new.name, new.qualname, new.signature, new.doc);
        END;
    )SQL");
}

// Drop FTS sync triggers for bulk loading (avoids per-row trigger overhead)
inline void drop_sync_triggers(Connection& conn) {
    conn.exec("DROP TRIGGER IF EXISTS nodes_fts_insert");
    conn.exec("DROP TRIGGER IF EXISTS nodes_fts_delete");
    conn.exec("DROP TRIGGER IF EXISTS nodes_fts_update");
}

inline void rebuild(Connection& conn) {
    conn.exec("INSERT INTO nodes_fts(nodes_fts) VALUES('rebuild')");
}

} // namespace fts
} // namespace codetopo
