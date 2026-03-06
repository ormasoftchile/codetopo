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

#ifdef _WIN32
#include <windows.h>
#endif

namespace codetopo {
namespace fs = std::filesystem;

enum class FileEvent { Created, Modified, Deleted };

struct WatchEvent {
    FileEvent type;
    fs::path path;
};

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
    std::chrono::milliseconds debounce_;
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

    // Polling fallback for non-Windows platforms
    void run_polling() {
        std::unordered_map<std::string, fs::file_time_type> known_files;

        // Initial scan
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(root_, ec)) {
            if (entry.is_regular_file(ec)) {
                known_files[entry.path().string()] = entry.last_write_time(ec);
            }
        }

        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));

            std::vector<WatchEvent> events;
            std::unordered_set<std::string> current_files;

            for (auto& entry : fs::recursive_directory_iterator(root_, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                auto path_str = entry.path().string();
                current_files.insert(path_str);

                auto mtime = entry.last_write_time(ec);
                auto it = known_files.find(path_str);
                if (it == known_files.end()) {
                    events.push_back({FileEvent::Created, entry.path()});
                    known_files[path_str] = mtime;
                } else if (it->second != mtime) {
                    events.push_back({FileEvent::Modified, entry.path()});
                    it->second = mtime;
                }
            }

            // Detect deletions
            for (auto it = known_files.begin(); it != known_files.end();) {
                if (!current_files.count(it->first)) {
                    events.push_back({FileEvent::Deleted, it->first});
                    it = known_files.erase(it);
                } else {
                    ++it;
                }
            }

            if (!events.empty() && running_.load()) {
                callback_(events);
            }
        }
    }
};

} // namespace codetopo
