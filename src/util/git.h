#pragma once

#include <string>
#include <cstdio>
#include <array>

namespace codetopo {

// Run a git command and return trimmed stdout. Empty string on failure.
inline std::string git_command(const std::string& repo_root, const std::string& args) {
    std::string cmd = "git -C \"" + repo_root + "\" " + args;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    std::array<char, 256> buf;
    std::string result;
    while (fgets(buf.data(), buf.size(), pipe)) {
        result += buf.data();
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

inline std::string get_git_head(const std::string& repo_root) {
    return git_command(repo_root, "rev-parse HEAD");
}

inline std::string get_git_branch(const std::string& repo_root) {
    return git_command(repo_root, "rev-parse --abbrev-ref HEAD");
}

} // namespace codetopo
