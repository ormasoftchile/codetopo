#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "mcp/workspace_jobs.h"
#include "mcp/error.h"
#include "db/schema.h"
#include "db/queries.h"
#include <filesystem>
#include <chrono>
#include <thread>
#include <memory>
#include <future>

using namespace codetopo;
namespace fs = std::filesystem;

namespace {
struct JobFixture {
    fs::path base = fs::current_path() / ".codetopo-workspace-job-unit-test";
    fs::path primary = base / "primary";
    fs::path extra = base / "extra";
    std::string db;
    JobFixture() {
        fs::create_directories(primary / ".codetopo");
        fs::create_directories(extra);
        db = (primary / ".codetopo" / "index.sqlite").string();
        Connection conn(db);
        schema::ensure_schema(conn);
    }
    ~JobFixture() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }
};

std::string await_job(WorkspaceJobs& jobs, const std::string& id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (jobs.active() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE_FALSE(jobs.active());
    return jobs.status(id);
}

struct SourceAdmissionProbe {
    std::string lock_path;
    bool attached = false;
    bool admitted = false;
    bool validation_seen = false;
    bool merge_seen = false;
    bool detach_seen = false;
    bool reject_begin = false;
    bool begin_rejected = false;
    bool reject_commit = false;
    bool commit_rejected = false;
    int attempts = 0;
    std::promise<void>* merge_staged = nullptr;
    std::shared_future<void>* allow_commit = nullptr;
    std::promise<void>* merge_committed = nullptr;

    static int authorize(void* context, int action, const char* value,
                         const char*, const char*, const char*) {
        auto& probe = *static_cast<SourceAdmissionProbe*>(context);
        if (probe.reject_begin && action == SQLITE_TRANSACTION &&
            value && std::string(value) == "BEGIN") {
            probe.reject_begin = false;
            probe.begin_rejected = true;
            return SQLITE_DENY;
        }
        if (probe.reject_commit && action == SQLITE_TRANSACTION &&
            value && std::string(value) == "COMMIT") {
            probe.reject_commit = false;
            probe.commit_rejected = true;
            return SQLITE_DENY;
        }
        return SQLITE_OK;
    }

    static int trace(unsigned event, void* context, void* statement, void*) {
        auto& probe = *static_cast<SourceAdmissionProbe*>(context);
        const char* text = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (!text) return 0;
        std::string sql(text);
        if (event == SQLITE_TRACE_STMT && sql.starts_with("ATTACH DATABASE "))
            probe.attached = true;
        if (!probe.attached) return 0;
        probe.validation_seen |= sql.starts_with("SELECT 1 FROM src.roots");
        probe.merge_seen |= sql.starts_with("INSERT INTO nodes (");
        if (event == SQLITE_TRACE_PROFILE && sql.starts_with("INSERT INTO nodes (") &&
            probe.merge_staged) {
            probe.merge_staged->set_value();
            probe.merge_staged = nullptr;
            probe.allow_commit->wait();
        }
        if (event == SQLITE_TRACE_PROFILE && sql == "COMMIT" && probe.merge_committed) {
            probe.merge_committed->set_value();
            probe.merge_committed = nullptr;
        }
        // An independent source writer competes while each attached statement runs,
        // including DETACH completion, not just the phase notification before ATTACH.
        bool acquired = false;
        std::thread writer([&] {
            FileLock admission(probe.lock_path);
            acquired = admission.acquire();
        });
        writer.join();
        probe.admitted |= acquired;
        ++probe.attempts;
        if (event == SQLITE_TRACE_PROFILE && sql == "DETACH DATABASE src") {
            probe.detach_seen = true;
            probe.attached = false;
        }
        return 0;
    }
};

thread_local SourceAdmissionProbe* opening_source_probe = nullptr;

int install_source_trace(sqlite3* db, char**, const sqlite3_api_routines*) {
    if (opening_source_probe) {
        sqlite3_trace_v2(db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE,
                        SourceAdmissionProbe::trace, opening_source_probe);
        sqlite3_set_authorizer(db, SourceAdmissionProbe::authorize, opening_source_probe);
    }
    return SQLITE_OK;
}

struct SourceTraceRegistration {
    explicit SourceTraceRegistration(SourceAdmissionProbe& probe) {
        opening_source_probe = &probe;
        if (sqlite3_auto_extension(reinterpret_cast<void (*)()>(install_source_trace)) != SQLITE_OK) {
            opening_source_probe = nullptr;
            throw std::runtime_error("Could not register source admission trace");
        }
    }
    ~SourceTraceRegistration() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(install_source_trace));
        opening_source_probe = nullptr;
    }
};
}

TEST_CASE("Workspace jobs cancel queued work and join on shutdown", "[unit][workspace][contract]") {
    JobFixture fixture;
    std::mutex gate;
    std::unique_lock<std::mutex> held(gate);
    WorkspaceJobs jobs(gate);
    auto accepted = json_parse(jobs.start("refresh", fixture.extra.string(), fixture.db,
                                         fixture.primary.string()));
    REQUIRE(accepted);
    std::string id = json_get_str(accepted.root(), "job_id");
    REQUIRE_FALSE(id.empty());
    REQUIRE(jobs.active());
    REQUIRE_THROWS(jobs.start("remove", fixture.extra.string(), fixture.db,
                             fixture.primary.string()));
    auto cancelled = json_parse(jobs.status(id, true));
    REQUIRE(yyjson_get_bool(yyjson_obj_get(cancelled.root(), "cancel_requested")));
    jobs.shutdown(); // Must not wait for the deliberately held writer gate.
    auto final = json_parse(jobs.status(id));
    REQUIRE(std::string(json_get_str(final.root(), "status")) == "cancelled");
    REQUIRE_THROWS(jobs.status("not-a-job"));
}

TEST_CASE("Workspace jobs propagate failures and retain terminal results", "[unit][workspace][contract]") {
    JobFixture fixture;
    std::mutex gate;
    WorkspaceJobs jobs(gate);
    REQUIRE_THROWS(jobs.start("refresh", fixture.primary.string(), fixture.db, fixture.primary.string()));
    FileLock external(fixture.db + ".lock");
    REQUIRE(external.acquire());
    auto accepted = json_parse(jobs.start("remove", fixture.extra.string(), fixture.db,
                                         fixture.primary.string()));
    std::string id = json_get_str(accepted.root(), "job_id");
    auto failed = json_parse(await_job(jobs, id));
    REQUIRE(std::string(json_get_str(failed.root(), "status")) == "failed");
    REQUIRE(json_get_str(failed.root(), "error") != nullptr);
    external.release();
    auto next = json_parse(jobs.start("refresh", fixture.extra.string(), fixture.db,
                                     fixture.primary.string()));
    id = json_get_str(next.root(), "job_id");
    auto not_added = json_parse(await_job(jobs, id));
    REQUIRE(std::string(json_get_str(not_added.root(), "status")) == "failed");
    REQUIRE(std::string(json_get_str(not_added.root(), "error")).find("existing extra root") != std::string::npos);
    auto removal = json_parse(jobs.start("remove", fixture.extra.string(), fixture.db,
                                        fixture.primary.string()));
    id = json_get_str(removal.root(), "job_id");
    auto completed = json_parse(await_job(jobs, id));
    REQUIRE(std::string(json_get_str(completed.root(), "status")) == "completed");
    REQUIRE(yyjson_obj_get(completed.root(), "result"));
    auto after_cancel = json_parse(jobs.status(id, true));
    REQUIRE(std::string(json_get_str(after_cancel.root(), "status")) == "completed");
}

TEST_CASE("Workspace queries use scoped graph index plans without ANALYZE", "[unit][workspace][integration]") {
    JobFixture fixture;
    Connection conn(fixture.db);
    conn.exec("INSERT INTO roots(id,path,added_at) VALUES(1,'synthetic',datetime('now'))");
    conn.exec("WITH RECURSIVE x(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM x WHERE id<2000) "
              "INSERT INTO files(id,root_id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
              "SELECT id,CASE WHEN id>1900 THEN 1 ELSE NULL END,'fixture-'||id,'cpp',10,1,'hash','ok' FROM x");
    conn.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) "
              "SELECT id,'symbol',id,'function',CASE WHEN id=1999 THEN 'selected' ELSE 'other' END,'sym-'||id FROM files");
    conn.exec("INSERT INTO edges(src_id,dst_id,kind,confidence) VALUES(1999,2000,'calls',1)");
    conn.exec("CREATE TEMP TABLE workspace_lookup_names(name TEXT PRIMARY KEY)");
    conn.exec("INSERT INTO workspace_lookup_names VALUES('selected')");
    conn.exec("CREATE TEMP TABLE __ct_root_nodes(id INTEGER PRIMARY KEY)");
    conn.exec("CREATE TEMP TABLE __ct_map_nodes(source_id INTEGER PRIMARY KEY,target_id INTEGER UNIQUE)");
    const std::vector<std::string> queries = {
        "SELECT n.name,n.id,f.language FROM workspace_lookup_names w "
        "CROSS JOIN nodes n INDEXED BY idx_nodes_name_type ON n.name=w.name "
        "CROSS JOIN files f ON n.file_id=f.id WHERE n.node_type='symbol' "
        "AND n.kind IN ('function','method','constructor_fn') AND COALESCE(f.root_id,0)!=1",
        "SELECT n.name,n.id FROM files f CROSS JOIN nodes n INDEXED BY idx_nodes_file_id ON n.file_id=f.id "
        "WHERE f.root_id=1 AND n.node_type='symbol' AND n.kind IN ('function','method','constructor_fn')",
        "SELECT COUNT(*) FROM files f CROSS JOIN nodes n ON n.file_id=f.id WHERE f.root_id=1",
        "SELECT " + workspace_edge_count_sql("1"),
        "SELECT " + workspace_edge_count_sql("r.id") + " FROM roots r",
        workspace_node_ids_sql("1"),
        "SELECT n.id FROM temp.__ct_root_nodes t CROSS JOIN nodes n ON n.id=t.id WHERE n.node_type='symbol'",
        "DELETE FROM nodes WHERE id IN (SELECT id FROM temp.__ct_root_nodes)",
        "SELECT MAX(id) FROM nodes",
        "SELECT COUNT(*) FROM nodes WHERE id >= (SELECT MIN(id) FROM nodes)",
        "SELECT id,ROW_NUMBER() OVER (ORDER BY id) FROM nodes WHERE id >= (SELECT MIN(id) FROM nodes)",
        "SELECT m.target_id,e.dst_id FROM temp.__ct_map_nodes m CROSS JOIN edges e INDEXED BY idx_edges_src ON e.src_id=m.source_id",
        "SELECT e.src_id FROM edges e WHERE e.id >= (SELECT MIN(id) FROM edges)",
        "SELECT n.id FROM temp.__ct_map_nodes m CROSS JOIN nodes n ON n.id=m.source_id WHERE n.file_id IS NULL LIMIT 1",
        "SELECT name FROM nodes WHERE id=1",
        "SELECT confidence FROM edges WHERE src_id=1"
    };
    for (const auto& query : queries) {
        sqlite3_stmt* plan = nullptr;
        auto explain = "EXPLAIN QUERY PLAN " + query;
        REQUIRE(sqlite3_prepare_v2(conn.raw(), explain.c_str(), -1, &plan, nullptr) == SQLITE_OK);
        bool graph_search = false;
        while (sqlite3_step(plan) == SQLITE_ROW) {
            std::string detail = reinterpret_cast<const char*>(sqlite3_column_text(plan, 3));
            INFO(query << ": " << detail);
            REQUIRE_FALSE(detail.starts_with("SCAN nodes"));
            REQUIRE_FALSE(detail.starts_with("SCAN n"));
            REQUIRE_FALSE(detail.starts_with("SCAN edges"));
            REQUIRE_FALSE(detail.starts_with("SCAN e "));
            if (detail.find("SEARCH n ") != std::string::npos ||
                detail.find("SEARCH nodes ") != std::string::npos ||
                detail.find("SEARCH edges") != std::string::npos ||
                detail.find("SEARCH nodes") != std::string::npos ||
                detail.find("SEARCH e ") != std::string::npos) graph_search = true;
        }
        sqlite3_finalize(plan);
        REQUIRE(graph_search);
    }
}

TEST_CASE("Busy envelope explicitly permits retries", "[contract][workspace]") {
    McpError error{-32603, "busy", "Indexing in progress"};
    auto response = json_parse(error.to_json_rpc(7));
    auto* data = yyjson_obj_get(yyjson_obj_get(response.root(), "error"), "data");
    REQUIRE(std::string(json_get_str(data, "error_code")) == "busy");
    REQUIRE(yyjson_get_bool(yyjson_obj_get(data, "retryable")));
}

TEST_CASE("MCP request boundaries release cached read snapshots", "[unit][workspace][integration]") {
    JobFixture fixture;
    Connection reader(fixture.db, true);
    QueryCache cache(reader);
    auto* stmt = cache.get("roots", "SELECT COUNT(*) FROM roots");
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    REQUIRE(sqlite3_txn_state(reader.raw(), "main") == SQLITE_TXN_READ);
    cache.reset_all();
    REQUIRE(sqlite3_txn_state(reader.raw(), "main") == SQLITE_TXN_NONE);
    Connection writer(fixture.db);
    writer.exec("PRAGMA busy_timeout=100");
    writer.exec("INSERT INTO roots(path,added_at) VALUES('new-root',datetime('now'))");
    REQUIRE(sqlite3_wal_checkpoint_v2(writer.raw(), nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                     nullptr, nullptr) == SQLITE_OK);
    stmt = cache.get("roots", "SELECT COUNT(*) FROM roots");
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(stmt, 0) == 1);
}

TEST_CASE("WAL read requests stay coherent across concurrent graph and schema commits", "[unit][workspace][integration]") {
    JobFixture fixture;
    {
        Connection seed(fixture.db);
        seed.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
                  "VALUES(1,'snapshot.c','c',10,1,'hash','ok')");
        seed.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) "
                  "VALUES(1,'symbol',1,'function','old','one'),(2,'symbol',1,'function','target','two')");
        seed.exec("INSERT INTO edges(src_id,dst_id,kind,confidence) VALUES(1,2,'calls',1)");
    }
    Connection reader(fixture.db, true);
    QueryCache cache(reader);
    auto query = [&](const char* key, const char* sql) {
        auto* stmt = cache.get(key, sql);
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) throw SqliteError(rc, sqlite3_errmsg(reader.raw()));
        return std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    };
    std::promise<void> updated, commit;
    auto ready = updated.get_future();
    auto release = commit.get_future();
    auto writer = std::async(std::launch::async, [&] {
        try {
            Connection conn(fixture.db);
            conn.exec("BEGIN IMMEDIATE");
            conn.exec("UPDATE nodes SET name='new' WHERE id=1");
            conn.exec("UPDATE edges SET confidence=0.5 WHERE src_id=1");
            conn.exec("CREATE INDEX snapshot_test_index ON files(language)");
            updated.set_value();
            release.wait();
            conn.exec("COMMIT");
        } catch (...) {
            try { updated.set_exception(std::current_exception()); } catch (...) {}
            throw;
        }
    });
    ready.get();
    std::string before, after, old_edge;
    try {
        ReadSnapshot snapshot(reader);
        before = query("node", "SELECT name FROM nodes WHERE id=1");
        commit.set_value();
        writer.get(); // Commit finishes even though this request's snapshot is pinned.
        after = query("node", "SELECT name FROM nodes WHERE id=1");
        old_edge = query("edge", "SELECT confidence FROM edges WHERE src_id=1");
        cache.reset_all();
    } catch (...) {
        try { commit.set_value(); } catch (...) {}
        cache.reset_all();
        throw;
    }
    REQUIRE(before == "old");
    REQUIRE(after == "old");
    REQUIRE(old_edge == "1.0");
    REQUIRE(sqlite3_txn_state(reader.raw(), "main") == SQLITE_TXN_NONE);
    {
        ReadSnapshot snapshot(reader);
        // v2 statements reprepare correctly after a schema commit; only this thread
        // owns the connection and cache, including when clearing it.
        REQUIRE(query("node", "SELECT name FROM nodes WHERE id=1") == "new");
        REQUIRE(query("edge", "SELECT confidence FROM edges WHERE src_id=1") == "0.5");
        cache.reset_all();
    }
    Connection checkpoint(fixture.db);
    checkpoint.exec("PRAGMA busy_timeout=100");
    REQUIRE(sqlite3_wal_checkpoint_v2(checkpoint.raw(), nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                     nullptr, nullptr) == SQLITE_OK);
}

TEST_CASE("Read snapshots unwind and real read locks have a bounded typed failure", "[unit][workspace][contract]") {
    JobFixture fixture;
    Connection writer(fixture.db);
    writer.exec("PRAGMA journal_mode=DELETE");
    Connection reader(fixture.db, true);
    {
        REQUIRE_THROWS([&] {
            ReadSnapshot snapshot(reader);
            throw std::runtime_error("handler failed");
        }());
        REQUIRE(sqlite3_txn_state(reader.raw(), "main") == SQLITE_TXN_NONE);
    }
    writer.exec("BEGIN EXCLUSIVE");
    auto started = std::chrono::steady_clock::now();
    int error = SQLITE_OK;
    try { ReadSnapshot snapshot(reader); } catch (const SqliteError& e) { error = e.code(); }
    auto elapsed = std::chrono::steady_clock::now() - started;
    writer.exec("ROLLBACK");
    REQUIRE(error == SQLITE_BUSY);
    REQUIRE(elapsed < std::chrono::milliseconds(1500));
    REQUIRE(sqlite3_get_autocommit(reader.raw()));
    ReadSnapshot next(reader);
}

TEST_CASE("Read-only opens never create missing or independent in-memory indexes", "[unit][workspace][contract]") {
    JobFixture fixture;
    REQUIRE_THROWS(Connection(fixture.base / "missing.sqlite", true));
    REQUIRE_FALSE(fs::exists(fixture.base / "missing.sqlite"));
    Connection memory(":memory:");
    schema::ensure_schema(memory);
    memory.exec("INSERT INTO roots(path,added_at) VALUES('memory',datetime('now'))");
    QueryCache cache(memory);
    {
        ReadSnapshot snapshot(memory);
        auto* stmt = cache.get("root", "SELECT path FROM roots WHERE id=1");
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        REQUIRE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) == "memory");
        cache.reset_all();
    }
    REQUIRE(sqlite3_txn_state(memory.raw(), "main") == SQLITE_TXN_NONE);
}

TEST_CASE("Committed graph reads overlap real workspace replacement through commit and detach", "[unit][workspace][integration]") {
    JobFixture fixture;
    fs::create_directories(fixture.extra / ".codetopo");
    auto source_db = (fixture.extra / ".codetopo" / "index.sqlite").string();
    {
        Connection source(source_db);
        schema::ensure_schema(source);
        source.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
                    "VALUES(1,'merged.c','c',10,1,'hash','ok')");
        source.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) "
                    "VALUES(1,'symbol',1,'function','oldSymbol','merged-symbol')");
    }
    Config cfg;
    {
        WorkspaceDB initial(fixture.db);
        initial.add_root(fixture.extra.string(), cfg);
    }
    {
        Connection source(source_db);
        source.exec("UPDATE nodes SET name='newSymbol' WHERE id=1");
    }
    Connection reader(fixture.db, true);
    QueryCache cache(reader);
    auto name = [&] {
        auto* stmt = cache.get("merged", "SELECT name FROM nodes WHERE stable_key='1:merged-symbol'");
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) throw SqliteError(rc, sqlite3_errmsg(reader.raw()));
        return std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    };
    std::promise<void> staged, committed, release;
    auto staged_future = staged.get_future();
    auto committed_future = committed.get_future();
    auto release_future = release.get_future().share();
    SourceAdmissionProbe probe{source_db + ".lock"};
    probe.merge_staged = &staged;
    probe.merge_committed = &committed;
    probe.allow_commit = &release_future;
    auto writer = std::async(std::launch::async, [&] {
        try {
            SourceTraceRegistration registration(probe);
            WorkspaceDB workspace(fixture.db);
            workspace.add_root(fixture.extra.string(), cfg);
        } catch (...) {
            try { staged.set_exception(std::current_exception()); } catch (...) {}
            try { committed.set_exception(std::current_exception()); } catch (...) {}
            throw;
        }
    });
    std::string before_commit, after_commit;
    try {
        staged_future.get(); // The replacement INSERT has executed inside BEGIN EXCLUSIVE.
        {
            ReadSnapshot snapshot(reader);
            before_commit = name();
            release.set_value();
            committed_future.get();
            after_commit = name();
            cache.reset_all();
        }
        // Release the snapshot before waiting for the worker's TRUNCATE checkpoint.
        writer.get();
    } catch (...) {
        try { release.set_value(); } catch (...) {}
        cache.reset_all();
        throw;
    }
    REQUIRE(before_commit == "oldSymbol");
    REQUIRE(after_commit == "oldSymbol");
    REQUIRE(name() == "newSymbol");
    cache.reset_all();
    REQUIRE(probe.merge_seen);
    REQUIRE(probe.detach_seen);
    REQUIRE_FALSE(probe.admitted);
    REQUIRE(sqlite3_txn_state(reader.raw(), "main") == SQLITE_TXN_NONE);
}

TEST_CASE("Workspace scoped name lookup resolves both cross-root directions", "[unit][workspace][integration]") {
    JobFixture fixture;
    fs::create_directories(fixture.extra / ".codetopo");
    auto seed = [](const std::string& db, const std::string& definition, const std::string& call) {
        Connection conn(db);
        schema::ensure_schema(conn);
        conn.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
                  "VALUES(1,'fixture.c','c',10,1,'hash','ok')");
        conn.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key,is_definition) "
                  "VALUES(1,'symbol',1,'function','" + definition + "','sym-" + definition + "',1)");
        conn.exec("INSERT INTO refs(id,file_id,kind,name,start_line,start_col,end_line,end_col,containing_node_id) "
                  "VALUES(1,1,'call','" + call + "',1,0,1,1,1)");
    };
    seed(fixture.db, "primaryTarget", "mergedTarget");
    auto source = (fixture.extra / ".codetopo" / "index.sqlite").string();
    seed(source, "mergedTarget", "receiver.primaryTarget");
    WorkspaceDB workspace(fixture.db);
    Config cfg;
    auto added = workspace.add_root(fixture.extra.string(), cfg);
    REQUIRE(added.root_id > 0);
    for (int pass = 0; pass < 2; ++pass) {
        Connection conn(fixture.db, true);
        auto root_key = std::to_string(added.root_id) + ":sym-mergedTarget";
        sqlite3_stmt* identity = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(), "SELECT id FROM nodes WHERE stable_key=?", -1, &identity, nullptr) == SQLITE_OK);
        sqlite3_bind_text(identity, 1, root_key.c_str(), -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(identity) == SQLITE_ROW);
        const auto merged_id = sqlite3_column_int64(identity, 0);
        sqlite3_finalize(identity);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(),
            "SELECT dst_id FROM edges WHERE src_id=? AND kind='calls'", -1, &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, merged_id);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int64(stmt, 0) == 1);
        sqlite3_finalize(stmt);
        REQUIRE(sqlite3_prepare_v2(conn.raw(),
            "SELECT dst_id FROM edges WHERE src_id=1 AND kind='calls'", -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int64(stmt, 0) == merged_id);
        sqlite3_finalize(stmt);
        REQUIRE(conn.foreign_key_check() == 0);
        if (pass == 0) workspace.add_root(fixture.extra.string(), cfg);
    }
}

TEST_CASE("Workspace rejects unsafe nested source indexes before mutation", "[unit][workspace][integration]") {
    JobFixture fixture;
    fs::create_directories(fixture.extra / ".codetopo");
    {
        Connection source(fixture.extra / ".codetopo" / "index.sqlite");
        schema::ensure_schema(source);
        source.exec("INSERT INTO roots(path,added_at) VALUES('nested-root',datetime('now'))");
    }
    WorkspaceDB workspace(fixture.db);
    Config cfg;
    REQUIRE_THROWS(workspace.add_root(fixture.extra.string(), cfg));
    REQUIRE_FALSE(workspace.has_root(fixture.extra.string()));
    REQUIRE(workspace.list_roots().empty());
    // Failure detached the source and released its admission lock.
    {
        Connection source(fixture.extra / ".codetopo" / "index.sqlite");
        source.exec("DELETE FROM roots");
    }
    REQUIRE(workspace.add_root(fixture.extra.string(), cfg).root_id > 0);
}

TEST_CASE("Cached source merge excludes competing writers through detach", "[unit][workspace][lock][integration]") {
    JobFixture fixture;
    fs::create_directories(fixture.extra / ".codetopo");
    const auto source_db = (fixture.extra / ".codetopo" / "index.sqlite").string();
    {
        Connection source(source_db);
        schema::ensure_schema(source);
        source.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
                    "VALUES(1,'cached.c','c',10,1,'hash','ok')");
        source.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) "
                    "VALUES(1,'symbol',1,'function','cachedSymbol','cached-symbol')");
    }
    SourceAdmissionProbe probe{source_db + ".lock"};
    std::unique_ptr<WorkspaceDB> workspace;
    {
        SourceTraceRegistration registration(probe);
        workspace = std::make_unique<WorkspaceDB>(fixture.db);
    }
    Config cfg;
    {
        FileLock writer(source_db + ".lock");
        REQUIRE(writer.acquire());
        REQUIRE_THROWS_WITH(workspace->add_root(fixture.extra.string(), cfg),
                            "Source index busy: another indexer holds the writer lock");
        REQUIRE_FALSE(workspace->has_root(fixture.extra.string()));
        REQUIRE(probe.attempts == 0);
    }
    auto added = workspace->add_root(fixture.extra.string(), cfg);
    REQUIRE(added.symbols == 1);
    REQUIRE(probe.attempts > 0);
    REQUIRE(probe.validation_seen);
    REQUIRE(probe.merge_seen);
    REQUIRE(probe.detach_seen);
    REQUIRE_FALSE(probe.admitted);
    REQUIRE_FALSE(probe.attached);
    FileLock after_detach(source_db + ".lock");
    REQUIRE(after_detach.acquire());
    after_detach.release();

    // Validation failure must also detach before releasing source admission.
    {
        Connection source(source_db);
        source.exec("INSERT INTO roots(path,added_at) VALUES('nested-root',datetime('now'))");
    }
    probe.validation_seen = probe.merge_seen = probe.detach_seen = false;
    REQUIRE_THROWS(workspace->add_root(fixture.extra.string(), cfg));
    REQUIRE(probe.validation_seen);
    REQUIRE_FALSE(probe.merge_seen);
    REQUIRE(probe.detach_seen);
    REQUIRE_FALSE(probe.admitted);
    REQUIRE_FALSE(probe.attached);
    REQUIRE(after_detach.acquire());
    after_detach.release();
    {
        Connection source(source_db);
        source.exec("DELETE FROM roots");
    }
    REQUIRE(workspace->add_root(fixture.extra.string(), cfg).symbols == 1);
    REQUIRE_FALSE(probe.admitted);

    // Failed writer admission at BEGIN must detach even without an active transaction.
    probe.validation_seen = probe.merge_seen = probe.detach_seen = false;
    probe.reject_begin = true;
    REQUIRE_THROWS(workspace->add_root(fixture.extra.string(), cfg));
    REQUIRE(probe.begin_rejected);
    REQUIRE(probe.validation_seen);
    REQUIRE_FALSE(probe.merge_seen);
    REQUIRE(probe.detach_seen);
    REQUIRE_FALSE(probe.admitted);
    REQUIRE_FALSE(probe.attached);
    REQUIRE(after_detach.acquire());
    after_detach.release();
    REQUIRE(workspace->add_root(fixture.extra.string(), cfg).symbols == 1);

    probe.validation_seen = probe.merge_seen = probe.detach_seen = false;
    probe.reject_commit = true;
    REQUIRE_THROWS(workspace->add_root(fixture.extra.string(), cfg));
    REQUIRE(probe.commit_rejected);
    REQUIRE(probe.validation_seen);
    REQUIRE(probe.merge_seen);
    REQUIRE(probe.detach_seen);
    REQUIRE_FALSE(probe.admitted);
    REQUIRE_FALSE(probe.attached);
    REQUIRE(after_detach.acquire());
    after_detach.release();
    REQUIRE(workspace->add_root(fixture.extra.string(), cfg).symbols == 1);
}
