// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <atomic>
#include <mutex>

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
        // Runs `argv` DIRECTLY (no shell: argv[0] is the program, the rest its
        // arguments, each passed verbatim — no word-splitting, globbing or
        // substitution) in workingDir, streaming each output line to onLine
        // (Out::build when omitted). Blocks until the process exits. Returns true
        // when the exit code is 0. This is how the editor launches CMake, the
        // compilers, ffmpeg and every other tool: a path or a setting that holds
        // shell metacharacters cannot start a second command.
        bool run(const std::vector<std::string>& argv, const fs::path& workingDir,
                 const std::function<void(const std::string&)>& onLine = {});

        // Runs `argv` DIRECTLY like run(), quietly (no exit-code or spawn-failure line in the
        // console), in the current folder, and returns its output (stdout and stderr) with trailing
        // whitespace trimmed when it exits 0, "" otherwise. For tool probes (`which g++`,
        // `g++ -dumpmachine`, `ninja --version`): a tool path with spaces or shell metacharacters
        // is one literal argument.
        static std::string capture(const std::vector<std::string>& argv);

        // Runs `command` through the platform shell (/bin/sh -c, cmd.exe /s /c) in
        // workingDir. ONLY for a step that genuinely needs the shell (sourcing an
        // environment script); never build it from agent- or user-controlled
        // strings — pass those as argv to run() above instead.
        bool runShell(const std::string& command, const fs::path& workingDir,
                      const std::function<void(const std::string&)>& onLine = {});

        // Request cancellation and terminate the running process (if any).
        void cancel();
        bool isCancelRequested() const;
        void resetCancel();

        // Windows: command prefix that loads the MSVC toolchain environment
        // (vcvars) so non-Visual-Studio generators such as Ninja can find the
        // Windows SDK and CRT libraries. Returns "" when not needed / not found.
        static std::string msvcEnvPrefix(const std::string& generator);

        // `argv` as one command line quoted per the C runtime's rules (Windows): for the rare case
        // that needs a shell wrapper (sourcing vcvars) while keeping each argument literal.
        static std::string quoteArgv(const std::vector<std::string>& argv);

#ifdef _WIN32
        static std::string runCaptureNoWindow(const std::string& command);
        static std::string findVswherePath();
        static std::string vsInstallationPath();
        static bool hasVSWithCppTools();
        static std::string findVcvarsall();
#endif

    private:
        void terminateCurrentProcess();

        std::atomic<bool> cancelRequested{false};
        bool quiet = false;   // capture(): no "exited with code" / spawn-failure lines

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

