// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ExportWindow.h"
#include "command/type/ShaderOverridesCmd.h"
#include "cook/BuildReport.h"
#include "window/EditorModal.h"
#include "player/PlayerModules.h"
#include "texture/TextureCookReport.h"
#include "util/FileDialogs.h"
#include "AppSettings.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Generator.h"
#include "pool/ShaderPool.h"
#include "htmlui/effects/EffectRegistry.h"
#include "window/Widgets.h"
#include "external/IconsFontAwesome6.h"
#include "Out.h"
#include "Theme.h"
#include "Factory.h"
#include "SettingsCategoryList.h"

#include <algorithm>
#include "util/CommandRunner.h"
#include "util/FileUtils.h"
#include "ShippingTypes.h"

#include <cstdlib>
#include <iterator>

#include <vector>

namespace jasperengine::editor {

namespace fx = ::jasperengine::htmlui;

namespace {
    // Desktop exports build for the current host, so only expose backends
    // supported by that host's standalone CMake configuration.
    const std::vector<ShaderBackend>& desktopGraphicBackends = hostDesktopBackends();
    const int desktopGraphicBackendCount = static_cast<int>(desktopGraphicBackends.size());

    float textButtonWidth(const char* label) {
        return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    void exportBeginSettingsRow(const char* label) {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
    }

    bool isHostBackend(ShaderBackend backend) {
        for (ShaderBackend hostBackend : desktopGraphicBackends) {
            if (hostBackend == backend) return true;
        }
        return false;
    }
}

void ExportWindow::open(Project* project) {
    m_isOpen = true;
    m_step = Step::Settings;
    if (m_project != project) m_projectSettingsDirty = false;
    m_project = project;
    m_targetDir.clear();
    m_targetDirFromDefault = false;
    m_startSceneId = project->getStartSceneId();
    const SceneProject* startScene = project->getScene(m_startSceneId);
    if (!startScene || startScene->filepath.empty()) {
        m_startSceneId = NULL_PROJECT_SCENE;
        for (const auto& scene : project->getScenes()) {
            if (!scene.filepath.empty()) {
                m_startSceneId = scene.id;
                break;
            }
        }
    }
    m_selectedShaderIndex = -1;
    m_shaderAdditions.clear();
    m_shaderExclusions.clear();
    m_addShaderOpen = false;
    m_emsdkOverride.clear();
    m_emsdkInfo = EmsdkInfo();
    m_missingBuildTools.clear();
    m_graphicBackendIndex = 0;
    m_sourceBackendsConfigured = false;
    m_desktopBackendConfigured = false;
    m_selectedSceneId = NULL_PROJECT_SCENE;
    m_runAfterBuild = false;
    m_launchDone = false;
    m_playerSettingsRequested = false;
    m_publishingLoaded = false;

    populateBackendList();
    // Remember the platform between openings, like Unity's active build target.
    if (!m_platformInitialized) {
        m_platform = hostPlatform();
        m_buildPlayer = true;
    }
    selectPlatform(m_platform, m_buildPlayer);
}

const char* ExportWindow::localExportMode() const {
    if (m_mode == ExportMode::Player) {
        // the keys build_project uses too: this computer's platform keeps the Desktop one
        static const char* const keys[] = {"player-windows", "player-linux", "player-macos", "player-android", "player-ios", "web"};
        const ShipPlatform platform = shipPlatform();
        if (platform == hostBuildPlatform()) return "desktop";
        return keys[static_cast<int>(platform)];
    }
    return m_mode == ExportMode::SourceCode ? "sourceCode" : m_mode == ExportMode::Desktop ? "desktop" : "web";
}

void ExportWindow::refreshShaderSelection() {
    // The list a build ships before this window's picks (Project::refreshClosedSceneShaderKeys):
    // closed scenes as their files are now; what takes longer than a blink goes on in
    // drawShaderSection, a scene a frame
    std::set<ShaderKey> sceneKeys;
    if (m_project) {
        if (!m_project->isGeneratedExportRunning()) {
            ShaderListLimit limit;
            limit.maxMillis = 100;
            m_shaderListStatus = m_project->refreshClosedSceneShaderKeys(limit);
        }
        std::set<ShaderKey> collected;
        m_project->collectBuildShaderKeys(collected);
        sceneKeys = m_project->finishBuildShaderKeys(collected, {}, {});
    }

    std::set<ShaderKey> keys = sceneKeys;
    keys.insert(m_shaderAdditions.begin(), m_shaderAdditions.end());
    keys.insert(m_shaderExclusions.begin(), m_shaderExclusions.end());

    m_shaderEntries.clear();
    for (ShaderKey key : keys) {
        ShaderKey normalizedKey = ShaderPool::normalizeKey(key);
        ShaderEntry entry;
        entry.key = normalizedKey;
        entry.type = ShaderPool::getShaderTypeFromKey(normalizedKey);
        entry.properties = ShaderPool::getPropertiesFromKey(normalizedKey);
        uint16_t customId = ShaderPool::getCustomIdFromKey(normalizedKey);
        entry.displayName = Exporter::getShaderDisplayName(entry.type, entry.properties, customId);
        entry.fromScene = sceneKeys.count(normalizedKey) > 0;
        m_shaderEntries.push_back(entry);
    }

    std::sort(m_shaderEntries.begin(), m_shaderEntries.end(), [](const ShaderEntry& a, const ShaderEntry& b) {
        if (a.type != b.type) return (int)a.type < (int)b.type;
        return a.properties < b.properties;
    });
    m_selectedShaderIndex = -1;
}

void ExportWindow::includeShader(ShaderKey key) {
    std::set<ShaderKey> additions = m_shaderAdditions;
    std::set<ShaderKey> exclusions = m_shaderExclusions;
    exclusions.erase(key);
    const bool fromScene = std::any_of(m_shaderEntries.begin(), m_shaderEntries.end(), [key](const ShaderEntry& entry) {
        return entry.key == key && entry.fromScene;
    });
    if (!fromScene) additions.insert(key);
    applyShaderOverrides(additions, exclusions);
}

void ExportWindow::applyShaderOverrides(const std::set<ShaderKey>& additions, const std::set<ShaderKey>& exclusions) {
    // one project undo step, saved at once (as edit_build_shaders); the list shows the project's
    const ShaderOverrides& current = m_project->getShaderOverrides();
    if (current.additions != additions || current.exclusions != exclusions) {
        auto* command = new ShaderOverridesCmd(m_project, current, ShaderOverrides{additions, exclusions});
        command->setNoMerge();
        m_project->getProjectCommandHistory()->addCommand(command);
    }
    syncShaderOverrides(true);
}

void ExportWindow::syncShaderOverrides(bool force) {
    // the project's lists, which MCP edits and undo change while the window is open
    const ShaderOverrides& shaders = m_project->getShaderOverrides();
    if (!force && shaders.additions == m_shaderAdditions && shaders.exclusions == m_shaderExclusions) return;
    m_shaderAdditions = shaders.additions;
    m_shaderExclusions = shaders.exclusions;
    refreshShaderSelection();
}

void ExportWindow::populateBackendList() {
    m_backendEntries.clear();

    for (ShaderBackend backend : ShaderPool::getShaderBackends()) {
        BackendEntry entry;
        entry.backend = backend;
        entry.name = ShaderPool::getShaderBackendName(backend);
        entry.selected = isHostBackend(backend);
        m_backendEntries.push_back(entry);
    }
}

void ExportWindow::refreshEmsdkStatus() {
    m_emsdkInfo = Exporter::detectEmsdk(m_emsdkOverride);
}

void ExportWindow::selectMode(ExportMode mode) {
    m_mode = mode;
    loadSettingsFromProject();
    // Both probes spawn processes, so run them once here rather than per-frame.
    if (mode == ExportMode::Desktop) {
        m_missingBuildTools = Generator::checkBuildTools();
    } else if (mode == ExportMode::Web) {
        refreshEmsdkStatus();
    } else if (mode == ExportMode::Player) {
        // a player build compiles when the project needs native code: this computer's tools
        if (shipPlatform() == hostBuildPlatform()) m_missingBuildTools = Generator::checkBuildTools();
        if (shipPlatform() == ShipPlatform::Web) {
            m_emsdkOverride = AppSettings::getEmsdkPath();
            refreshEmsdkStatus();
        }
        refreshPlayerState();
    }
    m_step = Step::Settings;
}

void ExportWindow::loadSettingsFromProject() {
    if (!m_project) return;

    m_targetDir.clear();
    m_targetDirFromDefault = false;
    m_selectedShaderIndex = -1;
    m_sourceBackendsConfigured = false;
    m_selectSourceBackendTab = true;
    m_sourcePlatformWindows = false;
    m_sourcePlatformLinux = false;
    m_sourcePlatformMacOS = false;
    m_sourcePlatformIOS = false;
    m_sourcePlatformAndroid = false;
    m_sourcePlatformWeb = false;
    m_sourcePresetBackends.clear();
    m_desktopBackendConfigured = false;

    const ShaderOverrides& shaders = m_project->getShaderOverrides();
    m_shaderAdditions = shaders.additions;
    m_shaderExclusions = shaders.exclusions;
    refreshShaderSelection();

    if (m_mode == ExportMode::SourceCode) {
        const SourceCodeExportSettings& settings = m_project->getSourceCodeExportSettings();
        populateBackendList();
        m_sourceBackendsConfigured = settings.graphicBackendsConfigured;
        if (!settings.graphicBackendsConfigured) {
            #if defined(_WIN32)
            m_sourcePlatformWindows = true;
            #elif defined(__APPLE__)
            m_sourcePlatformMacOS = true;
            #else
            m_sourcePlatformLinux = true;
            #endif
        }
        auto addPresetBackends = [&](const std::set<ShaderBackend>& backends) {
            m_sourcePresetBackends.insert(backends.begin(), backends.end());
        };
        addPresetBackends(selectedSourcePlatformBackends());
        if (settings.graphicBackendsConfigured) {
            for (auto& entry : m_backendEntries) {
                entry.selected = settings.graphicBackends.count(entry.backend) > 0;
            }
        }
    } else if (m_mode == ExportMode::Desktop) {
        const DesktopExportSettings& settings = m_project->getDesktopExportSettings();
        m_graphicBackendIndex = 0;
        m_desktopBackendConfigured = settings.graphicBackendConfigured;
        if (settings.graphicBackendConfigured) {
            for (int i = 0; i < desktopGraphicBackendCount; ++i) {
                if (desktopGraphicBackends[i] == settings.graphicBackend) {
                    m_graphicBackendIndex = i;
                    break;
                }
            }
        }
    } else if (m_mode == ExportMode::Web) {
        m_emsdkOverride = AppSettings::getEmsdkPath();
    } else if (m_mode == ExportMode::Player && shipPlatform() == hostBuildPlatform()) {
        // this computer's player remembers its graphics API as the Desktop build did
        const DesktopExportSettings& settings = m_project->getDesktopExportSettings();
        if (settings.graphicBackendConfigured) {
            const std::string wanted = normalizeBackend(Exporter::getCMakeGraphicBackend(settings.graphicBackend));
            const std::vector<std::string> backends = jasperengine::editor::platformBackends(shipPlatform());
            auto it = std::find(backends.begin(), backends.end(), wanted);
            if (it != backends.end()) m_playerBackendIndex[static_cast<int>(m_platform)] = static_cast<int>(it - backends.begin());
        }
    }

    const auto file = m_project->getProjectPath() / "project.yaml";
    m_targetDir = AppSettings::getExportTargetDir(file, localExportMode());

    m_buildSettings = AppSettings::getBuildSettings(file);
    const unsigned int buildJobs = m_buildSettings.buildJobs;
    m_desktopBuildJobs = static_cast<int>(std::min(
        buildJobs ? buildJobs : Generator::getAutomaticParallelBuildJobs(), Generator::MAX_SUPPORTED_PARALLEL_BUILD_JOBS));
    if (m_targetDir.empty()) {
        m_targetDir = AppSettings::getDefaultExportDirectory();
        m_targetDirFromDefault = !m_targetDir.empty();
    }
}

bool ExportWindow::saveCurrentSettingsToProject(bool saveProjectFile) {
    if (!m_project) return false;

    const auto file = m_project->getProjectPath() / "project.yaml";
    // A directory taken from the editor-wide default is not the project's own
    fs::path targetDir = m_targetDirFromDefault ? AppSettings::getExportTargetDir(file, localExportMode()) : m_targetDir;
    if (!AppSettings::setExportTargetDir(file, localExportMode(), targetDir)) {
        Out::error("Could not save local export settings.");
        return false;
    }

    if (m_mode == ExportMode::Desktop || (m_mode == ExportMode::Player && shipPlatform() == hostBuildPlatform())) {
        // Only the job count belongs to this dialog, so the rest is re-read: the
        // compiler may have changed in Editor Settings since the mode was selected
        const unsigned int jobs = static_cast<unsigned int>(m_desktopBuildJobs);
        m_buildSettings = AppSettings::getBuildSettings(file);
        m_buildSettings.buildJobs = jobs == Generator::getAutomaticParallelBuildJobs() ? 0 : jobs;
        if (!AppSettings::setBuildSettings(file, m_buildSettings)) {
            Out::error("Could not save local build settings.");
            return false;
        }
    }

    bool changed = false;
    if (m_mode == ExportMode::SourceCode && m_sourceBackendsConfigured) {
        auto& settings = m_project->getSourceCodeExportSettings();
        std::set<ShaderBackend> backends;
        for (const auto& entry : m_backendEntries) if (entry.selected) backends.insert(entry.backend);
        changed = !settings.graphicBackendsConfigured || settings.graphicBackends != backends;
        settings.graphicBackends = backends;
        settings.graphicBackendsConfigured = true;
    } else if (m_mode == ExportMode::Desktop && m_desktopBackendConfigured) {
        auto& settings = m_project->getDesktopExportSettings();
        const auto backend = desktopGraphicBackends[m_graphicBackendIndex];
        changed = !settings.graphicBackendConfigured || settings.graphicBackend != backend;
        settings.graphicBackend = backend;
        settings.graphicBackendConfigured = true;
    } else if (m_mode == ExportMode::Player && shipPlatform() == hostBuildPlatform() &&
               m_playerBackendIndex.count(static_cast<int>(m_platform))) {
        auto& settings = m_project->getDesktopExportSettings();
        const ShaderBackend backend = shaderBackendOf(shipPlatform(), playerBackend());
        changed = !settings.graphicBackendConfigured || settings.graphicBackend != backend;
        settings.graphicBackend = backend;
        settings.graphicBackendConfigured = true;
    }

    // shader overrides are not written here: every edit applied them through ShaderOverridesCmd
    m_projectSettingsDirty = m_projectSettingsDirty || changed;
    if (saveProjectFile && m_projectSettingsDirty) {
        if (!m_project->saveProjectFile()) {
            Out::error("Could not save project export settings.");
            return false;
        }
        m_projectSettingsDirty = false;
    }
    return true;
}

void ExportWindow::show() {
    if (!m_isOpen) return;
    if (m_project) syncShaderOverrides(false);

    ImGui::OpenPopup("Build Settings##ExportModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (m_step == Step::Progress) {
        const float width = std::min(Theme::dpi(560.0f), viewport->WorkSize.x * 0.9f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, viewport->WorkSize.y * 0.9f));
    } else {
        ImGui::SetNextWindowSize(ImVec2(
            std::min(Theme::dpi(920.0f), viewport->WorkSize.x * 0.94f),
            std::min(Theme::dpi(660.0f), viewport->WorkSize.y * 0.94f)), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(Theme::dpi(ImVec2(640.0f, 460.0f)), viewport->WorkSize);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_Modal;
    if (m_step == Step::Progress) {
        flags |= ImGuiWindowFlags_AlwaysAutoResize;
    } else {
        flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

    bool popupOpen = EditorModal::begin("Build Settings##ExportModal", &m_isOpen, flags);

    if (popupOpen) {
        if (!m_isOpen) {
            // Closed via the title-bar X: kill a still-running export first.
            if (m_step == Step::Progress && m_exporter.isRunning()) {
                m_exporter.cancelExport();
            }
            ImGui::CloseCurrentPopup();
        } else if (m_step == Step::Progress) {
            drawProgress();
        } else {
            drawSettings();
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_isOpen = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

bool ExportWindow::consumePlayerSettingsRequest() {
    const bool requested = m_playerSettingsRequested;
    m_playerSettingsRequested = false;
    return requested;
}

ExportWindow::BuildPlatform ExportWindow::hostPlatform() {
    #if defined(_WIN32)
    return BuildPlatform::Windows;
    #elif defined(__APPLE__)
    return BuildPlatform::MacOS;
    #else
    return BuildPlatform::Linux;
    #endif
}

const char* ExportWindow::platformName(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Windows: return "Windows";
        case BuildPlatform::MacOS: return "macOS";
        case BuildPlatform::Linux: return "Linux";
        case BuildPlatform::Android: return "Android";
        case BuildPlatform::IOS: return "iOS";
        case BuildPlatform::Web: return "Web";
        case BuildPlatform::Custom: return "Custom Graphics APIs";
        default: return "";
    }
}

const char* ExportWindow::platformIcon(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Windows: return ICON_FA_WINDOW_MAXIMIZE;
        case BuildPlatform::MacOS: return ICON_FA_LAPTOP;
        case BuildPlatform::Linux: return ICON_FA_TERMINAL;
        case BuildPlatform::Android: return ICON_FA_MOBILE_SCREEN_BUTTON;
        case BuildPlatform::IOS: return ICON_FA_MOBILE_BUTTON;
        case BuildPlatform::Web: return ICON_FA_GLOBE;
        case BuildPlatform::Custom: return ICON_FA_MICROCHIP;
        default: return "";
    }
}

bool ExportWindow::platformCanBuildPlayer(BuildPlatform platform) const {
    // S30: every platform builds a game (iOS: an Xcode project, built on a Mac)
    return platform != BuildPlatform::Custom && platform != BuildPlatform::Count;
}

std::set<ShaderBackend> ExportWindow::platformBackends(BuildPlatform platform) const {
    // Same presets as the Custom Graphics APIs page
    using Target = jasperengine::editor::BuildPlatform;
    switch (platform) {
        case BuildPlatform::Windows: return sourceExportBackends(Target::Windows);
        case BuildPlatform::MacOS: return sourceExportBackends(Target::MacOS);
        case BuildPlatform::Linux: return sourceExportBackends(Target::Linux);
        case BuildPlatform::Android: return sourceExportBackends(Target::Android);
        case BuildPlatform::IOS: return sourceExportBackends(Target::IOS);
        case BuildPlatform::Web: return sourceExportBackends(Target::Web);
        default: return {};
    }
}

std::set<ShaderBackend> ExportWindow::selectedSourcePlatformBackends() const {
    std::set<ShaderBackend> backends;
    auto add = [&](BuildPlatform platform) {
        const std::set<ShaderBackend> preset = platformBackends(platform);
        backends.insert(preset.begin(), preset.end());
    };
    if (m_sourcePlatformWindows) add(BuildPlatform::Windows);
    if (m_sourcePlatformLinux) add(BuildPlatform::Linux);
    if (m_sourcePlatformMacOS) add(BuildPlatform::MacOS);
    if (m_sourcePlatformIOS) add(BuildPlatform::IOS);
    if (m_sourcePlatformAndroid) add(BuildPlatform::Android);
    if (m_sourcePlatformWeb) add(BuildPlatform::Web);
    return backends;
}

void ExportWindow::selectPlatform(BuildPlatform platform, bool buildPlayer) {
    if (!platformCanBuildPlayer(platform)) buildPlayer = false;
    m_platform = platform;
    m_buildPlayer = buildPlayer;
    m_platformInitialized = true;

    selectMode(buildPlayer ? ExportMode::Player : ExportMode::SourceCode);
}

void ExportWindow::setStartScene(uint32_t sceneId) {
    if (!m_project || sceneId == m_startSceneId) return;
    m_startSceneId = sceneId;
    m_projectSettingsDirty = m_projectSettingsDirty || m_project->getStartSceneId() != sceneId;
    m_project->setStartSceneId(sceneId);
    saveCurrentSettingsToProject();
}

void ExportWindow::drawScenesInBuild(float height) {
    const ImGuiStyle& style = ImGui::GetStyle();

    // Edit Scenes In Build can remove the start scene: fall back to the first saved one.
    if (const SceneProject* start = m_project->getScene(m_startSceneId); !start || start->filepath.empty()) {
        m_startSceneId = NULL_PROJECT_SCENE;
        for (const auto& scene : m_project->getScenes()) {
            if (!scene.filepath.empty()) {
                m_startSceneId = scene.id;
                break;
            }
        }
    }

    // Unity order: the start scene is index 0, the rest follow in project order.
    std::vector<const SceneProject*> scenes;
    if (const SceneProject* start = m_project->getScene(m_startSceneId); start && !start->filepath.empty()) {
        scenes.push_back(start);
    }
    for (const auto& scene : m_project->getScenes()) {
        if (!scene.filepath.empty() && scene.id != m_startSceneId) scenes.push_back(&scene);
    }

    const float buttonsHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    if (ImGui::BeginChild("##ScenesInBuild", ImVec2(0, std::max(1.0f, height - buttonsHeight)), ImGuiChildFlags_Borders)) {
        if (scenes.empty()) {
            ImGui::TextDisabled("No saved scenes. Save a scene to add it to the build.");
        } else if (ImGui::BeginTable("##ScenesInBuildTable", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Included", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("Scene", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Start Scene   00").x);

            for (size_t i = 0; i < scenes.size(); i++) {
                const SceneProject* scene = scenes[i];
                ImGui::PushID(static_cast<int>(scene->id));
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                // Every project scene ships; Edit Scenes In Build changes the list.
                bool included = true;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.0f));
                ImGui::BeginDisabled();
                ImGui::Checkbox("##included", &included);
                ImGui::EndDisabled();
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Every scene of the project is in the build.\nUse Edit Scenes In Build... to add or remove scenes.");
                }

                ImGui::TableNextColumn();
                std::error_code ec;
                fs::path relative = fs::relative(scene->filepath, m_project->getProjectPath(), ec);
                const std::string path = (ec || relative.empty()) ? scene->filepath.filename().generic_string()
                                                                  : relative.generic_string();
                if (ImGui::Selectable(path.c_str(), m_selectedSceneId == scene->id,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    m_selectedSceneId = scene->id;
                }
                if (ImGui::BeginPopupContextItem("##scene_context")) {
                    m_selectedSceneId = scene->id;
                    ImGui::BeginDisabled(i == 0);
                    if (ImGui::MenuItem("Set As Start Scene")) setStartScene(scene->id);
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                }

                ImGui::TableNextColumn();
                char indexText[32];
                snprintf(indexText, sizeof(indexText), "%zu", i);
                if (i == 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextUnformatted("Start Scene");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                }
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(indexText).x));
                ImGui::TextUnformatted(indexText);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    const char* startLabel = "Set As Start Scene";
    const char* editLabel = "Edit Scenes In Build...";
    const float startWidth = textButtonWidth(startLabel);
    const float editWidth = textButtonWidth(editLabel);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - startWidth - editWidth - style.ItemSpacing.x));
    const bool canSetStart = m_selectedSceneId != NULL_PROJECT_SCENE && m_selectedSceneId != m_startSceneId &&
                             m_project->getScene(m_selectedSceneId) != nullptr;
    ImGui::BeginDisabled(!canSetStart);
    if (ImGui::Button(startLabel)) setStartScene(m_selectedSceneId);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The start scene (index 0) is the first scene the player loads.");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_project->isAnyScenePlaying());
    if (ImGui::Button(editLabel)) m_scenesWindow.open(m_project);
    ImGui::EndDisabled();
}

void ExportWindow::drawPlatformList() {
    const float rowHeight = Theme::dpi(34.0f);
    const float baseSize = ImGui::GetStyle().FontSizeBase;
    {
        std::vector<std::string> names;
        for (int i = 0; i < static_cast<int>(BuildPlatform::Count); i++) names.push_back(platformName(static_cast<BuildPlatform>(i)));
        int chosen = static_cast<int>(m_platform);
        if (EditorModal::enumField("platform", chosen, names) && chosen != static_cast<int>(m_platform)) {
            selectPlatform(static_cast<BuildPlatform>(chosen), platformCanBuildPlayer(static_cast<BuildPlatform>(chosen)));
        }
    }
    for (int i = 0; i < static_cast<int>(BuildPlatform::Count); i++) {
        const BuildPlatform platform = static_cast<BuildPlatform>(i);
        if (platform == BuildPlatform::Custom) {
            ImGui::Separator();
        }
        ImGui::PushID(i);
        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##platform", m_platform == platform, 0, ImVec2(0, rowHeight))) {
            if (m_platform != platform) selectPlatform(platform, platformCanBuildPlayer(platform));
        }
        if (platform == hostPlatform()) {
            ImGui::SetItemTooltip("This computer: builds a ready-to-run game.");
        } else if (platform == BuildPlatform::IOS) {
            ImGui::SetItemTooltip("Requires a Mac with Xcode to build and sign; elsewhere the build is the Xcode project.");
        } else if (platform == BuildPlatform::Custom) {
            ImGui::SetItemTooltip("C++ source project with hand-picked graphics APIs.");
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        const float iconSize = baseSize * 1.35f;
        const float iconSlot = Theme::dpi(30.0f);
        ImFont* font = ImGui::GetFont();
        const char* icon = platformIcon(platform);
        const ImVec2 iconExtent = font->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon);
        drawList->AddText(font, iconSize,
                          ImVec2(rowPos.x + Theme::dpi(4.0f) + (iconSlot - iconExtent.x) * 0.5f, rowPos.y + (rowHeight - iconExtent.y) * 0.5f),
                          textColor, icon);
        const float textHeight = ImGui::GetFontSize();
        drawList->AddText(ImVec2(rowPos.x + Theme::dpi(8.0f) + iconSlot, rowPos.y + (rowHeight - textHeight) * 0.5f),
                          textColor, platformName(platform));
        ImGui::PopID();
    }
}

void ExportWindow::drawBuildTypeRow() {
    exportBeginSettingsRow("Build Type");
    if (!platformCanBuildPlayer(m_platform)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("C++ Source Project");
        ImGui::SetItemTooltip("Players for %s are built from the exported project with the platform's own tools.", platformName(m_platform));
        return;
    }

    const char* playerLabel = m_platform == BuildPlatform::Web ? "Web Game" : m_platform == BuildPlatform::IOS ? "Xcode Project / IPA"
                            : m_platform == BuildPlatform::Android ? "APK / App Bundle" : "Executable Game";
    const char* labels[] = {playerLabel, "C++ Source Project"};
    int index = m_buildPlayer ? 0 : 1;
    if (EditorModal::enumField("build_type", index, {labels[0], labels[1]})) selectPlatform(m_platform, index == 0);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##BuildType", &index, labels, 2)) {
        selectPlatform(m_platform, index == 0);
    }
}

// Unity's Development Build: a Release game that keeps the profiler (and its --profile options)
void ExportWindow::drawDevelopmentBuildRow() {
    exportBeginSettingsRow("Development Build");
    bool development = m_project->isDevelopmentBuild();
    if (EditorModal::checkbox("##DevelopmentBuild", "development_build", &development)) {
        m_project->setDevelopmentBuild(development);
        m_projectSettingsDirty = true;
    }
    ImGui::SetItemTooltip("Keeps the profiler in a Release build: --profile and --profile-capture=<file> record it (docs/profiling.md). Debug builds always have it.");
}

void ExportWindow::drawSourceBackendsRow() {
    exportBeginSettingsRow("Graphics APIs");
    std::string names;
    for (ShaderBackend backend : platformBackends(m_platform)) {
        if (!names.empty()) names += ", ";
        names += ShaderPool::getShaderBackendName(backend);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", names.c_str());
    ImGui::SetItemTooltip("Shaders are compiled for these graphics APIs. Pick them by hand under Custom Graphics APIs.");
}

fs::path ExportWindow::builtPlayerPath() const {
    // Mirrors Exporter::getAppName()
    std::string exeName = m_project->getName().empty() ? "jasperengine-project" : Factory::toIdentifier(m_project->getName());
    #ifdef _WIN32
    exeName += ".exe";
    #endif
    return m_targetDir / exeName;
}

void ExportWindow::launchBuiltPlayer() {
    if (m_mode != ExportMode::Desktop && !(m_mode == ExportMode::Player && shipPlatform() == hostBuildPlatform())) return;

    fs::path exePath = builtPlayerPath();
    if (m_mode == ExportMode::Player) {
        exePath = m_exporter.getResult().executable;
        #ifdef __APPLE__
        // a bundle opens through LaunchServices
        if (exePath.extension() == ".app") {
            const std::string command = "open '" + exePath.string() + "'";
            if (std::system(command.c_str()) != 0) Out::error("Build And Run: could not open %s", exePath.string().c_str());
            return;
        }
        #endif
    }
    std::error_code ec;
    if (!fs::exists(exePath, ec)) {
        Out::error("Build And Run: player not found at %s", exePath.string().c_str());
        return;
    }

    // The player resolves assets and lua relative to its working directory. Launched detached
    // WITHOUT a shell (the path is one argument), inheriting no descriptor, and never left as a
    // zombie child of the editor; an exec failure is reported (CommandRunner::launchDetached)
    std::string error;
    if (!CommandRunner::launchDetached(exePath, exePath.parent_path(), error)) {
        Out::error("Build And Run: could not start %s: %s", exePath.string().c_str(), error.c_str());
    } else {
        Out::info("Running %s", exePath.string().c_str());
    }
}

void ExportWindow::drawOutputDirRow(const char* label) {
    exportBeginSettingsRow(label);
    {
        float browseWidth = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2;
        float inputWidth = ImGui::GetContentRegionAvail().x - browseWidth - ImGui::GetStyle().ItemSpacing.x;

        Vector2 pathSize = Vector2(inputWidth, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2);
        std::string agentTarget = FileUtils::pathToUtf8(m_targetDir);
        if (EditorModal::pathField("output_directory", agentTarget)) {
            m_targetDir = FileUtils::pathFromUtf8(agentTarget);
            m_targetDirFromDefault = false;
            saveCurrentSettingsToProject();
        }
        Widgets::pathDisplay("##TargetPath", m_targetDir, pathSize);

        ImGui::SameLine();
        if (ImGui::Button("Browse##target")) {
            std::string homeDirPath;
            #ifdef _WIN32
            const char* homeDir = getenv("USERPROFILE");
            #else
            const char* homeDir = getenv("HOME");
            #endif
            if (homeDir) homeDirPath = std::filesystem::path(homeDir).string();

            std::string startDir = m_targetDir.empty() ? homeDirPath : m_targetDir.string();
            std::string selectedPath = FileDialogs::openFileDialog(startDir, FILE_DIALOG_ALL, true);
            if (!selectedPath.empty()) {
                m_targetDir = selectedPath;
                m_targetDirFromDefault = false;
                saveCurrentSettingsToProject();
            }
        }
    }
}

void ExportWindow::drawGraphicBackendRow() {
    exportBeginSettingsRow("Graphics API");

    if (m_mode == ExportMode::Web) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("WebGL 2 (OpenGL ES 3)");
        ImGui::SetItemTooltip("Web players use WebGL 2");
        return;
    }

    if (m_graphicBackendIndex < 0 || m_graphicBackendIndex >= desktopGraphicBackendCount) {
        m_graphicBackendIndex = 0;
    }

    const std::string preview = ShaderPool::getShaderBackendName(desktopGraphicBackends[m_graphicBackendIndex]);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##GraphicBackend", preview.c_str())) {
        for (int i = 0; i < desktopGraphicBackendCount; ++i) {
            const bool selected = (m_graphicBackendIndex == i);
            if (ImGui::Selectable(ShaderPool::getShaderBackendName(desktopGraphicBackends[i]).c_str(), selected)) {
                m_graphicBackendIndex = i;
                m_desktopBackendConfigured = true;
                saveCurrentSettingsToProject();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void ExportWindow::drawDesktopKitRows() {
    const std::string& cCompiler = m_buildSettings.cCompiler;
    const std::string& cxxCompiler = m_buildSettings.cxxCompiler;
    const std::string& generator = m_buildSettings.generator;

    std::string kitDisplay;
    if (cCompiler.empty() && cxxCompiler.empty() && generator.empty()) {
        kitDisplay = "Default (system toolchain)";
    } else {
        kitDisplay = !cxxCompiler.empty() ? cxxCompiler : cCompiler;
        if (!generator.empty()) {
            kitDisplay += kitDisplay.empty() ? generator : " (" + generator + ")";
        }
    }

    exportBeginSettingsRow("Compiler");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", kitDisplay.c_str());
    ImGui::SetItemTooltip("Change in Preferences > External Tools");

    exportBeginSettingsRow("Build Jobs");
    ImGui::SetNextItemWidth(-1);
    const int previousJobs = m_desktopBuildJobs;
    if (EditorModal::inputInt("##DesktopBuildJobs", "build_jobs", &m_desktopBuildJobs, 1, 8)) {
        m_desktopBuildJobs = std::clamp(m_desktopBuildJobs, 1, static_cast<int>(Generator::MAX_SUPPORTED_PARALLEL_BUILD_JOBS));
        if (m_desktopBuildJobs != previousJobs) {
            saveCurrentSettingsToProject();
        }
    }
    ImGui::SetItemTooltip("Parallel build jobs. Default: %u (automatic maximum).", Generator::getAutomaticParallelBuildJobs());
}

void ExportWindow::drawShaderSection() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Shader variants compiled into the build");
    ImGui::SameLine();

    const char* addLabel = ICON_FA_PLUS " Add";
    const bool hasSelection = m_selectedShaderIndex >= 0 && m_selectedShaderIndex < (int)m_shaderEntries.size();
    const bool selectionExcluded = hasSelection && m_shaderExclusions.count(m_shaderEntries[m_selectedShaderIndex].key) > 0;
    const char* deleteLabel = selectionExcluded ? ICON_FA_ROTATE_LEFT " Restore" : ICON_FA_TRASH " Delete";
    float buttonsGroupWidth = textButtonWidth(addLabel) + textButtonWidth(deleteLabel) + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonsGroupWidth);

    if (ImGui::Button(addLabel)) {
        m_addShaderOpen = true;
        m_addShaderTypeIndex = 0;
        memset(m_addShaderProps, 0, sizeof(m_addShaderProps));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button(deleteLabel)) {
        const auto key = m_shaderEntries[m_selectedShaderIndex].key;
        if (selectionExcluded) {
            includeShader(key);
        } else {
            std::set<ShaderKey> additions = m_shaderAdditions;
            std::set<ShaderKey> exclusions = m_shaderExclusions;
            additions.erase(key);
            exclusions.insert(key);
            applyShaderOverrides(additions, exclusions);
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();

    if (!m_shaderListStatus.complete()) {
        ImGui::TextDisabled("Reading %d closed scene(s) for this list...", (int)m_shaderListStatus.pendingScenes.size());
    }

    ImGui::BeginChild("ShaderList", ImVec2(0, Theme::dpi(160.0f)), true);
    {
        for (int i = 0; i < (int)m_shaderEntries.size(); i++) {
            const auto& entry = m_shaderEntries[i];
            bool isSelected = (m_selectedShaderIndex == i);
            const bool isOverride = m_shaderAdditions.count(entry.key) > 0;
            const bool isExcluded = m_shaderExclusions.count(entry.key) > 0;
            const char* icon = isExcluded ? ICON_FA_TRASH : isOverride ? ICON_FA_CIRCLE_PLUS : ICON_FA_CUBES;
            const std::string label = std::string(icon) + "  " + entry.displayName +
                                      (isExcluded ? " (Excluded)" : "") + "###shader_" + ShaderPool::getShaderKeyText(entry.key);

            // Excluded shaders remain selectable for Restore, but use disabled text.
            if (isExcluded) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                m_selectedShaderIndex = i;
            }
            if (isExcluded) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", isExcluded ? "Excluded from the build. Select this shader and click Restore to include it again."
                                       : isOverride ? "Shader override: manually added to the build."
                                                    : "Collected from the scenes and prefabs in the build.");
        }
    }
    ImGui::EndChild();

    ImGui::BeginDisabled(m_shaderAdditions.empty() && m_shaderExclusions.empty());
    if (ImGui::Button("Reset Shader Overrides")) {
        applyShaderOverrides({}, {});
    }
    ImGui::EndDisabled();
    drawAddShaderDialog();
}

void ExportWindow::applySourcePlatformPresets() {
    const std::set<ShaderBackend> required = selectedSourcePlatformBackends();

    for (auto& entry : m_backendEntries) {
        if (required.count(entry.backend) > 0) {
            // Preserve manual choices for backends already covered by a preset.
            if (m_sourcePresetBackends.count(entry.backend) == 0) {
                entry.selected = true;
            }
        } else if (m_sourcePresetBackends.count(entry.backend) > 0) {
            entry.selected = false;
        }
    }
    m_sourcePresetBackends = required;
    m_sourceBackendsConfigured = true;
}

void ExportWindow::drawBackendSection() {
    settingsui::boldText("Graphics APIs");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted("Use platform presets to select compatible graphics APIs, "
                               "then fine-tune the selection by hand. "
                               "Switching tabs keeps your current selection. "
                               "Shaders are compiled for every selected graphics API.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::Spacing();

    if (ImGui::BeginTabBar("source_backend_tabs")) {
        // Only backend choices are persisted, so show those directly when reloading.
        const bool selectPresets = m_selectSourceBackendTab && !m_sourceBackendsConfigured;
        const bool selectCustom = m_selectSourceBackendTab && m_sourceBackendsConfigured;
        m_selectSourceBackendTab = false;
        if (ImGui::BeginTabItem("Platform Presets", nullptr, selectPresets ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (ImGui::BeginTable("source_platforms_table", 3, ImGuiTableFlags_SizingStretchSame)) {
                auto platformCheck = [&](const char* label, bool& value) {
                    ImGui::TableNextColumn();
                    if (ImGui::Checkbox(label, &value)) {
                        applySourcePlatformPresets();
                        saveCurrentSettingsToProject();
                    }
                };
                platformCheck("Windows", m_sourcePlatformWindows);
                platformCheck("Linux", m_sourcePlatformLinux);
                platformCheck("macOS", m_sourcePlatformMacOS);
                platformCheck("iOS", m_sourcePlatformIOS);
                platformCheck("Android", m_sourcePlatformAndroid);
                platformCheck("Web", m_sourcePlatformWeb);
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("By Graphics API", nullptr, selectCustom ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (ImGui::BeginTable("backends_table", 3, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableSetupColumn("BackendCol1", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("BackendCol2", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("BackendCol3", ImGuiTableColumnFlags_WidthStretch);
                for (auto& entry : m_backendEntries) {
                    ImGui::TableNextColumn();
                    std::string checkboxId = "##backend_" + entry.name;
                    if (ImGui::Checkbox((entry.name + checkboxId).c_str(), &entry.selected)) {
                        m_sourceBackendsConfigured = true;
                        saveCurrentSettingsToProject();
                    }
                    ImGui::SetItemTooltip("%s", ShaderPool::getShaderLangStr(entry.backend).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    std::string selectedBackends;
    for (const auto& entry : m_backendEntries) {
        if (!entry.selected) continue;
        if (!selectedBackends.empty()) selectedBackends += ", ";
        selectedBackends += entry.name;
    }
    ImGui::Spacing();
    ImGui::TextWrapped("Selected graphics APIs: %s", selectedBackends.empty() ? "None" : selectedBackends.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

bool ExportWindow::drawPlatformSettings(bool& targetNotEmpty) {
    const bool isCustom = m_platform == BuildPlatform::Custom;

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(platformIcon(m_platform));
    ImGui::PopFont();
    ImGui::SameLine();
    settingsui::boldText(platformName(m_platform), 1.3f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0f);
    if (m_mode == ExportMode::Player) {
        if (m_methodChoice.method == BuildMethod::Compile)
            ImGui::Text("Builds the game from source: %s.", m_methodChoice.reasons.empty() ? "Compile chosen" : m_methodChoice.reasons.front().c_str());
        else
            ImGui::TextUnformatted("Builds the game with a prebuilt player: no compiler needed.");
    } else if (m_mode == ExportMode::Desktop) {
        ImGui::TextUnformatted("Builds a ready-to-run native player for this computer.");
    } else if (m_mode == ExportMode::Web) {
        ImGui::TextUnformatted("Builds an HTML and WebAssembly player with Emscripten.");
    } else if (isCustom) {
        ImGui::TextUnformatted("Exports the C++ source project with the graphics APIs picked below.");
    } else {
        ImGui::Text("Exports the C++ source project for %s.", platformName(m_platform));
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushItemWidth(-1);
    if (ImGui::BeginTable("export_settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(140.0f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (!isCustom) drawBuildTypeRow();
        drawDevelopmentBuildRow();
        drawOutputDirRow("Output Directory");
        if (m_mode == ExportMode::Player) {
            drawPlayerRows();
            if (m_methodChoice.method == BuildMethod::Compile && shipPlatform() == hostBuildPlatform()) drawDesktopKitRows();
        } else if (m_mode == ExportMode::Desktop) {
            drawGraphicBackendRow();
            drawDesktopKitRows();
        } else if (m_mode == ExportMode::Web) {
            drawGraphicBackendRow();
        } else if (!isCustom) {
            drawSourceBackendsRow();
        }

        ImGui::EndTable();
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();
    if (isCustom) {
        drawBackendSection();
    }

    // a scene a frame, unfolded or not: a scene it cannot read is shown below, outside the section
    if (m_project && !m_shaderListStatus.complete() && !m_project->isGeneratedExportRunning()) {
        ShaderListLimit oneScene;
        oneScene.maxScenes = 1;
        m_shaderListStatus = m_project->refreshClosedSceneShaderKeys(oneScene);
        if (m_shaderListStatus.complete()) {
            refreshShaderSelection();
        }
    }

    if (Theme::foldoutHeader("Shaders")) {
        drawShaderSection();
        ImGui::Spacing();
    }

    if (Theme::foldoutHeader("Textures")) {
        drawTextureSection();
        ImGui::Spacing();
    }

    // --- Validation warnings ---
    bool canExport = !m_targetDir.empty();
    // what the warnings below say, for agents: Build is greyed out for the first of them
    std::string problem = m_targetDir.empty() ? "Choose an output directory" : "";
    auto refuse = [&](const std::string& why) {
        if (problem.empty()) problem = why;
        canExport = false;
    };
    std::error_code ec;
    bool targetExists = !m_targetDir.empty() && fs::exists(m_targetDir, ec);
    targetNotEmpty = targetExists && !fs::is_empty(m_targetDir, ec);

    const ImVec4 warningColor(1.0f, 0.5f, 0.0f, 1.0f);
    ImGui::PushTextWrapPos(0.0f);
    if (m_targetDir.empty()) {
        ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " Choose an output directory");
    }
    if (targetNotEmpty) {
        ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " The output directory is not empty: existing files will be overwritten");
    }
    for (const UnreadableScene& scene : m_shaderListStatus.unreadableScenes) {
        ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " Cannot read scene %s: its shaders may be missing from the build", scene.path.c_str());
        ImGui::SetItemTooltip("%s\nThe shader list holds what it last knew of this scene.", scene.error.c_str());
    }

    bool hasSavedScenes = false;
    for (const auto& scene : m_project->getScenes()) {
        if (!scene.filepath.empty()) {
            hasSavedScenes = true;
            break;
        }
    }

    if (!hasSavedScenes) {
        ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " No saved scenes in the build");
        refuse("No saved scenes in the build");
    } else if (std::none_of(m_shaderEntries.begin(), m_shaderEntries.end(), [this](const ShaderEntry& entry) {
        return m_shaderExclusions.count(entry.key) == 0;
    })) {
        ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " No shaders selected for the build");
    }

    if (m_mode == ExportMode::SourceCode) {
        if (isCustom) {
            bool hasSelectedBackends = false;
            for (const auto& entry : m_backendEntries) {
                if (entry.selected) { hasSelectedBackends = true; break; }
            }
            if (!hasSelectedBackends) {
                ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " No graphics APIs selected");
                refuse("No graphics APIs selected");
            }
        }
    } else if (m_mode == ExportMode::Player) {
        std::string why;
        if (playerBuildBlocked(why)) {
            ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " %s", why.c_str());
            refuse(why);
        }
    } else if (m_mode == ExportMode::Desktop) {
        if (!m_missingBuildTools.empty()) {
            ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " Missing build tools:\n%s", m_missingBuildTools.c_str());
            refuse("Missing build tools: " + m_missingBuildTools);
        }
    } else if (m_mode == ExportMode::Web) {
        if (!m_emsdkInfo.found) {
            ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " Emscripten SDK not found. Set it in Preferences > External Tools.");
            refuse("Emscripten SDK not found. Set it in Preferences > External Tools.");
        }
        // emcc.bat runs through cmd.exe on Windows: no '%' in the project, build tree or output path
        const std::string pathRefusal = Exporter::webBuildRefusal(
            {m_project->getProjectPath(), Exporter::buildCacheDirFor(m_project, ExportMode::Web), m_targetDir});
        if (!pathRefusal.empty()) {
            ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " %s.", pathRefusal.c_str());
            refuse(pathRefusal);
        }
    }
    ImGui::PopTextWrapPos();
    EditorModal::error(problem);

    return canExport;
}

void ExportWindow::drawTextureSection() {
    // the families the platform loads (R9) and what their cooks weigh (R25)
    std::vector<std::string> platforms;
    switch (m_platform) {
        case BuildPlatform::Windows: platforms = {"windows"}; break;
        case BuildPlatform::MacOS: platforms = {"macos"}; break;
        case BuildPlatform::Linux: platforms = {"linux"}; break;
        case BuildPlatform::Android: platforms = {"android"}; break;
        case BuildPlatform::IOS: platforms = {"ios"}; break;
        case BuildPlatform::Web: platforms = {"web"}; break;
        default: platforms = {"windows", "linux", "macos", "ios", "android", "web"}; break;
    }
    const std::vector<TextureFamily> families =
        TextureCookQueue::familiesForExport(platforms, m_project->getTextureSettings().androidEtc2Fallback);
    TextureCookQueue& cooks = TextureCookQueue::get();
    if (cooks.getProject() != m_project) {
        ImGui::TextDisabled("Texture cooks start once the project is open.");
        return;
    }
    const std::vector<std::string> textures = projectTextureSources(*m_project, true);
    bool missing = false;
    if (ImGui::BeginTable("export_textures", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Family");
        ImGui::TableSetupColumn("Cooked");
        ImGui::TableSetupColumn("Download");
        ImGui::TableSetupColumn("VRAM");
        ImGui::TableHeadersRow();
        for (TextureFamily family : families) {
            int cooked = 0, total = 0;
            uint64_t disk = 0, vram = 0;
            for (const std::string& path : textures) {
                TextureCookState state;
                const bool known = cooks.getState(path, family, state);
                if (known && state.status == TextureCookStatus::Source) continue;
                total++;
                if (known && state.status == TextureCookStatus::Cooked) {
                    cooked++;
                    disk += state.record.diskBytes;
                    vram += state.record.gpuBytes;
                }
            }
            missing = missing || cooked < total;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(textureFamilyLabel(family));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d / %d", cooked, total);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(TextureCookReport::formatBytes(disk).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(TextureCookReport::formatBytes(vram).c_str());
        }
        ImGui::EndTable();
    }
    if (missing) {
        ImGui::TextDisabled("The totals cover the cooked textures; the export cooks the rest.");
    }
    if (std::find(families.begin(), families.end(), TextureFamily::AndroidEtc2) != families.end()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION
                           " Android ETC2 Fallback is on: the APK carries every texture twice (ASTC and ETC2)");
    }
    if (ImGui::Button("Cook Textures Now")) {
        cooks.requestAll(families, TextureCookQueue::Selected);
    }
    ImGui::SetItemTooltip("Cooks every texture for these families in the background, so the export finds them in the cache");
}

void ExportWindow::drawSettings() {
    if (ImGui::BeginTabBar("##BuildSettingsTabs")) {
        if (ImGui::BeginTabItem("Build")) {
            drawBuildTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Publish")) {
            drawPublishTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void ExportWindow::drawBuildTab() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float footerHeight = ImGui::GetFrameHeight() + style.ItemSpacing.y;

    settingsui::boldText("Scenes In Build");
    const float scenesHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.3f, Theme::dpi(110.0f), Theme::dpi(200.0f));
    drawScenesInBuild(scenesHeight);
    ImGui::Spacing();

    const float bottomHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - footerHeight - style.ItemSpacing.y);
    const float listWidth = std::min(Theme::dpi(230.0f), ImGui::GetContentRegionAvail().x * 0.35f);

    ImGui::BeginGroup();
    settingsui::boldText("Platform");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    if (ImGui::BeginChild("##PlatformList", ImVec2(listWidth, std::max(1.0f, bottomHeight - ImGui::GetTextLineHeightWithSpacing())),
                          ImGuiChildFlags_Borders)) {
        drawPlatformList();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    ImGui::SameLine();

    bool targetNotEmpty = false;
    bool canExport = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(12.0f, 8.0f)));
    if (ImGui::BeginChild("##PlatformSettings", ImVec2(0, bottomHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        canExport = drawPlatformSettings(targetNotEmpty);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // --- Footer: Player Settings... on the left, Build / Build And Run on the right ---
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight());
    if (ImGui::Button("Player Settings...")) {
        m_playerSettingsRequested = true;
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemTooltip("Open Project Settings > Player");
    // S04: a build referencing missing assets fails unless exported anyway
    ImGui::SameLine();
    EditorModal::checkbox("Export Anyway", "export_anyway", &m_allowMissingAssets);
    ImGui::SetItemTooltip("Build even when the build's scenes reference missing assets (Window > Assets > Missing Assets lists them)");

    const float actionWidth = Theme::dpi(120.0f);
    const float buttonsWidth = actionWidth * 2.0f + style.ItemSpacing.x;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - buttonsWidth);

    auto startBuild = [&](bool run) {
        m_runAfterBuild = run;
        m_launchDone = false;
        if (targetNotEmpty) {
            ImGui::OpenPopup("Directory Not Empty##ExportOverwrite");
        } else {
            startConfiguredExport(false);
        }
    };

    const bool runnable = m_mode == ExportMode::Desktop || (m_mode == ExportMode::Player && m_platform == hostPlatform());
    ImGui::BeginDisabled(!canExport || (m_download && m_download->running.load()));
    EditorModal::message("Build the project: the Build tab's fields, then ok (Build) or build_and_run. A target that is not empty asks first.");
    if (EditorModal::button("Build", "ok", ImVec2(actionWidth, 0))) startBuild(false);
    ImGui::SameLine();
    ImGui::BeginDisabled(!runnable);
    if (EditorModal::button("Build And Run", "build_and_run", ImVec2(actionWidth, 0))) startBuild(true);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!runnable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Build And Run needs a game for this computer.");
    }

    drawOverwriteConfirmDialog();
    drawKeystorePasswordDialog();
    m_scenesWindow.show();
}

void ExportWindow::drawOverwriteConfirmDialog() {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(Theme::dpi(ImVec2(440.0f, 0.0f)), ImVec2(Theme::dpi(440.0f), FLT_MAX));

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal;

    if (!EditorModal::begin("Directory Not Empty##ExportOverwrite", nullptr, flags)) return;
    EditorModal::message("This directory already contains files: " + m_targetDir.generic_string() + ". Files with the same name will be overwritten.");

    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " This directory already contains files:");
    ImGui::Spacing();

    Vector2 pathSize = Vector2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2);
    Widgets::pathDisplay("##OverwritePath", m_targetDir, pathSize);

    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    if (m_mode == ExportMode::Player) {
        ImGui::Text("Its contents are replaced by the new build once that is complete (until then they stay as they are).");
    } else {
        ImGui::Text("Files with the same name will be overwritten.");
    }
    if (m_mode == ExportMode::SourceCode) {
        ImGui::TextDisabled("Files left from an earlier build are not removed and may break it.");
    }
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float actionWidth = Theme::dpi(120.0f);
    float buttonsWidth = actionWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonsWidth);

    if (EditorModal::button("Overwrite", "yes", ImVec2(actionWidth, 0))) {
        ImGui::CloseCurrentPopup();
        startConfiguredExport(true);
    }
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(actionWidth, 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void ExportWindow::startConfiguredExport(bool overwriteTarget) {
    ExportConfig exportConfig;
    exportConfig.mode = m_mode;
    exportConfig.overwriteTarget = overwriteTarget;
    exportConfig.allowMissingAssets = m_allowMissingAssets;

    if (m_mode == ExportMode::Player) {
        // an Android release build with a keystore asks its passwords first (never saved)
        const AndroidProjectSettings& android = m_project->getAndroidProjectSettings();
        if (shipPlatform() == ShipPlatform::Android && !android.keystorePath.empty() && !m_askKeystorePasswords &&
            m_keystorePassword[0] == '\0' && !std::getenv("JASPER_ANDROID_KS_PASS")) {
            m_askKeystorePasswords = true;
            m_pendingOverwrite = overwriteTarget;
            return;
        }
        exportConfig.destinationDir = m_targetDir;
        exportConfig.platform = shipPlatform();
        exportConfig.graphicBackend = playerBackend();
        exportConfig.iosSimulator = m_iosSimulator;
        exportConfig.androidFormat = AndroidFormat::Aab;
        parseAndroidFormat(android.packageFormat, exportConfig.androidFormat);
        exportConfig.keystorePassword = m_keystorePassword;
        exportConfig.keyPassword = m_keyPassword;
        std::fill(std::begin(m_keystorePassword), std::end(m_keystorePassword), '\0');
        std::fill(std::begin(m_keyPassword), std::end(m_keyPassword), '\0');
        // what a Compile build of this computer or the web uses
        const LocalBuildSettings build = AppSettings::getBuildSettings(m_project->getProjectPath() / "project.yaml");
        exportConfig.cmakeCCompiler = build.cCompiler;
        exportConfig.cmakeCxxCompiler = build.cxxCompiler;
        exportConfig.cmakeGenerator = build.generator;
        exportConfig.buildJobs = static_cast<unsigned int>(m_desktopBuildJobs);
        exportConfig.emsdkPath = m_emsdkOverride;
    } else if (m_mode == ExportMode::SourceCode) {
        exportConfig.targetDir = m_targetDir;
        exportConfig.packNativeResources = m_project->shouldPackNativeResources();
        if (m_platform == BuildPlatform::Custom) {
            for (const auto& entry : m_backendEntries) {
                if (entry.selected) {
                    exportConfig.selectedBackends.insert(entry.backend);
                }
            }
        } else {
            exportConfig.selectedBackends = platformBackends(m_platform);
        }
    } else {
        exportConfig.destinationDir = m_targetDir;

        // A built export runs on a single backend, driving both its shaders and its CMake
        ShaderBackend backend = ShaderBackend::GLES3;
        if (m_mode == ExportMode::Desktop) {
            const int backendIndex =
                (m_graphicBackendIndex >= 0 && m_graphicBackendIndex < desktopGraphicBackendCount)
                    ? m_graphicBackendIndex
                    : 0;
            backend = desktopGraphicBackends[backendIndex];
        }
        exportConfig.selectedBackends.insert(backend);
        exportConfig.graphicBackend = Exporter::getCMakeGraphicBackend(backend);

        if (m_mode == ExportMode::Desktop) {
            const LocalBuildSettings build = AppSettings::getBuildSettings(m_project->getProjectPath() / "project.yaml");
            exportConfig.cmakeCCompiler = build.cCompiler;
            exportConfig.cmakeCxxCompiler = build.cxxCompiler;
            exportConfig.cmakeGenerator = build.generator;
            exportConfig.buildJobs = static_cast<unsigned int>(m_desktopBuildJobs);
            exportConfig.packNativeResources = m_project->shouldPackNativeResources();
        } else {
            exportConfig.emsdkPath = m_emsdkOverride;
        }
    }

    // Stored references are relative to these roots, set in the project settings
    exportConfig.assetsDir = m_project->getAssetsPath();
    exportConfig.luaDir = m_project->getLuaPath();

    // Set start scene
    const SceneProject* startScene = m_project->getScene(m_startSceneId);
    if (startScene && !startScene->filepath.empty()) {
        exportConfig.startSceneId = startScene->id;
        m_projectSettingsDirty = m_projectSettingsDirty || m_project->getStartSceneId() != exportConfig.startSceneId;
        m_project->setStartSceneId(exportConfig.startSceneId);
    }

    // Only the overrides: the exporter resolves them once every scene is loaded,
    // so closed scenes still contribute and an exclusion cannot empty the list early
    exportConfig.shaderAdditions = m_shaderAdditions;
    exportConfig.shaderExclusions = m_shaderExclusions;

    // An MCP agent's build job may be exporting or compiling the project right now
    const std::string conflict = editor::getEditorHost().backgroundWorkConflict("export");
    if (!conflict.empty()) {
        editor::getEditorHost().registerAlert("Cannot Export", conflict);
        return;
    }

    if (!saveCurrentSettingsToProject()) return;
    m_step = Step::Progress;
    m_exporter.startExport(m_project, exportConfig);
}

void ExportWindow::drawAddShaderDialog() {
    if (!m_addShaderOpen) return;

    ImGui::OpenPopup("Add Shader##AddShaderModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal;

    if (EditorModal::begin("Add Shader##AddShaderModal", &m_addShaderOpen, flags)) {
        // Shader type combo. Keep in sync with the ShaderType enum
        // (engine/core/render/Render.h); names come from ShaderPool so the
        // labels never drift.
        static const ShaderType typeValues[] = {
            ShaderType::POINTS, ShaderType::LINES, ShaderType::MESH, ShaderType::SKYBOX,
            ShaderType::DEPTH, ShaderType::GBUFFER, ShaderType::UI,
            ShaderType::SSAO, ShaderType::SSAO_BLUR,
            ShaderType::SSR, ShaderType::SSR_BLUR, ShaderType::COMPOSITE,
            ShaderType::SHADOW2D, ShaderType::BLIT, ShaderType::POSTPROCESS, ShaderType::BLOOM,
            ShaderType::HTMLUI, ShaderType::HTMLUI_GRADIENT, ShaderType::HTMLUI_POST, ShaderType::UI_EFFECT,
            ShaderType::PROBE_FILTER,
            ShaderType::ATMOSPHERE_DEPTH, ShaderType::VOLUMETRIC_FOG, ShaderType::ATMOSPHERE_DENOISE,
            ShaderType::VOLUMETRIC_CLOUDS, ShaderType::SKY_COMPOSITE, ShaderType::VOLUMETRIC_COMPOSITE, ShaderType::HTMLUI_PRIM
        };
        constexpr int typeCount = (int)(sizeof(typeValues) / sizeof(typeValues[0]));
        std::vector<std::string> typeNameStrs;
        std::vector<const char*> typeNames;
        for (ShaderType type : typeValues) {
            typeNameStrs.push_back(ShaderPool::getShaderTypeName(type, false));
        }
        for (const std::string& name : typeNameStrs) {
            typeNames.push_back(name.c_str());
        }

        ImGui::Text("Type");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const int shownType = m_addShaderTypeIndex;
        EditorModal::enumField("type", m_addShaderTypeIndex, std::vector<std::string>(typeNames.begin(), typeNames.begin() + typeCount));
        if (ImGui::Combo("##shader_type", &m_addShaderTypeIndex, typeNames.data(), typeCount) || m_addShaderTypeIndex != shownType) {
            // Reset properties when type changes
            memset(m_addShaderProps, 0, sizeof(m_addShaderProps));
            m_addShaderEffectIndex = 0;
        }

        // Property checkboxes
        ShaderType selectedType = typeValues[m_addShaderTypeIndex];
        int propCount = std::min(ShaderPool::getShaderPropertyCount(selectedType), SHADER_PROPERTY_BITS);

        // A UI effect (phase 5) is one of the effects: the engine's and the project's the
        // documents declared (@effect); its identity rides in the custom shader id
        const fx::EffectManifest* effect = nullptr;
        if (selectedType == ShaderType::UI_EFFECT) {
            const std::vector<const fx::EffectManifest*> effects = fx::EffectRegistry::get().all();
            std::vector<std::string> effectLabels;
            for (const fx::EffectManifest* m : effects) {
                effectLabels.push_back(m->name + " (" + fx::effectKindName(m->kind) + (m->builtin ? "" : ", " + m->sourcePath) + ")");
            }
            std::vector<const char*> effectNames;
            for (const std::string& label : effectLabels) effectNames.push_back(label.c_str());
            if (m_addShaderEffectIndex >= (int)effects.size()) m_addShaderEffectIndex = 0;
            ImGui::Text("Effect");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            EditorModal::enumField("effect", m_addShaderEffectIndex, effectLabels);
            ImGui::Combo("##shader_effect", &m_addShaderEffectIndex, effectNames.data(), (int)effectNames.size());
            if (!effects.empty()) effect = effects[m_addShaderEffectIndex];
            // a filter effect has the filter form only
            if (effect && effect->kind == fx::EffectKind::Filter) m_addShaderProps[0] = true;
            ImGui::TextDisabled("The filter form draws filter and backdrop-filter uses (and gradient text).");
        }

        if (propCount > 0) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Properties:");
            ImGui::Spacing();

            // Determine column count based on properties
            int columns = 1;
            if (propCount >= 6) columns = 2;
            if (propCount >= 12) columns = 3;

            if (ImGui::BeginTable("shader_props_table", columns, ImGuiTableFlags_NoBordersInBody)) {
                for (int i = 0; i < propCount; i++) {
                    ImGui::TableNextColumn();
                    std::string abbrev = ShaderPool::getShaderPropertyName(selectedType, i, true);
                    std::string fullName = ShaderPool::getShaderPropertyName(selectedType, i, false);
                    std::string label = fullName + " (" + abbrev + ")##prop_" + std::to_string(i);
                    EditorModal::checkbox(label.c_str(), label.c_str(), &m_addShaderProps[i]);
                }
                ImGui::EndTable();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Preview
        uint64_t props = 0;
        for (int i = 0; i < propCount; i++) {
            if (m_addShaderProps[i]) props |= shaderPropertyBit(i);
        }
        ShaderKey newKey = ShaderPool::getShaderKey(selectedType, props);
        if (selectedType == ShaderType::UI_EFFECT) {
            newKey = effect ? ShaderPool::normalizeKey(fx::EffectRegistry::get().shaderKey(*effect, (props & 1u) != 0)) : ShaderKey();
        }
        std::string preview = Exporter::getShaderDisplayName(selectedType, props, ShaderPool::getCustomIdFromKey(newKey));
        ImGui::Text("Preview: %s", preview.c_str());

        ImGui::Spacing();

        // Check for duplicate
        bool isDuplicate = false;
        for (const auto& entry : m_shaderEntries) {
            if (entry.key == newKey && m_shaderExclusions.count(newKey) == 0) { isDuplicate = true; break; }
        }
        if (isDuplicate) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "This shader already exists in the list");
        }

        // Buttons
        const float actionWidth = Theme::dpi(120.0f);
        float buttonsWidth = actionWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonsWidth);

        ImGui::BeginDisabled(isDuplicate || (selectedType == ShaderType::UI_EFFECT && !effect));
        if (EditorModal::button("Add", "yes", ImVec2(actionWidth, 0))) {
            includeShader(newKey);
            m_addShaderOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        if (EditorModal::button("Cancel", "cancel", ImVec2(actionWidth, 0)) ||
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            m_addShaderOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    } else {
        m_addShaderOpen = false;
    }
}

void ExportWindow::drawProgress() {
    ExportProgress progress = m_exporter.getProgress();

    // The export thread is not input: without this the dialog would only advance on
    // mouse moves, and the finished layout would wait for one.
    if (!progress.finished && !progress.failed) {
        Backend::getApp().requestRedraw();
    }

    ImGui::Dummy(ImVec2(0.0f, Theme::dpi(6.0f)));
    ImGui::TextUnformatted(m_mode == ExportMode::SourceCode ? "Exporting project..." : m_mode == ExportMode::Player ? "Building the game..." : "Building player...");
    ImGui::Spacing();

    ImGui::ProgressBar(progress.overallProgress, ImVec2(-1.0f, 0.0f));

    ImGui::Spacing();
    ImGui::Text("%s", progress.currentStep.c_str());
    EditorModal::message(std::string(m_mode == ExportMode::SourceCode ? "Exporting project: " : "Building player: ") +
                         (progress.failed ? "failed: " + progress.errorMessage : progress.finished ? "done" : progress.currentStep));

    // Last build-output line, clipped to a single line so the modal keeps its size.
    ImGui::BeginChild("##BuildDetail", ImVec2(0, ImGui::GetTextLineHeight() + Theme::dpi(2.0f)), false, ImGuiWindowFlags_NoScrollbar);
    if (!progress.detailLine.empty()) {
        ImGui::TextDisabled("%s", progress.detailLine.c_str());
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.0f, Theme::dpi(6.0f)));

    if (progress.failed) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped(ICON_FA_CIRCLE_XMARK " Build failed: %s", progress.errorMessage.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();

        float closeWidth = Theme::dpi(120.0f);
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - closeWidth);
        if (EditorModal::button("Close", "ok", ImVec2(closeWidth, 0))) {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    } else if (progress.finished) {
        if (m_runAfterBuild && !m_launchDone) {
            m_launchDone = true;
            launchBuiltPlayer();
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), ICON_FA_CIRCLE_CHECK " Build completed successfully!");
        // S53 R21: what ships, by category, with the full report a click away
        {
            const ExportResult built = m_exporter.getResult();
            if (!built.buildReport.empty()) {
                BuildReport report;
                std::string reportError;
                if (BuildReport::load(built.buildReport, report, reportError)) {
                    ImGui::TextUnformatted(("Build Report: " + BuildReport::formatBytes(report.totalBytes)).c_str());
                    for (const std::string& category : BuildReport::categoryNames()) {
                        const auto it = report.categories.find(category);
                        if (it == report.categories.end() || it->second == 0) continue;
                        ImGui::TextDisabled("  %-9s %s", category.c_str(), BuildReport::formatBytes(it->second).c_str());
                    }
                    if (ImGui::SmallButton(ICON_FA_CHART_BAR " Open Build Report")) editor::getEditorHost().openBuildReport(built.buildReport);
                }
            }
        }
        if (m_mode == ExportMode::Player) {
            const ExportResult result = m_exporter.getResult();
            ImGui::PushTextWrapPos(0.0f);
            if (result.methodUsed == BuildMethod::Prebuilt) ImGui::TextDisabled("Prebuilt player %s", result.playerId.c_str());
            else ImGui::TextDisabled("Compiled from source%s%s", result.compileReasons.empty() ? "" : ": ",
                                     result.compileReasons.empty() ? "" : result.compileReasons.front().c_str());
            // S34: the engine modules the game ships (Strip Engine Code), why each, and the rest
            if (!result.modules.empty() || !result.modulesLeftOut.empty()) {
                auto joined = [](const std::vector<std::string>& names) {
                    std::string text;
                    for (const std::string& name : names) text += (text.empty() ? "" : ", ") + player::moduleDisplayName(name);
                    return text.empty() ? std::string("none") : text;
                };
                ImGui::TextDisabled("Engine modules: %s. Left out: %s.", joined(result.modules).c_str(), joined(result.modulesLeftOut).c_str());
                if (ImGui::IsItemHovered() && !result.moduleEvidence.empty()) {
                    ImGui::BeginTooltip();
                    for (const ModuleEvidence& evidence : result.moduleEvidence)
                        ImGui::Text("%s: %s: %s", evidence.module.c_str(), evidence.where.c_str(), evidence.what.c_str());
                    ImGui::EndTooltip();
                }
            }
            for (const BuildWarning& warning : result.warnings)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s: %s", warning.code.c_str(), warning.message.c_str());
            for (const std::string& note : result.notes) ImGui::TextUnformatted(note.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();

        float closeWidth = Theme::dpi(120.0f);
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - closeWidth);
        if (EditorModal::button("Close", "ok", ImVec2(closeWidth, 0))) {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    } else {
        // Still running: cancelling also terminates the compiler subprocess.
        float cancelWidth = Theme::dpi(120.0f);
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - cancelWidth);
        if (EditorModal::button("Cancel", "cancel", ImVec2(cancelWidth, 0))) {
            m_exporter.cancelExport();
        }
    }
}

} // namespace jasperengine::editor
