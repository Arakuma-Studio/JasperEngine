// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <filesystem>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>  // pid_t
#endif

namespace jasperengine::editor {

    namespace fs = std::filesystem;

    // Runs shell commands with streamed output and cancellation support.
    // Shared by the play-mode Generator and the export pipeline.
    class CommandRunner {
    public:
        // Runs the command through the platform shell in workingDir, streaming
        // each output line to onLine (Out::build when omitted). Blocks until the
        // process exits. Returns true when the exit code is 0.
        bool run(const std::string& command, const fs::path& workingDir,
                 const std::function<void(const std::string&)>& onLine = {});
        // Runs args[0] with the other arguments as they are, no shell parsing any of them (the
        // platform tools of a build, docs/specs/S30-shipping.md D8); args[0] is looked up on PATH
        // when it has no directory. exitCode gets the process's code (-1: it did not run or was
        // cancelled). True when it is 0.
        bool runArgs(const std::vector<std::string>& args, const fs::path& workingDir,
                     const std::function<void(const std::string&)>& onLine = {}, int* exitCode = nullptr);

        // Request cancellation and terminate the running process (if any).
        void cancel();
        bool isCancelRequested() const;
        void resetCancel();

        // Windows: command prefix that loads the MSVC toolchain environment
        // (vcvars) so non-Visual-Studio generators such as Ninja can find the
        // Windows SDK and CRT libraries. Returns "" when not needed / not found.
        static std::string msvcEnvPrefix(const std::string& generator);

#ifdef _WIN32
        static std::string runCaptureNoWindow(const std::string& command);
        static std::string findVswherePath();
        static std::string vsInstallationPath();
        static bool hasVSWithCppTools();
        static std::string findVcvarsall();
#endif

    private:
#ifdef _WIN32
        bool spawn(const std::string& commandLine, const fs::path& workingDir, const std::function<void(const std::string&)>& onLine,
                   int* exitCode);
#else
        bool spawn(const std::vector<std::string>& argv, const std::function<void(const std::string&)>& onLine, int* exitCode);
#endif
        void terminateCurrentProcess();

        std::atomic<bool> cancelRequested{false};

#ifdef _WIN32
        HANDLE currentProcessHandle = NULL;
        // Job object the child is assigned to, so termination kills the whole
        // process tree (cmake and the compilers it spawns), not just cmd.exe.
        HANDLE currentJobHandle = NULL;
        std::mutex processHandleMutex;
#else
        pid_t currentProcessPid = 0;
        std::mutex processPidMutex;
#endif
    };

}

