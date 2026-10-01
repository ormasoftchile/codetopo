#pragma once

#include <chrono>
#include <string>
#include <thread>
#include <stdexcept>
#include <algorithm>
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace codetopo {

// Poll the transport, not a detached getline thread, so idle/EOF own no threads.
class StdioInput {
public:
    enum class Result { line, eof, idle };

    Result next(std::string& line, int idle_seconds) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(idle_seconds);
        while (true) {
            auto newline = pending_.find('\n');
            if (newline != std::string::npos) {
                line = pending_.substr(0, newline);
                pending_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return Result::line;
            }
            if (closed_) {
                if (pending_.empty()) return Result::eof;
                line.swap(pending_);
                return Result::line;
            }
            if (idle_seconds > 0 && std::chrono::steady_clock::now() >= deadline)
                return Result::idle;
            char buffer[4096];
#ifdef _WIN32
            auto input = GetStdHandle(STD_INPUT_HANDLE);
            DWORD available = sizeof(buffer);
            if (GetFileType(input) == FILE_TYPE_PIPE) {
                if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                    if (GetLastError() == ERROR_BROKEN_PIPE) { closed_ = true; continue; }
                    throw std::runtime_error("stdin pipe probe failed");
                }
                if (available == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    continue;
                }
            } else if (GetFileType(input) == FILE_TYPE_CHAR &&
                       WaitForSingleObject(input, 25) == WAIT_TIMEOUT) {
                continue;
            }
            DWORD read = 0;
            if (!ReadFile(input, buffer, (std::min)(available, DWORD(sizeof(buffer))), &read, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) { closed_ = true; continue; }
                throw std::runtime_error("stdin read failed");
            }
#else
            pollfd fd{STDIN_FILENO, POLLIN, 0};
            int rc = poll(&fd, 1, 25);
            if (rc < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("stdin poll failed");
            }
            if (rc == 0) continue;
            auto read = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (read < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("stdin read failed");
            }
#endif
            if (read == 0) closed_ = true;
            else pending_.append(buffer, static_cast<size_t>(read));
        }
    }

private:
    std::string pending_;
    bool closed_ = false;
};

} // namespace codetopo
