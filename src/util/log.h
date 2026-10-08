#pragma once

#include "util/json.h"
#include "util/stderr.h"

#include <string>
#include <algorithm>
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
#include <cstdio>
#include <string_view>
#include <functional>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace codetopo {

inline bool stderr_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

inline std::string stderr_ansi(std::string_view text, const char* code, bool enabled) {
    if (!enabled) return std::string(text);
    return std::string(code) + std::string(text) + "\033[0m";
}

inline std::string stderr_dim(std::string_view text, bool enabled) {
    return stderr_ansi(text, "\033[2m", enabled);
}

inline std::string stderr_bold(std::string_view text, bool enabled) {
    return stderr_ansi(text, "\033[1m", enabled);
}

inline std::string stderr_cyan(std::string_view text, bool enabled) {
    return stderr_ansi(text, "\033[36m", enabled);
}

inline std::string stderr_yellow(std::string_view text, bool enabled) {
    return stderr_ansi(text, "\033[33m", enabled);
}

inline std::string stderr_bold_green(std::string_view text, bool enabled) {
    return stderr_ansi(text, "\033[1;32m", enabled);
}

inline std::string stderr_bold_red(std::string_view text, bool enabled) {
    return stderr_ansi(text, "\033[1;31m", enabled);
}

inline std::string format_with_commas(int64_t value) {
    auto digits = std::to_string(value);
    std::string formatted;
    formatted.reserve(digits.size() + digits.size() / 3);
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count == 3) {
            formatted.push_back(',');
            count = 0;
        }
        formatted.push_back(*it);
        ++count;
    }
    std::reverse(formatted.begin(), formatted.end());
    return formatted;
}

inline std::atomic<bool>& mcp_notify_active() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline std::atomic<int>& mcp_log_level() {
    static std::atomic<int> level{1};
    return level;
}

inline std::optional<int> mcp_log_level_value(std::string_view level) {
    constexpr std::string_view levels[] = {
        "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"
    };
    for (int i = 0; i < 8; ++i) {
        if (level == levels[i]) return i;
    }
    return std::nullopt;
}

struct McpLogSink {
    std::mutex mutex;
    std::function<void(const std::string&)> write;
    std::filesystem::path path;
};

inline McpLogSink& mcp_log_sink() {
    static McpLogSink sink;
    return sink;
}

inline std::filesystem::path active_mcp_log_path() {
    auto& sink = mcp_log_sink();
    std::lock_guard lock(sink.mutex);
    return sink.path;
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
    write_stderr_line(line);
    {
        auto& sink = mcp_log_sink();
        std::lock_guard lock(sink.mutex);
        if (sink.write) sink.write(line);
    }

    if (mcp_notify_active().load(std::memory_order_relaxed) &&
        mcp_log_level().load(std::memory_order_relaxed) <= 1) {
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);
        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_str(doc.doc, root, "method", "notifications/message");
        auto* params = doc.new_obj();
        yyjson_mut_obj_add_str(doc.doc, params, "level", "info");
        yyjson_mut_obj_add_strcpy(doc.doc, params, "data", line.c_str());
        yyjson_mut_obj_add_val(doc.doc, root, "params", params);
        json_write_line(doc.to_string());
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
    bool good() const { return file_.is_open() && file_.good(); }

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

class ScopedMcpLogFile {
public:
    explicit ScopedMcpLogFile(const std::filesystem::path& path) : logger_(path) {
        if (!logger_.good()) {
            std::cerr << "WARN: Cannot open MCP diagnostic log: " << path.string()
                      << '\n' << std::flush;
            return;
        }
        auto& sink = mcp_log_sink();
        std::lock_guard lock(sink.mutex);
        previous_ = std::move(sink.write);
        previous_path_ = std::move(sink.path);
        sink.path = path;
        sink.write = [this, path](const std::string& line) {
            if (failed_) return;
            try {
                logger_.info(line);
                if (!logger_.good()) report_failure(path, "write failed");
            } catch (const std::filesystem::filesystem_error& e) {
                report_failure(path, e.what());
            }
        };
        enabled_ = true;
    }

    ~ScopedMcpLogFile() {
        if (enabled_) {
            auto& sink = mcp_log_sink();
            std::lock_guard lock(sink.mutex);
            sink.write = std::move(previous_);
            sink.path = std::move(previous_path_);
        }
    }

    bool enabled() const { return enabled_; }
    ScopedMcpLogFile(const ScopedMcpLogFile&) = delete;
    ScopedMcpLogFile& operator=(const ScopedMcpLogFile&) = delete;

private:
    Logger logger_;
    std::function<void(const std::string&)> previous_;
    std::filesystem::path previous_path_;
    bool enabled_ = false;
    bool failed_ = false;

    void report_failure(const std::filesystem::path& path, const std::string& reason) {
        failed_ = true;
        mcp_log_sink().path.clear();
        std::cerr << "WARN: MCP diagnostic log disabled after an error: "
                  << path.string() << ": " << reason << '\n' << std::flush;
    }
};

} // namespace codetopo
