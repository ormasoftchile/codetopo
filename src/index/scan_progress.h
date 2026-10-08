#pragma once

#include "util/stderr.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace codetopo {

struct ScanProgress {
    std::string phase;
    size_t directories = 0;
    size_t entries = 0;
    size_t sources = 0;
};

class ScanProgressReporter {
public:
    explicit ScanProgressReporter(
        const std::filesystem::path& path,
        std::chrono::milliseconds interval = std::chrono::seconds(5))
        : path_(path), interval_(interval) {
        write_locked();
        worker_ = std::thread([this] {
            std::unique_lock lock(mutex_);
            while (!condition_.wait_for(lock, interval_, [this] { return stopped_; })) {
                write_locked();
            }
        });
    }

    ~ScanProgressReporter() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void update(const ScanProgress& progress) {
        std::lock_guard lock(mutex_);
        bool phase_changed = progress_.phase != progress.phase;
        progress_ = progress;
        if (phase_changed) write_locked();
    }

private:
    std::filesystem::path path_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    ScanProgress progress_{"preparing"};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool stopped_ = false;
    bool warned_ = false;

    void write_locked() {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started_).count();
        std::string message = "scan: phase=" + progress_.phase +
            " directories=" + std::to_string(progress_.directories) +
            " entries=" + std::to_string(progress_.entries) +
            " sources=" + std::to_string(progress_.sources) +
            " elapsed=" + std::to_string(elapsed) + "s";
        write_stderr_line(message);
        std::ofstream output(path_, std::ios::trunc);
        output << message << '\n';
        output.flush();
        if (!output && !warned_) {
            warned_ = true;
            std::cerr << "WARN: Cannot write index scan progress: "
                      << path_.string() << '\n' << std::flush;
        }
    }
};

} // namespace codetopo
