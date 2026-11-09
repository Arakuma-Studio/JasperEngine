// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "yaml-cpp/yaml.h"
#include "hub/RecentProjects.h"

namespace jasperengine::editor {

// Preferences > Logging (settings.yaml "logging"). Defaults are the editor's behaviour
// before categories existed: Info everywhere, the log file at Info.
struct LoggingSettings {
    std::map<std::string, std::string> categoryLevels;   // category -> level name (only those changed)
    std::string fileLevel = "info";
    bool backtraceOnError = true;
    bool showSource = true;

    bool operator==(const LoggingSettings& other) const {
        return categoryLevels == other.categoryLevels && fileLevel == other.fileLevel &&
               backtraceOnError == other.backtraceOnError && showSource == other.showSource;
    }
};

// Preferences > Version Control (settings.yaml "version_control", docs/editor/version-control.md,
// docs/specs/S29-version-control.md D2): this user's tools and cadences. The project's own choice
// (provider, LFS, lockable scenes) is in project.yaml.
enum class LockOnEdit : int { Off, Ask, Auto };

struct VersionControlSettings {
    std::string gitPath;            // "" finds git on PATH
    std::string svnPath;            // "" finds svn on PATH
    LockOnEdit lockOnEdit = LockOnEdit::Ask;
    int pollSeconds = 10;           // 2..120
    int lockPollSeconds = 60;       // 0 off, 30..600
    int autoFetchMinutes = 0;       // 0 off
    bool commitAndPush = false;     // the Commit window's "Commit and Push", remembered
    bool showChanges = true;        // the Hierarchy's "Show VCS Changes"
    bool codeEditorChanges = true;  // the code editor's change markers

    bool operator==(const VersionControlSettings& other) const {
        return gitPath == other.gitPath && svnPath == other.svnPath && lockOnEdit == other.lockOnEdit &&
               pollSeconds == other.pollSeconds && lockPollSeconds == other.lockPollSeconds &&
               autoFetchMinutes == other.autoFetchMinutes && commitAndPush == other.commitAndPush && showChanges == other.showChanges &&
               codeEditorChanges == other.codeEditorChanges;
    }
    bool operator!=(const VersionControlSettings& other) const { return !(*this == other); }
};

// Preferences > Diagnostics (settings.yaml "diagnostics", docs/crash-diagnostics.md): the main
// thread's hang watchdog and the notice of an abnormal previous session. Editor-wide.
struct DiagnosticsSettings {
    static constexpr int minReportAfterSeconds = 1;
    static constexpr int maxReportAfterSeconds = 60;
    static constexpr int maxBreakLuaAfterSeconds = 120;

    bool hangWatchdog = true;
    int reportAfterSeconds = 3;       // the watchdog's threshold, 1..60
    int breakLuaAfterSeconds = 10;    // runaway Lua in Play is interrupted after it, 0..120 (0: never)
    bool showCrashNotice = true;      // "JasperEngine closed unexpectedly last time" at startup

    bool operator==(const DiagnosticsSettings& other) const {
        return hangWatchdog == other.hangWatchdog && reportAfterSeconds == other.reportAfterSeconds &&
               breakLuaAfterSeconds == other.breakLuaAfterSeconds && showCrashNotice == other.showCrashNotice;
    }
};

struct PanelVisibilitySettings {
    bool hierarchy = true;
    bool inspector = true;
    bool project = true;
    bool console = true;
    bool animation = true;
    bool poseAnimation = false;
    bool animator = false;
    bool terrain = false;
    bool textureSynthesis = false;
    bool audioMixer = false;
    bool particleEffect = false;
    bool soundBrowser = false;
    bool graphEditor = false;
    bool vertexAnimationBaker = false;
    bool physicsPlacement = false;
    bool spriteNormals = false;
    bool mcp = false;
    bool game = true;
    bool undoHistory = true;

    bool operator==(const PanelVisibilitySettings& other) const {
        return hierarchy == other.hierarchy
            && inspector == other.inspector
            && project == other.project
            && console == other.console
            && animation == other.animation
            && poseAnimation == other.poseAnimation
            && animator == other.animator
            && terrain == other.terrain
            && textureSynthesis == other.textureSynthesis
            && audioMixer == other.audioMixer
            && particleEffect == other.particleEffect
            && soundBrowser == other.soundBrowser
            && graphEditor == other.graphEditor
            && vertexAnimationBaker == other.vertexAnimationBaker
            && physicsPlacement == other.physicsPlacement
            && spriteNormals == other.spriteNormals
            && mcp == other.mcp
            && game == other.game
            && undoHistory == other.undoHistory;
    }

    bool operator!=(const PanelVisibilitySettings& other) const {
        return !(*this == other);
    }
};

// The Game view's Custom device (Device > Custom...): its portrait screen and insets in pixels
struct CustomDeviceSettings {
    int width = 1080;
    int height = 2340;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
    float cornerRadius = 0.0f;
    // Screen.dpi (Constant Physical Size); 0: the computer's
    float dpi = 0.0f;

    bool operator==(const CustomDeviceSettings& other) const {
        return width == other.width && height == other.height && top == other.top && right == other.right
            && bottom == other.bottom && left == other.left && cornerRadius == other.cornerRadius && dpi == other.dpi;
    }
    bool operator!=(const CustomDeviceSettings& other) const { return !(*this == other); }
};

// Game view toolbar state
struct GameViewSettings {
    int aspect = 0;              // index into the Game view's aspect list (0 = Free Aspect)
    float scale = 1.0f;          // zoom of the game image
    // S41: a Pixel Perfect Camera's scale k this view forces (0 = Auto, the largest that fits; 1..8)
    int pixelScale = 0;
    bool maximizeOnPlay = false;
    bool showStats = false;
    // Preview Atmosphere Quality: -1 the project's, else an AtmosphereQuality (Off..High)
    int atmosphereQuality = -1;
    // Device simulation in Play (spec S56 R5): the Device Tier (-1 Auto: none applies in the
    // editor, else a DeviceTier), its platform family (-1 Auto: Mobile with a simulated phone,
    // else Desktop; else a DevicePlatformGroup) and the thermal state (-1 the machine's, else a
    // ThermalState; not saved: a session's test)
    int deviceTier = -1;
    int devicePlatform = -1;
    int deviceThermal = -1;
    // device simulation (window/DeviceProfiles.h): a profile id ("" = none), how it is turned
    // (ScreenOrientation) and the simulated language (BCP 47, "" = the system's)
    std::string device;
    int orientation = 1;
    std::string language;
    // a fixed resolution (window/GameResolutions.h): the game renders at that size, fitted to
    // the view; "" = the aspect above (the view's own size). A device's screen wins over it.
    std::string resolution;
    int resolutionWidth = 0;
    int resolutionHeight = 0;
    float resolutionDpi = 0.0f;
    // the device's screen drawn over the game (rounded corners, cutout, home indicator) and
    // the safe area outlined (safe-area spec 6.6)
    bool deviceOverlay = true;
    bool safeAreaOutline = false;
    CustomDeviceSettings custom{};

    bool operator==(const GameViewSettings& other) const {
        return aspect == other.aspect && scale == other.scale && pixelScale == other.pixelScale
            && maximizeOnPlay == other.maximizeOnPlay && showStats == other.showStats
            && atmosphereQuality == other.atmosphereQuality
            && deviceTier == other.deviceTier && devicePlatform == other.devicePlatform && deviceThermal == other.deviceThermal
            && device == other.device && orientation == other.orientation && language == other.language
            && resolution == other.resolution && resolutionWidth == other.resolutionWidth
            && resolutionHeight == other.resolutionHeight && resolutionDpi == other.resolutionDpi
            && deviceOverlay == other.deviceOverlay && safeAreaOutline == other.safeAreaOutline && custom == other.custom;
    }
    bool operator!=(const GameViewSettings& other) const { return !(*this == other); }
};

// Scene view toolbar toggles that follow the user, not the scene (S26 R12, R16)
struct SceneViewSettings {
    bool exactPick = false;       // a click selects what is under it, never its prefab or model root
    bool alignToSurface = false;  // drops turn their up axis onto the surface normal
    bool rulers = false;          // top and left rulers in 2D, UI and orthographic axis views
    bool previewParticles = true; // animation edit previews (Animator, Animation windows) seek the Particle Systems under them
    // T1 R56: Alt+LMB paints the armed terrain or voxel brush's alternate instead of orbiting (Terrain3D's grammar)
    bool altAlternates = false;

    bool operator==(const SceneViewSettings& other) const {
        return exactPick == other.exactPick && alignToSurface == other.alignToSurface && rulers == other.rulers &&
               previewParticles == other.previewParticles && altAlternates == other.altAlternates;
    }
    bool operator!=(const SceneViewSettings& other) const { return !(*this == other); }
};

// How the Scene view draws lattice cages and edits their points (Preferences > Lattice)
struct LatticeViewSettings {
    std::array<float, 4> lineColor = {0.85f, 0.85f, 0.85f, 1.0f};     // edges at rest length
    float lineThickness = 1.0f;                                       // pixels, 1..4
    float behindOpacity = 0.35f;                                      // parts hidden by geometry, 0..1
    float pointSize = 7.0f;                                           // pixels, 3..20
    std::array<float, 4> pointColor = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> selectedPointColor = {1.0f, 0.6f, 0.0f, 1.0f};
    bool stretchColoring = true;                                      // edge colour by length / rest length
    std::array<float, 4> squashColor = {0.25f, 0.45f, 1.0f, 1.0f};    // at half the rest length
    std::array<float, 4> stretchColor = {1.0f, 0.25f, 0.2f, 1.0f};    // at twice the rest length
    int falloffCurve = 0;                                             // LatticeFalloffCurve: smooth, linear, sharp, constant
    float falloffRadius = 1.0f;                                       // lattice local units (the cage is 1 across)

    bool operator==(const LatticeViewSettings& other) const {
        return lineColor == other.lineColor && lineThickness == other.lineThickness
            && behindOpacity == other.behindOpacity && pointSize == other.pointSize
            && pointColor == other.pointColor && selectedPointColor == other.selectedPointColor
            && stretchColoring == other.stretchColoring && squashColor == other.squashColor
            && stretchColor == other.stretchColor && falloffCurve == other.falloffCurve
            && falloffRadius == other.falloffRadius;
    }
    bool operator!=(const LatticeViewSettings& other) const { return !(*this == other); }
};

// Window > Analysis > Profiler and Frame Debugger (S15 R34): editor-wide, never in the project
struct ProfilerSettings {
    int budgetFps = 60;          // the frame budget line: 30, 60, 90, 120, 144 or 240 fps (0: none)
    bool showCpu = true;         // the modules shown in the module list
    bool showGpu = true;
    bool showRendering = true;
    bool showMemory = true;
    bool showMonitors = true;
    int cpuView = 0;             // detail pane: 0 Hierarchy, 1 Timeline
    int sortColumn = 1;          // Hierarchy: 0 name, 1 total, 2 self, 3 calls
    bool sortAscending = false;
    bool pauseOnSpike = false;   // stop recording on a frame over pauseOverMs
    float pauseOverMs = 50.0f;
    int memoryGroup = 0;         // GPU Memory table: 0 by category, 1 by owner
    std::vector<std::string> monitors{"FPS", "Draw calls", "VRAM estimated"};
    int debuggerMode = 0;        // Frame Debugger preview: Color, Alpha, Depth, R, G, B

    bool operator==(const ProfilerSettings& other) const {
        return budgetFps == other.budgetFps && showCpu == other.showCpu && showGpu == other.showGpu &&
            showRendering == other.showRendering && showMemory == other.showMemory && showMonitors == other.showMonitors &&
            cpuView == other.cpuView && sortColumn == other.sortColumn && sortAscending == other.sortAscending &&
            pauseOnSpike == other.pauseOnSpike && pauseOverMs == other.pauseOverMs && memoryGroup == other.memoryGroup &&
            monitors == other.monitors && debuggerMode == other.debuggerMode;
    }
    bool operator!=(const ProfilerSettings& other) const { return !(*this == other); }
};

// Compiler paths and job count are machine-specific, so they live in the project's
// own .jasperengine/user/build.yaml rather than in project.yaml, and travel with the
// project when it moves.
struct LocalBuildSettings {
    std::string cCompiler;
    std::string cxxCompiler;
    std::string generator;
    unsigned int buildJobs = 0; // 0 = automatic
    bool configured = false;    // set on read: false = no entry, so default toolchain
};

class AppSettings {
private:
    static std::filesystem::path configFilePath;
    static YAML::Node settingsData;
    // settingsData as last read from (or written to) the file
    static YAML::Node loadedSettings;
    static bool autosaveEnabled;
    static int autosaveIntervalSeconds;
    static bool networkOffline;
    // settingsData parsed into the members
    static bool applySettings(const YAML::Node& node);
    static bool writeMerged();
    
    // Recent projects list (max 10)
    static std::vector<hub::RecentProject> recentProjects;
    static std::filesystem::path lastProjectPath;

    // Last compiler kit chosen in Editor Settings, so new projects inherit it
    // instead of silently defaulting each time.
    static std::string lastCMakeCCompiler;
    static std::string lastCMakeCxxCompiler;
    static std::string lastCMakeGenerator;

    // Emscripten SDK root override for Web exports ("" = auto-detect).
    // Machine-specific, so stored editor-wide rather than per-project.
    static std::string emsdkPath;
    // Preferences > External Tools (S30): androidSdk, jdk, steamcmd, rcodesign, playersDir
    static std::map<std::string, std::string> externalToolPaths;

    // cmake executable override ("" = look it up on PATH). Machine-specific,
    // stored editor-wide like emsdkPath.
    static std::string cmakePath;
    // ffmpeg executable for video imports ("" = JASPER_FFMPEG, then PATH). Machine-specific,
    // stored editor-wide like emsdkPath (htmlui/VideoImporter, phase 6).
    static std::string ffmpegPath;
    static std::filesystem::path defaultExportDirectory;
    
    // Window settings. The size is physical pixels, only meaningful again at the
    // scale it was captured at; 0 marks a file from before that was tracked.
    static int windowWidth;
    static int windowHeight;
    static bool isMaximized;
    static float windowUiScale;

    // The dock layout is pixels too, but keeps the scale it was built at even if
    // the window later moves to a monitor that changes windowUiScale.
    static float layoutUiScale;

    // Project window settings
    static int resourcesIconSize;
    static int resourcesLayout;      // 0=AUTO, 1=GRID, 2=SPLIT_FILES_ONLY, 3=SPLIT
    static int resourcesItemViewStyle; // 0=CARD, 1=CLASSIC
    static float resourcesLeftPanelWidth;

    // Code editor settings
    static float codeEditorFontSize;

    // Editor viewport settings
    static bool multiViewportEnabled;
    // the Scene view draws the component UI's navigation arrows (a Button's Navigation > Visualize)
    static bool uiNavigationVisualize;
    static bool editorVSyncEnabled;
    // Preferences > General > Show Project Hub at Startup (S27 R16): "when_no_project" or "always"
    static bool projectHubAlways;
    // Preferences > General > Editor Font > CJK Font Language: auto, japanese, korean,
    // simplified_chinese or traditional_chinese (SystemFonts)
    static std::string cjkFontLanguage;
    static bool soundBrowserAutoplay;
    static bool soundBrowserMatchLoudness;
    static std::filesystem::path hubLastLocation;
    static float uiScale;
    static bool poseDockGrown;
    static PanelVisibilitySettings panelVisibility;
    static GameViewSettings gameViewSettings;
    static SceneViewSettings sceneViewSettings;
    static LatticeViewSettings latticeViewSettings;
    static ProfilerSettings profilerSettings;
    static VersionControlSettings versionControlSettings;

    // MCP server over HTTP (Editor Settings / MCP window); stdio needs no setting
    static bool mcpHttpEnabled;
    static int mcpHttpPort;
    static std::string mcpHttpToken;
    static bool mcpHttpAllowChanges;
    static void loadMcpHttpToken();
    static bool writeMcpHttpTokenFile(const std::string& token);
    static std::string mcpToolGroups;

    // HTML UI (phase-2 §3.13.4): imports of remote resources when documents load; Info
    // diagnostics logged one by one
    static bool htmlUiAutoImport;
    static bool assetAutoRefresh;
    static std::string assetWatcher;          // auto, native, polling
    static float assetPollingInterval;        // seconds, 0.25..10
    static bool assetLogReloads;
    static bool htmlUiVerboseDiagnostics;
    static bool htmlUiPatchReload;   // S17: hot reloads patch .html documents in place (else rebuild them)
    // Scripting (S23)
    static std::string scriptChangesWhilePlaying;   // reload, after_stop, stop_and_reload
    static bool reloadCppDuringPlay;
    static std::string externalScriptEditor;        // builtin, vscode, visualstudio, rider, clion, xcode, sublime, zed, system, custom
    static std::string externalScriptEditorCommand; // the custom one: {file} {line} {column} {project}

    // Undo (Editor Settings > Undo): steps per history, the memory limit of every history
    // together (MB, 0 = unlimited), standalone selection changes as steps, and the Play Mode
    // Changes review on Stop
    static int undoMaxSteps;
    static int undoMemoryLimitMB;
    static bool undoRecordSelection;
    static bool undoReviewPlayChanges;
    static LoggingSettings loggingSettings;
    static DiagnosticsSettings diagnosticsSettings;

    // Private methods
    static void ensureConfigDirectory();

public:
    static std::filesystem::path getExportTargetDir(const std::filesystem::path& projectFile, const std::string& mode);
    static bool setExportTargetDir(const std::filesystem::path& projectFile, const std::string& mode, const std::filesystem::path& targetDir);

    static LocalBuildSettings getBuildSettings(const std::filesystem::path& projectFile);
    static bool setBuildSettings(const std::filesystem::path& projectFile, const LocalBuildSettings& value);

    // Initialization
    // preferUserDirectory: keep the files in the per-user config directory unless the working
    // directory already has a settings.yaml (MCP mode, started in the client's folder)
    static bool initialize(bool preferUserDirectory = false);
    // An explicit settings file (tests)
    static bool initializeWithFile(const std::filesystem::path& file);

    // Directory where editor config files live (settings.yaml, layout ini, ...)
    static std::filesystem::path getConfigDirectory();

    // Project settings
    static std::filesystem::path getLastProjectPath();
    static void setLastProjectPath(const std::filesystem::path& path);
    
    // File > Recent Projects: the first 10 recent projects that still exist
    static std::vector<std::filesystem::path> getRecentProjects();
    // every record (S27 R21: the Project Hub's list, missing projects included)
    static const std::vector<hub::RecentProject>& getRecentProjectRecords();
    static void setRecentProjectRecords(const std::vector<hub::RecentProject>& records);
    // `path` opened now (its name when known)
    static void addToRecentProjects(const std::filesystem::path& path, bool needSave = true, const std::string& name = std::string());
    static void clearRecentProjects();

    // Last compiler kit (empty strings = Default toolchain)
    static std::string getLastCMakeCCompiler();
    static std::string getLastCMakeCxxCompiler();
    static std::string getLastCMakeGenerator();
    static void setLastCMakeKit(const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator);

    // Emscripten SDK root override for Web exports ("" = auto-detect)
    static std::string getEmsdkPath();
    static void setEmsdkPath(const std::string& path);
    static std::string getExternalToolPath(const std::string& tool);
    static void setExternalToolPath(const std::string& tool, const std::string& path);

    // cmake executable override ("" = look it up on PATH)
    static std::string getCMakePath();
    static void setCMakePath(const std::string& path);
    // ffmpeg executable for the video importer ("" = JASPER_FFMPEG, then PATH)
    static std::string getFfmpegPath();
    static void setFfmpegPath(const std::string& path);
    static std::filesystem::path getDefaultExportDirectory();
    static void setDefaultExportDirectory(const std::filesystem::path& path);
    
    // Window settings
    static int getWindowWidth();
    static int getWindowHeight();
    static bool getIsMaximized();
    
    static float getWindowUiScale();
    static float getLayoutUiScale();

    static void setWindowWidth(int width);
    static void setWindowHeight(int height);
    static void setIsMaximized(bool maximized);
    static void setWindowUiScale(float scale);
    static void setLayoutUiScale(float scale);
    
    // Project window settings
    static int getResourcesIconSize();
    static int getResourcesLayout();
    static int getResourcesItemViewStyle();
    static float getResourcesLeftPanelWidth();
    
    static void setResourcesIconSize(int size);
    static void setResourcesLayout(int layout);
    static void setResourcesItemViewStyle(int style);
    static void setResourcesLeftPanelWidth(float width);

    // Code editor settings
    static constexpr float defaultCodeEditorFontSize = 14.0f; // VSCode default
    static constexpr float minCodeEditorFontSize = 8.0f;
    static constexpr float maxCodeEditorFontSize = 40.0f;

    static float getCodeEditorFontSize();
    static void setCodeEditorFontSize(float size);

    // Multi-viewport: allow dockable windows to be dragged out into their own OS window
    static bool getMultiViewportEnabled();
    static void setMultiViewportEnabled(bool enabled);
    static bool getUINavigationVisualize();
    static void setUINavigationVisualize(bool visualize);

    // The Pose Animation window grew the bottom dock once on its first open (not again)
    static bool getPoseDockGrown();
    static void setPoseDockGrown(bool grown);

    // VSync for the editor's own frames; Play mode and builds use the project setting
    static bool getEditorVSyncEnabled();
    static void setEditorVSyncEnabled(bool enabled);
    // Preferences > General > Editor Font > CJK Font Language (SystemFonts::isLanguageSetting)
    static std::string getCjkFontLanguage();
    static void setCjkFontLanguage(const std::string& language);
    // Show Project Hub at Startup: Always (true) or When No Project (false, the default)
    static bool getProjectHubAlways();
    static void setProjectHubAlways(bool always);
    // the Sound Browser's auditions (S37): on selecting a card, and at -20 LUFS-M
    static bool getSoundBrowserAutoplay();
    static void setSoundBrowserAutoplay(bool autoplay);
    static bool getSoundBrowserMatchLoudness();
    static void setSoundBrowserMatchLoudness(bool matched);
    // the Project Hub's last parent folder of a new or imported project ("" before any)
    static std::filesystem::path getHubLastLocation();
    static void setHubLastLocation(const std::filesystem::path& location);

    // Editor UI Scale (Preferences): multiplies the display scale the system reports
    static float getUiScale();
    static void setUiScale(float scale);

    // Panels toggled from the View menu
    static PanelVisibilitySettings getPanelVisibility();
    static void setPanelVisibility(const PanelVisibilitySettings& visibility);

    static GameViewSettings getGameViewSettings();
    static void setGameViewSettings(const GameViewSettings& settings);
    static SceneViewSettings getSceneViewSettings();
    static void setSceneViewSettings(const SceneViewSettings& settings);

    // Lattice cage drawing and point editing; set clamps every value to its range
    static LatticeViewSettings getLatticeViewSettings();
    static void setLatticeViewSettings(const LatticeViewSettings& settings);

    static ProfilerSettings getProfilerSettings();
    static void setProfilerSettings(const ProfilerSettings& settings);

    // MCP server HTTP transport: off by default, port 47615. Clients send
    // "Authorization: Bearer <token>"; the server makes the token the first time HTTP is
    // enabled. It is kept in its own file next to settings.yaml, readable by its owner only
    // (POSIX mode 0600; an mcp.http_token left in settings.yaml by older editors moves there).
    // With allow changes off, HTTP clients only get the read-only tools.
    static bool getMcpHttpEnabled();
    static int getMcpHttpPort();
    static std::string getMcpHttpToken();
    // mcp.tool_groups: the tool groups MCP sessions list besides core ("a,b" or "all")
    static std::string getMcpToolGroups();
    // The per-user config directory (settings.yaml of a desktop start, the MCP instance
    // registry): %APPDATA%/JasperEngine, ~/Library/Application Support/JasperEngine,
    // $XDG_CONFIG_HOME/jasperengine or ~/.config/jasperengine
    static std::filesystem::path getUserConfigDirectory();
    static void setMcpHttp(bool enabled, int port);
    static void setMcpHttpToken(const std::string& token);
    static std::filesystem::path getMcpHttpTokenPath();
    static bool getMcpHttpAllowChanges();
    static void setMcpHttpAllowChanges(bool allow);

    // HTML UI: download the remote resources (Google Fonts, stylesheets, images, Lottie) a
    // document references when it loads (default on); log every Info diagnostic (default off)
    // Asset Pipeline (S05 R26): Auto Refresh of files changed on disk (default on), the file
    // watcher (auto, native, polling), the polling interval (0.25-10 s), Console lines for
    // reloads (default on). Editor-wide, not per project.
    static bool getAutoRefresh();
    static void setAutoRefresh(bool enabled);
    static std::string getFileWatcher();
    static void setFileWatcher(const std::string& kind);
    static float getPollingInterval();
    static void setPollingInterval(float seconds);
    static bool getLogReloads();
    static void setLogReloads(bool enabled);

    // Scripting (S23 R8, R10): what a script saved while a scene plays does ("reload": Reload
    // And Continue Playing, the default; "after_stop": Reload After Stopping Play;
    // "stop_and_reload": Stop Playing And Reload), and whether C++ scripts reload too
    // (default off). Editor-wide.
    static std::string getScriptChangesWhilePlaying();
    static void setScriptChangesWhilePlaying(const std::string& policy);
    static bool getReloadCppDuringPlay();
    static void setReloadCppDuringPlay(bool enabled);
    // External Tools > External Script Editor (S23 R28): where Console locations and "Open in
    // External Editor" go (default the built-in Code Editor), and the Custom command template
    static std::string getExternalScriptEditor();
    static void setExternalScriptEditor(const std::string& id);
    static std::string getExternalScriptEditorCommand();
    static void setExternalScriptEditorCommand(const std::string& command);

    static bool getHtmlUiAutoImport();
    static void setHtmlUiAutoImport(bool enabled);
    static bool getHtmlUiVerboseDiagnostics();
    static void setHtmlUiVerboseDiagnostics(bool enabled);
    static bool getHtmlUiPatchReload();
    static void setHtmlUiPatchReload(bool patch);

    // Undo settings, applied to CommandHistory/Project when set or loaded (clamped to their ranges)
    static constexpr int minUndoMaxSteps = 50;
    static constexpr int maxUndoMaxSteps = 10000;
    static constexpr int defaultUndoMaxSteps = 1000;
    static constexpr int minUndoMemoryLimitMB = 64;
    static constexpr int maxUndoMemoryLimitMB = 8192;
    static constexpr int defaultUndoMemoryLimitMB = 512;
    static int getUndoMaxSteps();
    static int getUndoMemoryLimitMB();
    static bool getUndoRecordSelection();
    static bool getUndoReviewPlayChanges();
    static void setUndo(int maxSteps, int memoryLimitMB, bool recordSelection, bool reviewPlayChanges);
    static void applyUndoSettings();
    // Preferences > Logging; set applies them to the logger at once (then --log-level again)
    static LoggingSettings getLoggingSettings();
    static void setLoggingSettings(const LoggingSettings& settings);
    static void applyLoggingSettings();
    // Preferences > Diagnostics (clamped); set applies them to the running watchdog at once
    static DiagnosticsSettings getDiagnosticsSettings();
    static void setDiagnosticsSettings(const DiagnosticsSettings& settings);
    // Preferences > Version Control (clamped to their ranges); saved at once
    static VersionControlSettings getVersionControlSettings();
    static void setVersionControlSettings(const VersionControlSettings& settings);

    // Load and save settings
    static bool loadSettings();
    // Locked read-merge-write: only the top-level keys this process changed since its load
    // are written over the file's, recent projects merge as an ordered union (docs/data-safety.md)
    static bool saveSettings();

    // Scene Auto Save (Editor Settings > General): a backup of each modified scene, never the
    // scene file itself
    static constexpr int defaultAutosaveInterval = 60;
    static constexpr int minAutosaveInterval = 30;
    static constexpr int maxAutosaveInterval = 300;
    static bool isAutosaveEnabled();
    static void setAutosaveEnabled(bool enabled);
    static int getAutosaveIntervalSeconds();
    static void setAutosaveIntervalSeconds(int seconds);

    // Preferences > General > Network > Offline (settings.yaml network.offline): the editor
    // makes no network request of its own (Lospec, S43 R8) and says so before trying
    static bool isNetworkOffline();
    static void setNetworkOffline(bool offline);
};

} // namespace jasperengine::editor
