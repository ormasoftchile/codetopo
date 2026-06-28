#pragma once

#include "util/json.h"

#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <sstream>
#include <optional>
#include <cstdlib>

namespace codetopo {

// When true, mcp_log also sends notifications/message over stdout (JSON-RPC channel).
// Set to true after the MCP client sends notifications/initialized.
inline std::atomic<bool>& mcp_notify_active() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline void mcp_log(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[10];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    std::string line = std::string("[") + buf + "] " + msg;

    // Always write to stderr (visible in terminal / VS Code output channel).
    std::cerr << line << "\n" << std::flush;

    // Also send as MCP notifications/message over stdout once the session is live.
    if (mcp_notify_active().load(std::memory_order_relaxed)) {
        // Build: {"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"..."}}
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);
        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_str(doc.doc, root, "method", "notifications/message");
        auto* params = doc.new_obj();
        yyjson_mut_obj_add_str(doc.doc, params, "level", "info");
        yyjson_mut_obj_add_strcpy(doc.doc, params, "data", line.c_str());
        yyjson_mut_obj_add_val(doc.doc, root, "params", params);

        std::lock_guard<std::mutex> lk(mcp_stdout_mutex());
        std::cout << doc.to_string() << "\n";
        std::cout.flush();
    }
}

inline std::string truncate_for_log(std::string text, size_t max_len = 120) {
    if (text.size() <= max_len) return text;
    if (max_len <= 3) return text.substr(0, max_len);
    return text.substr(0, max_len - 3) + "...";
}

template <typename Duration>
inline std::string format_duration_ms(Duration duration) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return std::to_string(ms) + "ms";
}

template <typename Duration>
inline std::string format_duration_seconds(Duration duration) {
    double seconds = std::chrono::duration<double>(duration).count();
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << seconds << "s";
    return oss.str();
}

inline std::string json_for_log(yyjson_val* val, size_t max_len = 120) {
    if (!val) return "{}";
    size_t len = 0;
    char* json = yyjson_val_write(val, 0, &len);
    if (!json) return "{}";
    std::string out(json, len);
    free(json);
    return truncate_for_log(out, max_len);
}

inline std::optional<size_t> json_result_count(const std::string& json) {
    auto doc = json_parse(json);
    if (!doc) return std::nullopt;
    auto* root = doc.root();
    if (!root) return std::nullopt;
    if (yyjson_is_arr(root)) return yyjson_arr_size(root);
    if (!yyjson_is_obj(root)) return std::nullopt;
    auto* results = yyjson_obj_get(root, "results");
    if (results && yyjson_is_arr(results)) return yyjson_arr_size(results);
    return std::nullopt;
}

// T015: Structured logging with rotation (50 MB threshold, 3 retained files).
class Logger {
public:
    explicit Logger(const std::filesystem::path& log_path,
                    int64_t max_size = 50 * 1024 * 1024,
                    int max_files = 3)
        : log_path_(log_path)
        , max_size_(max_size)
        , max_files_(max_files) {
        file_.open(log_path_, std::ios::app);
    }

    ~Logger() {
        if (file_.is_open()) file_.close();
    }

    void info(const std::string& msg) { log("INFO", msg); }
    void warn(const std::string& msg) { log("WARN", msg); }
    void error(const std::string& msg) { log("ERROR", msg); }

    void log(const std::string& level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        rotate_if_needed();

        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif

        if (file_.is_open()) {
            file_ << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
                   << "." << std::setfill('0') << std::setw(3) << ms.count()
                   << "Z [" << level << "] " << msg << "\n";
            file_.flush();
        }
    }

private:
    std::filesystem::path log_path_;
    std::ofstream file_;
    int64_t max_size_;
    int max_files_;
    std::mutex mutex_;

    void rotate_if_needed() {
        if (!file_.is_open()) return;

        auto pos = file_.tellp();
        if (pos < 0 || pos < max_size_) return;

        file_.close();

        // Rotate: .3 → delete, .2 → .3, .1 → .2, current → .1
        for (int i = max_files_; i >= 1; --i) {
            auto old_name = log_path_;
            old_name += "." + std::to_string(i);
            if (i == max_files_) {
                std::filesystem::remove(old_name);
            } else {
                auto new_name = log_path_;
                new_name += "." + std::to_string(i + 1);
                if (std::filesystem::exists(old_name)) {
                    std::filesystem::rename(old_name, new_name);
                }
            }
        }

        auto first_rotated = log_path_;
        first_rotated += ".1";
        std::filesystem::rename(log_path_, first_rotated);

        file_.open(log_path_, std::ios::app);
    }
};

} // namespace codetopo
