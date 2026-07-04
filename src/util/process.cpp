#include "util/process.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
#include <signal.h>
extern char** environ;
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace codetopo {

std::string get_self_executable_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";
    // Convert wide to narrow
    int sz = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string result(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), result.data(), sz, nullptr, nullptr);
    return result;
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::string(buf);
    return "";
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    buf[len] = '\0';
    return std::string(buf);
#endif
}

int spawn_and_wait(const std::string& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
    // Build command line
    std::string cmdline = "\"" + exe + "\"";
    for (const auto& arg : args) {
        cmdline += " ";
        // Quote arguments that contain spaces
        if (arg.find(' ') != std::string::npos) {
            cmdline += "\"" + arg + "\"";
        } else {
            cmdline += arg;
        }
    }

    // Create Job Object so child is killed if supervisor dies
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);

    // Spawn WITHOUT inheriting handles (bInheritHandles = FALSE). Rationale:
    //  * MCP correctness: when the parent is the MCP server, its stdin carries
    //    JSON-RPC and the child must never consume it. A non-inheriting spawn
    //    guarantees that without handing the child an explicit NUL stdin.
    //  * Robustness: STARTF_USESTDHANDLES requires every std handle to be valid
    //    AND inheritable, else CreateProcess fails with ERROR_INVALID_PARAMETER
    //    (87). That happens whenever codetopo itself runs with redirected/absent
    //    streams (under ctest, `>NUL`, or as a service) — exactly how the
    //    supervised reindex worker is launched. The supervisor only needs the
    //    child's exit code, so inheriting std handles buys nothing.
    // The child is still bound to the supervisor's lifetime via the kill-on-close
    // job object created above.
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                        FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        std::cerr << "ERROR: Failed to spawn child process (error " << GetLastError() << ")\n";
        if (hJob) CloseHandle(hJob);
        return 1;
    }

    // Assign to job object before resuming
    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Wait for child
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    if (hJob) CloseHandle(hJob);

    return static_cast<int>(exit_code);

#else
    // POSIX: fork + exec
    pid_t pid;
    std::vector<const char*> argv;
    argv.push_back(exe.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    int rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr,
                         const_cast<char* const*>(argv.data()), environ);
    if (rc != 0) {
        std::cerr << "ERROR: posix_spawn failed (rc=" << rc << ")\n";
        return 1;
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);  // Convention: 128+signal
    return 1;
#endif
}

int spawn_and_wait_with_stall_timeout(
    const std::string& exe,
    const std::vector<std::string>& args,
    const std::string& progress_path,
    int stall_timeout_s)
{
#ifdef _WIN32
    // Windows: use spawn_and_wait (no stall detection implemented yet)
    (void)progress_path; (void)stall_timeout_s;
    return spawn_and_wait(exe, args);
#else
    pid_t pid;
    std::vector<const char*> argv;
    argv.push_back(exe.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    int rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr,
                         const_cast<char* const*>(argv.data()), environ);
    if (rc != 0) {
        std::cerr << "ERROR: posix_spawn failed (rc=" << rc << ")\n";
        return 1;
    }

    // Track the last known mtime of the progress file.
    // We only reset the stall clock when the mtime actually CHANGES.
    std::filesystem::file_time_type last_known_mtime = {};
    auto start_time = std::chrono::steady_clock::now();
    auto last_progress_seen = start_time;
    const auto stall_limit = std::chrono::seconds(stall_timeout_s);

    while (true) {
        // Check if child exited (non-blocking)
        int status = 0;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 1;
        }

        // Poll frequently so we notice a fast child promptly. The stall check
        // below is time-based (compares elapsed against stall_limit), so a short
        // poll interval does not affect stall semantics — it only removes the
        // fixed multi-second latency a coarse poll added to every (even no-op)
        // supervised reindex. Critical for lean delta reindex on large repos.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Reset stall clock ONLY when the progress file's mtime changes —
        // i.e., when the child actually commits a new batch.
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(progress_path, ec);
        if (!ec && mtime != last_known_mtime) {
            last_known_mtime = mtime;
            last_progress_seen = std::chrono::steady_clock::now();
        }

        // Stall detection: if no new progress for stall_limit seconds AND
        // total elapsed also exceeds the limit (grace period for initial scan).
        auto now = std::chrono::steady_clock::now();
        if (now - start_time > stall_limit && now - last_progress_seen > stall_limit) {
            std::cerr << "SUPERVISOR: child stalled (no progress in "
                      << stall_timeout_s << "s) — killing PID " << pid << "\n";
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return 128 + SIGKILL;
        }
    }
#endif
}

int spawn_and_read_stdout(const std::string& exe,
                          const std::vector<std::string>& args,
                          const std::function<void(const std::string&)>& on_line) {
#ifdef _WIN32
    // Create pipe for child stdout
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        std::cerr << "ERROR: CreatePipe failed\n";
        return 1;
    }
    // Parent's read end must not be inherited
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // Build command line
    std::string cmdline = "\"" + exe + "\"";
    for (const auto& arg : args) {
        cmdline += " ";
        if (arg.find(' ') != std::string::npos)
            cmdline += "\"" + arg + "\"";
        else
            cmdline += arg;
    }

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    // Every handle passed via STARTF_USESTDHANDLES must be valid AND inheritable,
    // otherwise CreateProcess fails with ERROR_INVALID_PARAMETER (87). GetStdHandle()
    // can return NULL/non-inheritable handles when this process runs with redirected
    // streams (ctest, `>NUL`, a service). The caller only reads the child's stdout
    // (via the pipe), so route stdin and stderr to explicit inheritable NUL handles.
    SECURITY_ATTRIBUTES nul_sa2 = {};
    nul_sa2.nLength = sizeof(nul_sa2);
    nul_sa2.bInheritHandle = TRUE;
    HANDLE nul_stdin2 = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &nul_sa2, OPEN_EXISTING, 0, nullptr);
    HANDLE nul_stderr2 = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &nul_sa2, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul_stdin2;
    si.hStdOutput = write_end;                       // child stdout → pipe
    si.hStdError = nul_stderr2;                       // stderr → NUL (not read)
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                        TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        std::cerr << "ERROR: Failed to spawn child process (error "
                  << GetLastError() << ")\n";
        CloseHandle(read_end);
        CloseHandle(write_end);
        CloseHandle(nul_stdin2);
        if (nul_stderr2 != INVALID_HANDLE_VALUE) CloseHandle(nul_stderr2);
        if (hJob) CloseHandle(hJob);
        return 1;
    }
    CloseHandle(nul_stdin2);
    if (nul_stderr2 != INVALID_HANDLE_VALUE) CloseHandle(nul_stderr2);

    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(write_end);  // Close write end in parent so reads see EOF

    // Read child stdout line by line
    std::string buffer;
    char chunk[4096];
    DWORD bytes_read;
    while (ReadFile(read_end, chunk, sizeof(chunk), &bytes_read, nullptr)
           && bytes_read > 0) {
        buffer.append(chunk, bytes_read);
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) on_line(line);
            buffer.erase(0, pos + 1);
        }
    }
    if (!buffer.empty()) {
        if (buffer.back() == '\r') buffer.pop_back();
        if (!buffer.empty()) on_line(buffer);
    }
    CloseHandle(read_end);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    if (hJob) CloseHandle(hJob);
    return static_cast<int>(exit_code);

#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        std::cerr << "ERROR: pipe() failed\n";
        return 1;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    std::vector<const char*> argv;
    argv.push_back(exe.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    pid_t pid;
    int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr,
                         const_cast<char* const*>(argv.data()), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0) {
        std::cerr << "ERROR: posix_spawn failed (rc=" << rc << ")\n";
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }
    close(pipefd[1]);  // Close write end in parent

    FILE* f = fdopen(pipefd[0], "r");
    char line_buf[8192];
    while (fgets(line_buf, sizeof(line_buf), f)) {
        std::string line(line_buf);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) on_line(line);
    }
    fclose(f);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
#endif
}

} // namespace codetopo
