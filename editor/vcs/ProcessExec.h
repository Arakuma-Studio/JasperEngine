// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// An argv-only process runner for the version control tools (docs/specs/S29-version-control.md
// R2, R3, R24). No shell on any platform: each argument reaches the child as exactly one argv
// entry, so paths and commit messages need no quoting and cannot inject commands. The builds keep
// util/CommandRunner (a shell, streamed output); VCS code never uses it.
//
// POSIX: posix_spawn of an absolute path in a session of its own, argv and envp built before the
// spawn, stdin from the given bytes or /dev/null, both pipes drained with poll(), every descriptor
// close-on-exec; a timeout or a cancel sends SIGTERM to the process group, then SIGKILL after 2 s.
// Windows: CreateProcessW with an absolute lpApplicationName and CommandLineToArgvW quoting,
// CREATE_NO_WINDOW, a kill-on-close job object, stdin through a pipe or NUL.

#include "jobs/Primitives.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::editor::vcs {

    namespace fs = std::filesystem;

    struct ProcessSpec {
        fs::path exe;                     // absolute (a relative one fails without a spawn)
        std::vector<std::string> args;    // argv[1...], UTF-8
        fs::path cwd;                     // empty: the editor's
        // set over the inherited environment (a value of "" sets it empty)
        std::vector<std::pair<std::string, std::string>> env;
        std::vector<std::string> unsetEnv;
        std::string stdinData;            // empty: stdin is /dev/null (NUL), the child reads EOF at once
        int timeoutMs = 30000;            // <= 0: none
        size_t maxOutputBytes = size_t(64) << 20;   // stdout + stderr kept; the rest is read and dropped
        const jobs::CancelToken* cancel = nullptr;
    };

    struct ProcessResult {
        int exitCode = -1;                // the exit status; 128 + signal when a signal ended it
        std::string out;
        std::string err;
        bool timedOut = false;
        bool cancelled = false;
        bool truncated = false;           // more than maxOutputBytes came: the rest was dropped
        bool spawnFailed = false;
        std::string spawnError;           // why the spawn failed

        bool ok() const { return !spawnFailed && !timedOut && !cancelled && exitCode == 0; }
        // stderr, else stdout, trimmed; the spawn error or the timeout when there was no output
        std::string message() const;
    };

    // Runs one process to its end and returns what it wrote. Never on the main thread (R24: a
    // debug build asserts; every build counts it, spawnCount(true)).
    ProcessResult runProcess(const ProcessSpec& spec);

    // Processes spawned by runProcess since start, on the main thread or off it (R24, AC18)
    uint64_t spawnCount(bool onMainThread);

    // How a Windows command line carries one argument so CommandLineToArgvW (and the C runtime)
    // parse it back unchanged (exposed for the tests; POSIX never quotes)
    std::string quoteWindowsArgument(std::string_view arg);

    // ---- executable resolution (R3)

    struct ExecutableLookup {
        fs::path configured;              // the per-user setting: used when set (must exist)
        fs::path projectDir;              // never searched, nor anything below it
        // macOS: /opt/homebrew/bin, /usr/local/bin and /usr/bin (the latter only when xcode-select
        // finds the command line tools, so the install dialog never shows)
        bool probeMacLocations = true;
        std::string pathOverride;         // tests: PATH's value instead of the environment's
        bool usePathOverride = false;
    };

    // The absolute executable of `name` ("git", "svn", "git-lfs"); empty with *why when there is
    // none. PATH's relative entries and the project directory are skipped, so a `git` planted in
    // the project never runs.
    fs::path findExecutable(std::string_view name, const ExecutableLookup& lookup, std::string* why = nullptr);

    // PATH without its relative entries and the ones inside `projectDir`, for the children (git
    // runs git-lfs from PATH)
    std::string sanitizedPath(std::string_view path, const fs::path& projectDir);

    // "2.34.1" from "git version 2.34.1", "git-lfs/3.0.2 (GitHub; ...)" or "1.14.1": the numbers
    // compared with `minimum` ("2.30"); false when the text has no version
    bool versionAtLeast(std::string_view versionText, std::string_view minimum, std::string* parsed = nullptr);

}
