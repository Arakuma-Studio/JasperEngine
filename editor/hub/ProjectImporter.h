// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Project Hub's imports (docs/specs/S27-editor-tools.md R22, R23, R34): a project from a zip
// archive (miniz) or a Git URL (the git executable, argv only, through ProcessStream). Both write
// to a staging folder next to the target and move the project into place only when it is
// complete; a failure or a cancel leaves nothing behind. Zip entries that are absolute, carry a
// drive letter, "..", a backslash, or are symbolic links or devices refuse the archive; so do more
// than 8 GiB uncompressed, more than 200,000 entries, or less free disk than the archive needs.
// The Git lookup and argv-only clone follow OpenEngine's ProjectAcquisition.cpp (MIT, NOTICE.md).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::hub {

    struct ZipLimits {
        uint64_t maxTotalBytes = uint64_t(8) << 30;
        size_t maxEntries = 200000;
    };

    // a fraction done (0-1) and a line of the log; false from the callback cancels
    using ImportProgress = std::function<bool(float fraction, const std::string& line)>;

    namespace importer {

        // Extracts every entry of `zip` into `destination` (made), after checking all of them.
        // False with `error` on a refused archive (nothing extracted) or a failure / cancel
        // (what was extracted stays for the caller to remove).
        bool extractZip(const std::filesystem::path& zip, const std::filesystem::path& destination, const ZipLimits& limits,
                        const ImportProgress& progress, std::string& error);

        // the shallowest folder (depth at most 2) of `root` holding a project.yaml; empty for none
        std::filesystem::path findProjectRoot(const std::filesystem::path& root);

        // A zip archive's project into <parent>/<its folder name> (or the archive's name when
        // project.yaml is at its top): `projectDir` set on success.
        bool importZip(const std::filesystem::path& zip, const std::filesystem::path& parent, const ZipLimits& limits,
                       const ImportProgress& progress, std::filesystem::path& projectDir, std::string& error);

        // the git executable: the configured one (Preferences > Version Control), PATH, then the
        // Windows default install folders; empty with `why` when there is none
        std::filesystem::path findGit(const std::string& configured, std::string& why);
        // "Receiving objects:  45% (9/20)" -> 0.45 within its phase; -1 for a line without a percent
        float gitProgress(const std::string& line);

        // git clone --depth 1 --progress -- <url> into <parent>/<repository name>, then git lfs
        // pull when .gitattributes has filter=lfs (a warning line when git-lfs is missing)
        bool importGit(const std::string& url, const std::filesystem::path& parent, const std::string& configuredGit,
                       const ImportProgress& progress, std::filesystem::path& projectDir, std::string& error);

        // the repository's folder name of a URL ("https://h/team/game.git" -> "game")
        std::string repositoryName(const std::string& url);

    }

    // One import at a time on a thread of its own, for the Hub window and MCP's import_project
    class ProjectImporter {
    public:
        ~ProjectImporter();

        // a zip path or a Git URL (https://, ssh://, user@host:path)
        bool start(const std::string& source, const std::filesystem::path& parent, const std::string& configuredGit, std::string& error);
        void cancel();
        bool running() const { return active && !finished; }
        bool done() const { return finished; }
        bool succeeded() const { return finished && ok; }
        float progress() const { return fraction; }
        std::vector<std::string> logTail(size_t lines) const;
        std::string error() const;
        std::filesystem::path projectDir() const;
        // the thread joined, the state cleared for the next import
        void reset();

        static bool isZipSource(const std::string& source);

    private:
        std::thread worker;
        std::atomic<bool> active{false};
        std::atomic<bool> finished{false};
        std::atomic<bool> cancelled{false};
        std::atomic<float> fraction{0.0f};
        bool ok = false;
        mutable std::mutex mutex;
        std::vector<std::string> log;
        std::string failure;
        std::filesystem::path result;
    };

}
