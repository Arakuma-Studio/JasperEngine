// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Windows argument quoting (quoteWindowsArgument) and the drain of both pipes follow
// OpenEngine's VCSCommandExecutor (Engine/Modules/VCSIntegration/Source/VCSCommandExecutor.cpp,
// QuoteArgument and ReadAvailablePipes; MIT, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors; NOTICE.md). The timeout, the tree kill, stdin control, the output cap,
// close-on-exec descriptors and the absolute executable are ours (docs/specs/S29-version-control.md R2).

#include "vcs/ProcessExec.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace jasperengine::editor::vcs {

namespace {

    using Clock = std::chrono::steady_clock;

    std::atomic<uint64_t> mainThreadSpawns{0};
    std::atomic<uint64_t> otherThreadSpawns{0};

    // A SIGTERM's grace before the SIGKILL (R2)
    constexpr int kKillGraceMs = 2000;
    // After the child exits, how long a grandchild holding its pipes may keep them open
    constexpr int kExitDrainMs = 200;

    std::string trimmed(std::string_view text) {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ') begin++;
        while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') end--;
        return std::string(text.substr(begin, end - begin));
    }

    bool envNameEquals(std::string_view a, std::string_view b) {
#ifdef _WIN32
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            const char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
            const char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
            if (ca != cb) return false;
        }
        return true;
#else
        return a == b;
#endif
    }

    bool overridden(std::string_view name, const ProcessSpec& spec) {
        for (const auto& [key, value] : spec.env) {
            if (envNameEquals(key, name)) return true;
        }
        for (const std::string& key : spec.unsetEnv) {
            if (envNameEquals(key, name)) return true;
        }
        return false;
    }

    // The output kept: stdout and stderr together stay within the cap, their buffers too (a
    // string never grows past what the cap still allows it)
    struct OutputSink {
        size_t cap;
        std::mutex mutex;
        size_t kept = 0;
        bool truncated = false;

        void append(std::string& into, const char* data, size_t size) {
            std::lock_guard<std::mutex> lock(mutex);
            if (kept >= cap) {
                truncated = true;
                return;
            }
            const size_t take = std::min(size, cap - kept);
            if (into.size() + take > into.capacity()) {
                const size_t room = into.size() + (cap - kept);
                into.reserve(std::min(room, std::max(into.capacity() * 2, into.size() + take)));
            }
            into.append(data, take);
            kept += take;
            if (take < size) truncated = true;
        }
    };

    void countSpawn() {
        const bool main = jobs::isMainThread();
        // R24: the main thread never waits for a process
        assert(!main && "runProcess on the main thread: version control runs on its worker");
        (main ? mainThreadSpawns : otherThreadSpawns)++;
    }

}

std::string ProcessResult::message() const {
    if (spawnFailed) return spawnError.empty() ? std::string("the process could not start") : spawnError;
    std::string text = trimmed(err);
    if (text.empty()) text = trimmed(out);
    if (timedOut) return text.empty() ? std::string("timed out") : "timed out: " + text;
    if (cancelled) return text.empty() ? std::string("cancelled") : "cancelled: " + text;
    if (text.empty() && exitCode != 0) return "exit code " + std::to_string(exitCode);
    return text;
}

uint64_t spawnCount(bool onMainThread) {
    return (onMainThread ? mainThreadSpawns : otherThreadSpawns).load();
}

std::string quoteWindowsArgument(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) return std::string(arg);
    std::string quoted = "\"";
    size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
        } else {
            quoted.append(backslashes, '\\');
        }
        quoted += c;
        backslashes = 0;
    }
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

#ifndef _WIN32

namespace {

    bool makePipe(int fds[2]) {
#if defined(__linux__)
        return pipe2(fds, O_CLOEXEC) == 0;
#else
        if (pipe(fds) != 0) return false;
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        return true;
#endif
    }

    void closeFd(int& fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

    void setNonBlocking(int fd) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    char** currentEnvironment() {
#if defined(__APPLE__)
        return *_NSGetEnviron();
#else
        return environ;
#endif
    }

    std::vector<std::string> buildEnvironment(const ProcessSpec& spec) {
        std::vector<std::string> entries;
        for (char** entry = currentEnvironment(); entry && *entry; ++entry) {
            const std::string_view text(*entry);
            const size_t equals = text.find('=');
            if (equals == std::string_view::npos || overridden(text.substr(0, equals), spec)) continue;
            entries.emplace_back(text);
        }
        for (const auto& [key, value] : spec.env) entries.push_back(key + "=" + value);
        return entries;
    }

    // SIGPIPE of a write to a child that closed its stdin is this thread's to swallow: blocked
    // while the run lasts, and a pending one consumed before the mask comes back
    class SigpipeGuard {
    public:
        SigpipeGuard() {
            sigset_t pipeSet;
            sigemptyset(&pipeSet);
            sigaddset(&pipeSet, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &pipeSet, &previous);
        }
        ~SigpipeGuard() {
            sigset_t pending;
            sigemptyset(&pending);
            if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1 && sigismember(&previous, SIGPIPE) == 0) {
                sigset_t pipeSet;
                sigemptyset(&pipeSet);
                sigaddset(&pipeSet, SIGPIPE);
                int signal = 0;
                sigwait(&pipeSet, &signal);
            }
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        }
        SigpipeGuard(const SigpipeGuard&) = delete;
        SigpipeGuard& operator=(const SigpipeGuard&) = delete;

    private:
        sigset_t previous;
    };

}

ProcessResult runProcess(const ProcessSpec& spec) {
    ProcessResult result;
    if (spec.exe.empty() || !spec.exe.is_absolute()) {
        result.spawnFailed = true;
        result.spawnError = "not an absolute executable path: " + spec.exe.string();
        return result;
    }
    countSpawn();
    SigpipeGuard sigpipe;

    // everything the child gets is built before the spawn
    const std::string exe = spec.exe.string();
    std::vector<std::string> argStorage;
    argStorage.reserve(spec.args.size() + 1);
    argStorage.push_back(exe);
    for (const std::string& arg : spec.args) argStorage.push_back(arg);
    std::vector<char*> argv;
    for (std::string& arg : argStorage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> envStorage = buildEnvironment(spec);
    std::vector<char*> envp;
    for (std::string& entry : envStorage) envp.push_back(entry.data());
    envp.push_back(nullptr);

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    int inPipe[2] = {-1, -1};
    int devNull = -1;
    auto failSpawn = [&](const std::string& what) {
        closeFd(outPipe[0]); closeFd(outPipe[1]);
        closeFd(errPipe[0]); closeFd(errPipe[1]);
        closeFd(inPipe[0]); closeFd(inPipe[1]);
        closeFd(devNull);
        result.spawnFailed = true;
        result.spawnError = what;
        return result;
    };
    if (!makePipe(outPipe) || !makePipe(errPipe)) return failSpawn(std::string("pipe: ") + std::strerror(errno));
    const bool feedStdin = !spec.stdinData.empty();
    if (feedStdin) {
        if (!makePipe(inPipe)) return failSpawn(std::string("pipe: ") + std::strerror(errno));
    } else {
        devNull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devNull < 0) return failSpawn(std::string("/dev/null: ") + std::strerror(errno));
    }
    const int stdinFd = feedStdin ? inPipe[0] : devNull;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdinFd, 0);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);
    const std::string cwd = spec.cwd.string();
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(POSIX_SPAWN_SETSID)
    // a session of its own: the group the timeout kills, and no controlling terminal to prompt on
    flags |= POSIX_SPAWN_SETSID;
#else
    flags |= POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setpgroup(&attr, 0);
#endif
#if defined(__APPLE__)
    // only the three descriptors above reach the child
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);
    sigset_t noSignals;
    sigemptyset(&noSignals);
    posix_spawnattr_setsigmask(&attr, &noSignals);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (int signal : {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD, SIGALRM, SIGUSR1, SIGUSR2}) sigaddset(&defaults, signal);
    posix_spawnattr_setsigdefault(&attr, &defaults);

    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    closeFd(outPipe[1]);
    closeFd(errPipe[1]);
    closeFd(inPipe[0]);
    closeFd(devNull);
    if (spawned != 0) {
        return failSpawn(exe + ": " + std::strerror(spawned));
    }

    OutputSink sink{spec.maxOutputBytes};
    setNonBlocking(outPipe[0]);
    setNonBlocking(errPipe[0]);
    if (feedStdin) setNonBlocking(inPipe[1]);
    size_t stdinWritten = 0;

    const Clock::time_point start = Clock::now();
    bool exited = false;
    int status = 0;
    bool stopping = false;          // SIGTERM sent
    bool killed = false;            // SIGKILL sent
    Clock::time_point termAt;
    Clock::time_point exitAt;
    char buffer[64 * 1024];

    auto stop = [&](bool timedOut) {
        if (stopping) return;
        stopping = true;
        if (timedOut) result.timedOut = true;
        else result.cancelled = true;
        termAt = Clock::now();
        kill(-pid, SIGTERM);
        kill(pid, SIGTERM);
    };

    while (true) {
        const bool outOpen = outPipe[0] >= 0;
        const bool errOpen = errPipe[0] >= 0;
        const bool inOpen = feedStdin && inPipe[1] >= 0;
        if (exited && !outOpen && !errOpen) break;

        const Clock::time_point now = Clock::now();
        if (!stopping) {
            if (spec.timeoutMs > 0 && now - start >= std::chrono::milliseconds(spec.timeoutMs)) stop(true);
            else if (spec.cancel && spec.cancel->isCancelled()) stop(false);
        }
        if (stopping && !killed && now - termAt >= std::chrono::milliseconds(kKillGraceMs)) {
            killed = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }
        // a grandchild holding the pipes does not keep this run alive past its child
        if (exited && now - exitAt >= std::chrono::milliseconds(kExitDrainMs)) break;

        pollfd fds[3];
        nfds_t count = 0;
        int outIndex = -1, errIndex = -1, inIndex = -1;
        if (outOpen) { outIndex = static_cast<int>(count); fds[count++] = {outPipe[0], POLLIN, 0}; }
        if (errOpen) { errIndex = static_cast<int>(count); fds[count++] = {errPipe[0], POLLIN, 0}; }
        if (inOpen) { inIndex = static_cast<int>(count); fds[count++] = {inPipe[1], POLLOUT, 0}; }
        const int ready = count > 0 ? poll(fds, count, 20) : 0;
        if (count == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (ready > 0) {
            auto drain = [&](int index, int& fd, std::string& into) {
                if (index < 0 || !(fds[index].revents & (POLLIN | POLLHUP | POLLERR))) return;
                while (true) {
                    const ssize_t n = read(fd, buffer, sizeof(buffer));
                    if (n > 0) {
                        sink.append(into, buffer, static_cast<size_t>(n));
                        continue;
                    }
                    if (n < 0 && errno == EINTR) continue;
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    closeFd(fd);   // end of file, or an error
                    break;
                }
            };
            drain(outIndex, outPipe[0], result.out);
            drain(errIndex, errPipe[0], result.err);
            if (inIndex >= 0) {
                if (fds[inIndex].revents & (POLLERR | POLLHUP)) {
                    closeFd(inPipe[1]);
                } else if (fds[inIndex].revents & POLLOUT) {
                    const size_t left = spec.stdinData.size() - stdinWritten;
                    const ssize_t n = write(inPipe[1], spec.stdinData.data() + stdinWritten, std::min<size_t>(left, 64 * 1024));
                    if (n > 0) stdinWritten += static_cast<size_t>(n);
                    else if (n < 0 && errno != EAGAIN && errno != EINTR) closeFd(inPipe[1]);
                    if (stdinWritten >= spec.stdinData.size()) closeFd(inPipe[1]);
                }
            }
        }
        if (!exited) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                exited = true;
                exitAt = Clock::now();
            }
        }
    }
    closeFd(outPipe[0]);
    closeFd(errPipe[0]);
    closeFd(inPipe[1]);
    if (!exited) {
        // the pipes closed before the child ended: wait for it, still under the timeout
        while (true) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno != EINTR)) break;
            const Clock::time_point now = Clock::now();
            if (!stopping) {
                if (spec.timeoutMs > 0 && now - start >= std::chrono::milliseconds(spec.timeoutMs)) stop(true);
                else if (spec.cancel && spec.cancel->isCancelled()) stop(false);
            }
            if (stopping && !killed && now - termAt >= std::chrono::milliseconds(kKillGraceMs)) {
                killed = true;
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    // what the group left running after a timeout goes with it
    if (stopping) kill(-pid, SIGKILL);

    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    result.truncated = sink.truncated;
    return result;
}

#else // _WIN32

namespace {

    std::wstring widen(std::string_view utf8) {
        if (utf8.empty()) return std::wstring();
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
        return wide;
    }

    std::string narrow(std::wstring_view wide) {
        if (wide.empty()) return std::string();
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
        return text;
    }

    std::string lastErrorText(const char* what) {
        const DWORD code = GetLastError();
        wchar_t* message = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
        std::string text = std::string(what) + ": " + (message ? trimmed(narrow(message)) : std::to_string(code));
        if (message) LocalFree(message);
        return text;
    }

    // "K=V\0K=V\0\0", sorted by name without case as CreateProcess documents
    std::wstring buildEnvironmentBlock(const ProcessSpec& spec) {
        std::vector<std::wstring> entries;
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
                const std::wstring text(entry);
                // "=C:=C:\dir": the per-drive current directories, kept
                const size_t equals = text.find(L'=', 1);
                if (equals == std::wstring::npos) continue;
                if (text[0] != L'=' && overridden(narrow(text.substr(0, equals)), spec)) continue;
                entries.push_back(text);
            }
            FreeEnvironmentStringsW(block);
        }
        for (const auto& [key, value] : spec.env) entries.push_back(widen(key) + L"=" + widen(value));
        std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });
        std::wstring out;
        for (const std::wstring& entry : entries) {
            out += entry;
            out.push_back(L'\0');
        }
        out.push_back(L'\0');
        return out;
    }

    void closeHandle(HANDLE& handle) {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = nullptr;
    }

}

ProcessResult runProcess(const ProcessSpec& spec) {
    ProcessResult result;
    if (spec.exe.empty() || !spec.exe.is_absolute()) {
        result.spawnFailed = true;
        result.spawnError = "not an absolute executable path: " + spec.exe.string();
        return result;
    }
    countSpawn();

    // everything the child gets is built before the spawn
    std::wstring commandLine = L"\"" + spec.exe.wstring() + L"\"";
    for (const std::string& arg : spec.args) commandLine += L" " + widen(quoteWindowsArgument(arg));
    std::wstring environment = buildEnvironmentBlock(spec);

    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    HANDLE inRead = nullptr, inWrite = nullptr;
    HANDLE job = nullptr;
    auto failSpawn = [&](const std::string& what) {
        closeHandle(outRead); closeHandle(outWrite);
        closeHandle(errRead); closeHandle(errWrite);
        closeHandle(inRead); closeHandle(inWrite);
        closeHandle(job);
        result.spawnFailed = true;
        result.spawnError = what;
        return result;
    };
    if (!CreatePipe(&outRead, &outWrite, &inherit, 0) || !CreatePipe(&errRead, &errWrite, &inherit, 0)) {
        return failSpawn(lastErrorText("CreatePipe"));
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    const bool feedStdin = !spec.stdinData.empty();
    if (feedStdin) {
        if (!CreatePipe(&inRead, &inWrite, &inherit, 0)) return failSpawn(lastErrorText("CreatePipe"));
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    } else {
        inRead = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        if (inRead == INVALID_HANDLE_VALUE) {
            inRead = nullptr;
            return failSpawn(lastErrorText("NUL"));
        }
    }

    // only these three handles reach the child, whatever else is inheritable in the editor
    HANDLE inherited[3] = {inRead, outWrite, errWrite};
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        return failSpawn(lastErrorText("handle list"));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inRead;
    startup.StartupInfo.hStdOutput = outWrite;
    startup.StartupInfo.hStdError = errWrite;
    startup.lpAttributeList = attributes;

    // a kill-on-close job: the child's tree ends with this run, whatever happens to the editor
    job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return failSpawn(lastErrorText("CreateJobObject"));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const std::wstring application = spec.exe.wstring();
    const std::wstring cwd = spec.cwd.wstring();
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                        environment.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    closeHandle(outWrite);
    closeHandle(errWrite);
    closeHandle(inRead);
    if (!created) return failSpawn(lastErrorText(spec.exe.string().c_str()));
    AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    closeHandle(process.hThread);

    OutputSink sink{spec.maxOutputBytes};
    auto reader = [&sink](HANDLE pipe, std::string* into) {
        char buffer[64 * 1024];
        DWORD read = 0;
        while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            sink.append(*into, buffer, read);
        }
    };
    std::thread outReader(reader, outRead, &result.out);
    std::thread errReader(reader, errRead, &result.err);
    std::thread inWriter;
    if (feedStdin) {
        inWriter = std::thread([&spec, &inWrite]() {
            size_t written = 0;
            while (written < spec.stdinData.size()) {
                DWORD n = 0;
                const DWORD chunk = static_cast<DWORD>(std::min<size_t>(spec.stdinData.size() - written, 64 * 1024));
                if (!WriteFile(inWrite, spec.stdinData.data() + written, chunk, &n, nullptr) || n == 0) break;
                written += n;
            }
            closeHandle(inWrite);
        });
    }

    const Clock::time_point start = Clock::now();
    bool stopping = false;
    while (WaitForSingleObject(process.hProcess, 20) == WAIT_TIMEOUT) {
        if (stopping) continue;
        const Clock::time_point now = Clock::now();
        const bool timedOut = spec.timeoutMs > 0 && now - start >= std::chrono::milliseconds(spec.timeoutMs);
        const bool cancelled = spec.cancel && spec.cancel->isCancelled();
        if (timedOut || cancelled) {
            stopping = true;
            result.timedOut = timedOut;
            result.cancelled = !timedOut;
            TerminateJobObject(job, 1);
        }
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    closeHandle(process.hProcess);

    // a grandchild still holding the pipes: its tree ends after a short drain
    const Clock::time_point exitAt = Clock::now();
    auto readersDone = [&]() {
        DWORD available = 0;
        return !PeekNamedPipe(outRead, nullptr, 0, nullptr, &available, nullptr) &&
               !PeekNamedPipe(errRead, nullptr, 0, nullptr, &available, nullptr);
    };
    while (!readersDone() && Clock::now() - exitAt < std::chrono::milliseconds(kExitDrainMs)) Sleep(5);
    // nothing breaks away from the job (no JOB_OBJECT_LIMIT_BREAKAWAY_OK) and the editor holds
    // no write end: with the job's processes gone the pipes close and the threads return
    TerminateJobObject(job, 1);
    outReader.join();
    errReader.join();
    if (inWriter.joinable()) inWriter.join();
    closeHandle(outRead);
    closeHandle(errRead);
    closeHandle(inWrite);
    closeHandle(job);
    result.truncated = sink.truncated;
    return result;
}

#endif

// ---------------------------------------------------------------- executable resolution

namespace {

    bool isExecutableFile(const fs::path& path) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return false;
#ifdef _WIN32
        return true;
#else
        return access(path.c_str(), X_OK) == 0;
#endif
    }

    bool isWithin(const fs::path& path, const fs::path& dir) {
        if (dir.empty()) return false;
        std::error_code ec;
        const fs::path a = fs::weakly_canonical(path, ec);
        const fs::path b = fs::weakly_canonical(dir, ec);
        const fs::path relative = (a.empty() ? path : a).lexically_relative(b.empty() ? dir : b);
        return !relative.empty() && *relative.begin() != "..";
    }

#ifdef _WIN32
    constexpr char kPathSeparator = ';';
#else
    constexpr char kPathSeparator = ':';
#endif

    std::vector<std::string> splitPath(std::string_view path) {
        std::vector<std::string> entries;
        size_t start = 0;
        while (start <= path.size()) {
            const size_t end = path.find(kPathSeparator, start);
            const std::string_view entry = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            entries.emplace_back(entry);
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return entries;
    }

    std::string environmentPath() {
#ifdef _WIN32
        const DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (size == 0) return std::string();
        std::wstring value(size, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"PATH", value.data(), size);
        value.resize(written);
        return narrow(value);
#else
        const char* value = std::getenv("PATH");
        return value ? std::string(value) : std::string();
#endif
    }

    // an entry PATH may name: absolute, and not the project's
    bool usableDirectory(const std::string& entry, const fs::path& projectDir, fs::path& out) {
        if (entry.empty()) return false;
#ifdef _WIN32
        // a quoted entry ("C:\Program Files\Git\cmd")
        std::wstring text = widen(entry);
        if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') text = text.substr(1, text.size() - 2);
        fs::path dir = fs::path(text);
#else
        fs::path dir = fs::path(entry);
#endif
        if (!dir.is_absolute()) return false;
        if (isWithin(dir, projectDir)) return false;
        out = dir.lexically_normal();
        return true;
    }

    fs::path searchDirectory(const fs::path& dir, std::string_view name) {
#ifdef _WIN32
        // CreateProcess runs programs only: no .cmd/.bat (those need cmd.exe)
        for (const char* extension : {".exe", ".com"}) {
            const fs::path candidate = dir / (std::string(name) + extension);
            if (isExecutableFile(candidate)) return candidate;
        }
        return {};
#else
        const fs::path candidate = dir / std::string(name);
        return isExecutableFile(candidate) ? candidate : fs::path();
#endif
    }

#if defined(__APPLE__)
    // /usr/bin/git is a shim that opens the command line tools' install dialog when they are
    // missing: used only when xcode-select names a developer directory
    bool commandLineToolsInstalled() {
        static const bool installed = []() {
            ProcessSpec spec;
            spec.exe = "/usr/bin/xcode-select";
            spec.args = {"-p"};
            spec.timeoutMs = 5000;
            const ProcessResult result = runProcess(spec);
            return result.ok() && !trimmed(result.out).empty();
        }();
        return installed;
    }
#endif

}

std::string sanitizedPath(std::string_view path, const fs::path& projectDir) {
    std::string out;
    for (const std::string& entry : splitPath(path)) {
        fs::path dir;
        if (!usableDirectory(entry, projectDir, dir)) continue;
        if (!out.empty()) out.push_back(kPathSeparator);
        out += entry;
    }
    return out;
}

fs::path findExecutable(std::string_view name, const ExecutableLookup& lookup, std::string* why) {
    if (!lookup.configured.empty()) {
        if (!lookup.configured.is_absolute()) {
            if (why) *why = "the configured path is not absolute: " + lookup.configured.string();
            return {};
        }
        if (!isExecutableFile(lookup.configured)) {
            if (why) *why = "the configured executable does not exist: " + lookup.configured.string();
            return {};
        }
        return lookup.configured.lexically_normal();
    }
    const std::string path = lookup.usePathOverride ? lookup.pathOverride : environmentPath();
    for (const std::string& entry : splitPath(path)) {
        fs::path dir;
        if (!usableDirectory(entry, lookup.projectDir, dir)) continue;
        const fs::path found = searchDirectory(dir, name);
        if (!found.empty()) return found;
    }
#if defined(__APPLE__)
    if (lookup.probeMacLocations) {
        for (const char* dir : {"/opt/homebrew/bin", "/usr/local/bin"}) {
            const fs::path found = searchDirectory(dir, name);
            if (!found.empty()) return found;
        }
        const fs::path system = searchDirectory("/usr/bin", name);
        if (!system.empty() && (name != "git" || commandLineToolsInstalled())) return system;
    }
#endif
    if (why) *why = std::string(name) + " was not found on PATH";
    return {};
}

bool versionAtLeast(std::string_view versionText, std::string_view minimum, std::string* parsed) {
    auto numbers = [](std::string_view text, std::string* asText) {
        std::vector<int> parts;
        size_t i = 0;
        while (i < text.size() && !(text[i] >= '0' && text[i] <= '9')) i++;
        const size_t begin = i;
        while (i < text.size()) {
            if (!(text[i] >= '0' && text[i] <= '9')) break;
            int value = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                value = value * 10 + (text[i] - '0');
                if (value > 1000000) value = 1000000;
                i++;
            }
            parts.push_back(value);
            if (i < text.size() && text[i] == '.' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') i++;
            else break;
        }
        if (asText) *asText = std::string(text.substr(begin, i - begin));
        return parts;
    };
    const std::vector<int> have = numbers(versionText, parsed);
    const std::vector<int> want = numbers(minimum, nullptr);
    if (have.empty()) return false;
    for (size_t i = 0; i < want.size(); i++) {
        const int part = i < have.size() ? have[i] : 0;
        if (part != want[i]) return part > want[i];
    }
    return true;
}

}
