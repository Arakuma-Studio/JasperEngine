// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The staging folders of the Project Hub's imports and template copies
// (docs/specs/S66-crash-leftovers.md): a copy is built in <parent>/<name>.importing and moved into
// place whole. Here is who owns such a folder, and what becomes of one whose owner died.
//
// The owner marker is the file <folder>.lock beside the folder (never in it: git clones into the
// folder, and a whole folder moves to be the project), holding an S03 lock (io::ScopedFileLock) for
// as long as the copy runs. The operating system releases it when the process dies, whatever pid
// the next process gets: a marker nobody holds has a dead owner, and its folder goes. A folder
// without a marker comes from an older editor, or may simply be a user's folder with such a name:
// it stays until someone removes it. The sweep only looks at the parents it is given and at the
// journal's folders, one level deep.

#include "io/ScopedFileLock.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace jasperengine::editor::hub::staging {

    // "<name>.importing", then "<name>.importing2", ...
    bool isStagingName(const std::string& folderName);

    // The folder of a journal: one small file per running copy, naming its staging folder, so the
    // next start finds it wherever it was (an import can go to any folder). Empty, the default:
    // no journal, and only the parents given to sweep are looked at.
    void setJournalDirectory(const std::filesystem::path& directory);

    // A free staging folder in `parent`, created empty and owned by this process until release().
    class Claim {
    public:
        Claim(const std::filesystem::path& parent, const std::string& name);
        ~Claim();

        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;

        bool ok() const { return !folderPath.empty(); }
        const std::string& error() const { return failure; }
        const std::filesystem::path& folder() const { return folderPath; }

        // Gives the folder up, after it was moved to its place or removed. One that could not be
        // removed keeps its marker: without an owner now, the next sweep retries.
        void release();

    private:
        std::filesystem::path folderPath;
        std::unique_ptr<io::ScopedFileLock> lock;
        std::string failure;
    };

    // A staging folder nothing removes by itself
    struct Leftover {
        std::filesystem::path folder;
        std::string reason;
    };

    struct Scope {
        // the folders whose staging folders (children, not deeper) are looked at
        std::vector<std::filesystem::path> parents;
        // folders never reported: the user's projects that happen to have such a name
        std::vector<std::filesystem::path> projects;
    };

    struct Sweep {
        std::vector<std::filesystem::path> removed;   // their owner was dead
        std::vector<std::filesystem::path> live;      // their owner runs: not touched
        std::vector<std::filesystem::path> failed;    // their owner was dead, the removal failed: retried next time
        std::vector<Leftover> leftovers;
    };

    // Removes the staging folders in `scope` and the journal whose owner died; lists the rest.
    // Never fails: a removal that fails is logged and stays for the next sweep.
    Sweep sweep(const Scope& scope);
    // The same look, changing nothing
    Sweep inspect(const Scope& scope);

    // Removes a folder a sweep listed as a leftover, at the user's request. False with `error` when
    // it is not a staging folder, its owner runs, or the removal failed.
    bool removeLeftover(const std::filesystem::path& folder, std::string& error);

    // Test seam: asked before a staging folder is removed; a non-empty error code makes that removal
    // fail with it. Empty function: no-op.
    using RemovalFaultInjector = std::function<std::error_code(const std::filesystem::path& folder)>;
    void setRemovalFaultInjector(RemovalFaultInjector injector);

    // Test hook (JASPER_TEST_STAGING_HOLD=<path>): a copy waits here, with part of its files in the
    // staging folder, while <path> exists, after creating <path>.held to say it got here
    void holdForTest();

}
