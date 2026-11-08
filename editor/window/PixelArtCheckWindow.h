// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > 2D > Pixel Art Check (docs/specs/S42-pixel-art-mode.md R19): the validator for people.
// Run (F5) on a scope (Open Scenes, Build Scenes, Selection, Project) and a rules filter, the
// verdict, the findings grouped by rule with their measured and expected values, a Fix button per
// finding (its second option in the row's menu), Fix All with a preview of every change, Copy
// Report (the JSON of check_pixel_art). The rules run on a worker; the window checks again after
// a fix or an asset import.

#include "jobs/JobSystem.h"
#include "pixelart/PixelArtCheck.h"

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class CommandCatalog;
    class Project;
    class ProjectWindow;

    class PixelArtCheckWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Pixel Art Check";

        enum class Scope { OpenScenes, BuildScenes, Selection, Project };

        PixelArtCheckWindow(Project* project, ProjectWindow* projectWindow);

        void show();
        void open();
        bool isOpen() const { return windowOpen; }
        bool takesKeys() const;
        void bindCommands(CommandCatalog& catalog);
        void resetForProjectSwitch();

        // runs the check (on a worker); the window shows the result when it is in
        void run();
        // opens the window on a finding (the Console's summary line): runs, then selects it
        void focusFinding(const std::string& key);
        // the Fix button of a finding (its option; empty: the default), as one undo step
        bool fixFinding(const std::string& key, const std::string& option = std::string());
        // every fixable finding, as one undo step
        bool fixAll();
        // what Fix All's preview lists (its count is the plan's)
        size_t fixAllChangeCount() const;

        // asset imports since the last run: checks again once they settle
        void assetsChanged();

        const pixelart::CheckResult* result() const { return hasResult ? &current : nullptr; }
        void setScope(Scope value) { scope = value; }

    private:
        Project* project;
        ProjectWindow* projectWindow;
        bool windowOpen = false;
        bool focused = false;
        bool focusRequested = false;
        Scope scope = Scope::BuildScenes;
        std::set<std::string> ruleFilter;     // empty: every rule
        jobs::Task<std::shared_ptr<pixelart::CheckResult>> running;
        bool rerun = false;
        bool hasResult = false;
        pixelart::CheckResult current;
        std::string selectedKey;
        std::string pendingFocus;
        std::string notice;
        bool previewOpen = false;
        bool assetsDirty = false;
        std::chrono::steady_clock::time_point assetsChangedAt;

        void drawToolbar();
        void drawFindings();
        void drawFixAllPreview();
        void selectTarget(const pixelart::Finding& finding, bool frame);
        void collectResult();
        bool applyOptions(const std::vector<std::pair<std::string, pixelart::FixOption>>& fixes, const std::string& stepName);
    };

}
