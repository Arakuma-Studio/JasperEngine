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

#ifndef _WIN32
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
        // or Emscripten environment). Windows: argv[0] is resolved by resolveWindowsProgram (on
        // that environment's PATH when given; never the current folder; batch files refused).
        bool run(const std::vector<std::string>& argv, const fs::path& workingDir,
                 const std::function<void(const std::string&)>& onLine = {},
                 const Environment* environment = nullptr);

        // run() with the exit code (docs/specs/S30-shipping.md D8: the platform tools of a build):
        // exitCode gets the process's code (-1: it did not run or was cancelled). True when it is 0.
        bool runArgs(const std::vector<std::string>& args, const fs::path& workingDir,
                     const std::function<void(const std::string&)>& onLine = {}, int* exitCode = nullptr);

        // Runs `argv` DIRECTLY like run(), quietly (no exit-code or spawn-failure line in the
        // console), in the current folder, and returns its output (stdout and stderr) with trailing
        // whitespace trimmed when it exits 0, "" otherwise. For tool probes (`which g++`,
        // `g++ -dumpmachine`, `cmake --version`, vswhere.exe): a tool path with spaces or shell
        // metacharacters is one literal argument. timeoutMs > 0: the process is terminated after
        // that long and "" returned (a GUI program asked for --version may never answer).
        static std::string capture(const std::vector<std::string>& argv, int timeoutMs = 0);

        // Starts `exe` (no arguments) detached in workingDir: no shell, its own session / process
        // group, stdin/stdout/stderr on the null device, no other descriptor or handle inherited,
        // and no child left to the editor (POSIX: a double fork, the intermediate reaped at once).
        // false + error when it could not be started (an exec failure is reported, not hidden).
        static bool launchDetached(const fs::path& exe, const fs::path& workingDir, std::string& error);

#ifndef _WIN32
        // A pipe whose ends are close-on-exec from the moment they exist: pipe2(O_CLOEXEC) where
        // there is one, else pipe + fcntl while holding spawnMutex(), which every spawner holds
        // around its fork / posix_spawn, so no concurrently started child inherits them (and keeps
        // a reader waiting for an end-of-file that never comes). false (errno set) on failure.
        static bool makeCloexecPipe(int fds[2]);
        // Held around every fork / posix_spawn the editor does (see makeCloexecPipe)
        static std::mutex& spawnMutex();
#endif

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

        // This process's environment (Windows: with the "=C:" drive entries, which a cmd.exe child
        // reads its drives' current folders from)
        static Environment currentEnvironment();
        // Sets NAME (case-insensitively on Windows, keeping the block sorted), adding it when missing
        static void setVariable(Environment& env, const std::string& name, const std::string& value);
        // NAME's value, "" when unset
        static std::string variable(const Environment& env, const std::string& name);

        // How run() picks the program on Windows (portable, for tests): a name with a folder is used
        // as given; a bare name is looked for in searchDirs in order (the editor's folder, the
        // system folders, then PATH: never the current folder); without an extension .exe, then
        // .com. A .bat / .cmd is refused: Windows runs batch files through cmd.exe, which parses
        // their arguments (BatBadBut). "" with `resolved` set, else why it can't run.
        static std::string resolveWindowsProgram(const fs::path& program, const std::vector<fs::path>& searchDirs, fs::path& resolved);

        // vswhere's argv: the program, the arguments, then -utf8 (without it vswhere prints in the
        // console's code page, so a localized Visual Studio name or a non-ASCII path would garble).
        // Portable, for tests.
        static std::vector<std::string> vswhereArguments(const std::string& program, std::vector<std::string> arguments);

#ifdef _WIN32
        // vswhere.exe run with these arguments (vswhereArguments), its trimmed output ("" when
        // vswhere is missing or fails). The only way the editor runs vswhere.
        static std::string vswhere(std::vector<std::string> arguments, int timeoutMs = 0);
        static bool hasVswhere();
        static std::string vsInstallationPath();
        static bool hasVSWithCppTools();
        static std::string findVcvarsall();
        // The ninja.exe of an installed Visual Studio's C++ CMake tools (which Visual Studio keeps
        // off PATH), empty when none: a Web build's generator when PATH has no Ninja
        static fs::path findVisualStudioNinja();
#endif

    private:
        void terminateCurrentProcess();
#ifdef _WIN32
        static std::string findVswherePath();
#endif

        std::atomic<bool> cancelRequested{false};
        bool quiet = false;   // capture(): no "exited with code" / spawn-failure lines
        int lastExitCode = -1;   // the last run()'s exit code (-1: not run, cancelled, a signal)

#ifdef _WIN32
        // HANDLEs (void*: <windows.h> stays out of the sources including this header)
        void* currentProcessHandle = nullptr;
        // Job object the child is assigned to, so termination kills the whole
        // process tree (cmake and the compilers it spawns), not just cmake.
        void* currentJobHandle = nullptr;
        std::mutex processHandleMutex;
#else
        pid_t currentProcessPid = 0;
        std::mutex processPidMutex;
#endif
    };

}

