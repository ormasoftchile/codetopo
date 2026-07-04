#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace codetopo {

// T014: PID-based lock file with stale-lock detection.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path) : path_(path) {}

    // Attempt to acquire the lock. Returns true on success.
    // If another live process holds it, returns false immediately.
    bool acquire() {
        if (held_) return true;
        holder_pid_ = 0;

        if (std::filesystem::exists(path_)) {
            auto holder_pid = read_pid();
            if (holder_pid > 0 && is_process_alive(holder_pid)) {
                holder_pid_ = holder_pid;
                return false;  // Live process holds lock
            }
            // Stale lock — break it
            std::filesystem::remove(path_);
            stale_broken_ = true;
        }

        // Write our PID
        std::ofstream f(path_);
        if (!f) return false;
#ifdef _WIN32
        f << GetCurrentProcessId();
#else
        f << getpid();
#endif
        f.close();
        held_ = true;
        return true;
    }

    bool acquire_blocking(
        std::chrono::milliseconds timeout,
        std::chrono::milliseconds initial_poll = std::chrono::milliseconds(250),
        std::chrono::milliseconds max_poll = std::chrono::milliseconds(2000),
        const std::function<void(int64_t, std::chrono::milliseconds)>& on_wait = {}) {
        if (acquire()) return true;
        if (timeout <= std::chrono::milliseconds::zero() || holder_pid_ <= 0) return false;

        if (initial_poll <= std::chrono::milliseconds::zero()) {
            initial_poll = std::chrono::milliseconds(250);
        }
        if (max_poll < initial_poll) max_poll = initial_poll;

        if (on_wait) on_wait(holder_pid_, timeout);

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto poll = initial_poll;
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            std::this_thread::sleep_for(std::min(poll, remaining));

            if (acquire()) return true;
            if (holder_pid_ <= 0) return false;

            poll = std::min(max_poll, poll * 2);
        }
    }

    void release() {
        if (held_) {
            std::filesystem::remove(path_);
            held_ = false;
        }
    }

    ~FileLock() { release(); }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    bool was_stale_broken() const { return stale_broken_; }
    int64_t holder_pid() const { return holder_pid_; }

private:
    std::filesystem::path path_;
    bool held_ = false;
    bool stale_broken_ = false;
    int64_t holder_pid_ = 0;

    int64_t read_pid() {
        std::ifstream f(path_);
        int64_t pid = 0;
        f >> pid;
        return pid;
    }

    static bool is_process_alive(int64_t pid) {
#ifdef _WIN32
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (!h) return false;
        DWORD code = 0;
        bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
        CloseHandle(h);
        return alive;
#else
        return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
    }
};

} // namespace codetopo
