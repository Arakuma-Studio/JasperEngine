// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's data-safety modals (docs/data-safety.md): "Recover Unsaved Changes" (a crash
// backup), "Saving Would Lose Data" (a save that would drop what a file's load could not read)
// and the second-instance dialog (another editor has the project open). Requests queue up and
// show one at a time; the editor answers its EditorHost questions with "not now" and the
// buttons act later.

#include "EditorHost.h"

#include <deque>
#include <functional>
#include <string>
#include <variant>

namespace jasperengine::editor {

    class Project;

    class DataSafetyDialogs {
    public:
        void requestRecovery(const SceneBackupInfo& backup);
        void requestDegradedSave(const DegradedSaveRequest& request);
        void requestLockedProject(const ProjectLockInfo& info);

        // Draws the front request (each frame, after the windows)
        void show(Project& project);
        bool isShowing() const;

        // Opens the Console filtered to a file (the Hierarchy banner's Details too)
        static void showFileInConsole(const std::string& file);

    private:
        Project* project = nullptr;
        std::deque<std::variant<SceneBackupInfo, DegradedSaveRequest, ProjectLockInfo>> queue;
        bool confirmingDiscard = false;

        void showRecovery(const SceneBackupInfo& backup);
        void showDegradedSave(const DegradedSaveRequest& request);
        void showLockedProject(const ProjectLockInfo& info);
        void pop();
    };

}
