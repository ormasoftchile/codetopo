#pragma once

// T093-T095: Filesystem watcher with debounce and event-to-action mapping.
// Platform-specific: Windows uses ReadDirectoryChangesW.

#include <filesystem>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace codetopo {
namespace fs = std::filesystem;

enum class FileEvent { Created, Modified, Deleted, BranchSwitch };

struct WatchEvent {
    FileEvent type;
    fs::path path;
};

// Detect .git/HEAD or .git/refs/ changes (branch switch indicators).
// The watcher is not git-aware — it just classifies these paths specially.
// The callback consumer interprets the meaning.
inline bool is_git_head_change(const fs::path& p) {
    auto s = p.generic_string();
    return s.find(".git/HEAD") != std::string::npos
        || s.find(".git/refs/") != std::string::npos;
}

// Directories the watcher must never descend into or fire events for. Mirrors
// Scanner::excluded_dirs() and additionally ignores `.codetopo` — the index DB
// lives there, and re-indexing writes index.sqlite-wal/-shm, which would
// otherwise register as changes and re-trigger the watcher in an endless
// no-op loop. `.git` is pruned here too; branch switches are detected via an
// explicit .git/HEAD stat instead.
inline bool is_ignored_watch_dir(const std::string& name) {
    static const std::unordered_set<std::string> dirs = {
        ".git", ".codetopo", "build", "out", "node_modules",
        "vcpkg", "vcpkg_installed", "vendor", "third_party",
        "__pycache__", ".venv", "target"
    };
    if (dirs.count(name)) return true;
    return name.size() >= 6 && name.compare(0, 6, "bazel-") == 0;
}

// True if any path component between root and full is an ignored watch dir.
inline bool watch_path_ignored(const fs::path& root, const fs::path& full) {
    std::error_code ec;
    auto rel = fs::relative(full, root, ec);
    if (ec) return false;
    for (const auto& part : rel) {
        if (is_ignored_watch_dir(part.string())) return true;
    }
    return false;
}

using WatchCallback = std::function<void(const std::vector<WatchEvent>&)>;

class Watcher {
public:
    Watcher(const fs::path& root, WatchCallback callback,
            std::chrono::milliseconds debounce = std::chrono::milliseconds(1000))
        : root_(fs::canonical(root))
        , callback_(std::move(callback))
        , debounce_(debounce)
        , running_(false) {}

    ~Watcher() { stop(); }

    void start() {
        running_ = true;
        watch_thread_ = std::thread([this]() { run(); });
    }

    void stop() {
        running_ = false;
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }

private:
    fs::path root_;
    WatchCallback callback_;
    [[maybe_unused]] std::chrono::milliseconds debounce_;
    std::atomic<bool> running_;
    std::thread watch_thread_;

    void run() {
#ifdef _WIN32
        run_windows();
#else
        run_polling();  // Fallback: poll for changes
#endif
    }

#ifdef _WIN32
    void run_windows() {
        HANDLE dir_handle = CreateFileW(
            root_.wstring().c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (dir_handle == INVALID_HANDLE_VALUE) return;

        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

        alignas(DWORD) char buffer[64 * 1024];

        while (running_) {
            DWORD bytes_returned = 0;
            ResetEvent(overlapped.hEvent);

            BOOL success = ReadDirectoryChangesW(
                dir_handle, buffer, sizeof(buffer), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION,
                nullptr, &overlapped, nullptr
            );

            if (!success) break;

            DWORD wait_result = WaitForSingleObject(overlapped.hEvent, 1000);
            if (wait_result == WAIT_TIMEOUT) continue;
            if (wait_result != WAIT_OBJECT_0) break;

            if (!GetOverlappedResult(dir_handle, &overlapped, &bytes_returned, FALSE))
                break;

            if (bytes_returned == 0) continue;

            // Parse the notification buffer
            std::vector<WatchEvent> events;
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);

            while (true) {
                std::wstring filename(info->FileName, info->FileNameLength / sizeof(WCHAR));
                fs::path full_path = root_ / filename;

                FileEvent type;
                switch (info->Action) {
                    case FILE_ACTION_ADDED: type = FileEvent::Created; break;
                    case FILE_ACTION_REMOVED: type = FileEvent::Deleted; break;
                    case FILE_ACTION_MODIFIED: type = FileEvent::Modified; break;
                    case FILE_ACTION_RENAMED_NEW_NAME: type = FileEvent::Created; break;
                    case FILE_ACTION_RENAMED_OLD_NAME: type = FileEvent::Deleted; break;
                    default: type = FileEvent::Modified; break;
                }

                events.push_back({type, full_path});

                if (info->NextEntryOffset == 0) break;
                info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                    reinterpret_cast<char*>(info) + info->NextEntryOffset);
            }

            // Classify .git/HEAD and .git/refs/ changes as branch switches, and
            // drop events under ignored directories (.codetopo, .git, build, …)
            // so the watcher never re-triggers on its own index DB writes.
            std::vector<WatchEvent> filtered;
            filtered.reserve(events.size());
            for (auto& ev : events) {
                if (is_git_head_change(ev.path)) {
                    ev.type = FileEvent::BranchSwitch;
                    filtered.push_back(ev);
                } else if (!watch_path_ignored(root_, ev.path)) {
                    filtered.push_back(ev);
                }
            }
            events.swap(filtered);

            // Debounce: wait for more events before dispatching
            std::this_thread::sleep_for(debounce_);

            if (!events.empty() && running_.load()) {
                callback_(events);
            }
        }

        CloseHandle(overlapped.hEvent);
        CloseHandle(dir_handle);
    }
#endif

    // Enumerate regular files under root_, pruning ignored directories
    // (.codetopo, .git, build, node_modules, …). Returns path → mtime.
    std::unordered_map<std::string, fs::file_time_type> collect_files() {
        std::unordered_map<std::string, fs::file_time_type> out;
        std::error_code ec;
        fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec);
        if (ec) return out;
        fs::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            const auto& entry = *it;
            std::error_code fec;
            if (entry.is_directory(fec)) {
                if (is_ignored_watch_dir(entry.path().filename().string()))
                    it.disable_recursion_pending();  // don't descend
                continue;
            }
            if (entry.is_regular_file(fec)) {
                auto mtime = entry.last_write_time(fec);
                if (!fec) out[entry.path().string()] = mtime;
            }
        }
        return out;
    }

    // Polling fallback for non-Windows platforms
    void run_polling() {
        auto known_files = collect_files();

        // Track .git/HEAD separately for branch-switch detection (the .git dir
        // itself is pruned from collect_files()).
        auto git_head = root_ / ".git" / "HEAD";
        fs::file_time_type git_head_mtime{};
        {
            std::error_code ec;
            if (fs::exists(git_head, ec)) git_head_mtime = fs::last_write_time(git_head, ec);
        }

        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));

            std::vector<WatchEvent> events;

            // Branch-switch detection via explicit .git/HEAD stat.
            {
                std::error_code ec;
                if (fs::exists(git_head, ec)) {
                    auto mtime = fs::last_write_time(git_head, ec);
                    if (!ec && mtime != git_head_mtime) {
                        events.push_back({FileEvent::BranchSwitch, git_head});
                        git_head_mtime = mtime;
                    }
                }
            }

            auto current = collect_files();

            // Created / Modified
            for (const auto& [path_str, mtime] : current) {
                auto it = known_files.find(path_str);
                if (it == known_files.end()) {
                    events.push_back({FileEvent::Created, path_str});
                } else if (it->second != mtime) {
                    events.push_back({FileEvent::Modified, path_str});
                }
            }
            // Deleted
            for (const auto& [path_str, mtime] : known_files) {
                (void)mtime;
                if (!current.count(path_str))
                    events.push_back({FileEvent::Deleted, path_str});
            }

            known_files = std::move(current);

            if (!events.empty() && running_.load()) {
                callback_(events);
            }
        }
    }
};

} // namespace codetopo
