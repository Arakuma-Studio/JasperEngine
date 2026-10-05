// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace jasperengine::editor {

    class CodeEditor;
    class ImageViewerWindow;
    class InspectorWindow;

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

        // Refresh the loading UI and native events at project loading checkpoints.
        virtual void reportLoadingProgress(const std::string& status = {});

        virtual void registerAlert(std::string title, std::string message);
        virtual void registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr);
        virtual void registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr, std::function<void()> onCancel = nullptr);
        // Save / Discard Changes / Cancel (Unity's dialog for leaving modified content)
        virtual void registerSaveDiscardAlert(std::string title, std::string message, std::function<void()> onSave,
            std::function<void()> onDiscard = nullptr, std::function<void()> onCancel = nullptr);
        virtual void registerSaveSceneDialog(uint32_t sceneId, std::function<void()> callback = nullptr);

        // Unity's Open Prefab: the asset in Prefab Mode. From an instance (contextSceneId,
        // contextRoot) it opens In Context unless inContext is false; from inside a prefab
        // stage it goes on top of it. The Project window, the Hierarchy and the Inspector's
        // prefab bar all open prefabs through here.
        virtual void openPrefabAsset(const std::filesystem::path& path, uint32_t contextSceneId = 0,
            uint32_t contextRoot = 0, bool inContext = true);
        virtual void registerProjectSaveDialog(std::function<void()> callback = nullptr);

        virtual InspectorWindow* getInspectorWindow() const;
        virtual CodeEditor* getCodeEditor() const;
        virtual ImageViewerWindow* getImageViewerWindow() const;
    };

    void setEditorHost(EditorHost* host);
    EditorHost& getEditorHost();

}
