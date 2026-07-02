#pragma once

#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>
#include <atomic>

namespace codetopo {

// T009: Worker thread pool using std::jthread with configurable thread count.
class ThreadPool {
public:
    explicit ThreadPool(int thread_count) : stop_(false) {
        for (int i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this] {
                            return stop_ || !tasks_.empty();
                        });
                        if (stop_ && tasks_.empty())
                            return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        // Timed join: wait up to 2s per thread. If a worker is stuck inside
        // ts_parser_parse() (a tree-sitter infinite loop that ignores cancellation),
        // joining forever would hang the process. Detaching the thread and calling
        // _exit(0) lets the OS clean up without UB from dangling stack references.
        for (auto& worker : workers_) {
            if (!worker.joinable()) continue;
            std::atomic<bool> joined{false};
            std::thread joiner([&worker, &joined]() {
                worker.join();
                joined.store(true, std::memory_order_release);
            });
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!joined.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    joiner.detach();
                    worker.detach();
                    had_stuck_threads_ = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (joiner.joinable()) joiner.join();
        }
    }

    // True if any worker threads were detached at shutdown (stuck in a parse).
    // Caller should call _exit(0) immediately after ThreadPool destructs to avoid
    // UB from detached threads accessing freed stack variables.
    bool had_stuck_threads() const { return had_stuck_threads_; }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit a task and get a future for its result.
    template<typename F>
    auto submit(F&& f) -> std::future<decltype(f())> {
        using ReturnType = decltype(f());
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::forward<F>(f));
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }

    int thread_count() const {
        return static_cast<int>(workers_.size());
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
    bool had_stuck_threads_ = false;
};

} // namespace codetopo
