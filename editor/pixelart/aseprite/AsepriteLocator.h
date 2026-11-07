// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Finding and launching Aseprite (docs/specs/S44-aseprite-import.md R32, §6.8). The candidate
// list is a pure function of an injected environment so every OS layout is testable on any host;
// the version check and the launch go through the editor's argv-only process helpers (no shell).

#pragma once

#include "pixelart/PixelProblem.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::pixelart::aseprite {

    // What the search reads from the machine; fromSystem() fills it from the real OS
    struct LocatorEnvironment {
        std::string os;                    // "windows", "macos", "linux"
        std::string preferencePath;        // externalTools.aseprite.path ("" = unset)
        std::function<std::string(const std::string&)> env;                 // getenv, "" when unset
        std::function<bool(const std::filesystem::path&)> isExecutable;     // a regular file that can run
        std::function<std::string(const std::filesystem::path&)> readFile;  // "" when unreadable
        std::filesystem::path home;        // HOME (Windows: USERPROFILE)

        static LocatorEnvironment fromSystem(const std::string& preferencePath);
    };

    struct AsepriteInstall {
        std::filesystem::path executable;  // absolute
        // "preference", "JASPER_ASEPRITE", "program_files", "program_files_x86", "local_app_data",
        // "steam", "path", "applications", "user_applications", "opt"
        std::string source;
        std::string version;               // "Aseprite 1.3.10" from --version, "" when not confirmed
    };

    // The `"path"` values of a Steam libraryfolders.vdf (both the current nested layout and the
    // old `"1" "D:\\Lib"` one), unescaped
    std::vector<std::filesystem::path> steamLibraryFolders(const std::string& vdf);

    // The installs that exist, in the documented order (§6.8), each executable once. Pure: reads
    // the machine only through `env`.
    std::vector<AsepriteInstall> asepriteCandidates(const LocatorEnvironment& env);

    // The first candidate, unconfirmed; none gives `aseprite_not_found`
    std::optional<AsepriteInstall> findAseprite(const LocatorEnvironment& env, PixelProblem* problem);

    // `<exe> --version` (no shell, 3 s timeout): "Aseprite 1.3.10", or "" when it fails or the
    // program does not say it is Aseprite. Never on the main thread (vcs::runProcess).
    std::string asepriteVersion(const std::filesystem::path& executable);

    // The first candidate --version confirms, else the first candidate with an empty version (a
    // program that runs but prints nothing still opens files). Spawns processes: off the main thread.
    std::optional<AsepriteInstall> detectAseprite(const LocatorEnvironment& env, PixelProblem* problem);

    // detectAseprite on a thread of its own, once per session and preference value: the editor
    // never waits on a process to draw Preferences or a context menu
    class AsepriteLocator {
    public:
        static AsepriteLocator& instance();
        ~AsepriteLocator();

        // Detects for this preference unless a detection for it is done or under way. Any thread.
        void start(const std::string& preferencePath);
        bool ready() const;
        // Valid once ready(): the install, or nullopt with problem() saying why
        std::optional<AsepriteInstall> result() const;
        PixelProblem problem() const;
        // Forgets the result (Detect, or the preference changed): the next start() detects again
        void invalidate();

    private:
        void run();

        mutable std::mutex mutex;
        std::thread worker;
        bool running = false;              // the worker is inside run()
        bool pending = false;              // a request the worker has not taken yet
        bool requested = false;            // start() since the last invalidate()
        bool done = false;
        std::string preference;            // of the latest request
        uint64_t generation = 0;           // a result of an older request is dropped
        std::optional<AsepriteInstall> found;
        PixelProblem missing;
    };

    // Opens files in Aseprite and does not wait: absolute paths only, arguments as an array, no
    // shell. False with a problem when the executable or a file is not absolute or the start fails.
    bool openInAseprite(const AsepriteInstall& install, const std::vector<std::filesystem::path>& files, PixelProblem* problem);

}
