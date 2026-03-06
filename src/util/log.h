#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <cstdint>

namespace codetopo {

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
