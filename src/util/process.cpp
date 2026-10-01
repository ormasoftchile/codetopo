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
    const auto started = std::chrono::steady_clock::now();
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

    STARTUPINFOEXA si = {};
    si.StartupInfo.cb = sizeof(STARTUPINFOA);
    HANDLE child_input = nullptr, input_writer = nullptr, child_log = nullptr;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    bool redirect = CreatePipe(&child_input, &input_writer, &security, 0) &&
        DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
            GetCurrentProcess(), &child_log, 0, TRUE, DUPLICATE_SAME_ACCESS);
    if (input_writer) CloseHandle(input_writer); // Child sees EOF, never MCP input.
    SIZE_T attribute_size = 0;
    std::vector<unsigned char> attribute_storage;
    bool attributes_initialized = false;
    HANDLE inherited[] = {child_input, child_log};
    if (redirect) {
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        attribute_storage.resize(attribute_size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        attributes_initialized = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attribute_size);
        redirect = attributes_initialized && UpdateProcThreadAttribute(si.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
        if (redirect) {
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = child_input;
            si.StartupInfo.hStdOutput = child_log;
            si.StartupInfo.hStdError = child_log;
        }
    }
    // Inherit only EOF input and stderr diagnostics, never MCP stdout or other handles.
    PROCESS_INFORMATION pi = {};
    bool spawned = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
        redirect, CREATE_SUSPENDED | (redirect ? EXTENDED_STARTUPINFO_PRESENT : 0),
        nullptr, nullptr, &si.StartupInfo, &pi);
    DWORD spawn_error = spawned ? 0 : GetLastError();
    if (attributes_initialized) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (child_input) CloseHandle(child_input);
    if (child_log) CloseHandle(child_log);
    if (!spawned) {
        std::cerr << "ERROR: Failed to spawn child process (error " << spawn_error << ")\n";
        if (hJob) CloseHandle(hJob);
        return 1;
    }

    // Assign to job object before resuming
    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    std::cerr << "[child] started pid=" << pi.dwProcessId << "\n";
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Wait for child
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    if (hJob) CloseHandle(hJob);
    std::cerr << "[child] exited pid=" << pi.dwProcessId << " exit=" << static_cast<int>(exit_code)
              << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started).count() << "\n";

    return static_cast<int>(exit_code);

#else
    // POSIX: fork + exec
    pid_t pid;
    std::vector<const char*> argv;
    argv.push_back(exe.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
    int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr,
                         const_cast<char* const*>(argv.data()), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        std::cerr << "ERROR: posix_spawn failed (rc=" << rc << ")\n";
        return 1;
    }

    int status = 0;
    std::cerr << "[child] started pid=" << pid << "\n";
    waitpid(pid, &status, 0);
    std::cerr << "[child] exited pid=" << pid << " wait_status=" << status
              << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started).count() << "\n";

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

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
    int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr,
                         const_cast<char* const*>(argv.data()), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        std::cerr << "ERROR: posix_spawn failed (rc=" << rc << ")\n";
        return 1;
    }

    // Track the last known mtime of the progress file.
    // We only reset the stall clock when the mtime actually CHANGES.
    std::filesystem::file_time_type last_known_mtime = {};
    auto start_time = std::chrono::steady_clock::now();
    std::cerr << "[child] started pid=" << pid << "\n";
    auto last_progress_seen = start_time;
    const auto stall_limit = std::chrono::seconds(stall_timeout_s);

    while (true) {
        // Check if child exited (non-blocking)
        int status = 0;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            std::cerr << "[child] exited pid=" << pid << " wait_status=" << status
                      << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start_time).count() << "\n";
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
