#pragma once

#include "db/connection.h"
#include "db/schema.h"
#include "util/lock.h"
#include "util/log.h"
#include "util/process.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace codetopo {

enum class ReindexReason { startup, watcher, manual };

inline std::vector<std::string> build_reindex_arguments(
    const std::string& root,
    const std::string& db) {
    return {"index", "--root", root, "--db", db};
}

inline std::string read_index_generation(const std::string& db) {
    try {
        Connection conn(db, true);
        return schema::get_kv(conn, "index_generation", "");
    } catch (const std::exception& e) {
        mcp_log("reindex: could not read index generation: "
                + truncate_for_log(e.what()));
        return "";
    }
}

// Manages a spawned index supervisor, coalesces local triggers, and prevents
// independent MCP/watch processes from duplicating the same automatic work.
struct ReindexState {
    using Spawn = std::function<int(
        const std::string&, const std::vector<std::string>&)>;

    std::mutex mutex;
    std::atomic<bool> running{false};
    bool queued = false;
    bool queued_full = false;
    std::unordered_set<std::string> queued_paths;
    uint64_t trigger_count = 0;
    std::thread monitor_thread;
    std::atomic<uint64_t> list_counter{0};
    std::atomic<bool> stopping{false};
    std::mutex* writer_gate = nullptr;
    Spawn spawn = [](const std::string& exe, const std::vector<std::string>& args) {
        return spawn_and_wait(exe, args);
    };
    std::chrono::milliseconds automatic_coordination_timeout{1500};
    std::chrono::milliseconds manual_coordination_timeout{30000};

    ~ReindexState() { stop(); }

    void stop() {
        stopping = true;
        if (monitor_thread.joinable()) {
            mcp_log("shutdown: draining index child");
            monitor_thread.join();
        }
    }

    void trigger(const std::string& root, const std::string& db,
                 std::function<void()> on_complete,
                 const std::vector<std::string>& paths = {},
                 bool full_reindex = false,
                 ReindexReason reason = ReindexReason::manual,
                 std::chrono::system_clock::time_point automatic_since = {}) {
        uint64_t starting_trigger = 0;
        if (automatic_since == std::chrono::system_clock::time_point{}) {
            automatic_since = std::chrono::system_clock::now();
        }
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (stopping) return;
            starting_trigger = ++trigger_count;
            queued = true;
            queued_full = queued_full || full_reindex || paths.empty();
            if (queued_full) {
                queued_paths.clear();
            } else {
                queued_paths.insert(paths.begin(), paths.end());
            }
            if (running) {
                mcp_log("reindex: already running, queued");
                return;
            }
            running = true;
        }

        if (monitor_thread.joinable()) monitor_thread.join();
        monitor_thread = std::thread([=, this]() {
            namespace fs = std::filesystem;
            try {
                std::unique_lock<std::mutex> gate;
                if (writer_gate) {
                    gate = std::unique_lock<std::mutex>(*writer_gate, std::defer_lock);
                    while (!gate.try_lock()) {
                        if (stopping) {
                            running = false;
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                }

                const bool automatic = reason != ReindexReason::manual;
                const auto generation_before = read_index_generation(db);
                auto coordination_path = fs::path(db);
                coordination_path += ".reconcile.lock";
                FileLock coordination(coordination_path);
                auto timeout = automatic
                    ? automatic_coordination_timeout
                    : manual_coordination_timeout;
                if (!coordination.acquire_blocking(
                        timeout,
                        std::chrono::milliseconds(100),
                        std::chrono::milliseconds(500))) {
                    std::lock_guard<std::mutex> lk(mutex);
                    queued = false;
                    queued_full = false;
                    queued_paths.clear();
                    running = false;
                    mcp_log("reindex: coalesced with another MCP/watch process (PID "
                            + std::to_string(coordination.holder_pid()) + ")");
                    return;
                }

                if (automatic) {
                    auto generation_after = read_index_generation(db);
                    bool completed_since_trigger = false;
                    if (!generation_after.empty()) {
                        try {
                            auto generation_ns = std::stoll(generation_after);
                            auto trigger_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                automatic_since.time_since_epoch()).count();
                            completed_since_trigger = generation_ns > trigger_ns;
                        } catch (const std::exception&) {
                            completed_since_trigger = false;
                        }
                    }
                    if (!generation_after.empty() &&
                        (generation_after != generation_before || completed_since_trigger)) {
                        bool skip_duplicate = false;
                        {
                            std::lock_guard<std::mutex> lk(mutex);
                            if (trigger_count == starting_trigger) {
                                queued = false;
                                queued_full = false;
                                queued_paths.clear();
                                running = false;
                                skip_duplicate = true;
                            }
                        }
                        if (skip_duplicate) {
                            mcp_log("reindex: another process completed reconciliation; skipped duplicate");
                            if (on_complete) on_complete();
                            return;
                        }
                    }
                }

                while (true) {
                    bool run_full = false;
                    std::unordered_set<std::string> run_paths;
                    {
                        std::lock_guard<std::mutex> lk(mutex);
                        if (!queued || stopping) {
                            running = false;
                            break;
                        }
                        run_full = queued_full;
                        run_paths = queued_paths;
                        queued = false;
                        queued_full = false;
                        queued_paths.clear();
                    }

                    if (!run_full && run_paths.empty()) run_full = true;

                    std::optional<fs::path> changed_file;
                    auto args = build_reindex_arguments(root, db);
                    if (!run_full) {
                        std::error_code ec;
                        auto dir = fs::path(root) / ".codetopo";
                        fs::create_directories(dir, ec);
                        if (!ec) {
                            auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            auto seq = list_counter.fetch_add(1, std::memory_order_relaxed);
                            auto path = dir / ("changed-files-" + std::to_string(now) + "-"
                                               + std::to_string(seq) + ".lst");
                            std::ofstream out(path, std::ios::trunc);
                            if (out) {
                                for (const auto& p : run_paths) out << p << '\n';
                                out.close();
                                if (out) changed_file = path;
                            }
                        }
                        if (changed_file) {
                            args.push_back("--changed-file");
                            args.push_back(changed_file->string());
                        } else {
                            run_full = true;
                            mcp_log("reindex: could not write changed-file list; falling back to full");
                        }
                    }

                    auto started = std::chrono::steady_clock::now();
                    mcp_log(run_full ? "reindex: started (full)"
                                      : "reindex: started (targeted, "
                                          + std::to_string(run_paths.size()) + " paths)");
                    auto exe = get_self_executable_path();
                    int rc = spawn(exe, args);
                    if (changed_file) {
                        std::error_code ec;
                        fs::remove(*changed_file, ec);
                    }
                    auto elapsed = std::chrono::steady_clock::now() - started;
                    if (rc == 0) {
                        mcp_log("reindex: done (" + format_duration_seconds(elapsed) + ")");
                        if (on_complete) on_complete();
                    } else {
                        mcp_log("reindex: failed (" + format_duration_seconds(elapsed)
                                + ", exit=" + std::to_string(rc) + ")");
                    }
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(mutex);
                running = false;
                queued = false;
                mcp_log("reindex: worker failed: " + truncate_for_log(e.what()));
            } catch (...) {
                std::lock_guard<std::mutex> lk(mutex);
                running = false;
                queued = false;
                mcp_log("reindex: worker failed: unknown error");
            }
        });
    }
};

} // namespace codetopo
