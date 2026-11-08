// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's version control operations on project files (docs/specs/S29-version-control.md
// R8, R11, R19): the asset/.meta pairing, the undoable revert, the scene merge of a conflicted
// document. The UI windows and the MCP tools share them; each finishes on the main thread.

#include "vcs/VcsService.h"
#include "vcs/scene/SceneMerge.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::vcs {

    // R8: x.png and x.png.meta are one item. The keys with the changed .meta of each asset among
    // them added (a lone .meta stays as it is); lock applies to the asset only.
    std::vector<RepoPath> withChangedMetas(const VcsSnapshot& snapshot, const std::vector<RepoPath>& keys);

    // The keys of absolute paths below the repository, or false with *why for one that is not
    bool keysOf(const VcsService& service, const std::vector<std::filesystem::path>& paths, std::vector<RepoPath>& keys, std::string* why);

    // Why these files cannot be reverted (empty when they can): new files have nothing to go back
    // to (Delete them instead; svn's revert unschedules an add and keeps the file:
    // `unschedulesAdds`), unchanged ones nothing to undo
    std::string revertRefusal(const VcsSnapshot& snapshot, const std::vector<RepoPath>& keys, bool unschedulesAdds);

    // The names of the open scenes and prefab stages among the files that have unsaved changes
    // ("Revert discards unsaved changes in <scene>")
    std::vector<std::string> dirtyDocumentsAmong(Project& project, const std::vector<std::filesystem::path>& files);

    // R11: backs the files (and their .meta) up, reverts them on the worker, then pushes
    // "Revert N files" (RevertFilesCmd) to the project history and reloads what changed.
    // done(result, undo name); a failed revert pushes nothing.
    void revertFiles(Project& project, const std::vector<std::filesystem::path>& files,
                     std::function<void(const VcsResult&, const std::string& undoName)> done);

    // R19 Smart Merge: the conflict's three sides (git stages / svn's .rOLD .mine .rNEW), read on
    // the worker and merged by SceneMerge (with the resolutions chosen so far)
    struct SmartMergeOutcome {
        bool ok = false;
        std::string error;
        ConflictSides sides;
        scene::MergeResult merge;
    };
    void smartMerge(VcsService& service, const RepoPath& key, std::map<std::string, scene::MergeSide> resolutions,
                    std::function<void(const SmartMergeOutcome&)> done);

}
