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
            workers_.emplace_back([this](std::stop_token stoken) {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this, &stoken] {
                            return stop_ || !tasks_.empty() || stoken.stop_requested();
                        });
                        if ((stop_ || stoken.stop_requested()) && tasks_.empty())
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
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        // jthread joins automatically on destruction
    }

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
    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
};

} // namespace codetopo
