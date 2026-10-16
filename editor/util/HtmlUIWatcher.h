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
    //
    // p5-tooling: each change reloads once, whichever path sees it first. The poll waits for a
    // document's changed files to settle (settleSec since their last write: files written back
    // to back are one edit, one reload), and holds while an MCP call that may write files runs
    // (`hold`): the call's own writes (import_files' manifest, files and rewrite) are reloaded by
    // its sync in one go, and what changed meanwhile by the next call's pick-up or the next poll.
    class HtmlUIWatcher {
    public:
        static constexpr double intervalSec = 0.25;
        static constexpr double settleSec = 0.3;

        // App::engineRender, every frame (throttled inside). Returns the ids of the scenes in
        // which a document reloaded, so their views redraw.
        std::vector<uint32_t> update(Project* project, bool hold = false);
        // the next update checks now (Project::notifyHtmlUIFileSaved)
        void requestCheck();

    private:
        std::chrono::steady_clock::time_point lastCheck{};
        bool checkRequested = false;
    };

}
