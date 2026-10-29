// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// je_vcs_process_helper <mode> [args...]: the child ProcessExecTest runs (docs/specs/
// S29-version-control.md AC1-AC3).
//   args <a>...        writes each argument followed by a NUL to stdout
//   env <NAME>...      writes each variable's value (or "<unset>") followed by a NUL
//   late <bytes>       writes that many 'L' bytes and exits at once
//   stdin              reads stdin to its end, writes how many bytes came
//   flood <bytes>      writes that many 'x' bytes to stdout
//   tree <pid file>    starts a grandchild that sleeps, writes both pids to the file, sleeps
//   sleep <ms>         sleeps
//   exit <code>        exits with that code
// With JE_VCS_HELPER_MARKER set it first creates that file, whatever the mode: a copy planted as
// `git` in a project shows whether it ran.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

    void writeAll(const char* data, size_t size) {
        while (size > 0) {
            const size_t written = std::fwrite(data, 1, size, stdout);
            if (written == 0) return;
            data += written;
            size -= written;
        }
    }

    long long currentPid() {
#ifdef _WIN32
        return static_cast<long long>(GetCurrentProcessId());
#else
        return static_cast<long long>(getpid());
#endif
    }

}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (const char* marker = std::getenv("JE_VCS_HELPER_MARKER"); marker && *marker) {
        std::ofstream out(marker, std::ios::trunc);
        out << "ran\n";
    }
    if (argc < 2) return 64;
    const std::string mode = argv[1];
    if (mode == "args") {
        for (int i = 2; i < argc; i++) writeAll(argv[i], std::strlen(argv[i]) + 1);
        return 0;
    }
    if (mode == "env") {
        for (int i = 2; i < argc; i++) {
            const char* value = std::getenv(argv[i]);
            const std::string text = value ? value : "<unset>";
            writeAll(text.c_str(), text.size() + 1);
        }
        return 0;
    }
    if (mode == "late" && argc >= 3) {
        const std::string block(static_cast<size_t>(std::atoll(argv[2])), 'L');
        writeAll(block.data(), block.size());
        std::fflush(stdout);
        // no wait for the parent: exits right after the write
        std::_Exit(0);
    }
    if (mode == "stdin") {
        size_t total = 0;
        char buffer[4096];
        size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) total += n;
        std::printf("%zu", total);
        return 0;
    }
    if (mode == "flood" && argc >= 3) {
        long long left = std::atoll(argv[2]);
        const std::string chunk(64 * 1024, 'x');
        while (left > 0) {
            const size_t size = static_cast<size_t>(std::min<long long>(left, static_cast<long long>(chunk.size())));
            writeAll(chunk.data(), size);
            left -= static_cast<long long>(size);
        }
        return 0;
    }
    if (mode == "tree" && argc >= 3) {
        long long grandchild = -1;
#ifdef _WIN32
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring commandLine = L"\"" + std::wstring(self) + L"\" sleep 30000";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(self, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            grandchild = static_cast<long long>(process.dwProcessId);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
#else
        const pid_t pid = fork();
        if (pid == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            _exit(0);
        }
        grandchild = static_cast<long long>(pid);
#endif
        {
            std::ofstream out(argv[2], std::ios::trunc);
            out << currentPid() << " " << grandchild << "\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (mode == "sleep" && argc >= 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoll(argv[2])));
        return 0;
    }
    if (mode == "exit" && argc >= 3) {
        return std::atoi(argv[2]);
    }
    return 64;
}
