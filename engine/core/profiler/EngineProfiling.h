// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef EngineProfiling_h
#define EngineProfiling_h

// What the Engine does for the profiler (spec S15 R17, R18, R38), in one place so Engine.cpp
// keeps one line per site: the job system's profiler hooks (every job a scope named by its
// label, waits as "Wait: ..." scopes, worker threads named), the built-in monitors published
// at the end of each game frame, and the command-line capture (--profile, --profile-capture,
// --profile-frames). Compiled with the profiler only.

#include "Export.h"

#include <string>
#include <vector>

namespace jasperengine::profiler {

#if JASPERENGINE_PROFILER

    struct CommandLineOptions {
        bool record = false;            // --profile: record from the start
        std::string capturePath;        // --profile-capture=<path>
        uint32_t captureFrames = 300;   // --profile-frames=N
        bool error = false;
        std::string message;            // what was wrong with them
    };
    // reads --profile, --profile-capture=<path> (or "--profile-capture <path>") and
    // --profile-frames=N (or "--profile-frames N")
    JASPERENGINE_API CommandLineOptions parseCommandLine(const std::vector<std::string>& args);

    // Engine::systemInit: the job hooks, and the options of the command line
    JASPERENGINE_API void engineInit(const std::vector<std::string>& args);
    // Engine::systemDraw, after the commit: the built-in monitors; with --profile-capture, the
    // capture starts once every scene is loaded and is written when its frames are in
    // ("Profiler capture written: <path>")
    JASPERENGINE_API void engineFrameEnd(bool scenesLoaded);
    JASPERENGINE_API void engineShutdown();

    // the process's resident memory, bytes (0 where the platform does not say)
    JASPERENGINE_API uint64_t processResidentBytes();

#endif

}

#endif /* EngineProfiling_h */
