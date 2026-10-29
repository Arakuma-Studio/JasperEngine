// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>
#include <utility>
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

    // Runs external programs (never through a shell) with streamed output and cancellation support.
    // Shared by the play-mode Generator and the export pipeline.
    class CommandRunner {
    public:
        // A process environment: NAME, VALUE pairs (UTF-8)
        using Environment = std::vector<std::pair<std::string, std::string>>;

        // Runs `argv` DIRECTLY (no shell: argv[0] is the program, the rest its
        // arguments, each passed verbatim — no word-splitting, globbing or
        // substitution) in workingDir, streaming each output line to onLine
        // (Out::build when omitted). Blocks until the process exits. Returns true
        // when the exit code is 0. This is how the editor launches CMake, the
        // compilers, ffmpeg and every other tool: a path or a setting that holds
        // shell metacharacters cannot start a second command.
        // environment (optional): the child's WHOLE environment instead of the editor's (the MSVC
        // environment below). Windows: argv[0] without a folder is looked up on that
        // environment's PATH first (as cmd.exe would after vcvars), then as CreateProcessW does.
        bool run(const std::vector<std::string>& argv, const fs::path& workingDir,
                 const std::function<void(const std::string&)>& onLine = {},
                 const Environment* environment = nullptr);

        // Runs `argv` DIRECTLY like run(), quietly (no exit-code or spawn-failure line in the
        // console), in the current folder, and returns its output (stdout and stderr) with trailing
        // whitespace trimmed when it exits 0, "" otherwise. For tool probes (`which g++`,
        // `g++ -dumpmachine`, `cmake --version`, vswhere.exe): a tool path with spaces or shell
        // metacharacters is one literal argument. timeoutMs > 0: the process is terminated after
        // that long and "" returned (a GUI program asked for --version may never answer).
        static std::string capture(const std::vector<std::string>& argv, int timeoutMs = 0);

        // Request cancellation and terminate the running process (if any).
        void cancel();
        bool isCancelRequested() const;
        void resetCancel();

        // Windows + the Ninja generator: the MSVC toolchain environment (vcvarsall.bat x64) so CMake,
        // Ninja and the compilers find the Windows SDK and CRT libraries (the Visual Studio generator
        // sets it up itself, MinGW must not get it). Loaded ONCE per architecture per session by
        // `cmd.exe /d /u /v:off /s /c "call "<vcvarsall.bat>" x64 >nul 2>&1 && set"` (the vcvars path,
        // from vswhere, is the only value in that line: quoted, refused when it holds '"', '%' or a
        // control character) and parsed (parseEnvironmentOutput); the CMake steps then run directly
        // with it (run(..., &env)), so no shell ever sees a project path or a build setting.
        // true + env empty: not needed here (or vcvars not found: warned, run with the editor's
        // own environment as before); true + env: use it; false: vcvars was found but could not be
        // loaded (error says why; its own output went to the console).
        static bool msvcEnvironment(const std::string& generator, Environment& env, std::string& error);

        // What `set` printed, as an Environment: UTF-16LE (cmd.exe /u; with or without a BOM) or
        // 8-bit text; one NAME=VALUE per line (CRLF or LF; a value may hold '=' and ';'); lines
        // without '=' or whose name is empty or starts with whitespace (banner text, the "=C:=C:\"
        // drive entries) skipped; a name seen again (case-insensitively) keeps the last value; sorted
        // by name case-insensitively, as a CreateProcess environment block wants.
        static Environment parseEnvironmentOutput(const std::string& bytes);

#ifdef _WIN32
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
        // process tree (cmake and the compilers it spawns), not just cmake.
        HANDLE currentJobHandle = NULL;
        std::mutex processHandleMutex;
#else
        pid_t currentProcessPid = 0;
        std::mutex processPidMutex;
#endif
    };

}

