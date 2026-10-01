#pragma once

#include "db/workspace.h"
#include "util/json.h"
#include "util/lock.h"
#include "util/log.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace codetopo {

// One owned worker, one retained job. No worker ever touches the MCP connection.
class WorkspaceJobs {
public:
    explicit WorkspaceJobs(std::mutex& gate) : gate_(gate) {}
    ~WorkspaceJobs() { shutdown(); }

    bool active() const { return active_.load(); }

    std::string start(const std::string& operation, const std::string& path,
                      const std::string& db, const std::string& primary) {
        if (path.empty()) throw std::runtime_error("missing required parameter: path");
        auto target = std::filesystem::canonical(path).string();
        if (!std::filesystem::is_directory(target))
            throw std::runtime_error("workspace path must be a directory");
        if (std::filesystem::equivalent(target, primary))
            throw std::runtime_error("primary root is not an extra workspace root");
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_) throw std::runtime_error("workspace job already active");
        if (worker_.joinable()) worker_.join();
        cancel_ = false;
        active_ = true;
        id_ = "workspace-" + std::to_string(++sequence_);
        operation_ = operation;
        path_ = target;
        status_ = "queued";
        phase_ = "waiting_for_writer";
        error_.clear();
        result_.clear();
        started_ = std::chrono::steady_clock::now();
        finished_ = {};
        try {
            worker_ = std::thread([this, operation, target, db] {
                try {
                    std::unique_lock<std::mutex> gate(gate_, std::defer_lock);
                    while (!gate.try_lock()) {
                        checkpoint("waiting_for_writer");
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    checkpoint("acquiring_lock");
                    FileLock writer(db + ".lock");
                    if (!writer.acquire())
                        throw std::runtime_error("database busy: another indexer holds the writer lock");
                    checkpoint("opening_workspace");
                    WorkspaceDB ws(db);
                    JsonMutDoc doc;
                    auto* out = doc.new_obj();
                    doc.set_root(out);
                    if (operation == "remove") {
                        checkpoint("removing");
                        auto r = ws.remove_root(target);
                        yyjson_mut_obj_add_int(doc.doc, out, "files_removed", r.files);
                        yyjson_mut_obj_add_int(doc.doc, out, "symbols_removed", r.symbols);
                        yyjson_mut_obj_add_int(doc.doc, out, "edges_removed", r.edges);
                    } else {
                        if (operation == "refresh") {
                            if (!ws.has_root(target))
                                throw std::runtime_error("refresh requires an existing extra root");
                        }
                        Config cfg;
                        cfg.thread_count = (std::min)(4, cfg.effective_thread_count());
                        cfg.parse_timeout_s = 5;
                        auto r = ws.add_root(target, cfg, true,
                            [this](const std::string& phase) { checkpoint(phase); });
                        yyjson_mut_obj_add_int(doc.doc, out, "root_id", r.root_id);
                        yyjson_mut_obj_add_int(doc.doc, out, "file_count", r.files);
                        yyjson_mut_obj_add_int(doc.doc, out, "symbol_count", r.symbols);
                        yyjson_mut_obj_add_int(doc.doc, out, "edge_count", r.edges);
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        result_ = doc.to_string();
                        status_ = "completed";
                        phase_ = "done";
                    }
                } catch (const Cancelled&) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_ = "cancelled";
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_ = "failed";
                    error_ = e.what();
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_ = "failed";
                    error_ = "unknown workspace worker error";
                }
                std::string diagnostic;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    finished_ = std::chrono::steady_clock::now();
                    diagnostic = "workspace job: " + id_ + " " + status_ + " phase=" + phase_
                        + " elapsed=" + format_duration_seconds(finished_ - started_)
                        + (error_.empty() ? "" : " error=" + truncate_for_log(error_));
                }
                mcp_log(diagnostic);
                active_ = false;
            });
        } catch (...) {
            active_ = false;
            status_ = "failed";
            error_ = "could not start workspace worker";
            finished_ = std::chrono::steady_clock::now();
            throw;
        }
        return snapshot_locked();
    }

    std::string status(const std::string& id, bool cancel = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id.empty() || id != id_) throw std::runtime_error("unknown job_id");
        if (cancel && active_) cancel_ = true;
        return snapshot_locked();
    }

    void shutdown() {
        cancel_ = true;
        if (worker_.joinable()) {
            mcp_log("shutdown: draining workspace job (safe phases are not interrupted)");
            worker_.join();
        }
    }

private:
    struct Cancelled {};
    std::mutex& gate_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> active_{false}, cancel_{false};
    uint64_t sequence_ = 0;
    std::string id_, operation_, path_, status_, phase_, error_, result_;
    std::chrono::steady_clock::time_point started_{}, finished_{};

    void checkpoint(const std::string& phase) {
        std::string diagnostic;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancel_) throw Cancelled{};
            phase_ = phase;
            status_ = "running";
            diagnostic = "workspace job: " + id_ + " phase=" + phase;
        }
        mcp_log(diagnostic);
    }

    std::string snapshot_locked() const {
        JsonMutDoc doc;
        auto* out = doc.new_obj();
        doc.set_root(out);
        yyjson_mut_obj_add_strcpy(doc.doc, out, "job_id", id_.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, out, "operation", operation_.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, out, "path", path_.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, out, "status", status_.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, out, "phase", phase_.c_str());
        yyjson_mut_obj_add_bool(doc.doc, out, "cancel_requested", cancel_);
        yyjson_mut_obj_add_str(doc.doc, out, "cancellation_policy",
            "between phases only; indexing child and committed merge finish safely");
        auto end = active_ ? std::chrono::steady_clock::now() : finished_;
        yyjson_mut_obj_add_int(doc.doc, out, "elapsed_ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(end - started_).count());
        if (!error_.empty()) yyjson_mut_obj_add_strcpy(doc.doc, out, "error", error_.c_str());
        if (!result_.empty()) {
            auto parsed = json_parse(result_);
            yyjson_mut_obj_add_val(doc.doc, out, "result", yyjson_val_mut_copy(doc.doc, parsed.root()));
        }
        return doc.to_string();
    }
};

} // namespace codetopo
