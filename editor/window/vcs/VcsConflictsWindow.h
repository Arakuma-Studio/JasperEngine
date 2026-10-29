// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "vcs/IVcsProvider.h"
#include "vcs/scene/SceneMerge.h"

#include <map>
#include <string>

namespace jasperengine::editor::vcs {

    class VcsUi;

    // Window > Version Control > Conflicts (docs/specs/S29-version-control.md R19): the files in
    // conflict. A scene or prefab gets Smart Merge (the three sides merged property by property by
    // SceneMerge), Ours/Theirs for each conflict left, and Mark Resolved (the file written, then
    // git add / svn resolve --accept working); other files Use Ours, Use Theirs or Open in
    // External Tool. A conflicted scene does not open until it is resolved.
    class VcsConflictsWindow {
    public:
        explicit VcsConflictsWindow(VcsUi* ui);

        void open();
        void toggle() { windowOpen = !windowOpen; }
        bool isOpen() const { return windowOpen; }
        void show();

    private:
        void startSmartMerge(const RepoPath& key);
        void remerge();
        void markResolved();

        VcsUi* ui;
        bool windowOpen = false;
        RepoPath selected;
        bool merging = false;
        bool merged = false;
        std::string error;
        std::string status;
        ConflictSides sides;
        scene::MergeResult result;
        // every conflict Smart Merge reported, by key, and the side chosen for it
        std::vector<scene::MergeConflict> conflicts;
        std::map<std::string, scene::MergeSide> resolutions;
    };

}
