#pragma once

#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

namespace codetopo {

inline void write_stderr_line(std::string_view text) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    std::string line(text);
    line.push_back('\n');
    std::cerr.write(line.data(), static_cast<std::streamsize>(line.size()));
    std::cerr.flush();
}

} // namespace codetopo
