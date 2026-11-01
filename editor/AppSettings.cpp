// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AppSettings.h"
#include "Out.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/VideoImporter.h"
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace jasperengine::editor {

namespace {

// a colour as [r, g, b, a] (0..1)
void readColor(const YAML::Node& node, std::array<float, 4>& color) {
    if (!node || !node.IsSequence() || node.size() != 4) return;
    for (size_t i = 0; i < 4; i++) {
        color[i] = node[i].as<float>();
    }
}

YAML::Node colorNode(const std::array<float, 4>& color) {
    YAML::Node node(YAML::NodeType::Sequence);
    for (float channel : color) {
        node.push_back(channel);
    }
    node.SetStyle(YAML::EmitterStyle::Flow);
    return node;
}

std::filesystem::path userConfigDirectory() {
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"); appData && *appData) {
        return std::filesystem::path(appData) / "JasperEngine";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "JasperEngine";
    }
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "jasperengine";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "jasperengine";
    }
#endif
    return std::filesystem::current_path();
}

bool isWritableDirectory(const std::filesystem::path& dir) {
    const std::filesystem::path probe = dir / ".jasperengine_write_test";
    std::error_code ec;
    {
        std::ofstream f(probe);
        if (!f.is_open()) return false;
    }
    std::filesystem::remove(probe, ec);
    return true;
}

} // namespace

// Initialize static members
std::filesystem::path AppSettings::configFilePath;
YAML::Node AppSettings::settingsData;
std::vector<std::filesystem::path> AppSettings::recentProjects;
std::filesystem::path AppSettings::lastProjectPath;
std::string AppSettings::lastCMakeCCompiler;
std::string AppSettings::lastCMakeCxxCompiler;
std::string AppSettings::lastCMakeGenerator;
std::string AppSettings::emsdkPath;
std::string AppSettings::cmakePath;
std::string AppSettings::ffmpegPath;
std::filesystem::path AppSettings::defaultExportDirectory;
int AppSettings::windowWidth = 1280;
int AppSettings::windowHeight = 720;
bool AppSettings::isMaximized = false;
float AppSettings::windowUiScale = 0.0f;
float AppSettings::layoutUiScale = 0.0f;
int AppSettings::resourcesIconSize = 32;
int AppSettings::resourcesLayout = 0;
int AppSettings::resourcesItemViewStyle = 1;
float AppSettings::resourcesLeftPanelWidth = 200.0f;
float AppSettings::codeEditorFontSize = AppSettings::defaultCodeEditorFontSize;
bool AppSettings::multiViewportEnabled = false;
bool AppSettings::poseDockGrown = false;
bool AppSettings::editorVSyncEnabled = true;
float AppSettings::uiScale = 1.0f;
PanelVisibilitySettings AppSettings::panelVisibility;
GameViewSettings AppSettings::gameViewSettings;
LatticeViewSettings AppSettings::latticeViewSettings;
bool AppSettings::mcpHttpEnabled = false;
int AppSettings::mcpHttpPort = 47615;
std::string AppSettings::mcpHttpToken;
bool AppSettings::mcpHttpAllowChanges = true;
bool AppSettings::htmlUiAutoImport = true;
bool AppSettings::htmlUiVerboseDiagnostics = false;

bool AppSettings::initialize(bool preferUserDirectory) {
    // Settings live in the working directory, where a run from a build tree
    // keeps them. A desktop launcher starts the app in "/" instead, so there
    // they go to the per-user config directory: nothing could be saved
    // otherwise. A project folder is never used (settings.yaml, the layout ini
    // and editor.log would land in the game), nor, with preferUserDirectory
    // (`jasperengine-editor mcp`, started in whatever folder the MCP client is
    // in), a working directory that has no settings.yaml yet.
    const std::filesystem::path workingDir = std::filesystem::current_path();
    const std::filesystem::path workingDirSettings = workingDir / "settings.yaml";

    std::error_code ec;
    const bool projectFolder = std::filesystem::exists(workingDir / "project.yaml", ec);
    if (std::filesystem::exists(workingDirSettings, ec) && !projectFolder) {
        configFilePath = workingDirSettings;
    } else {
        const std::filesystem::path userDir = userConfigDirectory();
        if (preferUserDirectory || projectFolder || std::filesystem::exists(userDir / "settings.yaml", ec) ||
            !isWritableDirectory(workingDir)) {
            configFilePath = userDir / "settings.yaml";
        } else {
            configFilePath = workingDirSettings;
        }
    }

    ensureConfigDirectory();
    const bool loaded = loadSettings();
    loadMcpHttpToken();
    return loaded;
}

std::filesystem::path AppSettings::getMcpHttpTokenPath() {
    return getConfigDirectory() / "mcp_http_token";
}

void AppSettings::loadMcpHttpToken() {
    const std::filesystem::path path = getMcpHttpTokenPath();
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::string token;
        std::getline(in, token);
        while (!token.empty() && (token.back() == '\r' || token.back() == ' ' || token.back() == '\t')) token.pop_back();
        if (!token.empty()) mcpHttpToken = token;
#ifndef _WIN32
        // One others could read (made by hand, or copied): owner-only from now on
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
        }
#endif
        return;
    }
    // Older settings kept it in settings.yaml (mcp.http_token): it moves to its own file
    if (!mcpHttpToken.empty() && writeMcpHttpTokenFile(mcpHttpToken)) saveSettings();
}

bool AppSettings::writeMcpHttpTokenFile(const std::string& token) {
    const std::filesystem::path path = getMcpHttpTokenPath();
    const std::string content = token + "\n";
#ifdef _WIN32
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    const bool ok = static_cast<bool>(out);
#else
    // Owner-only: the token gives full control of the editor to whoever reads it. The mode
    // is set before the token is written, also on a file that was readable before.
    bool ok = false;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        ok = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
        size_t written = 0;
        while (ok && written < content.size()) {
            const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) ok = false;
            else written += static_cast<size_t>(n);
        }
        if (::close(fd) != 0) ok = false;
    }
#endif
    if (!ok) Out::error("Failed to write the MCP HTTP token to %s", path.string().c_str());
    return ok;
}

std::filesystem::path AppSettings::getConfigDirectory() {
    if (configFilePath.empty()) {
        return std::filesystem::current_path();
    }
    return configFilePath.parent_path();
}

void AppSettings::ensureConfigDirectory() {
    std::filesystem::path configDir = configFilePath.parent_path();
    if (!std::filesystem::exists(configDir)) {
        try {
            std::filesystem::create_directories(configDir);
        } catch (const std::exception& e) {
            Out::error("Failed to create config directory: %s", e.what());
        }
    }
}

bool AppSettings::loadSettings() {
    try {
        if (!std::filesystem::exists(configFilePath)) {
            settingsData = YAML::Node();
            return false;
        }
        
        settingsData = YAML::LoadFile(configFilePath.string());
        
        // Load last project path
        if (settingsData["last_project_path"]) {
            std::string path = settingsData["last_project_path"].as<std::string>();
            if (!path.empty() && std::filesystem::exists(path)) {
                lastProjectPath = std::filesystem::path(path);
            }
        }
        
        // Load last compiler kit
        if (settingsData["cmake_kit"]) {
            auto kitNode = settingsData["cmake_kit"];
            if (kitNode["c_compiler"]) lastCMakeCCompiler = kitNode["c_compiler"].as<std::string>();
            if (kitNode["cxx_compiler"]) lastCMakeCxxCompiler = kitNode["cxx_compiler"].as<std::string>();
            if (kitNode["generator"]) lastCMakeGenerator = kitNode["generator"].as<std::string>();
        }

        // Load Emscripten SDK path override
        if (settingsData["emsdk"] && settingsData["emsdk"]["path"]) {
            emsdkPath = settingsData["emsdk"]["path"].as<std::string>();
        }

        // Load cmake executable override
        if (settingsData["cmake"] && settingsData["cmake"]["path"]) {
            cmakePath = settingsData["cmake"]["path"].as<std::string>();
        }

        // Load the video importer's ffmpeg override
        if (settingsData["ffmpeg"] && settingsData["ffmpeg"]["path"]) {
            ffmpegPath = settingsData["ffmpeg"]["path"].as<std::string>();
            htmlui::VideoImporter::setFfmpegPath(ffmpegPath);
        }
        if (settingsData["export"] && settingsData["export"]["default_dir"]) {
            defaultExportDirectory = settingsData["export"]["default_dir"].as<std::string>();
        }

        // Load recent projects
        recentProjects.clear();
        if (settingsData["recent_projects"]) {
            for (const auto& path : settingsData["recent_projects"]) {
                std::string pathStr = path.as<std::string>();
                if (!pathStr.empty() && std::filesystem::exists(pathStr)) {
                    recentProjects.push_back(std::filesystem::path(pathStr));
                }
            }
        }
        
        // Load window settings
        if (settingsData["window"]) {
            auto windowNode = settingsData["window"];
            if (windowNode["width"]) {
                windowWidth = windowNode["width"].as<int>();
            }
            if (windowNode["height"]) {
                windowHeight = windowNode["height"].as<int>();
            }
            if (windowNode["maximized"]) {
                isMaximized = windowNode["maximized"].as<bool>();
            }
            if (windowNode["ui_scale"]) {
                windowUiScale = windowNode["ui_scale"].as<float>();
            }
            if (windowNode["layout_ui_scale"]) {
                layoutUiScale = windowNode["layout_ui_scale"].as<float>();
            }
        }
        
        // Load resources window settings
        if (settingsData["project_window"]) {
            auto resNode = settingsData["project_window"];
            if (resNode["icon_size"]) {
                resourcesIconSize = resNode["icon_size"].as<int>();
            }
            if (resNode["layout"]) {
                resourcesLayout = resNode["layout"].as<int>();
            }
            if (resNode["item_view_style"]) {
                resourcesItemViewStyle = resNode["item_view_style"].as<int>();
            }
            if (resNode["left_panel_width"]) {
                resourcesLeftPanelWidth = resNode["left_panel_width"].as<float>();
            }
        }

        // Load code editor settings
        if (settingsData["code_editor"]) {
            auto codeNode = settingsData["code_editor"];
            if (codeNode["font_size"]) {
                setCodeEditorFontSize(codeNode["font_size"].as<float>());
            }
        }

        // Load editor viewport settings
        if (settingsData["editor"]) {
            auto editorNode = settingsData["editor"];
            if (editorNode["multi_viewport"]) {
                multiViewportEnabled = editorNode["multi_viewport"].as<bool>();
            }
            if (editorNode["pose_dock_grown"]) {
                poseDockGrown = editorNode["pose_dock_grown"].as<bool>();
            }
            if (editorNode["vsync"]) {
                editorVSyncEnabled = editorNode["vsync"].as<bool>();
            }
            if (editorNode["ui_scale"]) {
                setUiScale(editorNode["ui_scale"].as<float>());
            }
            if (editorNode["panels"]) {
                auto panelsNode = editorNode["panels"];
                if (panelsNode["hierarchy"]) panelVisibility.hierarchy = panelsNode["hierarchy"].as<bool>();
                if (panelsNode["inspector"]) panelVisibility.inspector = panelsNode["inspector"].as<bool>();
                if (panelsNode["project"]) panelVisibility.project = panelsNode["project"].as<bool>();
                if (panelsNode["console"]) panelVisibility.console = panelsNode["console"].as<bool>();
                if (panelsNode["animation"]) panelVisibility.animation = panelsNode["animation"].as<bool>();
                if (panelsNode["poseAnimation"]) panelVisibility.poseAnimation = panelsNode["poseAnimation"].as<bool>();
                if (panelsNode["animator"]) panelVisibility.animator = panelsNode["animator"].as<bool>();
                if (panelsNode["terrain"]) panelVisibility.terrain = panelsNode["terrain"].as<bool>();
                if (panelsNode["textureSynthesis"]) panelVisibility.textureSynthesis = panelsNode["textureSynthesis"].as<bool>();
                if (panelsNode["audioMixer"]) panelVisibility.audioMixer = panelsNode["audioMixer"].as<bool>();
                if (panelsNode["vertexAnimationBaker"]) panelVisibility.vertexAnimationBaker = panelsNode["vertexAnimationBaker"].as<bool>();
                if (panelsNode["physicsPlacement"]) panelVisibility.physicsPlacement = panelsNode["physicsPlacement"].as<bool>();
                if (panelsNode["spriteNormals"]) panelVisibility.spriteNormals = panelsNode["spriteNormals"].as<bool>();
                if (panelsNode["mcp"]) panelVisibility.mcp = panelsNode["mcp"].as<bool>();
                if (panelsNode["game"]) panelVisibility.game = panelsNode["game"].as<bool>();
            }
            if (editorNode["game_view"]) {
                auto gameNode = editorNode["game_view"];
                if (gameNode["aspect"]) gameViewSettings.aspect = gameNode["aspect"].as<int>();
                if (gameNode["scale"]) gameViewSettings.scale = gameNode["scale"].as<float>();
                if (gameNode["maximize_on_play"]) gameViewSettings.maximizeOnPlay = gameNode["maximize_on_play"].as<bool>();
                if (gameNode["stats"]) gameViewSettings.showStats = gameNode["stats"].as<bool>();
                if (gameNode["device"]) gameViewSettings.device = gameNode["device"].as<std::string>();
                if (gameNode["orientation"]) gameViewSettings.orientation = gameNode["orientation"].as<int>();
                if (gameNode["language"]) gameViewSettings.language = gameNode["language"].as<std::string>();
                if (gameNode["resolution"] && gameNode["resolution"].IsMap()) {
                    const YAML::Node& resolution = gameNode["resolution"];
                    if (resolution["name"]) gameViewSettings.resolution = resolution["name"].as<std::string>();
                    if (resolution["width"]) gameViewSettings.resolutionWidth = resolution["width"].as<int>();
                    if (resolution["height"]) gameViewSettings.resolutionHeight = resolution["height"].as<int>();
                    if (resolution["dpi"]) gameViewSettings.resolutionDpi = resolution["dpi"].as<float>();
                }
                if (gameNode["device_overlay"]) gameViewSettings.deviceOverlay = gameNode["device_overlay"].as<bool>();
                if (gameNode["safe_area_outline"]) gameViewSettings.safeAreaOutline = gameNode["safe_area_outline"].as<bool>();
                if (const YAML::Node customNode = gameNode["custom_device"]; customNode && customNode.IsMap()) {
                    CustomDeviceSettings& custom = gameViewSettings.custom;
                    if (customNode["width"]) custom.width = customNode["width"].as<int>();
                    if (customNode["height"]) custom.height = customNode["height"].as<int>();
                    if (customNode["top"]) custom.top = customNode["top"].as<float>();
                    if (customNode["right"]) custom.right = customNode["right"].as<float>();
                    if (customNode["bottom"]) custom.bottom = customNode["bottom"].as<float>();
                    if (customNode["left"]) custom.left = customNode["left"].as<float>();
                    if (customNode["corner_radius"]) custom.cornerRadius = customNode["corner_radius"].as<float>();
                    if (customNode["dpi"]) custom.dpi = customNode["dpi"].as<float>();
                }
            }
        }

        // The former AI assistant's "ai_assistant" section is ignored (and dropped on save)
        if (settingsData["mcp"]) {
            auto mcpNode = settingsData["mcp"];
            if (mcpNode["http_enabled"]) mcpHttpEnabled = mcpNode["http_enabled"].as<bool>();
            if (mcpNode["http_port"]) mcpHttpPort = mcpNode["http_port"].as<int>();
            if (mcpNode["http_token"]) mcpHttpToken = mcpNode["http_token"].as<std::string>();
            if (mcpNode["allow_changes"]) mcpHttpAllowChanges = mcpNode["allow_changes"].as<bool>();
        }

        if (settingsData["lattice"]) {
            auto latticeNode = settingsData["lattice"];
            LatticeViewSettings lattice = latticeViewSettings;
            readColor(latticeNode["line_color"], lattice.lineColor);
            if (latticeNode["line_thickness"]) lattice.lineThickness = latticeNode["line_thickness"].as<float>();
            if (latticeNode["behind_opacity"]) lattice.behindOpacity = latticeNode["behind_opacity"].as<float>();
            if (latticeNode["point_size"]) lattice.pointSize = latticeNode["point_size"].as<float>();
            readColor(latticeNode["point_color"], lattice.pointColor);
            readColor(latticeNode["selected_point_color"], lattice.selectedPointColor);
            if (latticeNode["stretch_coloring"]) lattice.stretchColoring = latticeNode["stretch_coloring"].as<bool>();
            readColor(latticeNode["squash_color"], lattice.squashColor);
            readColor(latticeNode["stretch_color"], lattice.stretchColor);
            if (latticeNode["falloff_curve"]) lattice.falloffCurve = latticeNode["falloff_curve"].as<int>();
            if (latticeNode["falloff_radius"]) lattice.falloffRadius = latticeNode["falloff_radius"].as<float>();
            setLatticeViewSettings(lattice);
        }

        if (settingsData["html_ui"]) {
            auto htmlUiNode = settingsData["html_ui"];
            if (htmlUiNode["auto_import"]) htmlUiAutoImport = htmlUiNode["auto_import"].as<bool>();
            if (htmlUiNode["verbose_diagnostics"]) htmlUiVerboseDiagnostics = htmlUiNode["verbose_diagnostics"].as<bool>();
        }
        UIDocumentSystem::setVerboseDiagnostics(htmlUiVerboseDiagnostics);

        return true;
    } catch (const std::exception& e) {
        Out::error("Failed to load settings: %s", e.what());
        return false;
    }
}

bool AppSettings::saveSettings() {
    try {
        // Update settings data
        
        // Project settings
        if (!lastProjectPath.empty() && std::filesystem::exists(lastProjectPath)) {
            settingsData["last_project_path"] = lastProjectPath.string();
        } else {
            settingsData.remove("last_project_path");
        }
        
        YAML::Node recentProjectsNode;
        for (const auto& path : recentProjects) {
            recentProjectsNode.push_back(path.string());
        }
        settingsData["recent_projects"] = recentProjectsNode;

        // Last compiler kit
        if (!lastCMakeCCompiler.empty() || !lastCMakeCxxCompiler.empty() || !lastCMakeGenerator.empty()) {
            YAML::Node kitNode;
            kitNode["c_compiler"] = lastCMakeCCompiler;
            kitNode["cxx_compiler"] = lastCMakeCxxCompiler;
            kitNode["generator"] = lastCMakeGenerator;
            settingsData["cmake_kit"] = kitNode;
        } else {
            settingsData.remove("cmake_kit");
        }

        // Emscripten SDK path override
        if (!emsdkPath.empty()) {
            YAML::Node emsdkNode;
            emsdkNode["path"] = emsdkPath;
            settingsData["emsdk"] = emsdkNode;
        } else {
            settingsData.remove("emsdk");
        }

        // cmake executable override
        if (!cmakePath.empty()) {
            YAML::Node cmakeNode;
            cmakeNode["path"] = cmakePath;
            settingsData["cmake"] = cmakeNode;
        } else {
            settingsData.remove("cmake");
        }

        // the video importer's ffmpeg override
        if (!ffmpegPath.empty()) {
            YAML::Node ffmpegNode;
            ffmpegNode["path"] = ffmpegPath;
            settingsData["ffmpeg"] = ffmpegNode;
        } else {
            settingsData.remove("ffmpeg");
        }

        if (!defaultExportDirectory.empty()) {
            YAML::Node exportNode;
            exportNode["default_dir"] = defaultExportDirectory.string();
            settingsData["export"] = exportNode;
        } else {
            settingsData.remove("export");
        }
        
        // Window settings
        YAML::Node windowNode;
        windowNode["width"] = windowWidth;
        windowNode["height"] = windowHeight;
        windowNode["maximized"] = isMaximized;
        // Omitted at 0 so "never recorded" stays distinguishable from a scale.
        if (windowUiScale > 0.0f) {
            windowNode["ui_scale"] = windowUiScale;
        }
        if (layoutUiScale > 0.0f) {
            windowNode["layout_ui_scale"] = layoutUiScale;
        }
        settingsData["window"] = windowNode;
        
        // Project window settings
        YAML::Node resNode;
        resNode["icon_size"] = resourcesIconSize;
        resNode["layout"] = resourcesLayout;
        resNode["item_view_style"] = resourcesItemViewStyle;
        resNode["left_panel_width"] = resourcesLeftPanelWidth;
        settingsData["project_window"] = resNode;

        // Code editor settings
        YAML::Node codeNode;
        codeNode["font_size"] = codeEditorFontSize;
        settingsData["code_editor"] = codeNode;

        // Editor viewport settings
        YAML::Node editorNode;
        editorNode["multi_viewport"] = multiViewportEnabled;
        editorNode["pose_dock_grown"] = poseDockGrown;
        editorNode["vsync"] = editorVSyncEnabled;
        editorNode["ui_scale"] = uiScale;
        YAML::Node panelsNode;
        panelsNode["hierarchy"] = panelVisibility.hierarchy;
        panelsNode["inspector"] = panelVisibility.inspector;
        panelsNode["project"] = panelVisibility.project;
        panelsNode["console"] = panelVisibility.console;
        panelsNode["animation"] = panelVisibility.animation;
        panelsNode["poseAnimation"] = panelVisibility.poseAnimation;
        panelsNode["animator"] = panelVisibility.animator;
        panelsNode["terrain"] = panelVisibility.terrain;
        panelsNode["textureSynthesis"] = panelVisibility.textureSynthesis;
        panelsNode["audioMixer"] = panelVisibility.audioMixer;
        panelsNode["vertexAnimationBaker"] = panelVisibility.vertexAnimationBaker;
        panelsNode["physicsPlacement"] = panelVisibility.physicsPlacement;
        panelsNode["spriteNormals"] = panelVisibility.spriteNormals;
        panelsNode["mcp"] = panelVisibility.mcp;
        panelsNode["game"] = panelVisibility.game;
        editorNode["panels"] = panelsNode;
        YAML::Node gameNode;
        gameNode["aspect"] = gameViewSettings.aspect;
        gameNode["scale"] = gameViewSettings.scale;
        gameNode["maximize_on_play"] = gameViewSettings.maximizeOnPlay;
        gameNode["stats"] = gameViewSettings.showStats;
        if (!gameViewSettings.device.empty()) gameNode["device"] = gameViewSettings.device;
        if (gameViewSettings.orientation != GameViewSettings{}.orientation) gameNode["orientation"] = gameViewSettings.orientation;
        if (!gameViewSettings.language.empty()) gameNode["language"] = gameViewSettings.language;
        if (!gameViewSettings.resolution.empty()) {
            YAML::Node resolution;
            resolution["name"] = gameViewSettings.resolution;
            resolution["width"] = gameViewSettings.resolutionWidth;
            resolution["height"] = gameViewSettings.resolutionHeight;
            if (gameViewSettings.resolutionDpi > 0.0f) resolution["dpi"] = gameViewSettings.resolutionDpi;
            gameNode["resolution"] = resolution;
        }
        if (!gameViewSettings.deviceOverlay) gameNode["device_overlay"] = false;
        if (gameViewSettings.safeAreaOutline) gameNode["safe_area_outline"] = true;
        if (gameViewSettings.custom != CustomDeviceSettings{}) {
            const CustomDeviceSettings& custom = gameViewSettings.custom;
            YAML::Node customNode;
            customNode["width"] = custom.width;
            customNode["height"] = custom.height;
            customNode["top"] = custom.top;
            customNode["right"] = custom.right;
            customNode["bottom"] = custom.bottom;
            customNode["left"] = custom.left;
            customNode["corner_radius"] = custom.cornerRadius;
            if (custom.dpi > 0.0f) customNode["dpi"] = custom.dpi;
            gameNode["custom_device"] = customNode;
        }
        editorNode["game_view"] = gameNode;
        settingsData["editor"] = editorNode;

        // MCP server
        YAML::Node mcpNode;
        mcpNode["http_enabled"] = mcpHttpEnabled;
        mcpNode["http_port"] = mcpHttpPort;
        // The token lives in its own owner-only file (getMcpHttpTokenPath), not here
        mcpNode["allow_changes"] = mcpHttpAllowChanges;
        settingsData["mcp"] = mcpNode;

        YAML::Node latticeNode;
        latticeNode["line_color"] = colorNode(latticeViewSettings.lineColor);
        latticeNode["line_thickness"] = latticeViewSettings.lineThickness;
        latticeNode["behind_opacity"] = latticeViewSettings.behindOpacity;
        latticeNode["point_size"] = latticeViewSettings.pointSize;
        latticeNode["point_color"] = colorNode(latticeViewSettings.pointColor);
        latticeNode["selected_point_color"] = colorNode(latticeViewSettings.selectedPointColor);
        latticeNode["stretch_coloring"] = latticeViewSettings.stretchColoring;
        latticeNode["squash_color"] = colorNode(latticeViewSettings.squashColor);
        latticeNode["stretch_color"] = colorNode(latticeViewSettings.stretchColor);
        latticeNode["falloff_curve"] = latticeViewSettings.falloffCurve;
        latticeNode["falloff_radius"] = latticeViewSettings.falloffRadius;
        settingsData["lattice"] = latticeNode;

        YAML::Node htmlUiNode;
        htmlUiNode["auto_import"] = htmlUiAutoImport;
        htmlUiNode["verbose_diagnostics"] = htmlUiVerboseDiagnostics;
        settingsData["html_ui"] = htmlUiNode;
        if (settingsData["ai_assistant"]) settingsData.remove("ai_assistant");

        // Save to file
        std::ofstream fout(configFilePath.string());
        if (!fout) {
            Out::error("Failed to open editor settings for writing: %s", configFilePath.string().c_str());
            return false;
        }
        fout << YAML::Dump(settingsData);
        fout.close();
        if (!fout) {
            Out::error("Failed to write editor settings: %s", configFilePath.string().c_str());
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        Out::error("Failed to save settings: %s", e.what());
        return false;
    }
}

std::filesystem::path AppSettings::getLastProjectPath() {
    return lastProjectPath;
}

void AppSettings::setLastProjectPath(const std::filesystem::path& path) {
    lastProjectPath = path;
    // Also add to recent projects
    addToRecentProjects(path, false);

    saveSettings();
}

std::string AppSettings::getLastCMakeCCompiler() {
    return lastCMakeCCompiler;
}

std::string AppSettings::getLastCMakeCxxCompiler() {
    return lastCMakeCxxCompiler;
}

std::string AppSettings::getLastCMakeGenerator() {
    return lastCMakeGenerator;
}

void AppSettings::setLastCMakeKit(const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator) {
    lastCMakeCCompiler = cCompiler;
    lastCMakeCxxCompiler = cxxCompiler;
    lastCMakeGenerator = generator;
    saveSettings();
}

std::string AppSettings::getEmsdkPath() {
    return emsdkPath;
}

void AppSettings::setEmsdkPath(const std::string& path) {
    emsdkPath = path;
    saveSettings();
}

std::string AppSettings::getCMakePath() {
    return cmakePath;
}

void AppSettings::setCMakePath(const std::string& path) {
    cmakePath = path;
    saveSettings();
}

std::string AppSettings::getFfmpegPath() {
    return ffmpegPath;
}

void AppSettings::setFfmpegPath(const std::string& path) {
    ffmpegPath = path;
    htmlui::VideoImporter::setFfmpegPath(path);
    saveSettings();
}

std::filesystem::path AppSettings::getDefaultExportDirectory() {
    return defaultExportDirectory;
}

void AppSettings::setDefaultExportDirectory(const std::filesystem::path& path) {
    defaultExportDirectory = path;
    saveSettings();
}

std::vector<std::filesystem::path> AppSettings::getRecentProjects() {
    return recentProjects;
}

void AppSettings::addToRecentProjects(const std::filesystem::path& path, bool needSave) {
    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }
    
    // Check if path already exists
    auto it = std::find_if(recentProjects.begin(), recentProjects.end(),
        [&path](const std::filesystem::path& p) {
            return p == path;
        });
    
    // If found, remove it (to add it to the front)
    if (it != recentProjects.end()) {
        recentProjects.erase(it);
    }
    
    // Add to the front
    recentProjects.insert(recentProjects.begin(), path);
    
    // Keep only the most recent 10 projects
    if (recentProjects.size() > 10) {
        recentProjects.resize(10);
    }
    
    // Save changes
    if (needSave)
        saveSettings();
}

void AppSettings::clearRecentProjects() {
    recentProjects.clear();
    saveSettings();
}

int AppSettings::getWindowWidth() {
    return windowWidth;
}

int AppSettings::getWindowHeight() {
    return windowHeight;
}

bool AppSettings::getIsMaximized() {
    return isMaximized;
}

void AppSettings::setWindowWidth(int width) {
    windowWidth = width;
}

void AppSettings::setWindowHeight(int height) {
    windowHeight = height;
}

void AppSettings::setIsMaximized(bool maximized) {
    isMaximized = maximized;
}

float AppSettings::getWindowUiScale() {
    return windowUiScale;
}

void AppSettings::setWindowUiScale(float scale) {
    windowUiScale = scale;
}

float AppSettings::getLayoutUiScale() {
    return layoutUiScale;
}

void AppSettings::setLayoutUiScale(float scale) {
    layoutUiScale = scale;
}

int AppSettings::getResourcesIconSize() {
    return resourcesIconSize;
}

int AppSettings::getResourcesLayout() {
    return resourcesLayout;
}

int AppSettings::getResourcesItemViewStyle() {
    return resourcesItemViewStyle;
}

float AppSettings::getResourcesLeftPanelWidth() {
    return resourcesLeftPanelWidth;
}

void AppSettings::setResourcesIconSize(int size) {
    resourcesIconSize = size;
}

void AppSettings::setResourcesLayout(int layout) {
    resourcesLayout = layout;
}

void AppSettings::setResourcesItemViewStyle(int style) {
    resourcesItemViewStyle = style;
}

void AppSettings::setResourcesLeftPanelWidth(float width) {
    resourcesLeftPanelWidth = width;
}

float AppSettings::getCodeEditorFontSize() {
    return codeEditorFontSize;
}

void AppSettings::setCodeEditorFontSize(float size) {
    codeEditorFontSize = std::clamp(size, minCodeEditorFontSize, maxCodeEditorFontSize);
}

bool AppSettings::getMultiViewportEnabled() {
    return multiViewportEnabled;
}

void AppSettings::setMultiViewportEnabled(bool enabled) {
    multiViewportEnabled = enabled;
}

bool AppSettings::getPoseDockGrown() {
    return poseDockGrown;
}

void AppSettings::setPoseDockGrown(bool grown) {
    poseDockGrown = grown;
}

bool AppSettings::getHtmlUiAutoImport() {
    return htmlUiAutoImport;
}

void AppSettings::setHtmlUiAutoImport(bool enabled) {
    htmlUiAutoImport = enabled;
}

bool AppSettings::getHtmlUiVerboseDiagnostics() {
    return htmlUiVerboseDiagnostics;
}

void AppSettings::setHtmlUiVerboseDiagnostics(bool enabled) {
    htmlUiVerboseDiagnostics = enabled;
    UIDocumentSystem::setVerboseDiagnostics(enabled);
}

bool AppSettings::getEditorVSyncEnabled() {
    return editorVSyncEnabled;
}

void AppSettings::setEditorVSyncEnabled(bool enabled) {
    editorVSyncEnabled = enabled;
}

float AppSettings::getUiScale() {
    return uiScale;
}

void AppSettings::setUiScale(float scale) {
    // clamp lets NaN through
    uiScale = std::isnan(scale) ? 1.0f : std::clamp(scale, 0.5f, 3.0f);
}

PanelVisibilitySettings AppSettings::getPanelVisibility() {
    return panelVisibility;
}

void AppSettings::setPanelVisibility(const PanelVisibilitySettings& visibility) {
    panelVisibility = visibility;
    saveSettings();
}

GameViewSettings AppSettings::getGameViewSettings() {
    return gameViewSettings;
}

void AppSettings::setGameViewSettings(const GameViewSettings& settings) {
    gameViewSettings = settings;
    saveSettings();
}

LatticeViewSettings AppSettings::getLatticeViewSettings() {
    return latticeViewSettings;
}

// Not saved here: Preferences saves once it applies all its pages
void AppSettings::setLatticeViewSettings(const LatticeViewSettings& settings) {
    auto finiteOr = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
    auto clampColor = [](std::array<float, 4> color, const std::array<float, 4>& fallback) {
        for (size_t i = 0; i < 4; i++) {
            color[i] = std::isfinite(color[i]) ? std::clamp(color[i], 0.0f, 1.0f) : fallback[i];
        }
        return color;
    };
    const LatticeViewSettings defaults;
    LatticeViewSettings value = settings;
    value.lineColor = clampColor(value.lineColor, defaults.lineColor);
    value.lineThickness = std::clamp(finiteOr(value.lineThickness, defaults.lineThickness), 1.0f, 4.0f);
    value.behindOpacity = std::clamp(finiteOr(value.behindOpacity, defaults.behindOpacity), 0.0f, 1.0f);
    value.pointSize = std::clamp(finiteOr(value.pointSize, defaults.pointSize), 3.0f, 20.0f);
    value.pointColor = clampColor(value.pointColor, defaults.pointColor);
    value.selectedPointColor = clampColor(value.selectedPointColor, defaults.selectedPointColor);
    value.squashColor = clampColor(value.squashColor, defaults.squashColor);
    value.stretchColor = clampColor(value.stretchColor, defaults.stretchColor);
    value.falloffCurve = std::clamp(value.falloffCurve, 0, 3);
    value.falloffRadius = std::clamp(finiteOr(value.falloffRadius, defaults.falloffRadius), 0.01f, 10.0f);
    latticeViewSettings = value;
}

bool AppSettings::getMcpHttpEnabled() {
    return mcpHttpEnabled;
}

int AppSettings::getMcpHttpPort() {
    return mcpHttpPort;
}

std::string AppSettings::getMcpHttpToken() {
    return mcpHttpToken;
}

void AppSettings::setMcpHttp(bool enabled, int port) {
    mcpHttpEnabled = enabled;
    mcpHttpPort = port;
    saveSettings();
}

void AppSettings::setMcpHttpToken(const std::string& token) {
    mcpHttpToken = token;
    writeMcpHttpTokenFile(token);
}

bool AppSettings::getMcpHttpAllowChanges() {
    return mcpHttpAllowChanges;
}

void AppSettings::setMcpHttpAllowChanges(bool allow) {
    mcpHttpAllowChanges = allow;
    saveSettings();
}

} // namespace jasperengine::editor

namespace jasperengine::editor {

namespace {

// Compiler kit, job count and export destinations describe one machine, so they sit
// with the project's other per-checkout state. Keyed by absolute path in the
// editor-wide settings.yaml they broke as soon as the project moved.
std::filesystem::path localSettingsFile(const std::filesystem::path& projectFile) {
    const std::filesystem::path projectDir = projectFile.has_filename() && projectFile.filename() == "project.yaml"
        ? projectFile.parent_path()
        : projectFile;
    return projectDir / ".jasperengine" / "user" / "build.yaml";
}

std::string legacyProjectKey(const std::filesystem::path& projectFile) {
    return std::filesystem::absolute(projectFile).lexically_normal().generic_string();
}

YAML::Node loadLocalSettings(const std::filesystem::path& projectFile) {
    const std::filesystem::path file = localSettingsFile(projectFile);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return YAML::Node();
    }
    try {
        YAML::Node node = YAML::LoadFile(file.string());
        return node.IsMap() ? node : YAML::Node();
    } catch (const std::exception& e) {
        Out::warning("Ignoring unreadable local build settings \"%s\": %s", file.string().c_str(), e.what());
        return YAML::Node();
    }
}

bool storeLocalSettings(const std::filesystem::path& projectFile, const YAML::Node& node) {
    const std::filesystem::path file = localSettingsFile(projectFile);
    std::error_code ec;
    // .jasperengine/user/build.yaml: this machine's settings, which only the editor writes. Through
    // FileUtils (beneath the project in a write scope, no link followed), with a token naming this one
    // file so an MCP-set build_jobs saves while no tool can write the file itself (util/SafeWrite.h)
    if (!std::filesystem::is_directory(file.parent_path(), ec)) {
        safewrite::EditorInternal makeDir(file.parent_path());
        FileUtils::createDirectories(file.parent_path(), ec);
        if (ec) {
            Out::error("Failed to create local settings directory: %s", ec.message().c_str());
            return false;
        }
    }
    safewrite::EditorInternal ownBuildSettings(file);
    const std::string text = "# Build configuration for this machine only. Version control ignores .jasperengine/.\n" + YAML::Dump(node) + "\n";
    if (!FileUtils::writeFileReplacing(file, text)) {
        Out::error("Failed to write local build settings: %s", file.string().c_str());
        return false;
    }
    return true;
}

// One-shot move of the entries an earlier editor wrote into settings.yaml, which is
// left alone since an older editor sharing this machine still reads it.
YAML::Node migrateLegacySection(const std::filesystem::path& projectFile, YAML::Node& local,
                                const YAML::Node& editorSettings, const char* section, const char* localKey) {
    if (local[localKey]) {
        return local[localKey];
    }
    const YAML::Node root = editorSettings[section];
    if (!root || !root.IsMap()) {
        return YAML::Node();
    }
    const YAML::Node entry = root[legacyProjectKey(projectFile)];
    if (!entry || !entry.IsMap()) {
        return YAML::Node();
    }
    local[localKey] = YAML::Clone(entry);
    storeLocalSettings(projectFile, local);
    return local[localKey];
}

} // namespace

std::filesystem::path AppSettings::getExportTargetDir(const std::filesystem::path& projectFile, const std::string& mode) {
    YAML::Node local = loadLocalSettings(projectFile);
    const YAML::Node exports = migrateLegacySection(projectFile, local, settingsData, "project_exports", "exports");
    if (!exports || !exports.IsMap()) return {};
    const YAML::Node entry = exports[mode];
    if (!entry || !entry.IsMap()) return {};
    if (!entry["targetDir"]) return {};

    // The editor regenerates this file, so a value that will not convert falls back
    // to the default instead of throwing out of whichever dialog asked
    try {
        return std::filesystem::path(entry["targetDir"].as<std::string>());
    } catch (const std::exception& e) {
        Out::warning("Ignoring an export directory in \"%s\": %s", localSettingsFile(projectFile).string().c_str(), e.what());
        return {};
    }
}

bool AppSettings::setExportTargetDir(const std::filesystem::path& projectFile, const std::string& mode, const std::filesystem::path& targetDir) {
    if (getExportTargetDir(projectFile, mode) == targetDir) return true;

    YAML::Node local = loadLocalSettings(projectFile);
    if (!targetDir.empty()) {
        YAML::Node entry(YAML::NodeType::Map);
        entry["targetDir"] = targetDir.generic_string();
        local["exports"][mode] = entry;
    } else if (local["exports"]) {
        local["exports"].remove(mode);
        if (!local["exports"].size()) local.remove("exports");
    }
    return storeLocalSettings(projectFile, local);
}

LocalBuildSettings AppSettings::getBuildSettings(const std::filesystem::path& projectFile) {
    LocalBuildSettings result;
    YAML::Node local = loadLocalSettings(projectFile);
    const YAML::Node entry = migrateLegacySection(projectFile, local, settingsData, "project_builds", "build");
    if (!entry || !entry.IsMap()) return result;

    try {
        if (entry["c_compiler"]) result.cCompiler = entry["c_compiler"].as<std::string>();
        if (entry["cxx_compiler"]) result.cxxCompiler = entry["cxx_compiler"].as<std::string>();
        if (entry["generator"]) result.generator = entry["generator"].as<std::string>();
        if (entry["build_jobs"]) result.buildJobs = entry["build_jobs"].as<unsigned int>();
        result.configured = true;
    } catch (const std::exception& e) {
        // A value that will not convert falls back to the default toolchain
        Out::warning("Ignoring a build setting in \"%s\": %s", localSettingsFile(projectFile).string().c_str(), e.what());
        return LocalBuildSettings();
    }

    return result;
}

bool AppSettings::setBuildSettings(const std::filesystem::path& projectFile, const LocalBuildSettings& value) {
    const auto previous = getBuildSettings(projectFile);
    if (previous.configured && previous.cCompiler == value.cCompiler
        && previous.cxxCompiler == value.cxxCompiler && previous.generator == value.generator
        && previous.buildJobs == value.buildJobs) {
        return true;
    }

    // Written even when empty: that entry pins the project to the default toolchain
    YAML::Node entry(YAML::NodeType::Map);
    if (!value.cCompiler.empty()) entry["c_compiler"] = value.cCompiler;
    if (!value.cxxCompiler.empty()) entry["cxx_compiler"] = value.cxxCompiler;
    if (!value.generator.empty()) entry["generator"] = value.generator;
    if (value.buildJobs) entry["build_jobs"] = value.buildJobs;

    YAML::Node local = loadLocalSettings(projectFile);
    local["build"] = entry;
    return storeLocalSettings(projectFile, local);
}

} // namespace jasperengine::editor
