// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AppSettings.h"
#include "DocumentFile.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/PlayChangeReview.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "io/ScopedFileLock.h"
#include "util/FileUtils.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/VideoImporter.h"
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace jasperengine::editor {

namespace {

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
bool AppSettings::editorVSyncEnabled = true;
PanelVisibilitySettings AppSettings::panelVisibility;
GameViewSettings AppSettings::gameViewSettings;
bool AppSettings::mcpHttpEnabled = false;
int AppSettings::mcpHttpPort = 47615;
std::string AppSettings::mcpHttpToken;
bool AppSettings::htmlUiAutoImport = true;
bool AppSettings::assetAutoRefresh = true;
std::string AppSettings::assetWatcher = "auto";
float AppSettings::assetPollingInterval = 1.0f;
bool AppSettings::assetLogReloads = true;
int AppSettings::undoMaxSteps = AppSettings::defaultUndoMaxSteps;
int AppSettings::undoMemoryLimitMB = AppSettings::defaultUndoMemoryLimitMB;
bool AppSettings::undoRecordSelection = true;
bool AppSettings::undoReviewPlayChanges = true;
bool AppSettings::htmlUiVerboseDiagnostics = false;
LoggingSettings AppSettings::loggingSettings;

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
    return loadSettings();
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
            if (editorNode["vsync"]) {
                editorVSyncEnabled = editorNode["vsync"].as<bool>();
            }
            if (editorNode["autosave"]) {
                auto autosaveNode = editorNode["autosave"];
                if (autosaveNode["enabled"]) autosaveEnabled = autosaveNode["enabled"].as<bool>();
                if (autosaveNode["interval_seconds"]) setAutosaveIntervalSeconds(autosaveNode["interval_seconds"].as<int>());
            }
            if (editorNode["panels"]) {
                auto panelsNode = editorNode["panels"];
                if (panelsNode["hierarchy"]) panelVisibility.hierarchy = panelsNode["hierarchy"].as<bool>();
                if (panelsNode["inspector"]) panelVisibility.inspector = panelsNode["inspector"].as<bool>();
                if (panelsNode["project"]) panelVisibility.project = panelsNode["project"].as<bool>();
                if (panelsNode["console"]) panelVisibility.console = panelsNode["console"].as<bool>();
                if (panelsNode["animation"]) panelVisibility.animation = panelsNode["animation"].as<bool>();
                if (panelsNode["terrain"]) panelVisibility.terrain = panelsNode["terrain"].as<bool>();
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
            }
        }

        // The former AI assistant's "ai_assistant" section is ignored (and dropped on save)
        if (settingsData["mcp"]) {
            auto mcpNode = settingsData["mcp"];
            if (mcpNode["http_enabled"]) mcpHttpEnabled = mcpNode["http_enabled"].as<bool>();
            if (mcpNode["http_port"]) mcpHttpPort = mcpNode["http_port"].as<int>();
            if (mcpNode["http_token"]) mcpHttpToken = mcpNode["http_token"].as<std::string>();
        }

        if (settingsData["asset_pipeline"]) {
            auto assetNode = settingsData["asset_pipeline"];
            if (assetNode["auto_refresh"]) assetAutoRefresh = assetNode["auto_refresh"].as<bool>();
            if (assetNode["watcher"]) setFileWatcher(assetNode["watcher"].as<std::string>());
            if (assetNode["polling_interval"]) setPollingInterval(assetNode["polling_interval"].as<float>());
            if (assetNode["log_reloads"]) assetLogReloads = assetNode["log_reloads"].as<bool>();
        }

        if (settingsData["html_ui"]) {
            auto htmlUiNode = settingsData["html_ui"];
            if (htmlUiNode["auto_import"]) htmlUiAutoImport = htmlUiNode["auto_import"].as<bool>();
            if (htmlUiNode["verbose_diagnostics"]) htmlUiVerboseDiagnostics = htmlUiNode["verbose_diagnostics"].as<bool>();
        }
        UIDocumentSystem::setVerboseDiagnostics(htmlUiVerboseDiagnostics);

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
        editorNode["vsync"] = editorVSyncEnabled;
        YAML::Node autosaveNode;
        autosaveNode["enabled"] = autosaveEnabled;
        autosaveNode["interval_seconds"] = autosaveIntervalSeconds;
        editorNode["autosave"] = autosaveNode;
        YAML::Node panelsNode;
        panelsNode["hierarchy"] = panelVisibility.hierarchy;
        panelsNode["inspector"] = panelVisibility.inspector;
        panelsNode["project"] = panelVisibility.project;
        panelsNode["console"] = panelVisibility.console;
        panelsNode["animation"] = panelVisibility.animation;
        panelsNode["terrain"] = panelVisibility.terrain;
        panelsNode["mcp"] = panelVisibility.mcp;
        panelsNode["game"] = panelVisibility.game;
        panelsNode["undo_history"] = panelVisibility.undoHistory;
        editorNode["panels"] = panelsNode;
        YAML::Node gameNode;
        gameNode["aspect"] = gameViewSettings.aspect;
        gameNode["scale"] = gameViewSettings.scale;
        gameNode["maximize_on_play"] = gameViewSettings.maximizeOnPlay;
        gameNode["stats"] = gameViewSettings.showStats;
        editorNode["game_view"] = gameNode;
        settingsData["editor"] = editorNode;

        // MCP server
        YAML::Node mcpNode;
        mcpNode["http_enabled"] = mcpHttpEnabled;
        mcpNode["http_port"] = mcpHttpPort;
        if (!mcpHttpToken.empty()) mcpNode["http_token"] = mcpHttpToken;
        settingsData["mcp"] = mcpNode;

        YAML::Node assetNode;
        assetNode["auto_refresh"] = assetAutoRefresh;
        assetNode["watcher"] = assetWatcher;
        assetNode["polling_interval"] = assetPollingInterval;
        assetNode["log_reloads"] = assetLogReloads;
        settingsData["asset_pipeline"] = assetNode;

        YAML::Node htmlUiNode;
        htmlUiNode["auto_import"] = htmlUiAutoImport;
        htmlUiNode["verbose_diagnostics"] = htmlUiVerboseDiagnostics;
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
    // recent projects: an ordered union, this process's first
    YAML::Node recent(YAML::NodeType::Sequence);
    std::vector<std::string> seen;
    auto addRecent = [&](const YAML::Node& list) {
        if (!list || !list.IsSequence()) return;
        for (const auto& entry : list) {
            const std::string path = entry.as<std::string>();
            if (std::find(seen.begin(), seen.end(), path) == seen.end()) {
                seen.push_back(path);
                recent.push_back(path);
            }
        }
    };
    addRecent(settingsData["recent_projects"]);
    addRecent(current["recent_projects"]);
    merged["recent_projects"] = recent;

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

bool AppSettings::getEditorVSyncEnabled() {
    return editorVSyncEnabled;
}

void AppSettings::setEditorVSyncEnabled(bool enabled) {
    editorVSyncEnabled = enabled;
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
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        Out::error("Failed to create local settings directory: %s", ec.message().c_str());
        return false;
    }

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
