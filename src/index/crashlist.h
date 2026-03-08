#pragma once
// crashlist.h — Persistent tracking of files that crash tree-sitter.
//
// Two mechanisms:
//   1) Journal: written before each batch, cleared after. If the process
//      dies mid-batch, the journal survives and those files become "suspects".
//   2) Crashlist: files confirmed to crash the parser (even in subprocess).
//      Persisted to .codetopo/crashes.json. Skipped on future runs.

#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <yyjson.h>

namespace codetopo {

struct CrashEntry {
    std::string path;
    std::string error;
    std::string date;
    int retry_count = 0;
};

class CrashList {
public:
    explicit CrashList(const std::string& codetopo_dir)
        : dir_(codetopo_dir)
        , crashlist_path_(dir_ + "/crashes.json")
        , journal_path_(dir_ + "/journal.txt") {
        load();
    }

    // Check if a file is in the crashlist (should be skipped)
    bool should_skip(const std::string& rel_path) const {
        return crashed_paths_.count(rel_path) > 0;
    }

    // Get list of suspect files from a previous crash (journal existed on startup)
    const std::vector<std::string>& suspects() const { return suspects_; }
    bool has_suspects() const { return !suspects_.empty(); }

    // Journal: write the batch of files about to be processed.
    // If the process crashes, this file survives.
    void journal_batch(const std::vector<std::string>& rel_paths) {
        std::ofstream f(journal_path_, std::ios::trunc);
        for (auto& p : rel_paths) f << p << "\n";
        f.flush();
    }

    // Journal: clear after batch is successfully committed
    void journal_clear() {
        std::filesystem::remove(journal_path_);
    }

    // Record a confirmed crash
    void record_crash(const std::string& rel_path, const std::string& error) {
        // Update or add entry
        bool found = false;
        for (auto& e : entries_) {
            if (e.path == rel_path) {
                e.error = error;
                e.date = now_iso();
                e.retry_count++;
                found = true;
                break;
            }
        }
        if (!found) {
            entries_.push_back({rel_path, error, now_iso(), 0});
        }
        crashed_paths_.insert(rel_path);
        save();
    }

    // Remove a file from crashlist (e.g., after successful retry)
    void remove_entry(const std::string& rel_path) {
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [&](const CrashEntry& e) { return e.path == rel_path; }),
            entries_.end());
        crashed_paths_.erase(rel_path);
        save();
    }

    // Clean up all inflight markers (call after successful indexing)
    void clear_inflight_files() {
        for (auto& entry : std::filesystem::directory_iterator(dir_)) {
            auto name = entry.path().filename().string();
            if (name.starts_with("inflight-") && name.ends_with(".txt")) {
                std::error_code ec;
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    // Get all crash entries (for reporting)
    const std::vector<CrashEntry>& entries() const { return entries_; }

    int count() const { return static_cast<int>(entries_.size()); }

private:
    std::string dir_;
    std::string crashlist_path_;
    std::string journal_path_;
    std::vector<CrashEntry> entries_;
    std::set<std::string> crashed_paths_;
    std::vector<std::string> suspects_;

    void load() {
        // Load crashlist
        if (std::filesystem::exists(crashlist_path_)) {
            std::ifstream f(crashlist_path_);
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string json = ss.str();

            yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
            if (doc) {
                yyjson_val* root = yyjson_doc_get_root(doc);
                yyjson_val* arr = yyjson_obj_get(root, "crashes");
                if (arr && yyjson_is_arr(arr)) {
                    size_t idx, max;
                    yyjson_val* val;
                    yyjson_arr_foreach(arr, idx, max, val) {
                        CrashEntry e;
                        auto* p = yyjson_obj_get(val, "path");
                        auto* err = yyjson_obj_get(val, "error");
                        auto* d = yyjson_obj_get(val, "date");
                        auto* r = yyjson_obj_get(val, "retry_count");
                        if (p) e.path = yyjson_get_str(p);
                        if (err) e.error = yyjson_get_str(err);
                        if (d) e.date = yyjson_get_str(d);
                        if (r) e.retry_count = static_cast<int>(yyjson_get_int(r));
                        entries_.push_back(e);
                        crashed_paths_.insert(e.path);
                    }
                }
                yyjson_doc_free(doc);
            }
        }

        // Load suspects from journal AND inflight files (previous crash)
        std::set<std::string> suspect_set;

        if (std::filesystem::exists(journal_path_)) {
            std::ifstream f(journal_path_);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty()) suspect_set.insert(line);
            }
        }

        // Read inflight-*.txt files (per-thread crash markers)
        try {
            for (auto& entry : std::filesystem::directory_iterator(dir_)) {
                auto name = entry.path().filename().string();
                if (name.starts_with("inflight-") && name.ends_with(".txt")) {
                    std::ifstream f(entry.path());
                    std::string line;
                    if (std::getline(f, line) && !line.empty()) {
                        suspect_set.insert(line);
                    }
                    f.close();
                    std::error_code ec;
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        } catch (...) {
            // Directory iteration can fail if files are locked — continue
        }

        suspects_.assign(suspect_set.begin(), suspect_set.end());
    }

    void save() {
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        for (auto& e : entries_) {
            yyjson_mut_val* obj = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, obj, "path", e.path.c_str());
            yyjson_mut_obj_add_str(doc, obj, "error", e.error.c_str());
            yyjson_mut_obj_add_str(doc, obj, "date", e.date.c_str());
            yyjson_mut_obj_add_int(doc, obj, "retry_count", e.retry_count);
            yyjson_mut_arr_append(arr, obj);
        }
        yyjson_mut_obj_add_val(doc, root, "crashes", arr);

        char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
        if (json) {
            std::ofstream f(crashlist_path_, std::ios::trunc);
            f << json;
            free(json);
        }
        yyjson_mut_doc_free(doc);
    }

    static std::string now_iso() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return buf;
    }
};

} // namespace codetopo
