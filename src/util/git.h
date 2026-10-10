#pragma once

#include <string>
#include <cstdio>
#include <array>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace codetopo {

// Run a git command and return trimmed stdout. Empty string on failure.
// CRITICAL for MCP mode: child must NOT inherit parent's stdin (the JSON-RPC pipe)
// or stdout (the JSON-RPC output). On Windows we use CreateProcess with
// PROC_THREAD_ATTRIBUTE_LIST so only the explicitly listed handles are inherited.
inline std::string git_command(const std::string& repo_root, const std::string& args) {
    std::string cmd = "git -C \"" + repo_root + "\" " + args;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Pipe for child stdout
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return "";
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // NUL for child stdin+stderr (so child never touches parent's MCP pipe)
    HANDLE nul_in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE nul_err = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, 0, nullptr);

    // Build attribute list so ONLY these 3 handles are inherited (not parent stdin/stdout)
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    auto attr_buf = std::make_unique<char[]>(attr_size);
    auto attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.get());
    InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size);

    HANDLE inherit_handles[] = { nul_in, write_end, nul_err };
    UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                              inherit_handles, sizeof(inherit_handles), nullptr, nullptr);

    STARTUPINFOEXA si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.hStdInput  = nul_in;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError  = nul_err;
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.lpAttributeList        = attr_list;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, const_cast<char*>(cmd.c_str()),
                              nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                              nullptr, nullptr,
                              reinterpret_cast<LPSTARTUPINFOA>(&si), &pi);

    CloseHandle(write_end);
    CloseHandle(nul_in);
    CloseHandle(nul_err);
    DeleteProcThreadAttributeList(attr_list);

    if (!ok) {
        CloseHandle(read_end);
        return "";
    }

    std::string result;
    char buf[256];
    DWORD bytes_read;
    while (ReadFile(read_end, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        result.append(buf, bytes_read);
    }
    CloseHandle(read_end);

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    cmd += " < /dev/null 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    std::array<char, 256> buf;
    while (fgets(buf.data(), buf.size(), pipe)) {
        result += buf.data();
    }
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

struct GitWorktreeInfo {
    bool is_git_repo = false;
    bool is_worktree = false;
    std::filesystem::path worktree_root;
    std::filesystem::path primary_repo_root;
    std::filesystem::path common_git_dir;
    std::filesystem::path git_dir;
    std::filesystem::path head_file_path;
};

// Resolves Git worktree topology. Fast path checks filesystem directly without
// spawning git child processes (critical for high-frequency staleness checks).
inline GitWorktreeInfo resolve_worktree_info(const std::filesystem::path& repo_root) {
    namespace fs = std::filesystem;
    GitWorktreeInfo info;
    std::error_code ec;

    if (repo_root.empty() || !fs::exists(repo_root, ec) || ec) {
        return info;
    }

    fs::path norm_root = fs::canonical(repo_root, ec);
    if (ec) norm_root = repo_root.lexically_normal();

    fs::path git_path = norm_root / ".git";
    if (fs::is_directory(git_path, ec)) {
        info.is_git_repo = true;
        info.is_worktree = false;
        info.worktree_root = norm_root;
        info.primary_repo_root = norm_root;
        info.common_git_dir = git_path;
        info.git_dir = git_path;
        fs::path head = git_path / "HEAD";
        if (fs::exists(head, ec)) {
            info.head_file_path = head;
        }
        return info;
    }

    if (fs::is_regular_file(git_path, ec)) {
        std::ifstream fin(git_path);
        if (fin) {
            std::string line;
            if (std::getline(fin, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                         line.back() == ' ' || line.back() == '\t')) {
                    line.pop_back();
                }
                constexpr std::string_view kPrefix = "gitdir:";
                if (line.rfind(kPrefix, 0) == 0) {
                    std::string target_str = line.substr(kPrefix.size());
                    while (!target_str.empty() && (target_str.front() == ' ' || target_str.front() == '\t')) {
                        target_str.erase(0, 1);
                    }
                    fs::path target_path(target_str);
                    if (target_path.is_relative()) {
                        target_path = (norm_root / target_path).lexically_normal();
                    }
                    fs::path canon_target = fs::canonical(target_path, ec);
                    if (!ec) target_path = canon_target;

                    info.is_git_repo = true;
                    info.is_worktree = true;
                    info.worktree_root = norm_root;
                    info.git_dir = target_path;

                    fs::path head = target_path / "HEAD";
                    if (fs::exists(head, ec)) {
                        info.head_file_path = head;
                    }

                    fs::path commondir_file = target_path / "commondir";
                    if (fs::exists(commondir_file, ec)) {
                        std::ifstream cfin(commondir_file);
                        std::string cline;
                        if (std::getline(cfin, cline)) {
                            while (!cline.empty() && (cline.back() == '\r' || cline.back() == '\n' ||
                                                     cline.back() == ' ' || cline.back() == '\t')) {
                                cline.pop_back();
                            }
                            while (!cline.empty() && (cline.front() == ' ' || cline.front() == '\t')) {
                                cline.erase(0, 1);
                            }
                            fs::path cdir_path(cline);
                            if (cdir_path.is_relative()) {
                                cdir_path = (target_path / cdir_path).lexically_normal();
                            }
                            fs::path canon_cdir = fs::canonical(cdir_path, ec);
                            if (!ec) cdir_path = canon_cdir;

                            info.common_git_dir = cdir_path;
                            info.primary_repo_root = cdir_path.parent_path();
                        }
                    }
                    return info;
                }
            }
        }
    }

    // Fallback: spawn git rev-parse if .git wasn't directly in repo_root (e.g. bare repo or custom setup)
    std::string root_str = norm_root.string();
    std::string toplevel_out = git_command(root_str, "rev-parse --show-toplevel");
    if (!toplevel_out.empty()) {
        fs::path top(toplevel_out);
        fs::path canon_top = fs::canonical(top, ec);
        if (!ec) top = canon_top;
        if (!fs::equivalent(top, norm_root, ec)) {
            // norm_root is an arbitrary subdirectory inside an enclosing repository, not a repo root
            return info;
        }
    } else {
        return info;
    }

    std::string git_dir_out = git_command(root_str, "rev-parse --git-dir");
    if (!git_dir_out.empty()) {
        fs::path gd(git_dir_out);
        if (gd.is_relative()) gd = (norm_root / gd).lexically_normal();
        fs::path canon_gd = fs::canonical(gd, ec);
        if (!ec) gd = canon_gd;

        std::string common_dir_out = git_command(root_str, "rev-parse --git-common-dir");
        fs::path cd = common_dir_out.empty() ? gd : fs::path(common_dir_out);
        if (cd.is_relative()) cd = (norm_root / cd).lexically_normal();
        fs::path canon_cd = fs::canonical(cd, ec);
        if (!ec) cd = canon_cd;

        info.is_git_repo = true;
        info.git_dir = gd;
        info.common_git_dir = cd;
        info.worktree_root = norm_root;
        info.primary_repo_root = cd.parent_path();

        std::error_code eq_ec;
        info.is_worktree = !fs::equivalent(gd, cd, eq_ec);

        fs::path head = gd / "HEAD";
        if (fs::exists(head, ec)) info.head_file_path = head;
        return info;
    }

    return info;
}

inline std::filesystem::path get_git_head_path(const std::filesystem::path& repo_root) {
    auto info = resolve_worktree_info(repo_root);
    return info.head_file_path;
}

} // namespace codetopo
