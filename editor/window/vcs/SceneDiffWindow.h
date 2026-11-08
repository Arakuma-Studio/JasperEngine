// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "vcs/scene/SceneDiff.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace jasperengine::editor::vcs {

    class VcsUi;

    // Window > Version Control > Scene Diff (docs/specs/S29-version-control.md R32): an open scene
    // or prefab against its baseline (git HEAD, svn BASE, or a revision History picked) as a tree of
    // GameObjects, components and properties in their state colours; "Changed only"; a click
    // selects the GameObject in the Hierarchy; Revert to Baseline on every row. A closed file
    // shows its saved state (and opens to revert).
    class SceneDiffWindow {
    public:
        explicit SceneDiffWindow(VcsUi* ui);

        void open(uint32_t sceneId, const std::string& rev);
        // a scene or prefab file that is not open: its saved state against the baseline
        void openFile(const std::filesystem::path& file);
        void toggle() { windowOpen = !windowOpen; }
        bool isOpen() const { return windowOpen; }
        // the diff it shows, watched while the window is open (VcsUi::update)
        void watch();
        void show();

    private:
        void drawObject(const scene::ObjectDiff& object, const scene::SceneYamlModel* baseline, bool canRevert);
        void revert(const scene::ObjectDiff& object, const scene::ComponentDiff* component, const scene::PropertyDiff* property);
        // the revision compared with, and the text field's
        void setRev(const std::string& revision);

        VcsUi* ui;
        bool windowOpen = false;
        uint32_t sceneId = 0;
        std::string rev = "HEAD";
        char revInput[72] = "HEAD";
        bool changedOnly = true;
        std::string status;

        // file mode
        std::filesystem::path file;
        bool fileDiffPending = false;
        std::string fileError;
        std::shared_ptr<const scene::SceneDiffResult> fileDiff;
    };

}
