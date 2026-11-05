// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "render/RenderStats.h"
#include "imgui.h"
#include "json.hpp"

#include "EditorHost.h"
#include "PlatformMenu.h"
#include "Project.h"
#include "command/UndoTimeline.h"
#include "layout/LayoutManager.h"
#include "shortcuts/CommandCatalog.h"
#include "shortcuts/ShortcutDispatcher.h"


#include "hub/ProjectHubWindow.h"
#include "recorder/RecorderWindow.h"

#include <array>
#include <future>

#include "window/dialog/ProjectSaveDialog.h"
#include "window/dialog/SceneSaveDialog.h"
#include "window/dialog/DataSafetyDialogs.h"
#include "window/dialog/ExportWindow.h"
#include "window/dialog/EditorSettingsWindow.h"
#include "window/dialog/ProjectSettingsWindow.h"
#include "window/dialog/PrefabsWindow.h"
#include "window/dialog/AboutWindow.h"
#include "window/dialog/ScenesWindow.h"


#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct ImGuiDockNode;

namespace jasperengine::editor::mcp {
    class McpActivityWindow;
    struct McpLaunchOptions;
}

namespace jasperengine::editor::vcs {
    class VcsUi;
}

namespace jasperengine::editor{

    class InspectorWindow;
    class HierarchyWindow;
    class ConsoleWindow;
    class SceneWindow;
    class ProjectWindow;
    class ImageViewerWindow;
    class CodeEditor;
    class AnimationWindow;
    class PoseAnimationWindow;
    class AnimatorWindow;
    class AvatarWindow;
    class TerrainEditWindow;
    class TextureSynthesisWindow;
    class AudioMixerWindow;
    class VertexAnimationBakerWindow;
    class PhysicsPlacementWindow;
    class SystemsWindow;
    class SpriteNormalsWindow;
    class GameWindow;
    class MainToolbar;
    class StatusBar;
    class PlayInput;
    class LoadingWindow;
    class MissingAssetsWindow;
    class AssetReferencesWindow;
    class SpriteNormalsWatcher;
    class JobsMemoryWindow;
    class MeshAuditWindow;
    class BuildReportWindow;
    class ProfilerWindow;
    class FrameDebuggerWindow;
    class UndoHistoryWindow;
    class InputActionsWindow;
    class NavigationWindow;
    class PlayModeChangesWindow;
    class SearchPalette;
    class ShortcutsWindow;
    class BookmarksWindow;

    // A dialog waiting for an answer, as MCP reports it to agents (App::getOpenDialog)
    struct EditorDialogInfo {
        std::string kind;                  // "alert", "save_dialog", "modal", or "progress" (the
                                           // build progress, which closes by itself)
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

        std::thread::id mainThreadId;
        // the editor scheduler's Real clock
        std::chrono::steady_clock::time_point lastSchedulerTime{};

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
        std::vector<float> frameTimes;
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
        int benchmarkGpuInstancing = -1;   // --gpu-instancing on|off (-1: the project's)
        int benchmarkDepthPrepass = -1;
        int benchmarkLitQuality = -1;   // --lit-quality: 0 standard, 1 high
        // --play: the scene is measured playing (scripts, animation, physics), not in edit mode
        bool benchmarkPlay = false;
        bool benchmarkPlayStarted = false;
        int benchmarkPhase = 0;
        int benchmarkPhaseFrames = 0;
        std::vector<float> benchmarkFps;
        std::vector<uint32_t> benchmarkDraws;
        std::vector<uint32_t> benchmarkInstances;
        std::vector<uint64_t> benchmarkTris;
        // the game frame's render counters (S55 R7, by kind of pass) and its draw-and-commit CPU
        // time (Engine::getFrameDrawCpuTime), per measured frame
        std::vector<RenderStats> benchmarkRenderStats;
        std::vector<double> benchmarkRenderCpuMs;
        // --ui-stats (S16 R29): the scene's HTML UI statistics, summed over the measured frames
        bool benchmarkUiStats = false;
        std::vector<std::pair<std::string, double>> benchmarkUiTotals;
        std::chrono::steady_clock::time_point benchmarkWaitStart{};
        std::chrono::steady_clock::time_point benchmarkMeasureStart{};
        // --gpu-passes: the GPU time of every pass of the measured frames (S15); --profile-capture
        // <path>: a .jetrace.json of them
        bool benchmarkGpuPasses = false;
        bool benchmarkProfile = false;
        std::filesystem::path benchmarkCapturePath;
        uint32_t benchmarkCaptureId = 0;
        std::vector<double> benchmarkCpuMs;
        std::vector<double> benchmarkGpuFrameMs;
        struct BenchmarkPass {
            std::string label;
            std::vector<double> frameMs;    // the label's passes summed, per measured frame
            std::vector<double> counts;
        };
        std::vector<BenchmarkPass> benchmarkGpuPassList;
        uint64_t benchmarkFirstFrame = 0;
        uint64_t benchmarkLastFrame = 0;
        uint64_t benchmarkGpuCollected = 0;
        int benchmarkDrainFrames = 0;
        std::chrono::steady_clock::time_point continuousFramesUntil{};
        void collectBenchmarkGpuFrame();

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
        PoseAnimationWindow* poseAnimationWindow;
        AnimatorWindow* animatorWindow;
        AvatarWindow* avatarWindow = nullptr;
        TerrainEditWindow* terrainEditWindow;
        TextureSynthesisWindow* textureSynthesisWindow;
        AudioMixerWindow* audioMixerWindow;
        VertexAnimationBakerWindow* vertexAnimationBakerWindow;
        PhysicsPlacementWindow* physicsPlacementWindow;
        SpriteNormalsWindow* spriteNormalsWindow;
        // which generated normal maps are stale, and their opt-in auto-regenerate
        SpriteNormalsWatcher* spriteNormalsWatcher;
        mcp::McpActivityWindow* mcpActivityWindow;
        SystemsWindow* systemsWindow;
        MissingAssetsWindow* missingAssetsWindow;
        AssetReferencesWindow* assetReferencesWindow;
        JobsMemoryWindow* jobsMemoryWindow;
        MeshAuditWindow* meshAuditWindow;
        BuildReportWindow* buildReportWindow;
        ProfilerWindow* profilerWindow;
        FrameDebuggerWindow* frameDebuggerWindow;
        UndoHistoryWindow* undoHistoryWindow;
        InputActionsWindow* inputActionsWindow;
        // Window > AI > Navigation (S19)
        NavigationWindow* navigationWindow;
        PlayModeChangesWindow* playModeChangesWindow;
        // S25: Edit > Search All..., Edit > Shortcuts... (and Help > Keyboard Shortcuts),
        // Window > General > Bookmarks
        SearchPalette* searchPalette;
        ShortcutsWindow* shortcutsWindow;
        BookmarksWindow* bookmarksWindow;
        PlayInput* playInput;
        GameWindow* gameWindow;
        MainToolbar* mainToolbar;
        StatusBar* statusBar;
        // S29: version control (Commit, History, Scene Diff, Conflicts, the banners and menus)
        vcs::VcsUi* vcsUi;

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


        // Scale the persisted dock layout was written at, and whether the
        // restored layout has been matched to the current one.
        float layoutUiScale = 0.0f;
        bool layoutScaleApplied = false;

        // Sizes scaleDockChildren() capped, so scaling back down restores them.
        struct CappedDockSize {
            float capped;
            float uncapped;
        };
        std::map<ImGuiID, CappedDockSize> cappedDockSizes;

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
        // Assets > Create > Native Plugin...
        NativePluginDialog nativePluginDialog;
        PrefabsWindow prefabsWindow;
        AboutWindow aboutWindow;
        // S27: the Project Hub, and whether it covers the editor's own temporary project (then no
        // project is open: Esc has nothing to return to, MCP answers "no project open")
        ProjectHubWindow projectHub;
        RecorderWindow recorderWindow;
        void setupRecorder();
        bool hubOverTemp = false;
        // a project being made from a template off the main thread (R18)
        struct HubCreation {
            std::future<std::string> copy;     // the error, empty on success
            hub::ProjectTemplate info;
            std::string name;
            std::filesystem::path dir;
        };
        std::unique_ptr<HubCreation> hubCreation;
        bool hubCreationOpening = false;
        bool hubCreationPrompt = true;
        std::string hubCreationError;
        std::string hubMessage;
        // File > Save Project as Template...
        bool saveTemplatePopup = false;
        std::array<char, 128> saveTemplateName{};
        std::array<char, 512> saveTemplateDescription{};
        std::string saveTemplateMessage;

        void setupProjectHub();
        bool startupShowsHub();
        void hubOpenProject(const std::filesystem::path& dir, bool withoutLastScene);
        void hubCreateProject(const hub::ProjectTemplate& info, const std::string& name, const std::filesystem::path& dir);
        void updateHubCreation();
        void drawSaveProjectAsTemplate();
        // .jasperengine/user/hub-thumbnail.png from the Scene view (R21), as the project closes
        void writeHubThumbnail();
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
            Code,
            // the Pose Animation window, or a Scene view posing its character: its keys are the
            // window's (its edits' undo is the timeline's, as project-history steps)
            PoseAnimation,
            // the newest undoable step across the scene and project histories is a clip commit
            // made through the editor UI, whatever has focus (animation clips spec 9.R5b/R5c,
            // AnimationClipAppHooks::ownsUndo): the Animation window keeps its keys while it takes
            // them (the timeline undoes the commit, its newest step)
            AnimationClip,
            // the Sprite Normal Map window: its paint strokes first, then the scene of its last
            // Save & Assign while that assign is the scene's newest step, else the timeline
            SpriteNormals
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

        // S25: the command catalog's actions (AppCommands.cpp), the one key dispatcher and the
        // editor scripts' menu items as commands
        ShortcutDispatcher shortcutDispatcher{CommandCatalog::editor()};
        uint64_t scriptCommandsRevision = 0;
        PlatformMenuModel lastMenuModel;
        void registerCommands();
        void registerPanelClaims();
        void syncScriptCommands();
        ShortcutState shortcutState() const;
        void processShortcuts();
        // the Animation window (clip mode) and the Pose Animation window's key routing (S25 contexts)
        bool clipWindowTakesKeys() const;
        bool poseKeysFromSceneView() const;
        bool poseKeysActive() const;
        bool animationOrPoseOwnsLetterKeys() const;
        void recordMenuItems(const PlatformMenuModel& menu);
        // Window > Layouts (also the toolbar's Layout menu)
        LayoutManager layouts;
        std::vector<PlatformMenuItem> layoutMenuItems();
        void setupLayouts();
        // Default: Unity's layout, its panels open (what Reset Layout always did)
        void applyDefaultLayout();
        std::map<std::string, bool> panelStates() const;
        void applyPanelStates(const std::map<std::string, bool>& panels);

        PlatformMenuModel buildMenuModel();
        // The project's editor-script menu items (EditorScripts) in the main menu
        void addScriptMenus(PlatformMenuModel& menu);
        // A menu with editor-script items opened: their ---@validate functions run
        void validateScriptMenus(const PlatformMenuItem& menu);
        // Where Ctrl+Z and Edit > Undo go now (UndoTimeline::resolveTarget)
        UndoTimeline::Target undoTarget() const;
        // Edit > Undo/Redo and Ctrl+Z/Ctrl+Y: the Code editor's own, or one timeline step
        void undoRedo(bool undo);
        // Esc and Ctrl+Z in a drag, popups and focus loss (S24 R12)
        void handleInteractiveEdits(bool popupOpen);
        // The ImGui widget being dragged (an Inspector slider, a colour picker, an Animation
        // window key): its edits are one InteractiveEdit, opened by its first command
        InteractiveEdit widgetEdit;
        ImGuiID widgetEditItem = 0;
        void executeMenuCommand(const PlatformMenuCommand& command);
        void showImGuiMenuItems(const std::vector<PlatformMenuItem>& items);
        void showImGuiMenu(const PlatformMenuModel& menu);
        void showMenu();
        void showAlert();
        void showFooter();
        // "JasperEngine closed unexpectedly last time" under the menu bar, after the S03
        // recovery modal (Preferences > Diagnostics > Show Crash Notice On Startup)
        void showCrashNotice();
        // "The editor was not responding for 7.4 s (...)" above the footer for 8 s
        void showHangToast();
        void buildDockspace(bool resetLayout = false);
        void buildDefaultLayout();
        // Matches a layout restored from the ini to the current UI scale.
        void rescaleRestoredLayout();
        void scaleDockChildren(ImGuiDockNode* node, float ratio);
        // The layout scale and capped panel sizes, in their own section of the layout ini
        void registerLayoutSettings();
        // Follows Editor Settings > UI Scale, resizing the docked panels with it
        void applyUiScale();
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
        void pollExternalChanges();
        // main-thread time for the file watcher's batches per loop iteration (S05 R22)
        static constexpr double PUMP_BUDGET_MS = 1.0;
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

        // Between frames, before NewFrame (every backend loop): a project change, then a layout
        void beforeFrame();
        // The catalog's menu items of Window > Layouts, in an open ImGui menu (MainToolbar)
        void drawLayoutsMenu();
        LayoutManager& getLayouts() { return layouts; }
        ShortcutDispatcher& getShortcutDispatcher() { return shortcutDispatcher; }
        SearchPalette* getSearchPalette() const { return searchPalette; }
        ShortcutsWindow* getShortcutsWindow() const { return shortcutsWindow; }
        BookmarksWindow* getBookmarksWindow() const { return bookmarksWindow; }
        // Runs a main-menu command as a menu click would (the palette, run_command)
        void runMenuCommand(const PlatformMenuCommand& command) { executeMenuCommand(command); }
        // The main menu as it is now (the palette's commands, run_command's checks)
        PlatformMenuModel currentMenuModel() { return buildMenuModel(); }
        // The panels by window name, open or not (editor_status, the layouts)
        std::map<std::string, bool> openPanels() const { return panelStates(); }
        // The window with the keyboard focus (its root's ImGui name), "" when none
        std::string focusedWindowName() const;

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
        // S27: the Recorder (its window, F10, the toolbar's record button, MCP record_game)
        bool isRecording() const;
        void toggleRecording();
        RecorderWindow& getRecorderWindow() { return recorderWindow; }
        // S27: why MCP tools other than the project ones cannot run now ("" when they can)
        std::string projectUnavailableReason() const;
        // the Project Hub on a page (File > New Project..., Project Hub..., Open Project...)
        void openProjectHub(ProjectHubWindow::Page page);
        bool isProjectHubOpen() const { return projectHub.isOpen(); }
        // the hub's list of leftover staging folders changed outside it (MCP)
        void refreshHubLeftovers() { projectHub.startSweep(true); }
        // a project from a template (the Hub's Create, MCP create_project template=...): false
        // with `error` when it cannot start; the project opens once the copy is done
        bool createProjectFromTemplate(const std::string& templateId, const std::string& name, const std::filesystem::path& dir,
                                       std::string& error);
        // the template creation's state for MCP: running, and the last one's error ("" ok)
        bool isCreatingFromTemplate() const { return hubCreation != nullptr || hubCreationOpening; }
        const std::string& templateCreationError() const { return hubCreationError; }
        const std::string& getLoadingStatus() const { return loadingStatus; }
        void setLoadingPump(std::function<void(bool render)> pump);
        void reportLoadingProgress(const std::string& status = {}) override;

        void addNewSceneToDock(uint32_t sceneId) override;
        void addNewCodeWindowToDock(fs::path path, bool force = false);
        void addImageViewerWindowToDock(fs::path path, bool force = false);
        void clearSceneWindowState(uint32_t sceneId) override;
        void sceneDeleting(uint32_t sceneId) override;
        void prepareForProjectSwitch() override;
        // Runs a project switch between frames, with the loading screen (EditorHost)
        void requestProjectChange(std::function<void()> change) override;
        void playStarting(uint32_t sceneId) override;

        void handleExternalDrop(const std::vector<std::string>& paths);
        void handleExternalDragEnter();
        void handleExternalDragLeave();

        void resetLastActivatedScene() override;
        bool shouldSyncEngineApi() const override;
        void updateResourcesPath() override;
        void updateWindowTitle(const std::string& projectName) override;
        void stopTransientPreviews() override;
        // Clip files dropped onto a scene object (Scene view, Hierarchy): adds or reuses its
        // Animation Player with the clips, one scene step (AnimationClipAppHooks::dropClips).
        // False when not handled (no hook yet, or nothing to add).
        bool dispatchClipDrop(uint32_t sceneId, Entity target, const std::vector<std::string>& paths);
        void flushSceneMaterialWrites(uint32_t sceneId) override;
        void saveAllCodeEditors() override;
        void requestScenePlayFocus(uint32_t sceneId) override;
        std::string backgroundWorkConflict(const std::string& action) const override;

        // Assets > Import Unity Particle System...: a .prefab / .unity picked, imported into assets/fx (one undo step)
        void importUnityParticles();

        void registerAlert(std::string title, std::string message) override;
        void registerAlert(std::string title, std::string message, std::string note); // note is rendered highlighted below the message
        // A file is only referenceable from inside the assets root
        void registerOutsideAssetsAlert(const std::string& path);
        void registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr) override;
        void registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr, std::function<void()> onCancel = nullptr) override;
        void registerSaveDiscardAlert(std::string title, std::string message, std::function<void()> onSave, std::function<void()> onDiscard = nullptr, std::function<void()> onCancel = nullptr) override;
        void openPrefabAsset(const std::filesystem::path& path, uint32_t contextSceneId = 0, uint32_t contextRoot = 0, bool inContext = true) override;
        void openInputActions(const std::filesystem::path& path) override;
        void registerSaveSceneDialog(uint32_t sceneId, std::function<void()> callback = nullptr) override;
        DegradedSavePolicy confirmDegradedSave(const DegradedSaveRequest& request) override;
        RecoveryChoice resolveSceneRecovery(const SceneBackupInfo& backup) override;
        bool confirmOpenLockedProject(const ProjectLockInfo& info) override;
        void registerProjectSaveDialog(std::function<void()> callback = nullptr) override;

        // Thread-safe: schedules a task to run on the main/GL thread during the next frame.
        void enqueueMainThreadTask(std::function<void()> task) override;

        bool didRenderScene() const { return renderedSceneThisFrame; }
        bool hasPendingMainThreadTasks();
        // How long the idle backend loop may sleep: until the editor scheduler's next timer,
        // at most maxSeconds
        double idleWaitSeconds(double maxSeconds) const;
        // Main thread only: keeps the loop drawing while a worker thread drives the UI.
        void requestRedraw();
        // The editor's drawn frames since the last call (editor_status frame_ms, S05 R31):
        // {median, p99, max, frames}, each frame's time from its start to its present (an idle
        // loop draws on demand: the time between two frames would be the idling)
        nlohmann::json takeFrameStats();
        void recordFrameTime(double ms);
        bool consumeRedrawRequest();
        // One frame, without waking the loop: for animation that runs while idle.
        void requestFrame();
        bool consumeFrameRequest();
        // Every view redraws every frame for `seconds` from now, or while a profiler capture
        // runs (the profiler's captures, the Frame Debugger, an agent waiting for GPU times)
        void requestContinuousFrames(double seconds);
        bool wantsContinuousFrames() const;
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
        ProjectWindow* getProjectWindow() const override;
        AnimationWindow* getAnimationWindow() const;
        PoseAnimationWindow* getPoseAnimationWindow() const;
        AnimatorWindow* getAnimatorWindow() const { return animatorWindow; }
        AvatarWindow* getAvatarWindow() const;
        // the Avatar Configuration window on the Model of `entity` (anim-retarget 9)
        void openAvatarWindow(uint32_t sceneId, Entity entity);
        HierarchyWindow* getHierarchyWindow() const;
        TerrainEditWindow* getTerrainEditWindow() const;
        SystemsWindow* getSystemsWindow() const;
        JobsMemoryWindow* getJobsMemoryWindow() const;
        ProfilerWindow* getProfilerWindow() const { return profilerWindow; }
        FrameDebuggerWindow* getFrameDebuggerWindow() const { return frameDebuggerWindow; }
        UndoHistoryWindow* getUndoHistoryWindow() const { return undoHistoryWindow; }
        vcs::VcsUi* getVcsUi() const { return vcsUi; }
        PlayModeChangesWindow* getPlayModeChangesWindow() const { return playModeChangesWindow; }
        TextureSynthesisWindow* getTextureSynthesisWindow() const;
        VertexAnimationBakerWindow* getVertexAnimationBakerWindow() const;
        PhysicsPlacementWindow* getPhysicsPlacementWindow() const;
        SpriteNormalsWindow* getSpriteNormalsWindow() const;
        SpriteNormalsWatcher* getSpriteNormalsWatcher() const;
        void openSpriteNormals(const std::vector<std::filesystem::path>& images, bool editRecipe) override;
        void openSpriteNormalsForEntity(uint32_t sceneId, Entity entity, unsigned submesh, bool now) override;
        MainToolbar* getMainToolbar() const;
        void openTextureSynthesis(const std::filesystem::path& image, bool editRecipe) override;
        void openAudioMixerWindow(const std::filesystem::path& mixer = {}) override;
        void openBuildReport(const std::filesystem::path& report) override;
        void openMeshAudit() override;
        void openConsole() override;
        void openVertexAnimationBaker(const std::filesystem::path& path, bool bakeNow) override;
        // Game view: setOpen()/isOpen() toggle it (Window menu), requestFocus() shows it
        GameWindow* getGameWindow() const;
        SceneWindow* getSceneWindow() const;
        ConsoleWindow* getConsoleWindow() const;
        MissingAssetsWindow* getMissingAssetsWindow() const { return missingAssetsWindow; }
        void migrateAssetReferences();
        AssetReferencesWindow* getAssetReferencesWindow() const { return assetReferencesWindow; }
        // Preferences (Edit > Preferences), for the MCP focus_window tool
        EditorSettingsWindow& getEditorSettingsWindow() { return editorSettingsWindow; }
        // Project Settings (the Search palette's settings rows open it at their page)
        ProjectSettingsWindow& getProjectSettingsWindow() { return projectSettingsWindow; }
        // Preferences on one of its pages ("Lattice" from the Scene view's lattice menu)
        void openPreferences(const std::string& category);
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
