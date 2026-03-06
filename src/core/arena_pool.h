#pragma once

#include "core/arena.h"
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cassert>

namespace codetopo {

// T007: Central pool that owns N arenas and leases them to worker threads.
// Thread-safe: uses mutex + condvar for lease/return/wait.
class ArenaPool {
public:
    ArenaPool(int count, size_t arena_size) {
        arenas_.reserve(count);
        for (int i = 0; i < count; ++i) {
            arenas_.push_back(std::make_unique<Arena>(arena_size));
            available_.push_back(arenas_.back().get());
        }
    }

    // Lease an arena. Blocks if none available.
    Arena* lease() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !available_.empty(); });
        Arena* arena = available_.back();
        available_.pop_back();
        return arena;
    }

    // Return a leased arena to the pool. Resets it first.
    void release(Arena* arena) {
        assert(arena);
        arena->reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            available_.push_back(arena);
        }
        cv_.notify_one();
    }

    int total_count() const { return static_cast<int>(arenas_.size()); }

    int available_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(available_.size());
    }

private:
    std::vector<std::unique_ptr<Arena>> arenas_;
    std::vector<Arena*> available_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

// RAII guard that leases an arena on construction and returns it on destruction.
class ArenaLease {
public:
    explicit ArenaLease(ArenaPool& pool)
        : pool_(pool), arena_(pool.lease()) {}

    ~ArenaLease() {
        if (arena_) pool_.release(arena_);
    }

    ArenaLease(const ArenaLease&) = delete;
    ArenaLease& operator=(const ArenaLease&) = delete;

    ArenaLease(ArenaLease&& other) noexcept
        : pool_(other.pool_), arena_(other.arena_) {
        other.arena_ = nullptr;
    }

    Arena* get() const { return arena_; }
    Arena& operator*() const { return *arena_; }
    Arena* operator->() const { return arena_; }

private:
    ArenaPool& pool_;
    Arena* arena_;
};

} // namespace codetopo
