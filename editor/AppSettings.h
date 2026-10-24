// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "yaml-cpp/yaml.h"

namespace jasperengine::editor {

struct PanelVisibilitySettings {
    bool hierarchy = true;
    bool inspector = true;
    bool project = true;
    bool console = true;
    bool animation = true;
    bool terrain = false;
    bool textureSynthesis = false;
    bool physicsPlacement = false;
    bool mcp = false;
    bool game = true;

    bool operator==(const PanelVisibilitySettings& other) const {
        return hierarchy == other.hierarchy
            && inspector == other.inspector
            && project == other.project
            && console == other.console
            && animation == other.animation
            && terrain == other.terrain
            && textureSynthesis == other.textureSynthesis
            && physicsPlacement == other.physicsPlacement
            && mcp == other.mcp
            && game == other.game;
    }

    bool operator!=(const PanelVisibilitySettings& other) const {
        return !(*this == other);
    }
};

// Game view toolbar state
struct GameViewSettings {
    int aspect = 0;              // index into the Game view's aspect list (0 = Free Aspect)
    float scale = 1.0f;          // zoom of the game image
    bool maximizeOnPlay = false;
    bool showStats = false;

    bool operator==(const GameViewSettings& other) const {
        return aspect == other.aspect && scale == other.scale
            && maximizeOnPlay == other.maximizeOnPlay && showStats == other.showStats;
    }
    bool operator!=(const GameViewSettings& other) const { return !(*this == other); }
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
    
    // Recent projects list (max 10)
    static std::vector<std::filesystem::path> recentProjects;
    static std::filesystem::path lastProjectPath;

    // Last compiler kit chosen in Editor Settings, so new projects inherit it
    // instead of silently defaulting each time.
    static std::string lastCMakeCCompiler;
    static std::string lastCMakeCxxCompiler;
    static std::string lastCMakeGenerator;

    // Emscripten SDK root override for Web exports ("" = auto-detect).
    // Machine-specific, so stored editor-wide rather than per-project.
    static std::string emsdkPath;

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
    static bool editorVSyncEnabled;
    static PanelVisibilitySettings panelVisibility;
    static GameViewSettings gameViewSettings;

    // MCP server over HTTP (Editor Settings / MCP window); stdio needs no setting
    static bool mcpHttpEnabled;
    static int mcpHttpPort;
    static std::string mcpHttpToken;

    // HTML UI (phase-2 §3.13.4): imports of remote resources when documents load; Info
    // diagnostics logged one by one
    static bool htmlUiAutoImport;
    static bool htmlUiVerboseDiagnostics;

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

    // Directory where editor config files live (settings.yaml, layout ini, ...)
    static std::filesystem::path getConfigDirectory();

    // Project settings
    static std::filesystem::path getLastProjectPath();
    static void setLastProjectPath(const std::filesystem::path& path);
    
    static std::vector<std::filesystem::path> getRecentProjects();
    static void addToRecentProjects(const std::filesystem::path& path, bool needSave = true);
    static void clearRecentProjects();

    // Last compiler kit (empty strings = Default toolchain)
    static std::string getLastCMakeCCompiler();
    static std::string getLastCMakeCxxCompiler();
    static std::string getLastCMakeGenerator();
    static void setLastCMakeKit(const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator);

    // Emscripten SDK root override for Web exports ("" = auto-detect)
    static std::string getEmsdkPath();
    static void setEmsdkPath(const std::string& path);

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

    // VSync for the editor's own frames; Play mode and builds use the project setting
    static bool getEditorVSyncEnabled();
    static void setEditorVSyncEnabled(bool enabled);

    // Panels toggled from the View menu
    static PanelVisibilitySettings getPanelVisibility();
    static void setPanelVisibility(const PanelVisibilitySettings& visibility);

    static GameViewSettings getGameViewSettings();
    static void setGameViewSettings(const GameViewSettings& settings);

    // MCP server HTTP transport: off by default, port 47615. A non-empty token makes
    // clients send "Authorization: Bearer <token>".
    static bool getMcpHttpEnabled();
    static int getMcpHttpPort();
    static std::string getMcpHttpToken();
    static void setMcpHttp(bool enabled, int port);

    // HTML UI: download the remote resources (Google Fonts, stylesheets, images, Lottie) a
    // document references when it loads (default on); log every Info diagnostic (default off)
    static bool getHtmlUiAutoImport();
    static void setHtmlUiAutoImport(bool enabled);
    static bool getHtmlUiVerboseDiagnostics();
    static void setHtmlUiVerboseDiagnostics(bool enabled);

    // Load and save settings
    static bool loadSettings();
    static bool saveSettings();
};

} // namespace jasperengine::editor
