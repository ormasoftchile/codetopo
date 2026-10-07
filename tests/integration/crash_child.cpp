#include "util/process.h"
#include <sqlite3.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path root, db;
    bool resume = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--root" && i + 1 < args.size()) root = args[i + 1];
        if (args[i] == "--db" && i + 1 < args.size()) db = args[i + 1];
        if (args[i] == "--resume") resume = true;
    }
    if (root.empty() || db.empty()) return 2;
    auto exe = fs::path(codetopo::get_self_executable_path()).parent_path() / "codetopo.exe";
    {
        std::ofstream attempts(root / "attempts.log", std::ios::app);
        attempts << (resume ? "resume" : "initial") << '\n';
    }
    if (resume) return codetopo::spawn_and_wait(exe.string(), args);

    sqlite3* owner_db = nullptr;
    if (sqlite3_open(db.string().c_str(), &owner_db) != SQLITE_OK) {
        if (owner_db) sqlite3_close(owner_db);
        return 2;
    }
    const char* owner_schema =
        "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value TEXT NOT NULL)";
    if (sqlite3_exec(owner_db, owner_schema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(owner_db);
        return 2;
    }
    sqlite3_stmt* owner_stmt = nullptr;
    if (sqlite3_prepare_v2(owner_db,
            "INSERT OR REPLACE INTO kv(key,value) VALUES('repo_root',?)",
            -1, &owner_stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(owner_db);
        return 2;
    }
    auto canonical_root = fs::canonical(root).string();
    sqlite3_bind_text(
        owner_stmt, 1, canonical_root.c_str(), -1, SQLITE_TRANSIENT);
    int owner_rc = sqlite3_step(owner_stmt);
    sqlite3_finalize(owner_stmt);
    sqlite3_close(owner_db);
    if (owner_rc != SQLITE_DONE) return 2;

    auto initial_args = args;
    initial_args.insert(initial_args.end(), {"--only-files", "committed.c"});
    int rc = codetopo::spawn_and_wait(exe.string(), initial_args);
    if (rc != 0) return rc;
    {
        std::ofstream worklist(db.string() + ".worklist");
        for (const char* name : {"committed.c", "fault.c", "inflight1.c", "inflight2.c", "remaining.c"}) {
            auto file = root / name;
            worklist << name << '\t' << file.string() << "\tc\t"
                     << fs::file_size(file) << "\t0\n";
        }
        std::ofstream(db.string() + ".progress") << "committed.c\n";
    }
    // A real unhandled worker-thread SEH fault, not a success-shaped exit stub.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::thread worker([] {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    });
    worker.join();
    return 2;
}
