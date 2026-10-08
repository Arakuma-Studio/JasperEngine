// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Hot reload of HTML UI documents in edit and play mode (phase-1 spec §3.8.6). Every
    // 0.25 s, or at once after a save from the code editor, hands the files each document of
    // every open scene (playing copies included) depends on to that scene's
    // UIDocumentSystem::reloadChangedFiles. The engine keeps the per-document stamps, so a
    // change MCP's sync already reloaded is not reloaded again here.
    class HtmlUIWatcher {
    public:
        static constexpr double intervalSec = 0.25;

        // App::engineRender, every frame (throttled inside). Returns the ids of the scenes in
        // which a document reloaded, so their views redraw.
        std::vector<uint32_t> update(Project* project);
        // the next update checks now (Project::notifyHtmlUIFileSaved)
        void requestCheck();

    private:
        std::chrono::steady_clock::time_point lastCheck{};
        bool checkRequested = false;
    };

}
