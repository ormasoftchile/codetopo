#include "util/process.h"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
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
    // Open NUL for child stdin — child process doesn't need stdin, and inheriting
    // parent's stdin breaks MCP server mode (child would read JSON-RPC messages).
    SECURITY_ATTRIBUTES nul_sa = {};
    nul_sa.nLength = sizeof(nul_sa);
    nul_sa.bInheritHandle = TRUE;
    HANDLE nul_stdin = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &nul_sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul_stdin;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                        TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        std::cerr << "ERROR: Failed to spawn child process (error " << GetLastError() << ")\n";
        CloseHandle(nul_stdin);
        if (hJob) CloseHandle(hJob);
        return 1;
    }
    CloseHandle(nul_stdin);

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
    // Open NUL for child stdin (same rationale as spawn_and_wait)
    SECURITY_ATTRIBUTES nul_sa2 = {};
    nul_sa2.nLength = sizeof(nul_sa2);
    nul_sa2.bInheritHandle = TRUE;
    HANDLE nul_stdin2 = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &nul_sa2, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul_stdin2;
    si.hStdOutput = write_end;                       // child stdout → pipe
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);   // stderr inherited
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                        TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        std::cerr << "ERROR: Failed to spawn child process (error "
                  << GetLastError() << ")\n";
        CloseHandle(read_end);
        CloseHandle(write_end);
        CloseHandle(nul_stdin2);
        if (hJob) CloseHandle(hJob);
        return 1;
    }
    CloseHandle(nul_stdin2);

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
