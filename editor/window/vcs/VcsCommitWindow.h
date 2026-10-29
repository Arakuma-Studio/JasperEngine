// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Rebuilt in ImGui after OpenEngine's VcsCommitDialog (Apps/Editor/Source/VersionControl/Ui/,
// MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors; NOTICE.md): the layout of
// the change list and message only; their HTML/CSS UI is not used (docs/specs/README.md).

#pragma once

#include "vcs/VcsTypes.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor::vcs {

    class VcsUi;

    // Window > Version Control > Commit (docs/specs/S29-version-control.md R30): the changed files
    // with checkboxes (an asset and its .meta one row, R8), a filter, the state column, a
    // double-click opening the text diff or (scenes, prefabs) the Scene Diff; the message (a first
    // line of at most 72 characters), Commit and Push (git, remembered), progress with Cancel, the
    // output. Dirty selected scenes are saved first ("Save and Commit").
    class VcsCommitWindow {
    public:
        explicit VcsCommitWindow(VcsUi* ui);

        void open(const std::vector<std::filesystem::path>& preselect);
        void toggle() { windowOpen = !windowOpen; }
        bool isOpen() const { return windowOpen; }
        void show();

    private:
        struct Row {
            RepoPath key;            // the asset (or a lone .meta)
            RepoPath meta;           // its changed .meta ("" when none)
            std::string state;
            bool document = false;   // a scene or prefab
        };
        void rebuildRows();
        void commit(bool saveFirst);

        VcsUi* ui;
        bool windowOpen = false;
        uint64_t rowsVersion = ~uint64_t(0);
        std::vector<Row> rows;
        std::set<RepoPath, PathKeyLess> unchecked;     // what the user left out
        std::set<RepoPath, PathKeyLess> preselected;   // open(preselect): only these checked
        char filter[128] = {};
        char message[4096] = {};
        std::string output;
        bool outputError = false;
    };

}
