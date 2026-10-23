// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "LoadReport.h"
#include "SceneBackup.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class CodeEditor;
    class ProjectWindow;
    class ImageViewerWindow;
    class InspectorWindow;

    // A save that would drop what a document's load could not read (docs/data-safety.md)
    struct DegradedSaveRequest {
        // "Level1.scene"
        std::string documentName;
        std::filesystem::path path;
        // what the save would lose
        std::vector<LoadIssue> dropped;
        // run the same save again, accepting the loss
        std::function<void()> saveAnyway;
        // save to another file instead (the original keeps the data); empty when not offered
        std::function<void()> saveAs;
    };

    // Another editor holds the project's lock (.jasperengine/user/editor.lock)
    struct ProjectLockInfo {
        std::filesystem::path projectPath;
        std::string projectName;
        int64_t pid = 0;
        std::string host;
        // "09:12", empty when the owner did not record it
        std::string since;
        std::string editorVersion;
        // open it anyway (the lock is not taken)
        std::function<void()> openAnyway;
    };

    class EditorHost {
    public:
        virtual ~EditorHost() = default;

        virtual void enqueueMainThreadTask(std::function<void()> task);
        virtual void processMainThreadTasks();
        virtual bool isMainThread() const;

        virtual void addNewSceneToDock(uint32_t sceneId);
        virtual void clearSceneWindowState(uint32_t sceneId);
        virtual void prepareForProjectSwitch();
        // Runs a project switch (Project::openProjectNow, createProjectAt, loadProject) where
        // the editor can tear its windows down: between frames, with the loading screen.
        // The default runs it at once. The App ignores a request made while another is
        // pending or a project is loading.
        virtual void requestProjectChange(std::function<void()> change);
        virtual void resetLastActivatedScene();
        virtual bool shouldSyncEngineApi() const;
        virtual void updateResourcesPath();
        virtual void updateWindowTitle(const std::string& projectName);
        virtual void stopTransientPreviews();
        virtual void flushSceneMaterialWrites(uint32_t sceneId);
        virtual void saveAllCodeEditors();
        virtual void requestScenePlayFocus(uint32_t sceneId);
        // Why a human action can't start now because background work holds what it needs
        // (an MCP agent's export or C++ script build job); empty when it can.
        // action: "play" or "export".
        virtual std::string backgroundWorkConflict(const std::string& action) const;

        // Refresh the loading UI and native events at project loading checkpoints.
        virtual void reportLoadingProgress(const std::string& status = {});

        virtual void registerAlert(std::string title, std::string message);
        virtual void registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr);
        virtual void registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr, std::function<void()> onCancel = nullptr);
        // Save / Discard Changes / Cancel (Unity's dialog for leaving modified content)
        virtual void registerSaveDiscardAlert(std::string title, std::string message, std::function<void()> onSave,
            std::function<void()> onDiscard = nullptr, std::function<void()> onCancel = nullptr);
        virtual void registerSaveSceneDialog(uint32_t sceneId, std::function<void()> callback = nullptr);
        // Asked before a save that would drop data. Never through the alert defaults above:
        // the base (headless) host answers Refuse, the editor window shows "Saving Would Lose
        // Data" and answers Refuse for now, running request.saveAnyway / saveAs from its buttons.
        virtual DegradedSavePolicy confirmDegradedSave(const DegradedSaveRequest& request);
        // A scene file with a usable crash backup is opening. Never through the alert defaults:
        // the base (headless) host answers Keep (the backup stays, offered again); the editor
        // window answers Keep for now and shows "Recover Unsaved Changes", whose Restore and
        // Discard go through Project::restoreSceneFromBackup / discardSceneBackup.
        virtual RecoveryChoice resolveSceneRecovery(const SceneBackupInfo& backup);
        // Another live editor holds the project: true to open it anyway now. The base host
        // answers false (headless writers fail with project_locked); the editor window shows its
        // dialog, answers false and runs info.openAnyway from Open Anyway.
        virtual bool confirmOpenLockedProject(const ProjectLockInfo& info);

        // Unity's Open Prefab: the asset in Prefab Mode. From an instance (contextSceneId,
        // contextRoot) it opens In Context unless inContext is false; from inside a prefab
        // stage it goes on top of it. The Project window, the Hierarchy and the Inspector's
        // prefab bar all open prefabs through here.
        virtual void openPrefabAsset(const std::filesystem::path& path, uint32_t contextSceneId = 0,
            uint32_t contextRoot = 0, bool inContext = true);
        virtual void registerProjectSaveDialog(std::function<void()> callback = nullptr);

        virtual InspectorWindow* getInspectorWindow() const;
        virtual CodeEditor* getCodeEditor() const;
        virtual ProjectWindow* getProjectWindow() const;
        virtual ImageViewerWindow* getImageViewerWindow() const;
    };

    void setEditorHost(EditorHost* host);
    EditorHost& getEditorHost();

}
