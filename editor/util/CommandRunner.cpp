// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CommandRunner.cpp
#include "CommandRunner.h"
#include "FileUtils.h"
#include "TextEncoding.h"
#include "editor/Out.h"

#include <cctype>
#include <cstdlib>
#include <vector>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <thread>
#include <condition_variable>
#include <cwchar>
#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <io.h>
#else
    #include <signal.h>
    #include <unistd.h>
    #include <sys/wait.h>
    #include <fcntl.h>
    #include <spawn.h>

    extern char **environ;
#endif

using namespace jasperengine;

namespace {
    constexpr std::chrono::milliseconds kReadSleepMs{10};
    constexpr std::chrono::milliseconds kKillGracePeriod{100};

    char asciiLower(char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    bool equalIgnoreCase(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (asciiLower(a[i]) != asciiLower(b[i])) return false;
        }
        return true;
    }

    bool lessIgnoreCase(const std::string& a, const std::string& b) {
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; i++) {
            const unsigned char x = static_cast<unsigned char>(asciiLower(a[i]));
            const unsigned char y = static_cast<unsigned char>(asciiLower(b[i]));
            if (x != y) return x < y;
        }
        return a.size() < b.size();
    }

    void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // UTF-16LE bytes from `start` as UTF-8 (an unpaired surrogate becomes U+FFFD, an odd last byte
    // is dropped)
    std::string utf16leToUtf8(const std::string& bytes, size_t start) {
        std::string out;
        out.reserve((bytes.size() - start) / 2);
        auto unitAt = [&bytes](size_t i) -> std::uint32_t {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        };
        for (size_t i = start; i + 1 < bytes.size(); i += 2) {
            const std::uint32_t unit = unitAt(i);
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
                const std::uint32_t low = unitAt(i + 2);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                    i += 2;
                    continue;
                }
            }
            appendUtf8(out, (unit >= 0xD800 && unit <= 0xDFFF) ? 0xFFFD : unit);
        }
        return out;
    }
}

std::vector<std::string> editor::CommandRunner::vswhereArguments(const std::string& program, std::vector<std::string> arguments) {
    std::vector<std::string> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(program);
    for (std::string& argument : arguments) argv.push_back(std::move(argument));
    argv.push_back("-utf8");
    return argv;
}

#ifdef _WIN32
std::string editor::CommandRunner::findVswherePath() {
    auto tryEnv = [](const char* envVar) -> std::string {
        const char* pf = std::getenv(envVar);
        if (!pf) return "";
        fs::path p = fs::path(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
        if (fs::exists(p)) return p.string();
        return "";
    };
    std::string result = tryEnv("ProgramFiles(x86)");
    if (result.empty()) result = tryEnv("ProgramFiles");
    return result;
}

std::string editor::CommandRunner::vswhere(std::vector<std::string> arguments, int timeoutMs) {
    const std::string program = findVswherePath();
    if (program.empty()) return "";
    // vswhere.exe run directly with its arguments (capture: no cmd.exe, no window)
    std::string output = capture(vswhereArguments(program, std::move(arguments)), timeoutMs);
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) output.pop_back();
    return output;
}

bool editor::CommandRunner::hasVswhere() {
    return !findVswherePath().empty();
}

std::string editor::CommandRunner::vsInstallationPath() {
    return vswhere({"-products", "*", "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-property", "installationPath"});
}

bool editor::CommandRunner::hasVSWithCppTools() {
    return !vsInstallationPath().empty();
}

// Path to vcvarsall.bat for the latest VS install with C++ tools, or "".
std::string editor::CommandRunner::findVcvarsall() {
    std::string installPath = vsInstallationPath();
    if (installPath.empty()) return "";
    fs::path vcvars = fs::path(installPath) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
    std::error_code ec;
    if (fs::exists(vcvars, ec)) return vcvars.string();
    return "";
}

std::filesystem::path editor::CommandRunner::findVisualStudioNinja() {
    // every install (Build Tools included), newest first; vswhere writes UTF-8 (-utf8)
    std::istringstream installs(vswhere({"-products", "*", "-sort", "-property", "installationPath"}));
    for (std::string install; std::getline(installs, install);) {
        while (!install.empty() && (install.back() == '\r' || install.back() == ' ')) install.pop_back();
        if (install.empty()) continue;
        const fs::path ninja = FileUtils::pathFromUtf8(install) / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja" / "ninja.exe";
        std::error_code ec;
        if (fs::is_regular_file(ninja, ec)) return ninja;
    }
    return fs::path();
}
#endif

editor::CommandRunner::Environment editor::CommandRunner::parseEnvironmentOutput(const std::string& bytes) {
    std::string text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        text = utf16leToUtf8(bytes, 2);
    } else if (bytes.size() >= 2 && bytes[0] != '\0' && bytes[1] == '\0') {
        text = utf16leToUtf8(bytes, 0);   // cmd.exe /u writes UTF-16LE without a BOM
    } else {
        text = bytes;
    }
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);

    Environment env;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        // no '=' (banner text), an empty name (the "=C:=C:\\..." drive entries), a name starting
        // with whitespace, or a stray NUL: not a variable
        if (eq == std::string::npos || eq == 0 || line[0] == ' ' || line[0] == '\t' ||
            line.find('\0') != std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        auto same = std::find_if(env.begin(), env.end(), [&name](const auto& entry) { return equalIgnoreCase(entry.first, name); });
        if (same != env.end()) {
            same->second = std::move(value);
        } else {
            env.emplace_back(std::move(name), std::move(value));
        }
    }
    std::stable_sort(env.begin(), env.end(), [](const auto& a, const auto& b) { return lessIgnoreCase(a.first, b.first); });
    return env;
}

#ifdef _WIN32
namespace {
    std::wstring widen(const std::string& text, UINT codePage) {
        if (text.empty()) return std::wstring();
        const int size = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0) return std::wstring();
        std::wstring wide(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
        return wide;
    }

    // A CreateProcessW (CREATE_UNICODE_ENVIRONMENT) block: NAME=VALUE\0 ... \0
    std::wstring environmentBlock(const editor::CommandRunner::Environment& env) {
        std::wstring block;
        for (const auto& entry : env) {
            block += widen(entry.first, CP_UTF8);
            block.push_back(L'=');
            block += widen(entry.second, CP_UTF8);
            block.push_back(L'\0');
        }
        if (env.empty()) block.push_back(L'\0');
        block.push_back(L'\0');
        return block;
    }

    std::string narrow(const wchar_t* text, size_t length) {
        if (length == 0) return std::string();
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return std::string();
        std::string out(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), size, nullptr, nullptr);
        return out;
    }

    // Where a bare program name is looked for (resolveWindowsProgram): the editor's own folder,
    // the system and Windows folders (as CreateProcessW does, minus the current folder), then the
    // PATH of the environment the program gets (`environment`, else the editor's)
    std::vector<std::filesystem::path> windowsSearchDirs(const editor::CommandRunner::Environment* environment) {
        std::vector<std::filesystem::path> dirs;
        wchar_t buffer[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH) dirs.push_back(std::filesystem::path(std::wstring(buffer, length)).parent_path());
        UINT size = GetSystemDirectoryW(buffer, MAX_PATH);
        if (size > 0 && size < MAX_PATH) dirs.push_back(std::filesystem::path(std::wstring(buffer, size)));
        size = GetWindowsDirectoryW(buffer, MAX_PATH);
        if (size > 0 && size < MAX_PATH) dirs.push_back(std::filesystem::path(std::wstring(buffer, size)));
        std::wstring path;
        if (environment) {
            for (const auto& entry : *environment) {
                if (equalIgnoreCase(entry.first, "PATH")) path = widen(entry.second, CP_UTF8);
            }
        } else {
            const DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            if (needed > 0) {
                path.resize(needed);
                const DWORD got = GetEnvironmentVariableW(L"PATH", path.data(), needed);
                path.resize(got < needed ? got : 0);
            }
        }
        size_t pos = 0;
        while (pos <= path.size()) {
            size_t end = path.find(L';', pos);
            if (end == std::wstring::npos) end = path.size();
            std::wstring dir = path.substr(pos, end - pos);
            pos = end + 1;
            if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
            if (!dir.empty()) dirs.push_back(std::filesystem::path(dir));
        }
        return dirs;
    }

    // Runs `commandLine` with the system folder's cmd.exe, hidden, and collects everything it
    // writes to stdout/stderr. False when it could not be started.
    bool runCmdCollect(std::wstring commandLine, std::string& bytes, DWORD& exitCode) {
        wchar_t systemDir[MAX_PATH];
        const UINT length = GetSystemDirectoryW(systemDir, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return false;
        const std::wstring cmdExe = std::wstring(systemDir, length) + L"\\cmd.exe";

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE hRead = nullptr, hWrite = nullptr;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.hStdInput = nullptr;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(cmdExe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            CloseHandle(hRead);
            CloseHandle(hWrite);
            return false;
        }
        CloseHandle(hWrite);
        char buffer[4096];
        DWORD bytesRead = 0;
        while (ReadFile(hRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
            bytes.append(buffer, bytesRead);
        }
        CloseHandle(hRead);
        WaitForSingleObject(pi.hProcess, INFINITE);
        exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
}
#endif

std::string editor::CommandRunner::resolveWindowsProgram(const fs::path& program, const std::vector<fs::path>& searchDirs, fs::path& resolved) {
    resolved.clear();
    if (program.empty()) return "no program given";
    auto lowerExtension = [](const fs::path& path) {
        std::string ext = path.extension().string();
        for (char& c : ext) c = asciiLower(c);
        return ext;
    };
    auto batchRefusal = [&](const fs::path& path) -> std::string {
        const std::string ext = lowerExtension(path);
        if (ext == ".bat" || ext == ".cmd") {
            return path.string() + " is a batch file: Windows runs those through cmd.exe, which would parse its "
                   "arguments (paths with & | < > ^ % could run commands); point the setting at the program itself (.exe)";
        }
        return std::string();
    };
    // .exe / .com as given; any other name (none, or a dot in it: "tool-1.2") gets .exe, then .com
    // added (PATHEXT-style), then is tried as spelled; never .bat / .cmd
    auto candidatesOf = [&](const fs::path& base) {
        std::vector<fs::path> list;
        const std::string ext = lowerExtension(base);
        if (ext == ".exe" || ext == ".com") {
            list.push_back(base);
        } else {
            list.push_back(fs::path(base.native() + fs::path(".exe").native()));
            list.push_back(fs::path(base.native() + fs::path(".com").native()));
            if (!ext.empty()) list.push_back(base);
        }
        return list;
    };
    if (const std::string why = batchRefusal(program); !why.empty()) return why;
    std::error_code ec;
    const std::string text = program.string();
    const bool hasFolder = program.has_parent_path() || text.find_first_of("\\/:") != std::string::npos;
    if (hasFolder) {
        // as given (relative to the current folder only when spelled so, never searched)
        const fs::path base = program.is_absolute() ? program : fs::absolute(program, ec);
        for (const fs::path& candidate : candidatesOf(base)) {
            if (fs::is_regular_file(candidate, ec)) {
                resolved = candidate;
                return std::string();
            }
        }
        return text + " was not found";
    }
    for (const fs::path& dir : searchDirs) {
        if (dir.empty()) continue;
        for (const fs::path& candidate : candidatesOf(dir / program)) {
            if (!fs::is_regular_file(candidate, ec)) continue;
            if (const std::string why = batchRefusal(candidate); !why.empty()) return why;
            resolved = candidate;
            return std::string();
        }
    }
    return text + " was not found on PATH";
}

editor::CommandRunner::Environment editor::CommandRunner::currentEnvironment() {
    Environment env;
#ifdef _WIN32
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return env;
    for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
        const std::string text = narrow(entry, std::wcslen(entry));
        // the "=C:=C:\\..." drive entries are kept (their name starts with '='): a cmd.exe child
        // finds each drive's current folder in them
        const size_t eq = text.find('=', 1);
        if (eq == std::string::npos) continue;
        setVariable(env, text.substr(0, eq), text.substr(eq + 1));
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string text(*entry);
        const size_t eq = text.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        env.emplace_back(text.substr(0, eq), text.substr(eq + 1));
    }
#endif
    return env;
}

void editor::CommandRunner::setVariable(Environment& env, const std::string& name, const std::string& value) {
    for (auto& entry : env) {
#ifdef _WIN32
        const bool same = equalIgnoreCase(entry.first, name);
#else
        const bool same = entry.first == name;
#endif
        if (same) {
            entry.second = value;
            return;
        }
    }
    env.emplace_back(name, value);
#ifdef _WIN32
    std::stable_sort(env.begin(), env.end(), [](const auto& a, const auto& b) { return lessIgnoreCase(a.first, b.first); });
#endif
}

std::string editor::CommandRunner::variable(const Environment& env, const std::string& name) {
    for (const auto& entry : env) {
#ifdef _WIN32
        if (equalIgnoreCase(entry.first, name)) return entry.second;
#else
        if (entry.first == name) return entry.second;
#endif
    }
    return std::string();
}

bool editor::CommandRunner::msvcEnvironment(const std::string& generator, Environment& env, std::string& error) {
    env.clear();
    error.clear();
#ifdef _WIN32
    // Ninja (unlike the Visual Studio generator) runs the compiler directly and
    // does NOT set up the MSVC toolchain environment. A standalone Clang or MSVC
    // toolchain then cannot find the Windows SDK and CRT import libraries, so
    // linking fails with errors like "could not open 'kernel32.lib'". Loading
    // vcvars populates INCLUDE/LIB/PATH so the SDK is found. The Visual Studio
    // generator configures this itself, and MinGW must not use MSVC's env.
    if (generator != "Ninja") return true;
    const std::string vcvars = findVcvarsall();
    if (vcvars.empty()) {
        Out::warning("Could not locate the Visual Studio environment (vcvarsall.bat). If linking fails with missing '*.lib' files, run the editor from a Developer Command Prompt or install the Windows SDK with Visual Studio.");
        return true;
    }
    const std::string arch = "x64";

    static std::mutex cacheMutex;
    static std::map<std::string, Environment> cache;   // vcvars path + arch -> environment
    std::lock_guard<std::mutex> lock(cacheMutex);
    const std::string key = vcvars + "|" + arch;
    if (const auto it = cache.find(key); it != cache.end()) {
        env = it->second;
        return true;
    }

    // the vcvars path is the only value in the cmd.exe line: inside quotes & | < > ^ ( ) are
    // literal, so only what quotes can't protect is refused ('"' ends them, '%' expands a
    // variable even inside them, a control character could end the line)
    const std::wstring script = std::filesystem::path(vcvars).wstring();
    const bool unsafe = std::any_of(script.begin(), script.end(), [](wchar_t c) { return c == L'"' || c == L'%' || c < 0x20; });
    if (unsafe) {
        error = "The Visual Studio environment script " + vcvars + " has a '\"', '%' or control character in its path, "
                "which cmd.exe can't be given safely; run the editor from a Developer Command Prompt or use the Visual Studio generator.";
        return false;
    }
    const std::wstring warch = widen(arch, CP_UTF8);
    std::string bytes;
    DWORD exitCode = 1;
    const bool ran = runCmdCollect(L"cmd.exe /d /u /v:off /s /c \"call \"" + script + L"\" " + warch + L" >nul 2>&1 && set\"", bytes, exitCode);
    Environment loaded = (ran && exitCode == 0) ? parseEnvironmentOutput(bytes) : Environment();
    const bool hasPath = std::any_of(loaded.begin(), loaded.end(), [](const auto& entry) { return equalIgnoreCase(entry.first, "PATH"); });
    if (!hasPath) {
        // its own diagnostics ("[ERROR:...]" when it can't find the Windows SDK) to the console
        std::string output;
        DWORD diagnosticExit = 1;
        if (runCmdCollect(L"cmd.exe /d /v:off /s /c \"call \"" + script + L"\" " + warch + L"\"", output, diagnosticExit)) {
            size_t pos = 0;
            while (pos < output.size()) {
                size_t end = output.find('\n', pos);
                if (end == std::string::npos) end = output.size();
                std::string line = output.substr(pos, end - pos);
                pos = end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) Out::build("%s", line.c_str());
            }
        }
        error = "Could not load the Visual Studio environment (" + vcvars + " " + arch + "): see the Console for its output.";
        return false;
    }
    cache[key] = loaded;
    env = std::move(loaded);
    return true;
#else
    (void)generator;
    return true;
#endif
}

void editor::CommandRunner::cancel() {
    cancelRequested.store(true, std::memory_order_relaxed);
    terminateCurrentProcess();
}

bool editor::CommandRunner::isCancelRequested() const {
    return cancelRequested.load(std::memory_order_relaxed);
}

void editor::CommandRunner::resetCancel() {
    cancelRequested.store(false, std::memory_order_relaxed);
}

void editor::CommandRunner::terminateCurrentProcess() {
    #ifdef _WIN32
        HANDLE handleSnapshot = nullptr;
        HANDLE jobSnapshot = nullptr;
        {
            std::lock_guard<std::mutex> lock(processHandleMutex);
            handleSnapshot = currentProcessHandle;
            jobSnapshot = currentJobHandle;
        }
        if (jobSnapshot) {
            // Kills every process in the job: cmake plus the build tool /
            // compiler tree it spawned.
            TerminateJobObject(jobSnapshot, 1);
        } else if (handleSnapshot) {
            TerminateProcess(handleSnapshot, 1);
        }
    #else
        pid_t pidSnapshot = 0;
        {
            std::lock_guard<std::mutex> lock(processPidMutex);
            pidSnapshot = currentProcessPid;
        }
        if (pidSnapshot > 0) {
            // The child was spawned into its own process group (pgid == pid),
            // so signal the group to tear down the shell AND its descendants
            // (cmake, ninja/make, compilers).
            if (kill(-pidSnapshot, SIGTERM) != 0 && errno != ESRCH) {
                Out::warning("Failed to send SIGTERM to process group %d: %s", pidSnapshot, strerror(errno));
            }

            std::this_thread::sleep_for(kKillGracePeriod);
            if (kill(-pidSnapshot, 0) == 0) {
                kill(-pidSnapshot, SIGKILL);
            }
        }
    #endif
}

#ifdef _WIN32
namespace {
    // One argv entry quoted for a Windows command line per the rules the C runtime's argv parser
    // (and CommandLineToArgvW) use: wrap in quotes, double any run of backslashes that precedes a
    // quote or the closing quote, and escape embedded quotes with a backslash. So a value with
    // spaces, quotes or metacharacters reaches the program as one literal argument.
    std::string quoteWin(const std::string& arg) {
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
        std::string out = "\"";
        for (size_t i = 0; ; i++) {
            size_t backslashes = 0;
            while (i < arg.size() && arg[i] == '\\') { backslashes++; i++; }
            if (i == arg.size()) { out.append(backslashes * 2, '\\'); break; }
            if (arg[i] == '"') { out.append(backslashes * 2 + 1, '\\'); out.push_back('"'); }
            else { out.append(backslashes, '\\'); out.push_back(arg[i]); }
        }
        out.push_back('"');
        return out;
    }
    std::string argvToCommandLine(const std::vector<std::string>& argv) {
        std::string line;
        for (const std::string& a : argv) { if (!line.empty()) line.push_back(' '); line += quoteWin(a); }
        return line;
    }
}
#endif

#ifndef _WIN32
std::mutex& editor::CommandRunner::spawnMutex() {
    static std::mutex* mutex = new std::mutex();   // never destroyed: spawns may run at exit
    return *mutex;
}

bool editor::CommandRunner::makeCloexecPipe(int fds[2]) {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    std::lock_guard<std::mutex> lock(spawnMutex());
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}
#endif

bool editor::CommandRunner::launchDetached(const fs::path& exe, const fs::path& workingDir, std::string& error) {
    error.clear();
#ifdef _WIN32
    const std::wstring wexe = exe.wstring();
    const std::wstring wdir = workingDir.wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // the application name set: lpCommandLine is only argv[0] (quoted); nothing inherited
    std::wstring cmdLine = L"\"" + wexe + L"\"";
    if (!CreateProcessW(wexe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
                        nullptr, wdir.empty() ? nullptr : wdir.c_str(), &si, &pi)) {
        error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    // the exec result comes back through a close-on-exec pipe: closed unread when exec works,
    // the errno when it doesn't
    int errorPipe[2];
    if (!makeCloexecPipe(errorPipe)) {
        error = std::string("pipe: ") + strerror(errno);
        return false;
    }
    const std::string exeText = exe.string();
    const std::string dirText = workingDir.string();
    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) maxFd = 65536;
    char* const childArgv[] = { const_cast<char*>(exeText.c_str()), nullptr };

    pid_t child = -1;
    {
        std::lock_guard<std::mutex> spawning(spawnMutex());
        child = ::fork();
    }
    if (child < 0) {
        error = std::string("fork: ") + strerror(errno);
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        return false;
    }
    if (child == 0) {
        // async-signal-safe calls only from here on
        auto fail = [&errorPipe](int code, int exitCode) {
            ssize_t ignored = ::write(errorPipe[1], &code, sizeof code);
            (void)ignored;
            ::_exit(exitCode);
        };
        const pid_t grandchild = ::fork();
        if (grandchild < 0) fail(errno, 1);
        if (grandchild > 0) ::_exit(0);   // the intermediate: reaped by the editor right away
        ::setsid();
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
        }
        for (int fd = 3; fd < maxFd; fd++) {
            if (fd != errorPipe[1]) ::close(fd);
        }
        if (!dirText.empty() && ::chdir(dirText.c_str()) != 0) fail(errno, 126);
        ::execv(exeText.c_str(), childArgv);
        fail(errno, 127);
    }
    ::close(errorPipe[1]);
    int status = 0;
    while (::waitpid(child, &status, 0) == -1 && errno == EINTR) {
    }
    int childError = 0;
    ssize_t got = 0;
    do {
        got = ::read(errorPipe[0], &childError, sizeof childError);
    } while (got == -1 && errno == EINTR);
    ::close(errorPipe[0]);
    if (got == static_cast<ssize_t>(sizeof childError)) {
        error = strerror(childError);
        return false;
    }
    return true;
#endif
}

bool editor::CommandRunner::runArgs(const std::vector<std::string>& args, const fs::path& workingDir,
                                    const std::function<void(const std::string&)>& onLine, int* exitCode) {
    const bool ok = run(args, workingDir, onLine);
    if (exitCode) *exitCode = lastExitCode;
    return ok;
}

std::string editor::CommandRunner::capture(const std::vector<std::string>& argv, int timeoutMs) {
    CommandRunner runner;
    runner.quiet = true;
    std::string output;

    // timeoutMs > 0: a watchdog cancels the run (terminating the process) when it is still going
    std::mutex doneMutex;
    std::condition_variable doneSignal;
    bool done = false;
    std::thread watchdog;
    if (timeoutMs > 0) {
        watchdog = std::thread([&] {
            std::unique_lock<std::mutex> lock(doneMutex);
            if (!doneSignal.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return done; })) {
                lock.unlock();
                runner.cancel();
            }
        });
    }
    const bool ok = runner.run(argv, fs::path(), [&output](const std::string& line) {
        output += line;
        output.push_back('\n');
    });
    if (watchdog.joinable()) {
        {
            std::lock_guard<std::mutex> lock(doneMutex);
            done = true;
        }
        doneSignal.notify_all();
        watchdog.join();
    }
    if (!ok || runner.isCancelRequested()) return "";
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
        output.pop_back();
    return output;
}

bool editor::CommandRunner::run(const std::vector<std::string>& argv, const fs::path& workingDir, const std::function<void(const std::string&)>& onLine,
                                const Environment* environment) {
    lastExitCode = -1;
    if (argv.empty()) return false;
    cancelRequested.store(false, std::memory_order_relaxed);

    // every line as UTF-8, whatever code page the program wrote in
    auto emit = [&onLine](const std::string& rawLine) {
        const std::string line = TextEncoding::consoleToUtf8(rawLine);
        if (onLine) {
            onLine(line);
        } else {
            Out::build("%s", line.c_str());
        }
    };

    #ifdef _WIN32
        SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE hReadPipe = nullptr;
        HANDLE hWritePipe = nullptr;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
            Out::error("Failed to create pipe (Win32). Error code: %lu", GetLastError());
            return false;
        }
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(STARTUPINFOW);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError  = hWritePipe;
        si.hStdInput  = nullptr;

        PROCESS_INFORMATION pi{};
        // argv quoted per the C runtime's rules (quoteWin): CreateProcessW splits argv[0] off and
        // passes the rest to the child verbatim; no shell parses it, so a value with metacharacters
        // is one literal argument. The arguments are UTF-8; an environment (the MSVC or Emscripten
        // one) is passed as a UTF-16 block, and argv[0] is found on its PATH.
        std::wstring cmdLine = widen(argvToCommandLine(argv), CP_UTF8);
        // the program resolved here, never searched for by CreateProcessW (which would look in the
        // current folder first); a .bat / .cmd is refused (Windows would run it through cmd.exe)
        fs::path program;
        const std::string notRunnable = resolveWindowsProgram(fs::path(widen(argv[0], CP_UTF8)), windowsSearchDirs(environment), program);
        if (!notRunnable.empty()) {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            if (!quiet) Out::error("Not started: %s.", notRunnable.c_str());
            return false;
        }
        const std::wstring application = program.wstring();
        // Python children (Emscripten's emcc, jasperengine.py) print UTF-8 to the pipe, not the
        // ANSI code page that cannot hold every path, unless the environment says otherwise
        Environment childEnvironment = environment ? *environment : currentEnvironment();
        for (const char* name : {"PYTHONUTF8", "PYTHONIOENCODING"}) {
            if (variable(childEnvironment, name).empty())
                setVariable(childEnvironment, name, std::string(name) == "PYTHONUTF8" ? "1" : "utf-8");
        }
        std::wstring envBlock = environmentBlock(childEnvironment);
        const std::wstring workDir = workingDir.wstring();

        // Track the whole child tree in a job object so cancellation can kill
        // cmake and the compilers it spawns, not only cmd.exe. KILL_ON_JOB_CLOSE
        // also reaps the tree if the editor itself exits.
        HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
        if (hJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
            jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits));
        }

        if (!CreateProcessW(
                application.c_str(),
                cmdLine.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                static_cast<LPVOID>(envBlock.data()),
                workDir.empty() ? nullptr : workDir.c_str(),
                &si,
                &pi))
        {
            DWORD err = GetLastError();
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            if (hJob) CloseHandle(hJob);
            if (!quiet) Out::error("Failed to create process. Error code: %lu", err);
            return false;
        }

        // Assign before the first instruction runs so no child can escape the
        // job. On failure (e.g. nested-job restrictions) fall back to plain
        // process termination.
        if (hJob && !AssignProcessToJobObject(hJob, pi.hProcess)) {
            CloseHandle(hJob);
            hJob = nullptr;
        }
        ResumeThread(pi.hThread);

        CloseHandle(hWritePipe);

        {
            std::lock_guard<std::mutex> lock(processHandleMutex);
            currentProcessHandle = pi.hProcess;
            currentJobHandle = hJob;
        }

        constexpr size_t BUFFER_SIZE = 4096;
        char buffer[BUFFER_SIZE];
        std::string accumulator;

        auto drainPipe = [&]() {
            DWORD bytesAvailable = 0;
            while (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr) && bytesAvailable > 0) {
                DWORD bytesRead = 0;
                if (!ReadFile(hReadPipe, buffer, static_cast<DWORD>(BUFFER_SIZE - 1), &bytesRead, nullptr) || bytesRead == 0) {
                    break;
                }

                accumulator.append(buffer, bytesRead);

                size_t pos = 0;
                size_t nextPos = 0;
                while ((nextPos = accumulator.find('\n', pos)) != std::string::npos) {
                    std::string line = accumulator.substr(pos, nextPos - pos);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (!line.empty()) {
                        emit(line);
                    }
                    pos = nextPos + 1;
                }
                accumulator.erase(0, pos);
            }
        };

        DWORD exitCode = 1;
        bool finished = false;

        while (!finished) {
            if (cancelRequested.load(std::memory_order_relaxed)) {
                terminateCurrentProcess();
                CloseHandle(pi.hThread);
                {
                    std::lock_guard<std::mutex> lock(processHandleMutex);
                    currentProcessHandle = NULL;
                    currentJobHandle = NULL;
                }
                CloseHandle(pi.hProcess);
                if (hJob) CloseHandle(hJob);
                CloseHandle(hReadPipe);  // Close pipe at the end
                return false;
            }

            drainPipe();

            DWORD waitResult = WaitForSingleObject(pi.hProcess, 50);
            if (waitResult == WAIT_OBJECT_0) {
                finished = true;
                GetExitCodeProcess(pi.hProcess, &exitCode);
            } else if (waitResult == WAIT_FAILED) {
                finished = true;
            } else {
                std::this_thread::sleep_for(kReadSleepMs);
            }
        }

        // The process can exit with output still buffered in the pipe — for a
        // command that fails immediately that is ALL of its output. Drain it
        // so errors are not silently dropped.
        drainPipe();

        if (!accumulator.empty()) {
            emit(accumulator);
        }

        if (exitCode != 0 && !quiet) {
            Out::build("Process exited with code %lu", exitCode);
        }
        if (!cancelRequested.load(std::memory_order_relaxed)) lastExitCode = static_cast<int>(exitCode);

        CloseHandle(hReadPipe);
        CloseHandle(pi.hThread);

        {
            std::lock_guard<std::mutex> lock(processHandleMutex);
            currentProcessHandle = NULL;
            currentJobHandle = NULL;
        }
        CloseHandle(pi.hProcess);
        if (hJob) CloseHandle(hJob);

        return exitCode == 0;
    #else
        int pipefd[2];
        if (!makeCloexecPipe(pipefd)) {
            Out::error("Failed to create pipe (POSIX): %s", strerror(errno));
            return false;
        }

        // Make read end non-blocking
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        pid_t pid = 0;
        const std::string workDirStr = workingDir.string();
        // the program runs in workingDir without a shell: chdir in the child before exec
        if (!workDirStr.empty()) {
            posix_spawn_file_actions_addchdir_np(&actions, workDirStr.c_str());
        }

        // argv passed verbatim to the program: no /bin/sh, so no word-splitting or substitution
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);

        // Place the child in its own process group so cancellation can signal
        // the whole tree: killing only it would orphan cmake and the
        // compilers it spawned, leaving them running (and holding build locks).
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);

        // posix_spawnp searches PATH for a bare program name; an absolute path is used as given
        // an environment given: the child's whole environment (PATH lookup of argv[0] still uses ours)
        std::vector<std::string> envStrings;
        std::vector<char*> envp;
        if (environment) {
            envStrings.reserve(environment->size());
            for (const auto& entry : *environment) envStrings.push_back(entry.first + "=" + entry.second);
            for (std::string& entry : envStrings) envp.push_back(entry.data());
            envp.push_back(nullptr);
        }
        int spawnResult = 0;
        {
            std::lock_guard<std::mutex> spawning(spawnMutex());
            spawnResult = posix_spawnp(&pid, cargv[0], &actions, &attr, cargv.data(), environment ? envp.data() : environ);
        }
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[1]);

        if (spawnResult != 0) {
            close(pipefd[0]);
            if (!quiet) Out::error("Failed to spawn process (POSIX): %s", strerror(spawnResult));
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(processPidMutex);
            currentProcessPid = pid;
        }

        constexpr size_t BUFFER_SIZE = 4096;
        char buffer[BUFFER_SIZE];
        bool processExited = false;
        bool reaped = false;
        int reapedStatus = 0;

        // Accumulate reads and emit per line (a read() chunk can hold several
        // lines or a partial one), matching the Win32 path.
        std::string accumulator;
        auto emitLines = [&](const char* data, size_t length) {
            accumulator.append(data, length);
            size_t pos = 0;
            size_t nextPos = 0;
            while ((nextPos = accumulator.find('\n', pos)) != std::string::npos) {
                std::string line = accumulator.substr(pos, nextPos - pos);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    emit(line);
                }
                pos = nextPos + 1;
            }
            accumulator.erase(0, pos);
        };

        while (!processExited) {
            // Check for cancellation
            if (cancelRequested.load(std::memory_order_relaxed)) {
                terminateCurrentProcess();
                processExited = true;
                break;
            }

            // Try to read with a short timeout
            ssize_t bytesRead = read(pipefd[0], buffer, BUFFER_SIZE);

            if (bytesRead > 0) {
                emitLines(buffer, (size_t)bytesRead);
            } else if (bytesRead == 0) {
                // EOF - pipe closed
                processExited = true;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available, check process status
                int status;
                pid_t result = waitpid(pid, &status, WNOHANG);
                if (result == pid) {
                    // Reaped here: keep the status, the final waitpid below
                    // would get ECHILD and lose the real exit code.
                    reaped = true;
                    reapedStatus = status;
                    processExited = true;
                } else if (result == -1) {
                    processExited = true;
                }
                // Sleep briefly to avoid busy-waiting
                std::this_thread::sleep_for(kReadSleepMs);
            } else {
                // Read error
                Out::error("Error reading from pipe: %s", strerror(errno));
                processExited = true;
            }
        }

        // The process can exit with output still buffered in the pipe — for a
        // command that fails immediately that is ALL of its output. Drain it
        // (non-blocking read) so errors are not silently dropped.
        while (true) {
            ssize_t bytesRead = read(pipefd[0], buffer, BUFFER_SIZE);
            if (bytesRead <= 0) break;
            emitLines(buffer, (size_t)bytesRead);
        }
        if (!accumulator.empty()) {
            emit(accumulator);
        }

        // Always close the pipe after the loop
        close(pipefd[0]);

        int status = 0;
        if (reaped) {
            status = reapedStatus;
        } else {
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
                // retry on EINTR
            }
        }

        {
            std::lock_guard<std::mutex> lock(processPidMutex);
            currentProcessPid = 0;
        }

        if (cancelRequested.load(std::memory_order_relaxed)) {
            return false;
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            lastExitCode = code;
            if (code != 0 && !quiet) {
                Out::build("Process exited with code %d", code);
            }
            return code == 0;
        }
        if (WIFSIGNALED(status)) {
            Out::warning("Process terminated by signal %d", WTERMSIG(status));
        }
        return false;
    #endif
}
