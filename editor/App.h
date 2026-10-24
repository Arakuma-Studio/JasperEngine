// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"

#include "EditorHost.h"
#include "PlatformMenu.h"
#include "Project.h"
#include "command/UndoTimeline.h"

#include "window/InspectorWindow.h"
#include "window/HierarchyWindow.h"
#include "window/ConsoleWindow.h"
#include "window/SceneWindow.h"
#include "window/ProjectWindow.h"
#include "window/ImageViewerWindow.h"
#include "window/CodeEditor.h"
#include "window/AnimationWindow.h"
#include "window/TerrainEditWindow.h"
#include "window/GameWindow.h"
#include "window/MainToolbar.h"
#include "window/PlayInput.h"

#include "window/LoadingWindow.h"

#include "window/dialog/ProjectSaveDialog.h"
#include "window/dialog/SceneSaveDialog.h"
#include "window/dialog/DataSafetyDialogs.h"
#include "window/dialog/ExportWindow.h"
#include "window/dialog/EditorSettingsWindow.h"
#include "window/dialog/ProjectSettingsWindow.h"
#include "window/dialog/PrefabsWindow.h"
#include "window/dialog/AboutWindow.h"
#include "window/dialog/ScenesWindow.h"

#include "render/SceneRender.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor::mcp {
    class McpActivityWindow;
    struct McpLaunchOptions;
}

namespace jasperengine::editor{

    class JobsMemoryWindow;

    // A dialog waiting for an answer, as MCP reports it to agents (App::getOpenDialog)
    struct EditorDialogInfo {
        std::string kind;                  // "alert", "save_dialog" or "modal"
        std::string title;
        std::string message;
        std::vector<std::string> choices;  // answers App::answerDialog takes; empty: a human must close it
    };

    enum class AlertType {
        Info,
        Confirm,
        ThreeButton
    };

    struct AlertData{
        bool needShow = false;
        std::string title;
        std::string message;
        std::string note; // optional highlighted note rendered below the message
        AlertType type = AlertType::Info;
        // ThreeButton labels (Unity's Save / Discard Changes / Cancel dialog renames them)
        std::string yesLabel = "Yes";
        std::string noLabel = "No";
        std::function<void()> onYes = nullptr;
        std::function<void()> onNo = nullptr;
        std::function<void()> onCancel = nullptr;
    };

    enum class SaveDialogType {
        Scene,
        Project
    };

    struct SaveDialogQueueItem {
        SaveDialogType type;
        uint32_t sceneId;  // Only used for Scene dialogs
        std::function<void()> callback = nullptr;
    };

    class App : public EditorHost{
    private:
        Project project;

        std::mutex mainThreadTaskMutex;
        std::queue<std::function<void()>> mainThreadTasks;
        std::thread::id mainThreadId;

        // Whether engineRender() drew a scene this frame, and a wake callback the
        // backend loop uses to decide when it can idle.
        bool renderedSceneThisFrame = false;
        // when a stopped scene's HTML documents want their next update (CSS animations), by scene id
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> htmlUIRedrawTimes;
        std::function<void()> wakeCallback;
        // Guards wakeCallback: worker threads (MCP transports) wake the loop while the main
        // thread may be clearing it at shutdown
        mutable std::mutex wakeMutex;

        bool redrawRequested = false;
        bool frameRequested = false;

        // Active during startup and project changes; scene editing waits until loading finishes.
        bool projectLoading = true;
        bool pumpingLoading = false;
        std::string loadingStatus = "Starting...";
        std::function<void(bool)> loadingPump;
        std::function<void()> pendingProjectChange;
        std::chrono::steady_clock::time_point lastLoadingFrame{};

        // Footer stats, sampled only from consecutive drawn frames (see engineRender).
        bool renderedScenePrevFrame = false;
        float footerFramerate = 0.0f;
        float footerDeltaMs = 0.0f;

        bool benchmarkEnabled = false;
        bool benchmarkExit = false;
        bool benchmarkFailed = false;
        // `mcp --project` on a project another editor holds: project_locked, exit code 3
        int startupExitCode = 0;
        // the window title: the project's name, then " - Level1.scene (Partial Load)" while the
        // scene shown did not load completely
        std::string windowTitleProject;
        std::string windowTitleSuffix;
        bool benchmarkCameraReady = false;
        uint32_t benchmarkSceneId = 0;
        std::filesystem::path benchmarkProject;
        std::string benchmarkScene;
        std::string benchmarkName;
        std::filesystem::path benchmarkOut;
        int benchmarkWarmupFrames = 90;
        int benchmarkMeasureFrames = 180;
        // scene setting overrides: -1 keeps the scene's value
        int benchmarkMeshLod = -1;
        int benchmarkDepthPrepass = -1;
        int benchmarkPhase = 0;
        int benchmarkPhaseFrames = 0;
        std::vector<float> benchmarkFps;
        std::vector<uint32_t> benchmarkDraws;
        std::vector<uint32_t> benchmarkInstances;
        std::vector<uint64_t> benchmarkTris;
        std::chrono::steady_clock::time_point benchmarkWaitStart{};
        std::chrono::steady_clock::time_point benchmarkMeasureStart{};

        ImGuiID dockspace_id;
        ImGuiID dock_id_middle_top;

        static ImFont* codeFont;
        static ImFont* boldFont;

        HierarchyWindow* hierarchyWindow;
        InspectorWindow* inspectorWindow;
        ConsoleWindow* consoleWindow;
        SceneWindow* sceneWindow;
        CodeEditor* codeEditor;
        ProjectWindow* projectWindow;
        ImageViewerWindow* imageViewerWindow;
        AnimationWindow* animationWindow;
        TerrainEditWindow* terrainEditWindow;
        mcp::McpActivityWindow* mcpActivityWindow;
        JobsMemoryWindow* jobsMemoryWindow;
        PlayInput* playInput;
        GameWindow* gameWindow;
        MainToolbar* mainToolbar;

        LoadingWindow* loadingWindow;

        bool isInitialized;
        bool dockspaceNeedsRebuild;

        // True only during an explicit "Reset Layout": forces tabs back to their
        // default dock slot even when the ini has a saved position for them.
        bool forceDockTabs = false;

        // captureTabOrder() mirrors the live tab order into project.tabs every
        // frame; these debounce persisting that to workspace.yaml until the user
        // stops dragging (a reorder triggers no save on its own otherwise).
        bool tabsOrderDirty = false;
        double tabsOrderChangeTime = 0.0;

        // Backing buffer for ImGui's io.IniFilename (must outlive ImGui).
        std::string layoutIniPath;

        // Scale the persisted dock layout was written at, and whether the
        // restored layout has been matched to the current one.
        float layoutUiScale = 0.0f;
        bool layoutScaleApplied = false;

        AlertData alert;
        // Bumped by every registered alert, so MCP can tell a tool raised one
        uint64_t alertSerial = 0;
        // An MCP agent's answer to the alert, pressed by showAlert on the next frame
        std::string alertPendingChoice;
        ProjectSaveDialog projectSaveDialog;
        SceneSaveDialog sceneSaveDialog;
        // Recover Unsaved Changes, Saving Would Lose Data, the second-instance dialog
        DataSafetyDialogs dataSafetyDialogs;
        ExportWindow exportWindow;
        EditorSettingsWindow editorSettingsWindow;
        ProjectSettingsWindow projectSettingsWindow;
        PrefabsWindow prefabsWindow;
        AboutWindow aboutWindow;
        ScenesWindow scenesWindow;

        std::queue<SaveDialogQueueItem> saveDialogQueue;
        bool saveDialogInProgress = false;
        std::filesystem::path lastResourcesProjectPath;

        std::vector<std::string> droppedExternalPaths;
        bool isDroppedExternalPaths;

        uint32_t lastActivatedScene;
        // lastActivatedScene was activated for the running game (Game view target), not its Scene view
        bool lastActivatedGame = false;
        uint32_t pendingResizeScene = NULL_PROJECT_SCENE;

        enum class LastFocusedWindow {
            None,
            AnySceneWindow, // Represents any of the scene-related windows (Scene, Properties, Hierarchy)
            Resources,
            Code
        } lastFocusedWindow;

        void saveFunc();
        void saveAllFunc(std::function<void(bool)> callback = nullptr);
        void saveAllAndProject(std::function<void()> onSuccess);
        void openProjectFunc();
        bool canEditSelection(bool duplicate);
        void deleteSelection();
        void duplicateSelection();
        // Edit menu Select All / Cut / Copy / Paste of GameObjects (see EntityClipboard)
        bool isSceneEditContext() const;
        bool canSelectAll();
        bool canCutSelection();
        bool canCopySelection();
        bool canPasteSelection(bool asChild);
        void selectAll();
        void copySelection(bool cut);
        void pasteSelection(bool asChild);

        PlatformMenuModel buildMenuModel();
        // Where Ctrl+Z and Edit > Undo go now (UndoTimeline::resolveTarget)
        UndoTimeline::Target undoTarget() const;
        // Edit > Undo/Redo and Ctrl+Z/Ctrl+Y: the Code editor's own, or one timeline step
        void undoRedo(bool undo);
        // Esc and Ctrl+Z in a drag, popups and focus loss (S24 R12)
        void handleInteractiveEdits(bool popupOpen);
        void executeMenuCommand(const PlatformMenuCommand& command);
        void showImGuiMenuItems(const std::vector<PlatformMenuItem>& items);
        void showImGuiMenu(const PlatformMenuModel& menu);
        void showMenu();
        void showAlert();
        void showFooter();
        void buildDockspace(bool resetLayout = false);
        void buildDefaultLayout();
        // Matches a layout restored from the ini to the current UI scale.
        void rescaleRestoredLayout();
        void dockProjectTabs();
        void dockTabWindow(const std::string& windowName, bool force = false);
        void captureTabOrder();
        std::string tabWindowName(const TabEntry& tab) const;
        ImGuiID getCentralDockId();
        void applyPanelVisibilitySettings();
        void persistPanelVisibilitySettings();
        void processNextSaveDialog();
        bool popSaveDialogQueueItem();
        // Edit-mode Game view: draws sceneProject through its game camera
        void renderGameViewPass(SceneProject& sceneProject);
        void renderGameViewSecondary(SceneProject& sceneProject);
        void renderRunningScene(SceneProject& sceneProject);
        void pollHtmlUI();
        bool wantsHtmlUIRedraw(const SceneProject& sceneProject);
        void renderSceneBesideGame(SceneProject& sceneProject);

        void closeWindow();
        bool loadMcpStartupProject(const mcp::McpLaunchOptions& options);
        void parseBenchmarkArgs(int argc, char** argv);
        bool selectBenchmarkScene();
        void failBenchmark(const std::string& message);
        void tickBenchmark();
        void finishBenchmark();

    public:
        void processMainThreadTasks() override;
        bool isMainThread() const override;

        App();

        void setup();

        void show();

        void engineInit(int argc, char** argv);
        void engineViewLoaded();
        void loadStartupProject();
        void processProjectChange();
        void engineRender();
        void engineViewDestroyed();
        void engineShutdown();

        bool isBenchmarkMode() const { return benchmarkEnabled; }
        bool consumeBenchmarkExit();
        int getExitCode() const { return startupExitCode != 0 ? startupExitCode : (benchmarkFailed ? 1 : 0); }

        bool isProjectLoading() const { return projectLoading; }
        const std::string& getLoadingStatus() const { return loadingStatus; }
        void setLoadingPump(std::function<void(bool render)> pump);
        void reportLoadingProgress(const std::string& status = {}) override;

        void addNewSceneToDock(uint32_t sceneId) override;
        void addNewCodeWindowToDock(fs::path path, bool force = false);
        void addImageViewerWindowToDock(fs::path path, bool force = false);
        void clearSceneWindowState(uint32_t sceneId) override;
        void prepareForProjectSwitch() override;
        // Runs a project switch between frames, with the loading screen (EditorHost)
        void requestProjectChange(std::function<void()> change) override;

        void handleExternalDrop(const std::vector<std::string>& paths);
        void handleExternalDragEnter();
        void handleExternalDragLeave();

        void resetLastActivatedScene() override;
        bool shouldSyncEngineApi() const override;
        void updateResourcesPath() override;
        void updateWindowTitle(const std::string& projectName) override;
        void stopTransientPreviews() override;
        void flushSceneMaterialWrites(uint32_t sceneId) override;
        void saveAllCodeEditors() override;
        void requestScenePlayFocus(uint32_t sceneId) override;
        std::string backgroundWorkConflict(const std::string& action) const override;

        void registerAlert(std::string title, std::string message) override;
        void registerAlert(std::string title, std::string message, std::string note); // note is rendered highlighted below the message
        // A file is only referenceable from inside the assets root
        void registerOutsideAssetsAlert(const std::string& path);
        void registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr) override;
        void registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr, std::function<void()> onCancel = nullptr) override;
        void registerSaveDiscardAlert(std::string title, std::string message, std::function<void()> onSave, std::function<void()> onDiscard = nullptr, std::function<void()> onCancel = nullptr) override;
        void openPrefabAsset(const std::filesystem::path& path, uint32_t contextSceneId = 0, uint32_t contextRoot = 0, bool inContext = true) override;
        void registerSaveSceneDialog(uint32_t sceneId, std::function<void()> callback = nullptr) override;
        DegradedSavePolicy confirmDegradedSave(const DegradedSaveRequest& request) override;
        RecoveryChoice resolveSceneRecovery(const SceneBackupInfo& backup) override;
        bool confirmOpenLockedProject(const ProjectLockInfo& info) override;
        void registerProjectSaveDialog(std::function<void()> callback = nullptr) override;

        // Thread-safe: schedules a task to run on the main/GL thread during the next frame.
        void enqueueMainThreadTask(std::function<void()> task) override;

        bool didRenderScene() const { return renderedSceneThisFrame; }
        bool hasPendingMainThreadTasks();
        // Main thread only: keeps the loop drawing while a worker thread drives the UI.
        void requestRedraw();
        bool consumeRedrawRequest();
        // One frame, without waking the loop: for animation that runs while idle.
        void requestFrame();
        bool consumeFrameRequest();
        // Wakes the backend loop from another thread (enqueueMainThreadTask calls it)
        void setWakeCallback(std::function<void()> cb);

        // Stops background work outliving the main loop; run before backend teardown.
        void shutdownBackgroundWork();

        static std::filesystem::path getUserCacheBaseDir();
        static std::filesystem::path getUserShaderCacheDir();

        // Monospace font used by the code editor
        static ImFont* getCodeFont();
        // The UI font, bold (Unity's overridden prefab values); the UI font when not loaded
        static ImFont* getBoldFont();

        // ImGui sizes fonts by their line box (ascent-descent) while CSS/VSCode use the em size.
        // JetBrains Mono's line box is (1020 - -300) / 1000upm = 1.32em; multiply a CSS-like
        // font size by this to get the equivalent ImGui font size.
        static constexpr float codeFontEmScale = 1.32f;

        // Tab notification helpers
        static void pushTabNotificationStyle();
        static void popTabNotificationStyle();

        Project* getProject();
        const Project* getProject() const;

        InspectorWindow* getInspectorWindow() const override;
        CodeEditor* getCodeEditor() const override;
        ImageViewerWindow* getImageViewerWindow() const override;
        ProjectWindow* getProjectWindow() const;
        AnimationWindow* getAnimationWindow() const;
        HierarchyWindow* getHierarchyWindow() const;
        TerrainEditWindow* getTerrainEditWindow() const;
        JobsMemoryWindow* getJobsMemoryWindow() const;
        // Game view: setOpen()/isOpen() toggle it (Window menu), requestFocus() shows it
        GameWindow* getGameWindow() const;
        SceneWindow* getSceneWindow() const;
        ConsoleWindow* getConsoleWindow() const;
        PlayInput* getPlayInput() const;
        void resetLayout();

        // Window settings methods. Sizes are physical pixels, converted between
        // the saved scale and the uiScale the backend is opening the window on.
        int getInitialWindowWidth(float uiScale) const;
        int getInitialWindowHeight(float uiScale) const;
        bool getInitialWindowMaximized() const;
        void saveWindowSettings(int width, int height, bool maximized, float uiScale);
        void initializeSettings();
        void exit();

        // MCP: the dialog a human would have to answer (alert, save dialog, other modal)
        bool getOpenDialog(EditorDialogInfo& out) const;
        // MCP: answers it like pressing the button (applied on the next frame)
        bool answerDialog(const std::string& choice, std::string& error);
        uint64_t getAlertSerial() const { return alertSerial; }
        // MCP busy policy: an export started in the Build Settings window is running
        bool isExportRunning() const { return exportWindow.isExporting(); }
        // MCP stdio client gone: close without save prompts, reporting unsaved changes on stderr
        void quitWithoutPrompts(const std::string& reason);
    };

}
