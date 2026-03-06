#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace codetopo {

// T014: PID-based lock file with stale-lock detection and fail-immediately semantics.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path) : path_(path) {}

    // Attempt to acquire the lock. Returns true on success.
    // If another live process holds it, returns false immediately.
    bool acquire() {
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
