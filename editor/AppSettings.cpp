// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AppSettings.h"
#include "DocumentFile.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/PlayChangeReview.h"
#include "Out.h"
#include "util/EditorDiagnostics.h"
#include "io/AtomicFile.h"
#include "io/ScopedFileLock.h"
#include "util/FileUtils.h"
#include "util/SystemFonts.h"
#include "util/SafeWrite.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/VideoImporter.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ctime>

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

std::filesystem::path lockFileFor(const std::filesystem::path& settingsFile) {
    std::filesystem::path lock = settingsFile;
    lock += ".lock";
    return lock;
}

// An unreadable settings file is set aside, never written over: <file>.corrupt-<YYYYmmdd-HHMMSS>
void setAsideCorrupt(const std::filesystem::path& file, const std::string& reason) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    std::filesystem::path aside = file;
    aside += std::string(".corrupt-") + stamp;
    std::error_code ec;
    if (io::renameWithRetry(file, aside, ec)) {
        Out::warning("Editor settings \"%s\" could not be read (%s); set aside as \"%s\", starting from defaults.",
                     file.string().c_str(), reason.c_str(), aside.string().c_str());
    } else {
        Out::warning("Editor settings \"%s\" could not be read (%s) nor set aside: %s",
                     file.string().c_str(), reason.c_str(), ec.message().c_str());
    }
}

// The file's settings, null when absent; a file that does not parse is set aside
YAML::Node readSettingsFile(const std::filesystem::path& file) {
    std::string bytes;
    if (!DocumentFiles::readBytes(file, bytes)) {
        return YAML::Node();
    }
    try {
        YAML::Node node = YAML::Load(bytes);
        if (node.IsNull() || node.IsMap()) {
            return node;
        }
        setAsideCorrupt(file, "not a map");
    } catch (const YAML::Exception& e) {
        setAsideCorrupt(file, e.what());
    }
    return YAML::Node();
}

} // namespace

// Initialize static members
std::filesystem::path AppSettings::configFilePath;
YAML::Node AppSettings::settingsData;
YAML::Node AppSettings::loadedSettings;
bool AppSettings::autosaveEnabled = true;
int AppSettings::autosaveIntervalSeconds = AppSettings::defaultAutosaveInterval;
std::vector<hub::RecentProject> AppSettings::recentProjects;
std::filesystem::path AppSettings::lastProjectPath;
std::string AppSettings::lastCMakeCCompiler;
std::string AppSettings::lastCMakeCxxCompiler;
std::string AppSettings::lastCMakeGenerator;
std::string AppSettings::emsdkPath;
std::map<std::string, std::string> AppSettings::externalToolPaths;
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
bool AppSettings::projectHubAlways = false;
std::string AppSettings::cjkFontLanguage = "auto";
bool AppSettings::soundBrowserAutoplay = false;
bool AppSettings::soundBrowserMatchLoudness = true;
std::filesystem::path AppSettings::hubLastLocation;
float AppSettings::uiScale = 1.0f;
PanelVisibilitySettings AppSettings::panelVisibility;
GameViewSettings AppSettings::gameViewSettings;
SceneViewSettings AppSettings::sceneViewSettings;
LatticeViewSettings AppSettings::latticeViewSettings;
ProfilerSettings AppSettings::profilerSettings;
bool AppSettings::mcpHttpEnabled = false;
int AppSettings::mcpHttpPort = 47615;
std::string AppSettings::mcpHttpToken;
bool AppSettings::mcpHttpAllowChanges = true;
std::string AppSettings::mcpToolGroups;
bool AppSettings::htmlUiAutoImport = true;
bool AppSettings::assetAutoRefresh = true;
std::string AppSettings::assetWatcher = "auto";
float AppSettings::assetPollingInterval = 1.0f;
bool AppSettings::assetLogReloads = true;
std::string AppSettings::scriptChangesWhilePlaying = "reload";
bool AppSettings::reloadCppDuringPlay = false;
std::string AppSettings::externalScriptEditor = "builtin";
std::string AppSettings::externalScriptEditorCommand;
int AppSettings::undoMaxSteps = AppSettings::defaultUndoMaxSteps;
int AppSettings::undoMemoryLimitMB = AppSettings::defaultUndoMemoryLimitMB;
bool AppSettings::undoRecordSelection = true;
bool AppSettings::undoReviewPlayChanges = true;
bool AppSettings::htmlUiVerboseDiagnostics = false;
bool AppSettings::htmlUiPatchReload = true;
LoggingSettings AppSettings::loggingSettings;
DiagnosticsSettings AppSettings::diagnosticsSettings;
VersionControlSettings AppSettings::versionControlSettings;

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
    io::WriteResult result;
#ifdef _WIN32
    // (the user's profile directory keeps it from other users)
    result = io::writeFileAtomic(path, content);
#else
    // Owner-only: the token gives full control of the editor to whoever reads it. The
    // temporary file is owner-only before the token is written into it, and a file already
    // there is made so first (the publish keeps the mode of the file it replaces).
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
    }
    const std::filesystem::path temp = io::makeTemporarySiblingPath(path);
    std::filesystem::create_directories(path.parent_path(), ec);
    bool written = false;
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        written = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
        size_t done = 0;
        while (written && done < content.size()) {
            const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) written = false;
            else done += static_cast<size_t>(n);
        }
        if (::close(fd) != 0) written = false;
    }
    if (!written) {
        std::filesystem::remove(temp, ec);
        Out::error("Failed to write the MCP HTTP token to %s", path.string().c_str());
        return false;
    }
    // synced and renamed over the old token: a crash leaves the old one or the new one
    result = io::publishFile(temp, path);
#endif
    if (!result.ok()) {
        Out::error("Failed to write the MCP HTTP token to %s: %s", path.string().c_str(), io::describeWriteError(result).c_str());
    }
    return result.ok();
}

bool AppSettings::initializeWithFile(const std::filesystem::path& file) {
    configFilePath = file;
    ensureConfigDirectory();
    return loadSettings();
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
    if (!std::filesystem::exists(configFilePath)) {
        settingsData = YAML::Node();
        loadedSettings = YAML::Node();
        return false;
    }
    // under the lock: another editor may be writing it
    YAML::Node node;
    {
        io::ScopedFileLock lock(lockFileFor(configFilePath), io::ScopedFileLock::Mode::Shared);
        node = readSettingsFile(configFilePath);
    }
    if (!node.IsMap()) {
        settingsData = YAML::Node();
        loadedSettings = YAML::Node();
        return false;
    }
    return applySettings(node);
}

bool AppSettings::applySettings(const YAML::Node& node) {
    try {
        // rebound, not assigned (assigning a bound handle overwrites the node it refers to)
        settingsData.reset(node);
        // what each top-level key held when this process last read the file: a save writes
        // only the keys that changed since (another editor's changes to the others stay)
        loadedSettings.reset(YAML::Clone(node));
        
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
        if (settingsData["externalTools"] && settingsData["externalTools"].IsMap()) {
            externalToolPaths.clear();
            for (const auto& item : settingsData["externalTools"]) {
                if (item.second.IsScalar()) externalToolPaths[item.first.as<std::string>()] = item.second.as<std::string>();
            }
        }
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

        // Load recent projects: records, or the old list of paths (S27 R21); missing projects
        // stay listed (the Project Hub offers Locate... and Remove from List)
        recentProjects = hub::recent::fromYaml(settingsData["recent_projects"]);
        
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
            if (editorNode["project_hub_at_startup"] && editorNode["project_hub_at_startup"].IsScalar()) {
                projectHubAlways = editorNode["project_hub_at_startup"].as<std::string>() == "always";
            }
            if (editorNode["cjk_font_language"] && editorNode["cjk_font_language"].IsScalar()) {
                // an unknown value (a newer editor's) reads as auto
                cjkFontLanguage = SystemFonts::isLanguageSetting(editorNode["cjk_font_language"].as<std::string>())
                    ? editorNode["cjk_font_language"].as<std::string>() : "auto";
            }
            if (editorNode["sound_browser_autoplay"]) soundBrowserAutoplay = editorNode["sound_browser_autoplay"].as<bool>();
            if (editorNode["sound_browser_match_loudness"]) soundBrowserMatchLoudness = editorNode["sound_browser_match_loudness"].as<bool>();
            if (editorNode["hub_last_location"] && editorNode["hub_last_location"].IsScalar()) {
                hubLastLocation = FileUtils::pathFromUtf8(editorNode["hub_last_location"].as<std::string>());
            }
            if (editorNode["autosave"]) {
                auto autosaveNode = editorNode["autosave"];
                if (autosaveNode["enabled"]) autosaveEnabled = autosaveNode["enabled"].as<bool>();
                if (autosaveNode["interval_seconds"]) setAutosaveIntervalSeconds(autosaveNode["interval_seconds"].as<int>());
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
                if (panelsNode["soundBrowser"]) panelVisibility.soundBrowser = panelsNode["soundBrowser"].as<bool>();
                if (panelsNode["vertexAnimationBaker"]) panelVisibility.vertexAnimationBaker = panelsNode["vertexAnimationBaker"].as<bool>();
                if (panelsNode["physicsPlacement"]) panelVisibility.physicsPlacement = panelsNode["physicsPlacement"].as<bool>();
                if (panelsNode["spriteNormals"]) panelVisibility.spriteNormals = panelsNode["spriteNormals"].as<bool>();
                if (panelsNode["mcp"]) panelVisibility.mcp = panelsNode["mcp"].as<bool>();
                if (panelsNode["game"]) panelVisibility.game = panelsNode["game"].as<bool>();
                if (panelsNode["undo_history"]) panelVisibility.undoHistory = panelsNode["undo_history"].as<bool>();
            }
            if (editorNode["game_view"]) {
                auto gameNode = editorNode["game_view"];
                if (gameNode["aspect"]) gameViewSettings.aspect = gameNode["aspect"].as<int>();
                if (gameNode["scale"]) gameViewSettings.scale = gameNode["scale"].as<float>();
                if (gameNode["maximize_on_play"]) gameViewSettings.maximizeOnPlay = gameNode["maximize_on_play"].as<bool>();
                if (gameNode["stats"]) gameViewSettings.showStats = gameNode["stats"].as<bool>();
                if (gameNode["atmosphere_quality"]) gameViewSettings.atmosphereQuality = std::clamp(gameNode["atmosphere_quality"].as<int>(), -1, 3);
                if (gameNode["device_tier"]) gameViewSettings.deviceTier = std::clamp(gameNode["device_tier"].as<int>(), -1, 3);
                if (gameNode["device_platform"]) gameViewSettings.devicePlatform = std::clamp(gameNode["device_platform"].as<int>(), -1, 2);
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
            if (editorNode["scene_view"]) {
                auto sceneNode = editorNode["scene_view"];
                if (sceneNode["exact_pick"]) sceneViewSettings.exactPick = sceneNode["exact_pick"].as<bool>();
                if (sceneNode["align_to_surface"]) sceneViewSettings.alignToSurface = sceneNode["align_to_surface"].as<bool>();
                if (sceneNode["rulers"]) sceneViewSettings.rulers = sceneNode["rulers"].as<bool>();
                if (sceneNode["preview_particles"]) sceneViewSettings.previewParticles = sceneNode["preview_particles"].as<bool>();
            }
            if (editorNode["profiler"]) {
                auto profilerNode = editorNode["profiler"];
                ProfilerSettings& p = profilerSettings;
                if (profilerNode["budget_fps"]) p.budgetFps = std::clamp(profilerNode["budget_fps"].as<int>(), 0, 1000);
                if (profilerNode["cpu"]) p.showCpu = profilerNode["cpu"].as<bool>();
                if (profilerNode["gpu"]) p.showGpu = profilerNode["gpu"].as<bool>();
                if (profilerNode["rendering"]) p.showRendering = profilerNode["rendering"].as<bool>();
                if (profilerNode["memory"]) p.showMemory = profilerNode["memory"].as<bool>();
                if (profilerNode["monitors_module"]) p.showMonitors = profilerNode["monitors_module"].as<bool>();
                if (profilerNode["cpu_view"]) p.cpuView = std::clamp(profilerNode["cpu_view"].as<int>(), 0, 1);
                if (profilerNode["sort_column"]) p.sortColumn = std::clamp(profilerNode["sort_column"].as<int>(), 0, 4);
                if (profilerNode["sort_ascending"]) p.sortAscending = profilerNode["sort_ascending"].as<bool>();
                if (profilerNode["pause_on_spike"]) p.pauseOnSpike = profilerNode["pause_on_spike"].as<bool>();
                if (profilerNode["pause_over_ms"]) p.pauseOverMs = std::clamp(profilerNode["pause_over_ms"].as<float>(), 1.0f, 10000.0f);
                if (profilerNode["memory_group"]) p.memoryGroup = std::clamp(profilerNode["memory_group"].as<int>(), 0, 1);
                if (profilerNode["monitors"] && profilerNode["monitors"].IsSequence()) {
                    p.monitors.clear();
                    for (const auto& item : profilerNode["monitors"]) p.monitors.push_back(item.as<std::string>());
                }
                if (profilerNode["debugger_mode"]) p.debuggerMode = std::clamp(profilerNode["debugger_mode"].as<int>(), 0, 5);
            }
        }

        // The former AI assistant's "ai_assistant" section is ignored (and dropped on save)
        if (settingsData["mcp"]) {
            auto mcpNode = settingsData["mcp"];
            if (mcpNode["http_enabled"]) mcpHttpEnabled = mcpNode["http_enabled"].as<bool>();
            if (mcpNode["http_port"]) mcpHttpPort = mcpNode["http_port"].as<int>();
            if (mcpNode["http_token"]) mcpHttpToken = mcpNode["http_token"].as<std::string>();
            if (mcpNode["allow_changes"]) mcpHttpAllowChanges = mcpNode["allow_changes"].as<bool>();
            // "a,b", "all" or a list of group names
            if (mcpNode["tool_groups"]) {
                const YAML::Node groups = mcpNode["tool_groups"];
                mcpToolGroups.clear();
                if (groups.IsSequence()) {
                    for (const auto& item : groups) mcpToolGroups += (mcpToolGroups.empty() ? "" : ",") + item.as<std::string>();
                } else {
                    mcpToolGroups = groups.as<std::string>();
                }
            }
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

        if (settingsData["asset_pipeline"]) {
            auto assetNode = settingsData["asset_pipeline"];
            if (assetNode["auto_refresh"]) assetAutoRefresh = assetNode["auto_refresh"].as<bool>();
            if (assetNode["watcher"]) setFileWatcher(assetNode["watcher"].as<std::string>());
            if (assetNode["polling_interval"]) setPollingInterval(assetNode["polling_interval"].as<float>());
            if (assetNode["log_reloads"]) assetLogReloads = assetNode["log_reloads"].as<bool>();
        }

        if (settingsData["scripting"]) {
            auto scriptingNode = settingsData["scripting"];
            if (scriptingNode["changes_while_playing"]) setScriptChangesWhilePlaying(scriptingNode["changes_while_playing"].as<std::string>());
            if (scriptingNode["reload_cpp_during_play"]) reloadCppDuringPlay = scriptingNode["reload_cpp_during_play"].as<bool>();
            if (scriptingNode["external_script_editor"]) setExternalScriptEditor(scriptingNode["external_script_editor"].as<std::string>());
            if (scriptingNode["external_script_editor_command"]) externalScriptEditorCommand = scriptingNode["external_script_editor_command"].as<std::string>();
        }

        if (settingsData["html_ui"]) {
            auto htmlUiNode = settingsData["html_ui"];
            if (htmlUiNode["auto_import"]) htmlUiAutoImport = htmlUiNode["auto_import"].as<bool>();
            if (htmlUiNode["verbose_diagnostics"]) htmlUiVerboseDiagnostics = htmlUiNode["verbose_diagnostics"].as<bool>();
            if (htmlUiNode["hot_reload"]) htmlUiPatchReload = htmlUiNode["hot_reload"].as<std::string>() != "rebuild";
        }
        UIDocumentSystem::setVerboseDiagnostics(htmlUiVerboseDiagnostics);
        UIDocumentSystem::setDefaultReloadMode(htmlUiPatchReload ? UIReloadMode::PATCH : UIReloadMode::REBUILD);

        if (settingsData["undo"]) {
            auto undoNode = settingsData["undo"];
            if (undoNode["max_steps"]) undoMaxSteps = undoNode["max_steps"].as<int>();
            if (undoNode["memory_limit_mb"]) undoMemoryLimitMB = undoNode["memory_limit_mb"].as<int>();
            if (undoNode["record_selection"]) undoRecordSelection = undoNode["record_selection"].as<bool>();
            if (undoNode["review_play_changes"]) undoReviewPlayChanges = undoNode["review_play_changes"].as<bool>();
        }
        applyUndoSettings();
        loggingSettings = LoggingSettings();
        if (settingsData["logging"]) {
            auto loggingNode = settingsData["logging"];
            if (loggingNode["levels"] && loggingNode["levels"].IsMap()) {
                for (const auto& item : loggingNode["levels"]) {
                    LogLevel level;
                    const std::string name = item.second.as<std::string>();
                    if (parseLogLevel(name.c_str(), level)) {
                        loggingSettings.categoryLevels[item.first.as<std::string>()] = logLevelName(level);
                    }
                }
            }
            if (loggingNode["file_level"]) {
                LogLevel level;
                const std::string name = loggingNode["file_level"].as<std::string>();
                if (parseLogLevel(name.c_str(), level)) loggingSettings.fileLevel = logLevelName(level);
            }
            if (loggingNode["backtrace_on_error"]) loggingSettings.backtraceOnError = loggingNode["backtrace_on_error"].as<bool>();
            if (loggingNode["show_source"]) loggingSettings.showSource = loggingNode["show_source"].as<bool>();
        }
        applyLoggingSettings();
        DiagnosticsSettings loaded;
        if (settingsData["diagnostics"]) {
            auto diagnosticsNode = settingsData["diagnostics"];
            if (diagnosticsNode["hang_watchdog"]) loaded.hangWatchdog = diagnosticsNode["hang_watchdog"].as<bool>();
            if (diagnosticsNode["report_after_s"]) loaded.reportAfterSeconds = diagnosticsNode["report_after_s"].as<int>();
            if (diagnosticsNode["break_lua_after_s"]) loaded.breakLuaAfterSeconds = diagnosticsNode["break_lua_after_s"].as<int>();
            if (diagnosticsNode["show_crash_notice"]) loaded.showCrashNotice = diagnosticsNode["show_crash_notice"].as<bool>();
        }
        setDiagnosticsSettings(loaded);
        VersionControlSettings vcs;
        if (const YAML::Node vcsNode = settingsData["version_control"]; vcsNode && vcsNode.IsMap()) {
            if (vcsNode["git_path"]) vcs.gitPath = vcsNode["git_path"].as<std::string>();
            if (vcsNode["svn_path"]) vcs.svnPath = vcsNode["svn_path"].as<std::string>();
            if (vcsNode["lock_on_edit"]) {
                const std::string mode = vcsNode["lock_on_edit"].as<std::string>();
                vcs.lockOnEdit = mode == "off" ? LockOnEdit::Off : mode == "auto" ? LockOnEdit::Auto : LockOnEdit::Ask;
            }
            if (vcsNode["poll_seconds"]) vcs.pollSeconds = vcsNode["poll_seconds"].as<int>();
            if (vcsNode["lock_poll_seconds"]) vcs.lockPollSeconds = vcsNode["lock_poll_seconds"].as<int>();
            if (vcsNode["auto_fetch_minutes"]) vcs.autoFetchMinutes = vcsNode["auto_fetch_minutes"].as<int>();
            if (vcsNode["commit_and_push"]) vcs.commitAndPush = vcsNode["commit_and_push"].as<bool>();
            if (vcsNode["show_changes"]) vcs.showChanges = vcsNode["show_changes"].as<bool>();
        }
        versionControlSettings = vcs;
        versionControlSettings = getVersionControlSettings();

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
        
        settingsData["recent_projects"] = hub::recent::toYaml(recentProjects);

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

        // Preferences > External Tools
        if (!externalToolPaths.empty()) {
            YAML::Node toolsNode;
            for (const auto& [tool, path] : externalToolPaths) toolsNode[tool] = path;
            settingsData["externalTools"] = toolsNode;
        } else {
            settingsData.remove("externalTools");
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
        editorNode["project_hub_at_startup"] = projectHubAlways ? "always" : "when_no_project";
        editorNode["cjk_font_language"] = cjkFontLanguage;
        if (soundBrowserAutoplay) editorNode["sound_browser_autoplay"] = true;
        if (!soundBrowserMatchLoudness) editorNode["sound_browser_match_loudness"] = false;
        if (!hubLastLocation.empty()) editorNode["hub_last_location"] = FileUtils::pathToGenericUtf8(hubLastLocation);
        YAML::Node autosaveNode;
        autosaveNode["enabled"] = autosaveEnabled;
        autosaveNode["interval_seconds"] = autosaveIntervalSeconds;
        editorNode["autosave"] = autosaveNode;
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
        panelsNode["soundBrowser"] = panelVisibility.soundBrowser;
        panelsNode["vertexAnimationBaker"] = panelVisibility.vertexAnimationBaker;
        panelsNode["physicsPlacement"] = panelVisibility.physicsPlacement;
        panelsNode["spriteNormals"] = panelVisibility.spriteNormals;
        panelsNode["mcp"] = panelVisibility.mcp;
        panelsNode["game"] = panelVisibility.game;
        panelsNode["undo_history"] = panelVisibility.undoHistory;
        editorNode["panels"] = panelsNode;
        YAML::Node gameNode;
        gameNode["aspect"] = gameViewSettings.aspect;
        gameNode["scale"] = gameViewSettings.scale;
        gameNode["maximize_on_play"] = gameViewSettings.maximizeOnPlay;
        gameNode["stats"] = gameViewSettings.showStats;
        gameNode["atmosphere_quality"] = gameViewSettings.atmosphereQuality;
        if (gameViewSettings.deviceTier >= 0) gameNode["device_tier"] = gameViewSettings.deviceTier;
        if (gameViewSettings.devicePlatform >= 0) gameNode["device_platform"] = gameViewSettings.devicePlatform;
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
        YAML::Node sceneViewNode;
        sceneViewNode["exact_pick"] = sceneViewSettings.exactPick;
        sceneViewNode["align_to_surface"] = sceneViewSettings.alignToSurface;
        sceneViewNode["rulers"] = sceneViewSettings.rulers;
        sceneViewNode["preview_particles"] = sceneViewSettings.previewParticles;
        editorNode["scene_view"] = sceneViewNode;
        YAML::Node profilerNode;
        profilerNode["budget_fps"] = profilerSettings.budgetFps;
        profilerNode["cpu"] = profilerSettings.showCpu;
        profilerNode["gpu"] = profilerSettings.showGpu;
        profilerNode["rendering"] = profilerSettings.showRendering;
        profilerNode["memory"] = profilerSettings.showMemory;
        profilerNode["monitors_module"] = profilerSettings.showMonitors;
        profilerNode["cpu_view"] = profilerSettings.cpuView;
        profilerNode["sort_column"] = profilerSettings.sortColumn;
        profilerNode["sort_ascending"] = profilerSettings.sortAscending;
        profilerNode["pause_on_spike"] = profilerSettings.pauseOnSpike;
        profilerNode["pause_over_ms"] = profilerSettings.pauseOverMs;
        profilerNode["memory_group"] = profilerSettings.memoryGroup;
        for (const std::string& monitor : profilerSettings.monitors) profilerNode["monitors"].push_back(monitor);
        profilerNode["debugger_mode"] = profilerSettings.debuggerMode;
        editorNode["profiler"] = profilerNode;
        settingsData["editor"] = editorNode;

        // MCP server
        YAML::Node mcpNode;
        mcpNode["http_enabled"] = mcpHttpEnabled;
        mcpNode["http_port"] = mcpHttpPort;
        // The token lives in its own owner-only file (getMcpHttpTokenPath), not here
        mcpNode["allow_changes"] = mcpHttpAllowChanges;
        if (!mcpToolGroups.empty()) mcpNode["tool_groups"] = mcpToolGroups;
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

        YAML::Node assetNode;
        assetNode["auto_refresh"] = assetAutoRefresh;
        assetNode["watcher"] = assetWatcher;
        assetNode["polling_interval"] = assetPollingInterval;
        assetNode["log_reloads"] = assetLogReloads;
        settingsData["asset_pipeline"] = assetNode;

        YAML::Node scriptingNode;
        scriptingNode["changes_while_playing"] = scriptChangesWhilePlaying;
        scriptingNode["reload_cpp_during_play"] = reloadCppDuringPlay;
        scriptingNode["external_script_editor"] = externalScriptEditor;
        if (!externalScriptEditorCommand.empty()) scriptingNode["external_script_editor_command"] = externalScriptEditorCommand;
        settingsData["scripting"] = scriptingNode;

        YAML::Node htmlUiNode;
        htmlUiNode["auto_import"] = htmlUiAutoImport;
        htmlUiNode["verbose_diagnostics"] = htmlUiVerboseDiagnostics;
        htmlUiNode["hot_reload"] = htmlUiPatchReload ? "patch" : "rebuild";
        settingsData["html_ui"] = htmlUiNode;

        YAML::Node undoNode;
        undoNode["max_steps"] = undoMaxSteps;
        undoNode["memory_limit_mb"] = undoMemoryLimitMB;
        undoNode["record_selection"] = undoRecordSelection;
        undoNode["review_play_changes"] = undoReviewPlayChanges;
        settingsData["undo"] = undoNode;
        if (loggingSettings == LoggingSettings()) {
            if (settingsData["logging"]) settingsData.remove("logging");
        } else {
            YAML::Node loggingNode;
            YAML::Node levelsNode(YAML::NodeType::Map);
            for (const auto& [category, level] : loggingSettings.categoryLevels) levelsNode[category] = level;
            loggingNode["levels"] = levelsNode;
            loggingNode["file_level"] = loggingSettings.fileLevel;
            loggingNode["backtrace_on_error"] = loggingSettings.backtraceOnError;
            loggingNode["show_source"] = loggingSettings.showSource;
            settingsData["logging"] = loggingNode;
        }
        if (diagnosticsSettings == DiagnosticsSettings()) {
            if (settingsData["diagnostics"]) settingsData.remove("diagnostics");
        } else {
            YAML::Node diagnosticsNode;
            diagnosticsNode["hang_watchdog"] = diagnosticsSettings.hangWatchdog;
            diagnosticsNode["report_after_s"] = diagnosticsSettings.reportAfterSeconds;
            diagnosticsNode["break_lua_after_s"] = diagnosticsSettings.breakLuaAfterSeconds;
            diagnosticsNode["show_crash_notice"] = diagnosticsSettings.showCrashNotice;
            settingsData["diagnostics"] = diagnosticsNode;
        }
        if (versionControlSettings == VersionControlSettings()) {
            if (settingsData["version_control"]) settingsData.remove("version_control");
        } else {
            const VersionControlSettings& vcs = versionControlSettings;
            YAML::Node vcsNode;
            vcsNode["git_path"] = vcs.gitPath;
            vcsNode["svn_path"] = vcs.svnPath;
            vcsNode["lock_on_edit"] = vcs.lockOnEdit == LockOnEdit::Off ? "off" : vcs.lockOnEdit == LockOnEdit::Auto ? "auto" : "ask";
            vcsNode["poll_seconds"] = vcs.pollSeconds;
            vcsNode["lock_poll_seconds"] = vcs.lockPollSeconds;
            vcsNode["auto_fetch_minutes"] = vcs.autoFetchMinutes;
            vcsNode["commit_and_push"] = vcs.commitAndPush;
            vcsNode["show_changes"] = vcs.showChanges;
            settingsData["version_control"] = vcsNode;
        }
        if (settingsData["ai_assistant"]) settingsData.remove("ai_assistant");

        return writeMerged();
    } catch (const std::exception& e) {
        Out::error("Failed to save settings: %s", e.what());
        return false;
    }
}

bool AppSettings::writeMerged() {
    // read-merge-write under the lock (R11): two editors share this file
    io::ScopedFileLock lock(lockFileFor(configFilePath));
    if (!lock.isLocked() && lock.getStatus() != io::ScopedFileLock::Status::Unsupported) {
        Out::warning("Editor settings lock: %s", lock.failureReason().c_str());
    }
    YAML::Node current = readSettingsFile(configFilePath);
    YAML::Node merged = current.IsMap() ? current : YAML::Node(YAML::NodeType::Map);

    auto sameText = [](const YAML::Node& a, const YAML::Node& b) {
        return (a ? YAML::Dump(a) : std::string()) == (b ? YAML::Dump(b) : std::string());
    };
    if (settingsData.IsMap()) {
        for (auto it = settingsData.begin(); it != settingsData.end(); ++it) {
            const std::string key = it->first.as<std::string>();
            if (key == "recent_projects") {
                continue;
            }
            if (!sameText(it->second, loadedSettings[key])) {
                merged[key] = it->second;
            }
        }
    }
    // keys this process removed since its load, unless another editor changed them since
    if (loadedSettings.IsMap()) {
        for (auto it = loadedSettings.begin(); it != loadedSettings.end(); ++it) {
            const std::string key = it->first.as<std::string>();
            if (!settingsData[key] && sameText(merged[key], it->second)) {
                merged.remove(key);
            }
        }
    }
    // recent projects: an ordered union, this process's records first
    std::vector<hub::RecentProject> recent = hub::recent::fromYaml(settingsData["recent_projects"]);
    for (hub::RecentProject& other : hub::recent::fromYaml(current["recent_projects"])) {
        const bool seen = std::any_of(recent.begin(), recent.end(), [&](const hub::RecentProject& p) { return p.path == other.path; });
        if (!seen) recent.push_back(std::move(other));
    }
    hub::recent::enforceCap(recent);
    merged["recent_projects"] = hub::recent::toYaml(recent);

    io::AtomicWriteOptions options;
    options.durability = io::Durability::Document;
    const io::WriteResult written = io::writeFileAtomic(configFilePath, YAML::Dump(merged), options);
    if (!written.ok()) {
        Out::error(DocumentFiles::saveFailureMessage(configFilePath, written));
        return false;
    }
    // what is on disk now, the other editor's changes included
    applySettings(merged);
    return true;
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

std::string AppSettings::getExternalToolPath(const std::string& tool) {
    auto it = externalToolPaths.find(tool);
    return it != externalToolPaths.end() ? it->second : std::string();
}

void AppSettings::setExternalToolPath(const std::string& tool, const std::string& path) {
    if (path.empty()) externalToolPaths.erase(tool);
    else externalToolPaths[tool] = path;
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
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    for (const hub::RecentProject& p : recentProjects) {
        if (paths.size() >= hub::recent::kMenuCount) break;
        if (std::filesystem::exists(p.path, ec)) paths.push_back(p.path);
    }
    return paths;
}

const std::vector<hub::RecentProject>& AppSettings::getRecentProjectRecords() {
    return recentProjects;
}

void AppSettings::setRecentProjectRecords(const std::vector<hub::RecentProject>& records) {
    recentProjects = records;
    hub::recent::enforceCap(recentProjects);
    saveSettings();
}

void AppSettings::addToRecentProjects(const std::filesystem::path& path, bool needSave, const std::string& name) {
    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }
    hub::recent::touch(recentProjects, path, name, hub::recent::iso(std::chrono::system_clock::now()));
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

bool AppSettings::getAutoRefresh() {
    return assetAutoRefresh;
}

void AppSettings::setAutoRefresh(bool enabled) {
    assetAutoRefresh = enabled;
}

std::string AppSettings::getFileWatcher() {
    return assetWatcher;
}

void AppSettings::setFileWatcher(const std::string& kind) {
    // "off" is a command-line choice (--file-watcher), not a preference
    if (kind == "auto" || kind == "native" || kind == "polling") assetWatcher = kind;
}

float AppSettings::getPollingInterval() {
    return assetPollingInterval;
}

void AppSettings::setPollingInterval(float seconds) {
    assetPollingInterval = std::clamp(seconds, 0.25f, 10.0f);
}

bool AppSettings::getLogReloads() {
    return assetLogReloads;
}

void AppSettings::setLogReloads(bool enabled) {
    assetLogReloads = enabled;
}

std::string AppSettings::getScriptChangesWhilePlaying() {
    return scriptChangesWhilePlaying;
}

void AppSettings::setScriptChangesWhilePlaying(const std::string& policy) {
    if (policy == "reload" || policy == "after_stop" || policy == "stop_and_reload") scriptChangesWhilePlaying = policy;
}

bool AppSettings::getReloadCppDuringPlay() {
    return reloadCppDuringPlay;
}

void AppSettings::setReloadCppDuringPlay(bool enabled) {
    reloadCppDuringPlay = enabled;
}

std::string AppSettings::getExternalScriptEditor() {
    return externalScriptEditor;
}

void AppSettings::setExternalScriptEditor(const std::string& id) {
    static const char* const known[] = {"builtin", "vscode", "visualstudio", "rider", "clion", "xcode", "sublime", "zed", "system", "custom"};
    for (const char* name : known) {
        if (id == name) externalScriptEditor = id;
    }
}

std::string AppSettings::getExternalScriptEditorCommand() {
    return externalScriptEditorCommand;
}

void AppSettings::setExternalScriptEditorCommand(const std::string& command) {
    externalScriptEditorCommand = command;
}

int AppSettings::getUndoMaxSteps() {
    return undoMaxSteps;
}

int AppSettings::getUndoMemoryLimitMB() {
    return undoMemoryLimitMB;
}

bool AppSettings::getUndoRecordSelection() {
    return undoRecordSelection;
}

bool AppSettings::getUndoReviewPlayChanges() {
    return undoReviewPlayChanges;
}

void AppSettings::setUndo(int maxSteps, int memoryLimitMB, bool recordSelection, bool reviewPlayChanges) {
    undoMaxSteps = maxSteps;
    undoMemoryLimitMB = memoryLimitMB;
    undoRecordSelection = recordSelection;
    undoReviewPlayChanges = reviewPlayChanges;
    applyUndoSettings();
}

void AppSettings::applyUndoSettings() {
    undoMaxSteps = std::clamp(undoMaxSteps, minUndoMaxSteps, maxUndoMaxSteps);
    // 0 = unlimited, else within the range
    if (undoMemoryLimitMB != 0) {
        undoMemoryLimitMB = std::clamp(undoMemoryLimitMB, minUndoMemoryLimitMB, maxUndoMemoryLimitMB);
    }
    CommandHistory::maxEntries = static_cast<size_t>(undoMaxSteps);
    CommandHistory::memoryLimit = static_cast<size_t>(undoMemoryLimitMB) * 1024 * 1024;
    Project::recordSelectionInUndo = undoRecordSelection;
    PlayChangeReview::enabled = undoReviewPlayChanges;
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

bool AppSettings::getHtmlUiPatchReload() {
    return htmlUiPatchReload;
}

void AppSettings::setHtmlUiPatchReload(bool patch) {
    htmlUiPatchReload = patch;
    UIDocumentSystem::setDefaultReloadMode(patch ? UIReloadMode::PATCH : UIReloadMode::REBUILD);
}

LoggingSettings AppSettings::getLoggingSettings() {
    return loggingSettings;
}

void AppSettings::setLoggingSettings(const LoggingSettings& settings) {
    loggingSettings = settings;
    applyLoggingSettings();
}

void AppSettings::applyLoggingSettings() {
    // every category back to Info, then the ones the preferences change
    for (size_t i = 0; i < Logger::categoryCount(); i++) {
        Logger::setCategoryLevel(Logger::categoryAt(i), LogLevel::Info);
    }
    for (const auto& [category, levelName] : loggingSettings.categoryLevels) {
        LogLevel level;
        if (parseLogLevel(levelName.c_str(), level)) {
            Logger::setCategoryLevel(Logger::registerCategory(category, LogLevel::Info), level);
        }
    }
    LogLevel fileLevel = LogLevel::Info;
    parseLogLevel(loggingSettings.fileLevel.c_str(), fileLevel);
    Out::setFileLevel(fileLevel);
    Out::setFileShowsSource(loggingSettings.showSource);
    Logger::setBacktraceOnError(loggingSettings.backtraceOnError);
    Out::applyLevelOverride();
}

DiagnosticsSettings AppSettings::getDiagnosticsSettings() {
    return diagnosticsSettings;
}

void AppSettings::setDiagnosticsSettings(const DiagnosticsSettings& settings) {
    diagnosticsSettings = settings;
    diagnosticsSettings.reportAfterSeconds = std::clamp(settings.reportAfterSeconds,
        DiagnosticsSettings::minReportAfterSeconds, DiagnosticsSettings::maxReportAfterSeconds);
    diagnosticsSettings.breakLuaAfterSeconds = std::clamp(settings.breakLuaAfterSeconds, 0,
        DiagnosticsSettings::maxBreakLuaAfterSeconds);
    EditorDiagnostics::applySettings(diagnosticsSettings);
}

VersionControlSettings AppSettings::getVersionControlSettings() {
    VersionControlSettings vcs = versionControlSettings;
    vcs.pollSeconds = std::clamp(vcs.pollSeconds, 2, 120);
    if (vcs.lockPollSeconds != 0) vcs.lockPollSeconds = std::clamp(vcs.lockPollSeconds, 30, 600);
    vcs.autoFetchMinutes = std::clamp(vcs.autoFetchMinutes, 0, 24 * 60);
    return vcs;
}

void AppSettings::setVersionControlSettings(const VersionControlSettings& settings) {
    versionControlSettings = settings;
    versionControlSettings = getVersionControlSettings();
    saveSettings();
}

bool AppSettings::getEditorVSyncEnabled() {
    return editorVSyncEnabled;
}

void AppSettings::setEditorVSyncEnabled(bool enabled) {
    editorVSyncEnabled = enabled;
}

std::string AppSettings::getCjkFontLanguage() {
    return cjkFontLanguage;
}

void AppSettings::setCjkFontLanguage(const std::string& language) {
    cjkFontLanguage = SystemFonts::isLanguageSetting(language) ? language : "auto";
}

YAML::Node AppSettings::getTitleDesignerSettings() {
    const YAML::Node node = settingsData["title_designer"];
    return node && node.IsMap() ? YAML::Clone(node) : YAML::Node(YAML::NodeType::Map);
}

void AppSettings::setTitleDesignerSettings(const YAML::Node& node) {
    settingsData["title_designer"] = YAML::Clone(node);
}

bool AppSettings::getSoundBrowserAutoplay() {
    return soundBrowserAutoplay;
}

void AppSettings::setSoundBrowserAutoplay(bool autoplay) {
    soundBrowserAutoplay = autoplay;
}

bool AppSettings::getSoundBrowserMatchLoudness() {
    return soundBrowserMatchLoudness;
}

void AppSettings::setSoundBrowserMatchLoudness(bool matched) {
    soundBrowserMatchLoudness = matched;
}

bool AppSettings::getProjectHubAlways() {
    return projectHubAlways;
}

void AppSettings::setProjectHubAlways(bool always) {
    projectHubAlways = always;
}

std::filesystem::path AppSettings::getHubLastLocation() {
    return hubLastLocation;
}

void AppSettings::setHubLastLocation(const std::filesystem::path& location) {
    hubLastLocation = location;
    saveSettings();
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

SceneViewSettings AppSettings::getSceneViewSettings() {
    return sceneViewSettings;
}

void AppSettings::setSceneViewSettings(const SceneViewSettings& settings) {
    if (settings == sceneViewSettings) return;
    sceneViewSettings = settings;
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

ProfilerSettings AppSettings::getProfilerSettings() {
    return profilerSettings;
}

void AppSettings::setProfilerSettings(const ProfilerSettings& settings) {
    if (settings == profilerSettings) return;
    profilerSettings = settings;
    saveSettings();
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

std::string AppSettings::getMcpToolGroups() {
    return mcpToolGroups;
}

std::filesystem::path AppSettings::getUserConfigDirectory() {
    return userConfigDirectory();
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
    // .jasperengine/user/build.yaml: this machine's settings, which only the editor writes, with a
    // token naming this one file so an MCP-set build_jobs saves while no tool can write the file
    // itself (util/SafeWrite.h)
    if (!std::filesystem::is_directory(file.parent_path(), ec)) {
        safewrite::EditorInternal makeDir(file.parent_path());
        FileUtils::createDirectories(file.parent_path(), ec);
        if (ec) {
            Out::error("Failed to create local settings directory: %s", ec.message().c_str());
            return false;
        }
    }
    safewrite::EditorInternal ownBuildSettings(file);

    const std::string text = "# Build configuration for this machine only. Version control ignores .jasperengine/.\n" +
            YAML::Dump(node) + "\n";
    const io::WriteResult written = DocumentFiles::write(file, text);
    if (!written.ok()) {
        Out::error(DocumentFiles::saveFailureMessage(file, written));
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

bool AppSettings::isAutosaveEnabled() {
    return autosaveEnabled;
}

void AppSettings::setAutosaveEnabled(bool enabled) {
    autosaveEnabled = enabled;
}

int AppSettings::getAutosaveIntervalSeconds() {
    return autosaveIntervalSeconds;
}

void AppSettings::setAutosaveIntervalSeconds(int seconds) {
    autosaveIntervalSeconds = std::clamp(seconds, minAutosaveInterval, maxAutosaveInterval);
}

} // namespace jasperengine::editor
