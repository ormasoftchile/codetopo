#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "core/config.h"
#include "db/connection.h"
#include "db/fts.h"
#include "db/schema.h"
#include "db/workspace.h"
#include "index/persister.h"
#include "cli/cmd_index.h"
#include "util/lock.h"
#include "util/repo.h"

#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <memory>
#include <future>
#include <thread>

namespace fs = std::filesystem;
using namespace codetopo;

static void cleanup_workspace_test_dir(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

static int64_t scalar_count(Connection& conn, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    int64_t count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << content;
}

static void create_empty_index(const fs::path& root) {
    fs::create_directories(root / ".codetopo");
    Connection conn(root / ".codetopo" / "index.sqlite");
    schema::ensure_schema(conn);
    fts::create_sync_triggers(conn);
}

static void create_source_index(const fs::path& root) {
    fs::create_directories(root / ".codetopo");
    write_file(root / "src" / "lib.cpp",
               "int mergedNeedleSymbol() {\n"
               "  int uniqueContentNeedle = 42;\n"
               "  return uniqueContentNeedle;\n"
               "}\n");

    Connection conn(root / ".codetopo" / "index.sqlite");
    schema::ensure_schema(conn);
    conn.exec(
        "INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
        "VALUES(1, 'src/lib.cpp', 'cpp', 88, 1000, 'hash-lib', 'ok')");
    conn.exec(
        "INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, signature, is_definition, stable_key) "
        "VALUES(1, 'file', NULL, 'file', 'src/lib.cpp', NULL, NULL, 1, 'file:src/lib.cpp')");
    conn.exec(
        "INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, signature, start_line, end_line, is_definition, fingerprint, stable_key) "
        "VALUES(2, 'symbol', 1, 'function', 'mergedNeedleSymbol', 'mergedNeedleSymbol', 'mergedNeedleSymbol()', 1, 3, 1, 'abcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcd', 'sym:mergedNeedleSymbol')");
    conn.exec(
        "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
        "VALUES(2, 2, 'references', 1.0, 'test')");
    conn.exec(
        "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
        "VALUES(1, 2, 'contains', 1.0, 'file-node-src-test')");
    conn.exec(
        "INSERT INTO refs(id, file_id, kind, name, start_line, start_col, end_line, end_col, resolved_node_id, evidence, containing_node_id) "
        "VALUES(1, 1, 'call', 'mergedNeedleSymbol', 1, 1, 1, 20, 2, 'test', 2)");
    conn.exec(
        "INSERT INTO refs(id, file_id, kind, name, start_line, start_col, end_line, end_col, resolved_node_id, evidence, containing_node_id) "
        "VALUES(2, 1, 'http_call', '/api/merged-needle', 2, 1, 2, 24, NULL, 'http_client_call', 2)");
    schema::set_kv(conn, "repo_root", fs::canonical(root).string());
    schema::set_kv(conn, "index_state", "current");
    schema::set_kv(conn, "last_index_time", "2026-10-06T00:00:00Z");
    fts::rebuild(conn);
}

TEST_CASE("Workspace add bulk-merges rows and keeps FTS/remove correct", "[unit][workspace]") {
    auto base = fs::current_path() / ".codetopo-workspace-merge-test";
    cleanup_workspace_test_dir(base);
    fs::create_directories(base);

    auto main_root = fs::canonical(base).string() + "/main";
    auto src_root = fs::canonical(base).string() + "/srcroot";
    fs::create_directories(main_root);
    fs::create_directories(src_root);
    create_empty_index(main_root);
    create_source_index(src_root);

    Config cfg;
    auto ws = std::make_unique<WorkspaceDB>(default_db(main_root));
    auto result = ws->add_root(src_root, cfg);
    REQUIRE(result.root_id > 0);
    REQUIRE(result.files == 1);
    REQUIRE(result.symbols == 1);
    REQUIRE(result.edges == 2);
    REQUIRE(result.http_call_refs == 1);

    {
        Connection conn(default_db(main_root));
        auto file_node = scalar_count(conn, "SELECT id FROM nodes WHERE stable_key='1:file:src/lib.cpp'");
        auto symbol_node = scalar_count(conn, "SELECT id FROM nodes WHERE stable_key='1:sym:mergedNeedleSymbol'");
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM files WHERE root_id=" + std::to_string(result.root_id)) == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM edges WHERE src_id=" + std::to_string(file_node)) == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM refs WHERE containing_node_id=" + std::to_string(symbol_node)) == 2);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes WHERE id=" + std::to_string(symbol_node) + " AND fingerprint IS NOT NULL") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts WHERE content_fts MATCH 'uniqueContentNeedle'") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM kv WHERE key LIKE 'workspace_content_fts_pending:%'") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }

    auto second = ws->add_root(src_root, cfg);
    REQUIRE(second.root_id == result.root_id);
    REQUIRE(second.files == 1);
    REQUIRE(second.symbols == 1);
    REQUIRE(second.edges == 2);
    REQUIRE(second.http_call_refs == 1);
    {
        Connection conn(default_db(main_root));
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts_tracker") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }

    cfg.workspace_content_fts = true;
    auto third = ws->add_root(src_root, cfg);
    REQUIRE(third.root_id == result.root_id);
    REQUIRE(third.files == 1);
    REQUIRE(third.symbols == 1);
    REQUIRE(third.edges == 2);
    REQUIRE(third.http_call_refs == 1);
    {
        Connection conn(default_db(main_root));
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts WHERE content_fts MATCH 'uniqueContentNeedle'") >= 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts_tracker") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM kv WHERE key LIKE 'workspace_content_fts_pending:%'") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }

    auto roots = ws->list_roots();
    REQUIRE(roots.size() == 1);
    REQUIRE(roots[0].edges == 2);
    auto removed = ws->remove_root(src_root);
    REQUIRE(removed.files == 1);
    REQUIRE(removed.symbols == 1);
    REQUIRE(removed.edges == 2);
    {
        Connection conn(default_db(main_root));
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM roots") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM files") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts_tracker") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }
    ws.reset();
    cleanup_workspace_test_dir(base);
}

TEST_CASE("Workspace allocation preserves primary growth and legacy colliding ranges", "[unit][workspace][workspace-ids]") {
    const auto base = fs::current_path() / ".codetopo-workspace-ids-test";
    REQUIRE_FALSE(fs::exists(base));
    const auto main_root = base / "main";
    const auto src_root = base / "source";
    const auto other_root = base / "other";
    create_empty_index(main_root);
    create_source_index(src_root);
    create_source_index(other_root);
    Config cfg;
    cfg.workspace_content_fts = true;
    {
        WorkspaceDB ws(default_db(main_root.string()));
        auto first = ws.add_root(src_root.string(), cfg);
        {
            Connection conn(default_db(main_root.string()));
            schema::register_custom_functions(conn.raw());
            // Convert the initial root to the pre-correction offset layout.
            content_fts::delete_file(conn, 1);
            conn.exec("BEGIN IMMEDIATE");
            conn.exec("PRAGMA defer_foreign_keys=ON");
            conn.exec("UPDATE nodes SET id=CASE id WHEN 1 THEN 1000000001 ELSE 1000000003 END WHERE id IN (1,2)");
            conn.exec("UPDATE files SET id=1000000001 WHERE id=1");
            conn.exec("UPDATE nodes SET file_id=1000000001 WHERE file_id=1");
            conn.exec("UPDATE edges SET src_id=CASE src_id WHEN 1 THEN 1000000001 ELSE 1000000003 END,dst_id=1000000003 WHERE src_id IN (1,2)");
            conn.exec("UPDATE refs SET id=id+1000000002,file_id=1000000001,resolved_node_id=CASE WHEN resolved_node_id IS NOT NULL THEN 1000000003 END,containing_node_id=1000000003");
            conn.exec("COMMIT");
            content_fts::insert_file(conn, 1000000001, "old workspace content\n");
            // Primary rows may already occupy a legacy workspace's numeric range.
            conn.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
                      "VALUES(1000000002,'legacy.c','c',1,1,'legacy','ok')");
            conn.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) "
                      "VALUES(1000000002,'symbol',1000000002,'function','legacyPrimary','legacy-primary')");
            conn.exec("INSERT INTO refs(id,file_id,kind,name,containing_node_id,resolved_node_id) "
                      "VALUES(1000000002,1000000002,'call','legacyPrimary',1000000002,1000000002)");
            conn.exec("INSERT INTO edges(src_id,dst_id,kind,evidence) VALUES(1000000002,1000000002,'calls','legacy')");
            content_fts::insert_file(conn, 1000000002, "preserved primary content\n");
            Persister persister(conn);
            ScannedFile file;
            file.relative_path = "new.c";
            file.language = "c";
            REQUIRE(persister.persist_file(file, ExtractionResult{}, "new", "ok"));
            REQUIRE(persister.last_file_id() > 1000000002);
        }
        {
            Connection source(default_db(src_root.string()));
            source.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
                        "VALUES(1000000002,'new.c','c',1,1,'source-new','ok')");
            source.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) "
                        "VALUES(1000000002,'symbol',1000000002,'function','sourceNew','source-new')");
        }
        auto refreshed = ws.add_root(src_root.string(), cfg);
        REQUIRE(refreshed.root_id == first.root_id);
        REQUIRE(refreshed.files == 2);
        REQUIRE(refreshed.edges == 2);
        auto other = ws.add_root(other_root.string(), cfg);
        REQUIRE(other.root_id != first.root_id);
        {
            Connection conn(default_db(main_root.string()));
            REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM files WHERE root_id IS NULL") == 2);
            REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes WHERE id=1000000002 AND name='legacyPrimary'") == 1);
            REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM refs WHERE id=1000000002 AND resolved_node_id=1000000002") == 1);
            REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM edges WHERE src_id=1000000002 AND dst_id=1000000002") == 1);
            REQUIRE(conn.foreign_key_check() == 0);
        }
        REQUIRE(ws.remove_root(src_root.string()).edges == 2);
        REQUIRE(ws.list_roots().size() == 1);
        REQUIRE(ws.list_roots()[0].edges == 2);
        REQUIRE(ws.remove_root(other_root.string()).edges == 2);
        {
            Connection conn(default_db(main_root.string()));
            REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM files WHERE root_id IS NULL") == 2);
            REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes WHERE id=1000000002") == 1);
            REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'legacyPrimary'") == 1);
            REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts WHERE content_fts MATCH 'preserved'") == 1);
            REQUIRE(conn.foreign_key_check() == 0);
        }
    }
    cleanup_workspace_test_dir(base);
}

TEST_CASE("Persist admission waits through SQLite writer overlap without blocking committed reads", "[unit][sqlite-overlap]") {
    const auto base = fs::current_path() / ".codetopo-writer-admission-test";
    REQUIRE_FALSE(fs::exists(base));
    create_empty_index(base);
    const auto db = default_db(base.string());
    {
        Connection holder(db);
        holder.exec("BEGIN IMMEDIATE");
        std::promise<void> attempting;
        auto ready = attempting.get_future();
        auto writer = std::async(std::launch::async, [&] {
            Connection conn(db);
            schema::register_custom_functions(conn.raw());
            Persister persister(conn);
            attempting.set_value();
            persister.begin_batch();
            ScannedFile file;
            file.relative_path = "overlap.c";
            file.language = "c";
            persister.persist_file(file, ExtractionResult{}, "committed", "ok");
            persister.commit_batch();
        });
        ready.get();
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const bool still_waiting = writer.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
        Connection reader(db, true);
        const auto start = std::chrono::steady_clock::now();
        const auto old_count = scalar_count(reader, "SELECT COUNT(*) FROM files");
        const auto read_time = std::chrono::steady_clock::now() - start;
        holder.exec("ROLLBACK");
        writer.get();
        REQUIRE(still_waiting);
        REQUIRE(old_count == 0);
        REQUIRE(read_time < std::chrono::seconds(1));
        REQUIRE(scalar_count(reader, "SELECT COUNT(*) FROM files WHERE content_hash='committed'") == 1);
    }
    cleanup_workspace_test_dir(base);
}

TEST_CASE("Persistence thread errors roll back and return failure without crash or quarantine", "[unit][sqlite-overlap]") {
    const auto base = fs::current_path() / ".codetopo-persist-failure-test";
    REQUIRE_FALSE(fs::exists(base));
    create_empty_index(base);
    write_file(base / "one.c", "int expectedOne() { return 1; }\n");
    write_file(base / "two.c", "int expectedTwo() { return 2; }\n");
    Config cfg;
    cfg.repo_root = base;
    cfg.db_path = default_db(base.string());
    cfg.thread_count = 2;
    cfg.arena_size_mb = 8;
    cfg.batch_size = 1;
    {
        Connection conn(cfg.db_path);
        conn.exec("CREATE TRIGGER reject_persist BEFORE INSERT ON files BEGIN SELECT RAISE(ABORT,'deliberate persist failure'); END");
    }
    REQUIRE(run_index(cfg) == 1);
    {
        Connection conn(cfg.db_path);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM files") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM quarantine") == 0);
        conn.exec("DROP TRIGGER reject_persist");
    }
    REQUIRE(run_index(cfg) == 0);
    {
        Connection conn(cfg.db_path);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM files") == 2);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM quarantine") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }
    cleanup_workspace_test_dir(base);
}

TEST_CASE("Invalid source mappings fail atomically and detach before source admission is released", "[unit][workspace][workspace-ids]") {
    const auto base = fs::current_path() / ".codetopo-invalid-source-test";
    REQUIRE_FALSE(fs::exists(base));
    const auto primary = base / "main";
    const auto source = base / "source";
    create_empty_index(primary);
    create_source_index(source);
    {
        WorkspaceDB ws(default_db(primary.string()));
        Config cfg;
        REQUIRE(ws.add_root(source.string(), cfg).edges == 2);
        {
            Connection corrupt(default_db(source.string()));
            corrupt.exec("PRAGMA foreign_keys=OFF");
            corrupt.exec("UPDATE refs SET resolved_node_id=999 WHERE id=1");
        }
        REQUIRE_THROWS_WITH(ws.add_root(source.string(), cfg), "Source index has invalid node/file foreign keys");
        REQUIRE(ws.list_roots().size() == 1);
        REQUIRE(ws.list_roots()[0].edges == 2);
        {
            FileLock writer(default_db(source.string()) + ".lock");
            REQUIRE(writer.acquire());
            Connection repair(default_db(source.string()));
            repair.exec("UPDATE refs SET resolved_node_id=2 WHERE id=1");
            repair.exec("PRAGMA foreign_keys=OFF");
            repair.exec("INSERT INTO edges(id,src_id,dst_id,kind) VALUES(999,999,2,'calls')");
        }
        // A failed INSERT must finalize its statement so DETACH can still finish.
        REQUIRE_THROWS(ws.add_root(source.string(), cfg));
        {
            FileLock writer(default_db(source.string()) + ".lock");
            REQUIRE(writer.acquire());
            Connection repair(default_db(source.string()));
            repair.exec("DELETE FROM edges WHERE id=999");
        }
        REQUIRE(ws.add_root(source.string(), cfg).edges == 2);
        Connection conn(default_db(primary.string()));
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 1);
        REQUIRE(conn.foreign_key_check() == 0);
    }
    cleanup_workspace_test_dir(base);
}
