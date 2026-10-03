#pragma once

#include "index/scanner.h"
#include "index/extractor.h"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <atomic>
#include <string>

namespace codetopo {

// Result of parsing + extracting a single file (produced on worker threads).
struct ParsedFile {
    ScannedFile file;
    ExtractionResult extraction;
    std::string content_hash;
    std::string parse_status;   // ok, partial, failed, skipped
    std::string parse_error;
    std::string file_content;   // raw content for content_fts (moved from parse thread)
    bool has_error = false;
};

// DEC-038 OPT-3: Persist queue item (main thread → persist thread)
struct PersistItem {
    ParsedFile parsed;
    int work_list_index = 0;
    bool sentinel = false;  // signals end-of-work
};

// DEC-038 OPT-3: Bounded queue for persist pipeline
class PersistQueue {
    std::deque<PersistItem> queue_;
    std::mutex mutex_;
    std::condition_variable cv_not_full_;
    std::condition_variable cv_not_empty_;
    int capacity_;
    bool closed_ = false;

public:
    explicit PersistQueue(int capacity) : capacity_(capacity) {}

    void push(PersistItem&& item) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_not_full_.wait(lk, [this] { return queue_.size() < static_cast<size_t>(capacity_) || closed_; });
        if (closed_) return;
        queue_.push_back(std::move(item));
        cv_not_empty_.notify_one();
    }

    std::optional<PersistItem> pop() {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_not_empty_.wait(lk, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return std::nullopt;
        auto item = std::move(queue_.front());
        queue_.pop_front();
        cv_not_full_.notify_one();
        return item;
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

    bool empty() {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.empty();
    }

    void close() {
        std::unique_lock<std::mutex> lk(mutex_);
        closed_ = true;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    bool is_closed() {
        std::lock_guard<std::mutex> lk(mutex_);
        return closed_;
    }
};

// DEC-038 OPT-3: Persist thread state (shared atomics)
struct PersistThreadState {
    std::atomic<int> persisted_count{0};
    std::atomic<int> persist_errors{0};
    std::atomic<bool> fatal_error{false};
    std::string error_message;  // guarded by fatal_error flag
};

} // namespace codetopo
