// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What Pixel Art Mode does while it is on (docs/specs/S42-pixel-art-mode.md R1, R16, R25): it
// registers the import and Play hooks (none while it is off: zero cost), runs the validator on
// a worker after an import batch (debounced 1 s, once the new textures have their import
// settings) and at Play start, and logs one Console line per batch or Play when it finds
// WARN/FAIL findings; clicking the line opens the Pixel Art Check window on its finding. New 2D
// scenes get a Main Camera with a Pixel Perfect Camera (setupNewScene).

#include "jobs/JobSystem.h"
#include "pixelart/PixelArtCheck.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    class PixelArtMode {
    public:
        static PixelArtMode& get();

        // the project's Pixel Art settings changed: the hooks follow Pixel Art Mode and its
        // Check on Import / Check on Play switches
        void projectSettingsChanged(Project* project);
        bool importHookRegistered() const { return importHook; }
        bool playHookRegistered() const { return playHook; }

        // Project::onAssetFilesChanged, only while the import hook is registered: textures added
        // or modified (project-relative)
        void assetsChanged(Project* project, const std::vector<std::string>& textures);
        // Play is starting (App::playStarting), only while the Play hook is registered
        void playStarting(Project* project);
        // every editor frame: starts a debounced check, logs a finished one
        void tick(Project* project);

        // the summary line ("Pixel Art Check: 2 FAIL, 1 WARN - hero.png is filtered bilinear on
        // Player."), "" for a clean result
        static std::string summaryLine(const CheckResult& result);
        // the finding the last logged line named (the Console click opens it)
        const std::string& lastFindingKey() const { return lastKey; }
        // what a Console click on a summary runs (the App opens the window on the finding)
        void setOpenAction(std::function<void(const std::string& findingKey)> action) { openAction = std::move(action); }
        // the Pixel Art Check window's re-run after imports (any mode): told of every asset batch
        void setAssetsListener(std::function<void()> listener) { assetsListener = std::move(listener); }
        void notifyAssets() const { if (assetsListener) assetsListener(); }

        // a new 2D scene of a Pixel Art Mode project: a Main Camera with a Pixel Perfect Camera
        // (the project's default Upscale Mode), not an undo step (part of the creation)
        static void setupNewScene(Project& project, uint32_t sceneId);

    private:
        bool importHook = false;
        bool playHook = false;
        Project* owner = nullptr;
        std::set<std::string> pendingTextures;
        bool pending = false;
        bool pendingPlay = false;
        std::chrono::steady_clock::time_point pendingSince;
        std::chrono::steady_clock::time_point lastChange;
        jobs::Task<std::shared_ptr<CheckResult>> running;
        std::string runningReason;
        std::string lastKey;
        std::function<void(const std::string&)> openAction;
        std::function<void()> assetsListener;

        void start(Project* project, const std::string& reason);
    };

}
