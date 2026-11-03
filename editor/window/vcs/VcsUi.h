// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's version control UI (docs/specs/S29-version-control.md R12, R27-R32): the Commit,
// History, Scene Diff and Conflicts windows, the text diff viewer, the menus' actions with the
// reasons disabled items give, the status bar's branch segment, the Inspector's lock banner, the
// confirmations (a revert of unsaved documents, a save of a read-only file) and lock-on-edit.
// App owns one; everything here runs on the main thread, every VCS process off it.

#include "vcs/VcsTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::vcs {

    class VcsCommitWindow;
    class VcsHistoryWindow;
    class SceneDiffWindow;
    class VcsConflictsWindow;

    // Assets > Version Control and the Project window's and Hierarchy's context menus (R29)
    enum class VcsAction : uint8_t { Commit, Revert, Lock, Unlock, ShowHistory, DiffAgainstBaseline, Refresh };

    class VcsUi {
    public:
        static constexpr const char* COMMIT_WINDOW = "Commit";
        static constexpr const char* HISTORY_WINDOW = "History";
        static constexpr const char* SCENE_DIFF_WINDOW = "Scene Diff";
        static constexpr const char* CONFLICTS_WINDOW = "Conflicts";

        explicit VcsUi(Project* project);
        ~VcsUi();

        // Every frame, before the windows draw: the service's snapshot, the diffs the consumers
        // watch, lock-on-edit
        void update();
        // Every frame: the windows and dialogs
        void show();

        // ---- menus (R29)
        // Whether an action applies to these files; false with *why (the disabled item's tooltip)
        bool canRun(VcsAction action, const std::vector<std::filesystem::path>& files, std::string* why) const;
        void run(VcsAction action, const std::vector<std::filesystem::path>& files, uint32_t sceneId = 0);
        // The Version Control submenu of a context menu (Project window, Hierarchy)
        void drawContextMenu(const std::vector<std::filesystem::path>& files, uint32_t sceneId = 0);

        void openCommit(const std::vector<std::filesystem::path>& preselect = {});
        void openHistory(const std::filesystem::path& file = {});
        // a scene or prefab against a revision (HEAD: the baseline)
        void openSceneDiff(uint32_t sceneId, const std::string& rev = "HEAD");
        void openConflicts();
        // a text diff of a file against a revision
        void openTextDiff(const std::filesystem::path& file, const std::string& rev = "HEAD");
        void toggleWindow(const char* name);
        bool isWindowOpen(const char* name) const;

        // ---- the status bar (R29): branch, ahead/behind, provider, last error; a click opens Commit
        void drawStatusBarSegment();

        // ---- the Inspector (R28): true while the files are locked by someone else (read only)
        bool drawLockBanner(const std::vector<std::filesystem::path>& files);

        // ---- Conflicts: a conflicted scene refuses to open until it is resolved (R19)
        bool isConflicted(const std::filesystem::path& file) const;

        // ---- R12: a save refused because the file is read only (an unlocked lockable file)
        void offerReadOnlySave(const std::filesystem::path& file, std::function<void()> retrySave);

        Project* getProject() const { return project; }

        // A modal question with its choices, after the ones already asked (a null action: Cancel)
        void ask(std::string title, std::string message, std::vector<std::pair<std::string, std::function<void()>>> choices);

    private:
        struct Dialog;
        void drawDialogs();
        void drawTextDiff();
        void startRevert(const std::vector<std::filesystem::path>& files);
        void lockOnEdit();
        void lockFile(const std::filesystem::path& file, std::function<void(bool)> done);

        Project* project;
        std::unique_ptr<VcsCommitWindow> commitWindow;
        std::unique_ptr<VcsHistoryWindow> historyWindow;
        std::unique_ptr<SceneDiffWindow> sceneDiffWindow;
        std::unique_ptr<VcsConflictsWindow> conflictsWindow;
        std::vector<std::unique_ptr<Dialog>> dialogs;

        // the text diff viewer
        bool textDiffOpen = false;
        std::filesystem::path textDiffFile;
        std::string textDiffRev;
        std::string textDiff;
        bool textDiffPending = false;

        // lock-on-edit: each document's dirty state last frame, and what was decided for it
        std::map<uint32_t, bool> wasModified;
        std::set<std::filesystem::path> editAnyway;        // locked by someone else: edit anyway
        std::set<std::filesystem::path> editWithoutLock;   // Ask answered "Edit Without Lock"
        bool appFocused = true;
        uint64_t projectHistorySeen = 0;        // the project history's newest step id last seen
    };

}
