#include <catch2/catch_test_macros.hpp>
#include "cli/cmd_index.h"
#include "core/config.h"
#include "db/connection.h"
#include "db/schema.h"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace codetopo;

static int64_t targeted_scalar_int(Connection& conn, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr);
    int64_t value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static std::string targeted_scalar_text(Connection& conn, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr);
    std::string value;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0)) {
        value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return value;
}

static Config targeted_config(const fs::path& root) {
    Config cfg;
    cfg.repo_root = root;
    cfg.db_path = root / ".codetopo" / "index.sqlite";
    cfg.no_gitignore = true;
    cfg.supervised = true;
    cfg.thread_count = 1;
    cfg.batch_size = 1;
    cfg.parse_timeout_s = 2;
    cfg.extraction_timeout_s = 2;
    return cfg;
}

TEST_CASE("Targeted reindex updates listed files and prunes listed deletes only", "[unit][targeted_reindex]") {
    auto root = fs::current_path() / "build" / "test_targeted_reindex";
    fs::remove_all(root);
    fs::create_directories(root / ".codetopo");
    fs::create_directories(root / "src");

    {
        std::ofstream(root / "src" / "a.cpp") << "int alpha() { return 1; }\n";
        std::ofstream(root / "src" / "b.cpp") << "int beta() { return 2; }\n";
    }

    auto cfg = targeted_config(root);
    REQUIRE(run_index(cfg) == 0);

    int64_t b_file_id = 0;
    std::string old_a_hash;
    std::string old_b_hash;
    {
        Connection conn(cfg.db_path);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM files WHERE root_id IS NULL") == 2);
        old_a_hash = targeted_scalar_text(conn, "SELECT content_hash FROM files WHERE path = 'src/a.cpp'");
        old_b_hash = targeted_scalar_text(conn, "SELECT content_hash FROM files WHERE path = 'src/b.cpp'");
        b_file_id = targeted_scalar_int(conn, "SELECT id FROM files WHERE path = 'src/b.cpp'");
        REQUIRE(!old_a_hash.empty());
        REQUIRE(!old_b_hash.empty());
        REQUIRE(b_file_id > 0);
    }

    {
        std::ofstream(root / "src" / "a.cpp") << "int alpha_changed() { return 10; }\n";
        std::ofstream(root / "src" / "c.cpp") << "int should_not_be_seen() { return 3; }\n";
        fs::last_write_time(root / "src" / "a.cpp", fs::file_time_type::clock::now());
    }

    cfg.only_files = {"src/a.cpp"};
    REQUIRE(run_index(cfg) == 0);

    {
        Connection conn(cfg.db_path);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM files WHERE root_id IS NULL") == 2);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM files WHERE path = 'src/c.cpp'") == 0);
        REQUIRE(targeted_scalar_text(conn, "SELECT content_hash FROM files WHERE path = 'src/a.cpp'") != old_a_hash);
        REQUIRE(targeted_scalar_text(conn, "SELECT content_hash FROM files WHERE path = 'src/b.cpp'") == old_b_hash);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM nodes WHERE name = 'alpha_changed'") >= 1);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM nodes WHERE name = 'beta'") >= 1);
    }

    fs::remove(root / "src" / "b.cpp");
    auto list_file = root / ".codetopo" / "changed.lst";
    {
        std::ofstream(list_file) << "src/b.cpp\n";
    }

    cfg.only_files.clear();
    cfg.changed_file_lists = {list_file.string()};
    REQUIRE(run_index(cfg) == 0);

    {
        Connection conn(cfg.db_path);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM files WHERE path = 'src/b.cpp'") == 0);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM nodes WHERE name = 'beta'") == 0);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'beta'") == 0);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM content_fts WHERE file_id = " + std::to_string(b_file_id)) == 0);
        REQUIRE(targeted_scalar_int(conn, "SELECT COUNT(*) FROM files WHERE path = 'src/c.cpp'") == 0);
    }

    fs::remove_all(root);
}

TEST_CASE("Reparse repairs unchanged C++ extraction without clearing stable or additional-root identities",
          "[unit][targeted_reindex][cpp_binding]") {
    auto root = fs::current_path() / "build" / "test_cpp_reparse";
    fs::remove_all(root);
    fs::create_directories(root / ".codetopo");
    fs::create_directories(root / "src");
    std::ofstream(root / "src" / "a.cpp") << "int alpha() { return 1; }\n";
    std::ofstream(root / "src" / "b.cpp") << "int beta() { return 2; }\n";
    auto cfg = targeted_config(root);
    REQUIRE(run_index(cfg) == 0);
    int64_t alpha;
    int64_t beta;
    int64_t file_id;
    std::string hash;
    {
        Connection conn(cfg.db_path);
        schema::register_custom_functions(conn.raw());
        alpha = targeted_scalar_int(conn, "SELECT id FROM nodes WHERE stable_key='src/a.cpp::function::alpha'");
        beta = targeted_scalar_int(conn, "SELECT id FROM nodes WHERE stable_key='src/b.cpp::function::beta'");
        file_id = targeted_scalar_int(conn, "SELECT id FROM files WHERE path='src/a.cpp'");
        hash = targeted_scalar_text(conn, "SELECT content_hash FROM files WHERE id=" + std::to_string(file_id));
        conn.exec(
            "INSERT INTO nodes(node_type,file_id,kind,name,qualname,start_line,end_line,stable_key) VALUES("
            "'symbol'," + std::to_string(file_id) + ",'function','alpha','alpha',1,1,'src/a.cpp::function::alpha#2')");
        conn.exec(
            "INSERT INTO edges(src_id,dst_id,kind,confidence,evidence) VALUES(" +
            std::to_string(beta) + "," + std::to_string(alpha) + ",'calls',0.75,'name-match')");
        conn.exec(
            "INSERT INTO edges(src_id,dst_id,kind,confidence,evidence,source,observed_count) VALUES(" +
            std::to_string(beta) + "," + std::to_string(alpha) + ",'calls',0.99,'observed','runtime',42)");
        conn.exec("INSERT INTO roots(id,path,added_at) VALUES(1,'additional-root',datetime('now'))");
        conn.exec(
            "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status,root_id) "
            "VALUES(500,'additional-root/keep.cpp','cpp',1,1,'keep','ok',1)");
        conn.exec(
            "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) "
            "VALUES(500,'symbol',500,'function','keep','1:keep.cpp::function::keep')");
        conn.exec(
            "INSERT INTO edges(id,src_id,dst_id,kind,confidence,evidence) "
            "VALUES(500,500,500,'calls',0.9,'keep')");
    }
    cfg.reparse_unchanged = true;
    cfg.only_files = {"src/a.cpp", "src/b.cpp"};
    REQUIRE(run_index(cfg) == 0);
    {
        Connection conn(cfg.db_path);
        CHECK(targeted_scalar_int(conn, "SELECT id FROM nodes WHERE stable_key='src/a.cpp::function::alpha'") == alpha);
        CHECK(targeted_scalar_int(conn, "SELECT id FROM nodes WHERE stable_key='src/b.cpp::function::beta'") == beta);
        CHECK(targeted_scalar_int(conn, "SELECT id FROM files WHERE path='src/a.cpp'") == file_id);
        CHECK(targeted_scalar_text(conn, "SELECT content_hash FROM files WHERE id=" + std::to_string(file_id)) == hash);
        CHECK(targeted_scalar_int(conn, "SELECT COUNT(*) FROM nodes WHERE name='alpha'") == 1);
        CHECK(targeted_scalar_int(conn,
            "SELECT COUNT(*) FROM edges WHERE src_id=" + std::to_string(beta) +
            " AND dst_id=" + std::to_string(alpha) + " AND source='static'") == 0);
        CHECK(targeted_scalar_int(conn,
            "SELECT COUNT(*) FROM edges WHERE src_id=" + std::to_string(beta) +
            " AND dst_id=" + std::to_string(alpha) + " AND source='runtime' AND observed_count=42") == 1);
        CHECK(targeted_scalar_int(conn, "SELECT COUNT(*) FROM files WHERE id=500 AND root_id=1") == 1);
        CHECK(targeted_scalar_int(conn, "SELECT COUNT(*) FROM nodes WHERE id=500 AND name='keep'") == 1);
        CHECK(targeted_scalar_int(conn, "SELECT COUNT(*) FROM edges WHERE id=500 AND evidence='keep'") == 1);
        CHECK(conn.foreign_key_check() == 0);
    }
    fs::remove_all(root);
}
