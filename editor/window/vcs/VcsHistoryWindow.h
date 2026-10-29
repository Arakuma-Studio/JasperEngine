// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Rebuilt in ImGui after OpenEngine's VcsLogViewer (Apps/Editor/Source/VersionControl/Ui/, MIT,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors; NOTICE.md): its columns only.

#pragma once

#include "vcs/VcsTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::vcs {

    class VcsUi;

    // Window > Version Control > History (docs/specs/S29-version-control.md R31): the project's
    // log or one file's, 100 entries at a time with Load More; revision, author, date, message. A
    // file's revision offers Diff with Working Copy (a scene or prefab: the Scene Diff against that
    // revision) and Copy Revision.
    class VcsHistoryWindow {
    public:
        static constexpr int kPage = 100;

        explicit VcsHistoryWindow(VcsUi* ui);

        void open(const std::filesystem::path& file);
        void toggle() { windowOpen = !windowOpen; }
        bool isOpen() const { return windowOpen; }
        void show();

    private:
        void load(bool more);

        VcsUi* ui;
        bool windowOpen = false;
        std::filesystem::path file;          // empty: the project
        std::vector<VcsLogEntry> entries;
        bool loading = false;
        bool exhausted = false;
        bool loaded = false;
        std::string error;
        uint64_t generation = 0;             // a new file drops the answers for the old one
        int selected = -1;
    };

}
