#pragma once

#include <string>
#include <cstdio>
#include <array>
#include <memory>

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

} // namespace codetopo
