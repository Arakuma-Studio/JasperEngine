// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorSettingsWindow.h"
#include "platform/ExternalTools.h"
#include "object/Lattice.h"
#include "window/CodeEditor.h"
#include "window/EditorModal.h"
#include "widget/ColorField.h"
#include "util/FileUtils.h"
#include "vcs/VcsService.h"
#include "diag/CrashHandler.h"
#include "SceneBackup.h"
#include "Out.h"

#include "AppSettings.h"
#include "App.h"
#include "Project.h"
#include "Theme.h"
#include "log/Logger.h"
#include "external/IconsFontAwesome6.h"
#include "util/FileDialogs.h"
#include "window/Widgets.h"
#include "SettingsCategoryList.h"

#include "reload/HotReloadService.h"
#include "util/ExternalCodeEditor.h"
#include "htmlui/VideoImporter.h"
#include "lattice/LatticeEditorOps.h"
#include "shortcuts/CommandCatalog.h"
#include "pixelart/aseprite/AsepriteEditorHooks.h"
#include "pixelart/aseprite/AsepriteLocator.h"
#include "Backend.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>

#include <cstdlib>
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shellapi.h>
#endif

namespace jasperengine::editor {

namespace {
    namespace fs = std::filesystem;

    static constexpr float prefsDialogWidth = 860.0f;
    static constexpr float prefsDialogHeight = 520.0f;
    static constexpr float prefsCategoryListWidth = 190.0f;

    enum class PreferencesCategory {
        General,
        CodeEditor,
        AssetPipeline,
        Scripting,
        Undo,
        Lattice,
        SceneView,
        ExternalTools,
        Logging,
        PixelArt,
        Diagnostics,
        ShaderCache,
        VersionControl,
        Count
    };

    // PreferencesCategory follows the order of the Preferences pages in the settings registry
    // (settings/SettingsRows.cpp), which lists them and their rows for the search

    // Sub-heading row inside a settings table (hidden while the search matches none of its rows)
    bool sectionRow(const char* title) {
        if (!settingsui::sectionVisible(title)) return false;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Spacing();
        settingsui::boldText(title);
        ImGui::TableNextColumn();
        return true;
    }
    static constexpr float prefsSettingsLabelWidth = 190.0f;
    static constexpr float prefsSettingsPanelPadding = 12.0f;
    static constexpr float prefsSettingsButtonWidth = 120.0f;
    static constexpr ImGuiWindowFlags prefsNoScrollFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    template <typename DrawContents>
    void prefsDrawSettingsPanel(const char* panelId, DrawContents drawContents) {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(prefsSettingsPanelPadding, prefsSettingsPanelPadding)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x, Theme::dpi(6.0f)));

        bool panelVisible = ImGui::BeginChild(panelId, ImVec2(0, 0), ImGuiChildFlags_Borders);
        float labelWidth = std::min(Theme::dpi(prefsSettingsLabelWidth), ImGui::GetContentRegionAvail().x * 0.4f);
        if (panelVisible && ImGui::BeginTable("##EditorSettingsTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            drawContents();
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
    }

    bool prefsBeginSettingsRow(const char* label, const char* tooltip = nullptr, bool defChanged = false) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        // the search's matches highlighted, a row the Search palette opened flashing
        settingsui::rowLabel(label);
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);

        bool reset = false;
        if (defChanged) {
            const ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, style.ItemSpacing.y));
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, style.FramePadding.y));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
            reset = ImGui::Button((ICON_FA_ROTATE_LEFT "##" + std::string(label)).c_str());
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
            ImGui::SetItemTooltip("Restore default");
        }

        ImGui::TableNextColumn();
        return reset;
    }

    void showDisabledItemTooltip(const std::string& text) {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;

        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    float buttonVisibleWidth(const char* label) {
        return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    void drawPathPicker(
        const char* id,
        const char* browseId,
        const char* autoId,
        fs::path displayPath,
        bool canAuto,
        bool selectDirectory,
        const std::function<void(const std::string&)>& onBrowse,
        const std::function<void()>& onAuto
    ) {
        const ImGuiStyle& style = ImGui::GetStyle();
        float browseWidth = buttonVisibleWidth(browseId);
        float autoWidth = buttonVisibleWidth(autoId);
        float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - browseWidth - autoWidth - style.ItemSpacing.x * 2.0f);

        Widgets::pathDisplay(id, displayPath, Vector2(pathWidth, ImGui::GetFrameHeight()));

        ImGui::SameLine();
        if (ImGui::Button(browseId)) {
            std::string startDir;
            if (!displayPath.empty() && displayPath.string().rfind("<", 0) != 0) {
                startDir = displayPath.parent_path().string();
            }
            std::string selectedPath = FileDialogs::openFileDialog(startDir, FILE_DIALOG_ALL, selectDirectory);
            if (!selectedPath.empty()) {
                onBrowse(selectedPath);
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!canAuto);
        if (ImGui::Button(autoId)) {
            onAuto();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Clear the override and use auto-detect again.");
    }
}

void EditorSettingsWindow::open(Project* project) {
    m_isOpen = true;
    m_project = project;
    m_cacheStatus.clear();

    m_availableKits = Generator::detectAvailableKits();
    m_cmakeKitIndex = 0;
    m_cmakeOverride = AppSettings::getCMakePath();
    m_cmakePickError.clear();
    refreshCMakeStatus();

    const LocalBuildSettings currentBuild = projectBuildSettings();
    const std::string& currentCxx = currentBuild.cxxCompiler;
    const std::string& currentGen = currentBuild.generator;
    if (!currentCxx.empty() || !currentGen.empty()) {
        for (size_t i = 0; i < m_availableKits.size(); i++) {
            if (m_availableKits[i].available && m_availableKits[i].cxxCompiler == currentCxx && m_availableKits[i].generator == currentGen) {
                m_cmakeKitIndex = static_cast<int>(i + 1);
                break;
            }
        }
    }

    m_emsdkOverride = AppSettings::getEmsdkPath();
    m_ffmpegOverride = AppSettings::getFfmpegPath();
    refreshFfmpegStatus();
    m_toolPaths.clear();
    for (const char* tool : {"androidSdk", "jdk", "steamcmd", "rcodesign", pixelart::aseprite::kAsepriteToolKey}) m_toolPaths[tool] = AppSettings::getExternalToolPath(tool);
    refreshPlayerRuntimes();
    m_editorVSyncEnabled = AppSettings::getEditorVSyncEnabled();
    m_projectHubAlways = AppSettings::getProjectHubAlways();
    m_uiScalePercent = static_cast<int>(std::lround(AppSettings::getUiScale() * 100.0f));
    m_autosaveEnabled = AppSettings::isAutosaveEnabled();
    m_autosaveInterval = AppSettings::getAutosaveIntervalSeconds();
    m_networkOffline = AppSettings::isNetworkOffline();
    m_htmlUiAutoImport = AppSettings::getHtmlUiAutoImport();
    m_autoRefresh = AppSettings::getAutoRefresh();
    m_fileWatcher = AppSettings::getFileWatcher() == "native" ? 1 : AppSettings::getFileWatcher() == "polling" ? 2 : 0;
    m_pollingInterval = AppSettings::getPollingInterval();
    m_logReloads = AppSettings::getLogReloads();
    {
        const std::string policy = AppSettings::getScriptChangesWhilePlaying();
        m_scriptChangesWhilePlaying = policy == "after_stop" ? 1 : policy == "stop_and_reload" ? 2 : 0;
    }
    m_reloadCppDuringPlay = AppSettings::getReloadCppDuringPlay();
    m_externalScriptEditor = 0;
    for (size_t i = 0; i < ExternalCodeEditor::choices().size(); i++) {
        if (AppSettings::getExternalScriptEditor() == ExternalCodeEditor::choices()[i].id) m_externalScriptEditor = static_cast<int>(i);
    }
    m_externalScriptEditorCommand = AppSettings::getExternalScriptEditorCommand();
    m_htmlUiVerboseDiagnostics = AppSettings::getHtmlUiVerboseDiagnostics();
    m_htmlUiPatchReload = AppSettings::getHtmlUiPatchReload();
    m_codeEditorFontSize = AppSettings::getCodeEditorFontSize();
    m_undoMaxSteps = AppSettings::getUndoMaxSteps();
    m_undoMemoryLimitMB = AppSettings::getUndoMemoryLimitMB();
    m_undoRecordSelection = AppSettings::getUndoRecordSelection();
    m_undoReviewPlayChanges = AppSettings::getUndoReviewPlayChanges();
    m_logging = AppSettings::getLoggingSettings();
    m_diagnostics = AppSettings::getDiagnosticsSettings();
    m_pixelArt = AppSettings::getPixelArtPreferences();
    m_versionControl = AppSettings::getVersionControlSettings();
    m_smartMergeStatus.clear();
    m_lattice = AppSettings::getLatticeViewSettings();
    m_sceneView = AppSettings::getSceneViewSettings();
    m_defaultExportDirectory = AppSettings::getDefaultExportDirectory();
    refreshEmsdkStatus();
}

bool EditorSettingsWindow::showPage(const std::string& name) {
    const int index = settingsui::pageIndex(SettingsScope::Preference, name);
    if (index < 0) return false;
    m_category = index;
    return true;
}

const char* EditorSettingsWindow::pageName() const {
    const SettingsPage* page = settingsui::pageAt(SettingsScope::Preference, m_category);
    return page ? page->displayName().c_str() : "";
}

void EditorSettingsWindow::open(Project* project, const std::string& category) {
    open(project);
    if (showPage(category)) {
        m_categorySearch[0] = '\0';
    }
}

LocalBuildSettings EditorSettingsWindow::projectBuildSettings() const {
    if (!m_project) return LocalBuildSettings();
    return AppSettings::getBuildSettings(m_project->getProjectPath() / "project.yaml");
}

void EditorSettingsWindow::refreshCMakeStatus() {
    m_cmakeInfo = Generator::detectCMake();
}

void EditorSettingsWindow::refreshEmsdkStatus() {
    m_emsdkInfo = Exporter::detectEmsdk(m_emsdkOverride);
}

// what the importer would use with the override being edited (it runs `ffmpeg -encoders`: only
// when the dialog opens or the path changes)
void EditorSettingsWindow::refreshFfmpegStatus() {
    std::string why;
    const std::string found = htmlui::VideoImporter::findFfmpeg(&why, &m_ffmpegOverride);
    m_ffmpegUsable = false;
    if (found.empty()) {
        m_ffmpegStatus = "Not found: " + why + ". Videos are imported unconverted (HTMLUI-IMP-0010).";
        return;
    }
    const std::vector<std::string> encoders = htmlui::VideoImporter::av1Encoders(found);
    if (encoders.empty()) {
        m_ffmpegStatus = found + " has no AV1 encoder (libsvtav1, libaom-av1 or librav1e): videos are imported unconverted.";
        return;
    }
    m_ffmpegUsable = true;
    m_ffmpegStatus = found + " (AV1: " + encoders.front() + ")";
}

void EditorSettingsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Preferences##EditorSettingsModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = viewport->GetWorkCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImVec2 size(
        std::min(Theme::dpi(prefsDialogWidth), viewport->WorkSize.x * 0.9f),
        std::min(Theme::dpi(prefsDialogHeight), viewport->WorkSize.y * 0.9f)
    );
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoResize |
                             prefsNoScrollFlags;

    bool popupOpen = EditorModal::begin("Preferences##EditorSettingsModal", &m_isOpen, flags);

    if (popupOpen) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            EditorModal::message("The editor's preferences; focus_window page opens a page, and the settings tools change them without this window.");
            drawSettings();
        }
        ImGui::EndPopup();
    }
}

void EditorSettingsWindow::drawSettings() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float footerY = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight();
    float regionHeight = std::max(1.0f, footerY - style.ItemSpacing.y * 2.0f - ImGui::GetCursorPosY());

    ImGui::BeginChild("##PreferencesRegion", ImVec2(0, regionHeight), ImGuiChildFlags_None, prefsNoScrollFlags);

    const float listWidth = std::min(Theme::dpi(prefsCategoryListWidth), ImGui::GetContentRegionAvail().x * 0.3f);
    // the Search palette opened a row: its page
    if (const std::string revealPage = settingsui::pendingRevealPage(); !revealPage.empty()) {
        const int index = settingsui::pageIndex(SettingsScope::Preference, revealPage);
        if (index >= 0) m_category = index;
    }
    settingsui::pageList("##PreferencesCategories", SettingsScope::Preference, m_category,
                         m_categorySearch, sizeof(m_categorySearch), ImVec2(listWidth, ImGui::GetContentRegionAvail().y));
    ImGui::SameLine();

    ImGui::BeginGroup();
    const SettingsPage* page = settingsui::pageAt(SettingsScope::Preference, m_category);
    settingsui::categoryTitle(page ? page->displayName().c_str() : "");
    settingsui::beginPage(page ? page->name : std::string(), m_categorySearch);
    switch (static_cast<PreferencesCategory>(m_category)) {
        case PreferencesCategory::General: drawGeneralSettings(); break;
        case PreferencesCategory::CodeEditor: drawCodeEditorSettings(); break;
        case PreferencesCategory::AssetPipeline: drawAssetPipelineSettings(); break;
        case PreferencesCategory::Scripting: drawScriptingSettings(); break;
        case PreferencesCategory::Undo: drawUndoSettings(); break;
        case PreferencesCategory::Lattice: drawLatticeSettings(); break;
        case PreferencesCategory::SceneView: drawSceneViewSettings(); break;
        case PreferencesCategory::ExternalTools: drawExternalToolsSettings(); break;
        case PreferencesCategory::Logging: drawLoggingSettings(); break;
        case PreferencesCategory::PixelArt: drawPixelArtSettings(); break;
        case PreferencesCategory::Diagnostics: drawDiagnosticsSettings(); break;
        case PreferencesCategory::ShaderCache: drawAdvancedSettings(); break;
        case PreferencesCategory::VersionControl: drawVersionControlSettings(); break;
        default: break;
    }
    settingsui::endPage();
    ImGui::EndGroup();

    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, footerY - style.ItemSpacing.y));
    ImGui::Separator();

    // Unity-style footer: buttons on the right, primary first.
    const float buttonWidth = Theme::dpi(prefsSettingsButtonWidth);
    const float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    ImGui::SetCursorPos(ImVec2(std::max(style.WindowPadding.x, ImGui::GetWindowContentRegionMax().x - buttonsWidth), footerY));

    if (EditorModal::button("OK", "ok", ImVec2(buttonWidth, 0))) {
        if (applySettings()) {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
}

// The system's file manager on a folder
void EditorSettingsWindow::openInFileManager(const fs::path& folder) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string quoted = "'";
    for (char c : folder.string()) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    #if defined(__APPLE__)
    const std::string command = "open " + quoted + " &";
    #else
    const std::string command = "xdg-open " + quoted + " >/dev/null 2>&1 &";
    #endif
    if (std::system(command.c_str()) != 0) {
        Out::warning("Could not open %s", folder.string().c_str());
    }
#endif
}

void EditorSettingsWindow::drawGeneralSettings() {
    prefsDrawSettingsPanel("##EditorGeneralSettingsPanel", [this]() {
        if (settingsui::beginRow("preferences.default_export_dir")) {
            if (prefsBeginSettingsRow("Default Build Directory",
                                 "Used by Build Settings when the current project has no saved output directory.",
                                 !m_defaultExportDirectory.empty())) {
                m_defaultExportDirectory.clear();
            }
            fs::path exportDisplay = m_defaultExportDirectory.empty()
                ? fs::path("<Not set>")
                : m_defaultExportDirectory;
            drawPathPicker(
                "##EditorDefaultExportDirectory", "Browse##editor_default_export_dir", "Clear##editor_default_export_dir",
                exportDisplay, !m_defaultExportDirectory.empty(), true,
                [this](const std::string& selectedPath) {
                    m_defaultExportDirectory = selectedPath;
                },
                [this]() {
                    m_defaultExportDirectory.clear();
                }
            );
        }

        if (settingsui::beginRow("preferences.project_hub_at_startup")) {
            if (prefsBeginSettingsRow("Show Project Hub at Startup",
                                 "When No Project: the hub opens when the last project is missing or fails to load. Always: every "
                                 "start. Shift held at start, or --hub, opens it either way.",
                                 m_projectHubAlways)) {
                m_projectHubAlways = false;
            }
            static const char* const hubModes[] = {"When No Project", "Always"};
            int mode = m_projectHubAlways ? 1 : 0;
            ImGui::SetNextItemWidth(Theme::dpi(160.0f));
            if (ImGui::Combo("##ProjectHubAtStartup", &mode, hubModes, 2)) m_projectHubAlways = mode == 1;
        }

        if (settingsui::beginRow("preferences.editor_vsync")) {
            if (prefsBeginSettingsRow("Editor VSync",
                                 "VSync for the editor UI. Play mode and exported builds use the project VSync setting.",
                                 m_editorVSyncEnabled != true)) {
                m_editorVSyncEnabled = true;
            }
            ImGui::Checkbox("##EditorVSync", &m_editorVSyncEnabled);
        }

        if (settingsui::beginRow("preferences.ui_scale")) {
            if (prefsBeginSettingsRow("UI Scale",
                                 "Size of the editor text and panels, on top of the display scale the system reports.",
                                 m_uiScalePercent != 100)) {
                m_uiScalePercent = 100;
            }
            static constexpr int uiScalePercents[] = { 50, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300 };
            ImGui::SetNextItemWidth(Theme::dpi(100.0f));
            if (ImGui::BeginCombo("##EditorUiScale", (std::to_string(m_uiScalePercent) + "%").c_str(), ImGuiComboFlags_HeightLarge)) {
                for (int percent : uiScalePercents) {
                    const bool selected = percent == m_uiScalePercent;
                    if (ImGui::Selectable((std::to_string(percent) + "%").c_str(), selected)) {
                        m_uiScalePercent = percent;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        sectionRow("Auto Save");
        if (settingsui::beginRow("preferences.auto_save")) {
            if (prefsBeginSettingsRow("Auto Save Scenes",
                                 "Keep a backup of each modified scene in .jasperengine/backup (the scene file itself is only written "
                                 "by Save). After a crash, opening the scene offers to restore it.",
                                 m_autosaveEnabled != true)) {
                m_autosaveEnabled = true;
            }
            ImGui::Checkbox("##AutosaveEnabled", &m_autosaveEnabled);
        }
        if (settingsui::beginRow("preferences.auto_save_interval")) {
            if (prefsBeginSettingsRow("Auto Save Interval (s)", "Seconds between the backups of a modified scene (30 to 300).",
                                 m_autosaveInterval != AppSettings::defaultAutosaveInterval)) {
                m_autosaveInterval = AppSettings::defaultAutosaveInterval;
            }
            ImGui::BeginDisabled(!m_autosaveEnabled);
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            ImGui::DragInt("##AutosaveInterval", &m_autosaveInterval, 1.0f, AppSettings::minAutosaveInterval,
                           AppSettings::maxAutosaveInterval, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
        }
        if (settingsui::beginRow("preferences.backups")) {
            prefsBeginSettingsRow("Backups", "The project's auto-saved backups, and what a crash left half-written (orphans).");
            ImGui::BeginDisabled(!m_project || m_project->getProjectPath().empty());
            if (ImGui::Button("Open Backup Folder")) {
                const fs::path folder = SceneBackups::root(m_project->getProjectPath());
                std::error_code ec;
                fs::create_directories(folder, ec);
                openInFileManager(folder);
            }
            ImGui::EndDisabled();
        }

        sectionRow("Network");
        if (settingsui::beginRow("preferences.network.offline")) {
            if (prefsBeginSettingsRow("Offline",
                                 "No network access from the editor: Import > From Lospec says so before any request (download "
                                 "the palette's file and import it instead). Games are not affected.",
                                 m_networkOffline != false)) {
                m_networkOffline = false;
            }
            ImGui::Checkbox("##NetworkOffline", &m_networkOffline);
        }

        sectionRow("HTML UI");
        if (settingsui::beginRow("preferences.html_ui_import_remote")) {
            if (prefsBeginSettingsRow("Import Remote Resources",
                                 "When a document loads, download the Google Fonts, stylesheets, images and Lottie animations it "
                                 "references by URL into assets/htmlui-imports (the game never uses the network).",
                                 m_htmlUiAutoImport != true)) {
                m_htmlUiAutoImport = true;
            }
            ImGui::Checkbox("##HtmlUiAutoImport", &m_htmlUiAutoImport);
        }
        if (settingsui::beginRow("preferences.html_ui_verbose_diagnostics")) {
            if (prefsBeginSettingsRow("Verbose Diagnostics",
                                 "Log every note of the HTML/CSS compatibility layer (rewrites, approximations) to the Console, "
                                 "not only errors, warnings and the per-document summary.",
                                 m_htmlUiVerboseDiagnostics != false)) {
                m_htmlUiVerboseDiagnostics = false;
            }
            ImGui::Checkbox("##HtmlUiVerboseDiagnostics", &m_htmlUiVerboseDiagnostics);
        }
        if (settingsui::beginRow("preferences.html_ui_hot_reload")) {
            if (prefsBeginSettingsRow("Hot Reload",
                                 "Patch: a saved .html document is reconciled with the live one (elements, Lua handles, listeners, "
                                 "form values, scroll and focus stay; onReload fires). Rebuild: it loads again (onUnload, onLoad).",
                                 m_htmlUiPatchReload != true)) {
                m_htmlUiPatchReload = true;
            }
            int reloadMode = m_htmlUiPatchReload ? 0 : 1;
            ImGui::SetNextItemWidth(Theme::dpi(260.0f));
            if (ImGui::Combo("##HtmlUiHotReload", &reloadMode, "Patch (keep runtime state)\0Rebuild\0"))
                m_htmlUiPatchReload = reloadMode == 0;
        }
    });
}

void EditorSettingsWindow::drawCodeEditorSettings() {
    prefsDrawSettingsPanel("##EditorCodeEditorSettingsPanel", [this]() {
        const bool fontChanged = std::fabs(m_codeEditorFontSize - AppSettings::defaultCodeEditorFontSize) > 1e-4f;
        if (settingsui::beginRow("preferences.code_editor_font_size")) {
            if (prefsBeginSettingsRow("Font Size", "Font size of the code editor. Ctrl + mouse wheel zooms it too.", fontChanged)) {
                m_codeEditorFontSize = AppSettings::defaultCodeEditorFontSize;
            }
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            ImGui::DragFloat("##CodeEditorFontSize", &m_codeEditorFontSize, 0.2f, AppSettings::minCodeEditorFontSize,
                             AppSettings::maxCodeEditorFontSize, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        }
    });
}

void EditorSettingsWindow::drawLatticeSettings() {
    prefsDrawSettingsPanel("##EditorLatticeSettingsPanel", [this]() {
        const LatticeViewSettings defaults;
        auto colorRow = [&](const char* key, const char* label, const char* tooltip, std::array<float, 4>& color,
                            const std::array<float, 4>& defaultColor) {
            if (!settingsui::beginRow(key)) return;
            if (prefsBeginSettingsRow(label, tooltip, color != defaultColor)) {
                color = defaultColor;
            }
            // a preference: the value as shown, applied live, the window's Esc puts it back
            ui::ColorFieldOptions colorOptions;
            colorOptions.linearStorage = false;
            colorOptions.label = label;
            colorOptions.width = Theme::dpi(120.0f);
            Vector4 value(color[0], color[1], color[2], color[3]);
            const ui::EditPhase phase = ui::colorField((std::string("##Lattice") + label).c_str(), value, colorOptions);
            if (phase == ui::EditPhase::Changing || phase == ui::EditPhase::Cancelled) color = {value.x, value.y, value.z, value.w};
        };

        if (sectionRow("Cage")) {
            colorRow("preferences.lattice.line_color", "Line Colour", "Edges of the cage at their rest length (the middle of the stretch gradient).",
                     m_lattice.lineColor, defaults.lineColor);
            if (settingsui::beginRow("preferences.lattice.line_thickness")) {
                if (prefsBeginSettingsRow("Line Thickness", "Width of the cage's edges in pixels.", m_lattice.lineThickness != defaults.lineThickness)) {
                    m_lattice.lineThickness = defaults.lineThickness;
                }
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                ImGui::SliderFloat("##LatticeLineThickness", &m_lattice.lineThickness, 1.0f, 4.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
            }
            if (settingsui::beginRow("preferences.lattice.behind_opacity")) {
                if (prefsBeginSettingsRow("Behind Geometry", "Opacity of the parts of the cage that meshes hide (3D views).",
                                     m_lattice.behindOpacity != defaults.behindOpacity)) {
                    m_lattice.behindOpacity = defaults.behindOpacity;
                }
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                ImGui::SliderFloat("##LatticeBehindOpacity", &m_lattice.behindOpacity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            }
        }

        if (sectionRow("Points")) {
            if (settingsui::beginRow("preferences.lattice.point_size")) {
                if (prefsBeginSettingsRow("Point Size", "Size of the point squares in pixels.", m_lattice.pointSize != defaults.pointSize)) {
                    m_lattice.pointSize = defaults.pointSize;
                }
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                ImGui::SliderFloat("##LatticePointSize", &m_lattice.pointSize, 3.0f, 20.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
            }
            colorRow("preferences.lattice.point_color", "Point Colour", nullptr, m_lattice.pointColor, defaults.pointColor);
            colorRow("preferences.lattice.selected_point_color", "Selected Point Colour", nullptr, m_lattice.selectedPointColor,
                     defaults.selectedPointColor);
        }

        if (sectionRow("Stretch")) {
            if (settingsui::beginRow("preferences.lattice.stretch_coloring")) {
                if (prefsBeginSettingsRow("Stretch Colouring", "Colour the edges by their length over their rest length: squashed edges "
                                     "towards the squash colour (at half), stretched ones towards the stretch colour (at twice).",
                                     m_lattice.stretchColoring != defaults.stretchColoring)) {
                    m_lattice.stretchColoring = defaults.stretchColoring;
                }
                ImGui::Checkbox("##LatticeStretchColoring", &m_lattice.stretchColoring);
            }
            ImGui::BeginDisabled(!m_lattice.stretchColoring);
            colorRow("preferences.lattice.squash_color", "Squash Colour", nullptr, m_lattice.squashColor, defaults.squashColor);
            colorRow("preferences.lattice.stretch_color", "Stretch Colour", nullptr, m_lattice.stretchColor, defaults.stretchColor);
            ImGui::EndDisabled();
        }

        if (sectionRow("Proportional Falloff")) {
            static const char* curves[] = {"Smooth", "Linear", "Sharp", "Constant"};
            if (settingsui::beginRow("preferences.lattice.falloff_curve")) {
                const std::string falloffKey = CommandCatalog::editor().menuShortcutText("lattice.toggleFalloff");
                const std::string tooltip = "How the points around the moved ones follow them" +
                    (falloffKey.empty() ? std::string(".") : " (" + falloffKey + " toggles the falloff).");
                if (prefsBeginSettingsRow("Falloff Curve", tooltip.c_str(), m_lattice.falloffCurve != defaults.falloffCurve)) {
                    m_lattice.falloffCurve = defaults.falloffCurve;
                }
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                ImGui::Combo("##LatticeFalloffCurve", &m_lattice.falloffCurve, curves, IM_ARRAYSIZE(curves));
            }
            if (settingsui::beginRow("preferences.lattice.falloff_radius")) {
                if (prefsBeginSettingsRow("Default Radius", "Falloff radius in lattice local units (the cage is 1 across); "
                                     "the mouse wheel changes it during a drag.", m_lattice.falloffRadius != defaults.falloffRadius)) {
                    m_lattice.falloffRadius = defaults.falloffRadius;
                }
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                ImGui::DragFloat("##LatticeFalloffRadius", &m_lattice.falloffRadius, 0.01f, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            }
        }
    });
}

void EditorSettingsWindow::drawSceneViewSettings() {
    prefsDrawSettingsPanel("##EditorSceneViewSettingsPanel", [this]() {
        const SceneViewSettings defaults;
        auto toggleRow = [&](const char* key, const char* label, const char* tooltip, bool& value, bool defaultValue) {
            if (!settingsui::beginRow(key)) return;
            if (prefsBeginSettingsRow(label, tooltip, value != defaultValue)) {
                value = defaultValue;
            }
            ImGui::Checkbox((std::string("##SceneView") + label).c_str(), &value);
        };
        if (sectionRow("Picking")) {
            toggleRow("preferences.scene_view.exact_pick", "Exact Pick",
                      "A click selects the GameObject under it, never its prefab or model root (the toolbar's Exact Pick).",
                      m_sceneView.exactPick, defaults.exactPick);
        }
        if (sectionRow("Drops")) {
            toggleRow("preferences.scene_view.align_to_surface", "Align to Surface",
                      "Objects dropped into the view turn their up axis onto the surface they land on (Alt while dropping does it once).",
                      m_sceneView.alignToSurface, defaults.alignToSurface);
        }
        if (sectionRow("Views")) {
            toggleRow("preferences.scene_view.rulers", "Rulers", "Top and left rulers in 2D, UI and orthographic axis views.",
                      m_sceneView.rulers, defaults.rulers);
        }
        if (sectionRow("Brushes")) {
            toggleRow("preferences.scene_view.alt_alternates", "Alt Alternates While Painting",
                      "With a terrain or voxel brush armed, Alt held is the tool's alternate and Alt+drag paints it instead of orbiting the view "
                      "(a tool with no alternate keeps orbit and pick).",
                      m_sceneView.altAlternates, defaults.altAlternates);
        }
        if (sectionRow("Animation")) {
            toggleRow("preferences.scene_view.preview_particles", "Preview Particle Systems",
                      "Animation previews in edit mode (the Animator and Animation windows) play and scrub the Particle Systems under the "
                      "previewed GameObject with the animation.",
                      m_sceneView.previewParticles, defaults.previewParticles);
        }
    });
}

void EditorSettingsWindow::drawAssetPipelineSettings() {
    prefsDrawSettingsPanel("##EditorAssetPipelinePanel", [this]() {
        if (settingsui::beginRow("preferences.auto_refresh")) {
            if (prefsBeginSettingsRow("Auto Refresh",
                                 "Reload textures, models, materials, shaders, audio, fonts, scripts and HTML UI when they change on "
                                 "disk. Off: the status bar counts the changes and Assets > Refresh (Ctrl+R) applies them.",
                                 m_autoRefresh != true)) {
                m_autoRefresh = true;
            }
            ImGui::Checkbox("##AssetAutoRefresh", &m_autoRefresh);
        }

        WatchBackendKind forced;
        const bool overridden = HotReloadService::getWatcherOverride(forced);
        if (settingsui::beginRow("preferences.file_watcher")) {
            if (prefsBeginSettingsRow("Watcher",
                                 overridden ? "How the editor finds changed files. --file-watcher (or JASPER_FILE_WATCHER) decides it for "
                                              "this run."
                                            : "How the editor finds changed files. Auto: the system's file notifications, and polling where "
                                              "they do not arrive (network drives, WSL, containers).",
                                 m_fileWatcher != 0)) {
                m_fileWatcher = 0;
            }
            static const char* const watchers[] = {"Auto", "Native", "Polling"};
            ImGui::SetNextItemWidth(Theme::dpi(160.0f));
            ImGui::BeginDisabled(overridden);
            ImGui::Combo("##AssetWatcher", &m_fileWatcher, watchers, IM_ARRAYSIZE(watchers));
            ImGui::EndDisabled();
        }

        // only polling has an interval
        if (m_fileWatcher == 2) {
            if (settingsui::beginRow("preferences.polling_interval")) {
                if (prefsBeginSettingsRow("Polling Interval", "Seconds between two looks at the project's files.",
                                     std::fabs(m_pollingInterval - 1.0f) > 1e-4f)) {
                    m_pollingInterval = 1.0f;
                }
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                ImGui::DragFloat("##AssetPollingInterval", &m_pollingInterval, 0.05f, 0.25f, 10.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
            }
        }

        if (settingsui::beginRow("preferences.log_reloads")) {
            if (prefsBeginSettingsRow("Log Reloads", "Write a [Hot Reload] line to the Console for what reloaded (failures always are).",
                                 m_logReloads != true)) {
                m_logReloads = true;
            }
            ImGui::Checkbox("##AssetLogReloads", &m_logReloads);
        }
    });
}

void EditorSettingsWindow::drawScriptingSettings() {
    prefsDrawSettingsPanel("##EditorScriptingPanel", [this]() {
        if (settingsui::beginRow("preferences.script_changes_while_playing")) {
            if (prefsBeginSettingsRow("Script Changes While Playing",
                                 "What saving a script does while a scene plays. Reload And Continue Playing swaps the new code into "
                                 "the running scripts: their fields keep their values, init() does not run again, onReload(info) does. "
                                 "A script with an error keeps running its old code.",
                                 m_scriptChangesWhilePlaying != 0)) {
                m_scriptChangesWhilePlaying = 0;
            }
            static const char* const policies[] = {"Reload And Continue Playing", "Reload After Stopping Play", "Stop Playing And Reload"};
            ImGui::SetNextItemWidth(Theme::dpi(240.0f));
            ImGui::Combo("##ScriptChangesWhilePlaying", &m_scriptChangesWhilePlaying, policies, IM_ARRAYSIZE(policies));
        }

        if (settingsui::beginRow("preferences.reload_cpp_during_play")) {
            if (prefsBeginSettingsRow("Reload C++ Scripts During Play",
                                 "Experimental. A saved .cpp or .h rebuilds the scripts in the background while the scene keeps "
                                 "playing, then swaps the new library in: DPROPERTY values carry over, other members only through "
                                 "saveReloadState/loadReloadState. A failed build keeps the last good one.",
                                 m_reloadCppDuringPlay)) {
                m_reloadCppDuringPlay = false;
            }
            ImGui::Checkbox("##ReloadCppDuringPlay", &m_reloadCppDuringPlay);
        }
    });
}

void EditorSettingsWindow::drawExternalScriptEditorSettings() {
    const auto& choices = ExternalCodeEditor::choices();
    if (settingsui::beginRow("preferences.external_script_editor")) {
        if (prefsBeginSettingsRow("External Script Editor",
                             "Where Console locations and Open in External Editor open a script. Built-in: the Code Editor.",
                             m_externalScriptEditor != 0)) {
            m_externalScriptEditor = 0;
        }
        ImGui::SetNextItemWidth(Theme::dpi(220.0f));
        if (ImGui::BeginCombo("##ExternalScriptEditor", choices[static_cast<size_t>(m_externalScriptEditor)].name)) {
            for (size_t i = 0; i < choices.size(); i++) {
                if (ImGui::Selectable(choices[i].name, static_cast<int>(i) == m_externalScriptEditor)) m_externalScriptEditor = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        if (std::string(choices[static_cast<size_t>(m_externalScriptEditor)].id) == "custom") {
            prefsBeginSettingsRow("Command", "The program and its arguments: {file} {line} {column} {project} are filled in. "
                                             "Quote arguments that contain spaces. Runs without a shell.");
            ImGui::SetNextItemWidth(-FLT_MIN);
            char buffer[512];
            std::snprintf(buffer, sizeof(buffer), "%s", m_externalScriptEditorCommand.c_str());
            if (ImGui::InputTextWithHint("##ExternalScriptEditorCommand", "myeditor --goto \"{file}:{line}\"", buffer, sizeof(buffer))) {
                m_externalScriptEditorCommand = buffer;
            }
        }
    }
}

void EditorSettingsWindow::drawExternalToolsSettings() {
    prefsDrawSettingsPanel("##EditorExternalToolsPanel", [this]() {
        sectionRow("Scripts");
        drawExternalScriptEditorSettings();
        sectionRow("Desktop Build");
        drawCMakeSettings();
        sectionRow("Web Build");
        drawWebSettings();
        sectionRow("Video Import");
        drawVideoImportSettings();
        sectionRow("Shipping");
        drawShippingSettings();
        sectionRow("Aseprite");
        drawAsepriteSettings();
    });
}

// S44 R32, §6.8: the Aseprite that Open in Aseprite starts. Detection runs on the locator's
// thread, so the page shows its answer when it comes and never waits for --version
void EditorSettingsWindow::drawAsepriteSettings() {
    if (!settingsui::beginRow("preferences.aseprite_path")) return;
    namespace ase = pixelart::aseprite;
    std::string& value = m_toolPaths[ase::kAsepriteToolKey];
    if (prefsBeginSettingsRow("Aseprite", "The Aseprite executable Open in Aseprite and double-click start. Empty: JASPER_ASEPRITE, "
                              "then the usual install places (Program Files, Applications, Steam, /opt) and PATH.", !value.empty())) {
        value.clear();
    }
    ase::AsepriteLocator& locator = ase::AsepriteLocator::instance();
    // the same preference is detected once; an edited one again
    locator.start(value);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Browse...").x + ImGui::CalcTextSize("Detect").x + style.FramePadding.x * 4.0f + style.ItemSpacing.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(Theme::dpi(80.0f), ImGui::GetContentRegionAvail().x - buttons));
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
    if (ImGui::InputTextWithHint("##AsepritePath", "<Auto-detect>", buffer, sizeof(buffer))) value = buffer;
    ImGui::SameLine();
    if (ImGui::Button("Browse...##aseprite")) {
        const std::string start = value.empty() ? std::string() : fs::path(value).parent_path().string();
        const std::string selected = FileDialogs::openFileDialog(start, FILE_DIALOG_ALL, false);
        if (!selected.empty()) value = selected;
    }
    ImGui::SameLine();
    if (ImGui::Button("Detect##aseprite")) {
        locator.invalidate();
        locator.start(value);
    }
    ImGui::SetItemTooltip("Looks for Aseprite again (after installing it, or moving it)");
    if (!locator.ready()) {
        ImGui::TextDisabled("Detecting...");
        Backend::getApp().requestRedraw();
    } else if (const std::optional<ase::AsepriteInstall> install = locator.result()) {
        ImGui::TextDisabled("Detected: %s (%s)", install->version.empty() ? "version unknown" : install->version.c_str(), install->source.c_str());
        ImGui::SetItemTooltip("%s", FileUtils::pathToUtf8(install->executable).c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(locator.problem().message.empty() ? "Aseprite was not found" : locator.problem().message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
}

void EditorSettingsWindow::refreshPlayerRuntimes() {
    m_playerRuntimes = PlayerRuntimes::list();
}

// S30 (docs/shipping.md): the tools prebuilt builds and releases run, and the player runtimes
void EditorSettingsWindow::drawShippingSettings() {
    struct ToolRow {
        const char* key;
        const char* tool;
        const char* label;
        const char* help;
        bool directory;
    };
    static const ToolRow rows[] = {
        {"preferences.android_sdk_path", "androidSdk", "Android SDK",
         "The Android SDK of prebuilt Android builds: build-tools 35+ (aapt2, zipalign, apksigner) and the target platform. "
         "Empty means ANDROID_HOME, then ANDROID_SDK_ROOT. No NDK or Gradle is needed.", true},
        {"preferences.jdk_path", "jdk", "JDK", "The JDK whose keytool, jarsigner and java sign Android builds. Empty means JAVA_HOME, then PATH.", true},
        {"preferences.steamcmd_path", "steamcmd", "SteamCMD", "steamcmd for publish steam-upload. Empty means STEAMCMD_HOME, then PATH.", false},
        {"preferences.rcodesign_path", "rcodesign", "rcodesign", "rcodesign signs macOS builds on any computer (codesign does on a Mac). Empty means PATH.", false},
    };
    for (const ToolRow& row : rows) {
        if (!settingsui::beginRow(row.key)) continue;
        std::string& value = m_toolPaths[row.tool];
        prefsBeginSettingsRow(row.label, row.help, !value.empty());
        const fs::path display = value.empty() ? fs::path("<Auto-detect>") : fs::path(value);
        drawPathPicker((std::string("##ShippingTool") + row.tool).c_str(), (std::string("Browse##") + row.tool).c_str(),
                       (std::string("Auto##") + row.tool).c_str(), display, !value.empty(), row.directory,
                       [&value](const std::string& selected) { value = selected; }, [&value]() { value.clear(); });
    }

    if (settingsui::beginRow("preferences.player_runtimes")) {
        prefsBeginSettingsRow("Player Runtimes", "The prebuilt players builds use, per platform, graphics API and flavour. "
            "Build Settings downloads them; an engine checkout builds its own (tools/build.sh players, JASPER_PLAYERS_DIR).", false);
        if (m_playerRuntimes.empty()) {
            ImGui::TextDisabled("None installed (%s)", PlayerRuntimes::installRoot().string().c_str());
        } else if (ImGui::BeginTable("##PlayerRuntimes", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Runtime");
            ImGui::TableSetupColumn("Editor");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            std::string removeId;
            for (const PlayerInfo& info : m_playerRuntimes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(info.id.c_str());
                ImGui::SetItemTooltip("%s", info.dir.string().c_str());
                ImGui::TableNextColumn();
                const bool current = info.engineVersion == PlayerRuntimes::editorVersion();
                if (current) ImGui::TextUnformatted(info.engineVersion.c_str());
                else ImGui::TextDisabled("%s (another editor's)", info.engineVersion.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.1f MB", double(info.sizeBytes) / (1024.0 * 1024.0));
                ImGui::TableNextColumn();
                const bool removable = info.dir.parent_path() == PlayerRuntimes::installRoot();
                ImGui::BeginDisabled(!removable);
                if (ImGui::SmallButton((std::string("Remove##") + info.id).c_str())) removeId = info.id;
                ImGui::EndDisabled();
                if (!removable) ImGui::SetItemTooltip("Installed with the editor or from JASPER_PLAYERS_DIR: remove it there.");
            }
            ImGui::EndTable();
            if (!removeId.empty()) {
                std::string error;
                m_playerRuntimeStatus = PlayerRuntimes::remove(removeId, {}, error) ? "Removed " + removeId : error;
                refreshPlayerRuntimes();
            }
        }
        if (ImGui::Button("Install from file...")) {
            const std::string archive = FileDialogs::openFileDialog("", FILE_DIALOG_ALL, false);
            if (!archive.empty()) {
                std::string installed, error;
                std::vector<std::string> warnings;
                if (PlayerRuntimes::install(archive, {}, installed, warnings, error)) {
                    m_playerRuntimeStatus = "Installed " + installed;
                    for (const std::string& warning : warnings) Out::warning("%s", warning.c_str());
                } else {
                    m_playerRuntimeStatus = error;
                    Out::error("Player runtime: %s", error.c_str());
                }
                refreshPlayerRuntimes();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh##PlayerRuntimes")) refreshPlayerRuntimes();
        if (!m_playerRuntimeStatus.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", m_playerRuntimeStatus.c_str());
            ImGui::PopTextWrapPos();
        }
    }
}

void EditorSettingsWindow::drawCMakeSettings() {
    {
        if (settingsui::beginRow("preferences.cmake_path")) {
            prefsBeginSettingsRow("CMake", "Path to cmake executable. Empty means auto-detect from PATH.", !m_cmakeOverride.empty());
            fs::path cmakeDisplay = m_cmakeOverride.empty()
                ? fs::path(m_cmakeInfo.found ? m_cmakeInfo.path : "<Not found on PATH>")
                : fs::path(m_cmakeOverride);
            drawPathPicker(
                "##EditorCMakePath", "Browse##editor_cmake", "Auto##editor_cmake",
                cmakeDisplay, !m_cmakeOverride.empty(), false,
                [this](const std::string& selectedPath) {
                    const std::string resolved = Generator::resolveCMakePath(selectedPath);
                    const std::string version = resolved.empty() ? std::string() : Generator::probeCMakeVersion(resolved);

                    if (version.empty()) {
                        m_cmakePickError = resolved.empty()
                            ? "No CMake executable in: " + selectedPath
                            : "Not a working CMake: " + resolved;
                    } else {
                        m_cmakeOverride = resolved;
                        m_cmakePickError.clear();
                    }
                    refreshCMakeStatus();
                },
                [this]() {
                    m_cmakeOverride.clear();
                    m_cmakePickError.clear();
                    refreshCMakeStatus();
                }
            );

            if (m_cmakeInfo.found) {
                ImGui::TextDisabled("Detected: %s", m_cmakeInfo.version.empty() ? m_cmakeInfo.path.c_str() : m_cmakeInfo.version.c_str());
            }
            if (!m_cmakePickError.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
                ImGui::TextWrapped("%s", m_cmakePickError.c_str());
                ImGui::PopStyleColor();
            }
        }

        if (settingsui::beginRow({"preferences.build_c_compiler", "preferences.build_cxx_compiler", "preferences.build_generator", "preferences.build_jobs"})) {
            if (prefsBeginSettingsRow("Compiler", "Compiler kit used to build C++ scripts, both when playing a scene and when exporting. "
                    "Set the language standard in Project Settings > Scripting > C++ Standard.", m_cmakeKitIndex != 0)) {
                m_cmakeKitIndex = 0;
            }
            if (m_cmakeKitIndex < 0 || m_cmakeKitIndex > static_cast<int>(m_availableKits.size())) {
                m_cmakeKitIndex = 0;
            }

            const char* currentLabel = m_cmakeKitIndex == 0
                ? "Default"
                : m_availableKits[m_cmakeKitIndex - 1].displayName.c_str();

            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##EditorCMakeKit", currentLabel)) {
                bool selected = m_cmakeKitIndex == 0;
                if (ImGui::Selectable("Default", selected)) m_cmakeKitIndex = 0;
                if (selected) ImGui::SetItemDefaultFocus();

                for (size_t i = 0; i < m_availableKits.size(); i++) {
                    const auto& kit = m_availableKits[i];
                    if (!kit.available) {
                        ImGui::BeginDisabled();
                        ImGui::Selectable((kit.displayName + "  (unavailable)").c_str(), false);
                        ImGui::EndDisabled();
                        showDisabledItemTooltip(kit.unavailableReason);
                        continue;
                    }

                    selected = m_cmakeKitIndex == static_cast<int>(i + 1);
                    if (ImGui::Selectable(kit.displayName.c_str(), selected)) {
                        m_cmakeKitIndex = static_cast<int>(i + 1);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (m_cmakeKitIndex > 0) {
                const auto& kit = m_availableKits[m_cmakeKitIndex - 1];
                if (!kit.cCompiler.empty() || !kit.cxxCompiler.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("C: %s\nCXX: %s", kit.cCompiler.c_str(), kit.cxxCompiler.c_str());
                    ImGui::PopStyleColor();
                }
            } else {
                // The combo fell back to "Default" because the stored kit was not detected
                const LocalBuildSettings stored = projectBuildSettings();
                if (!stored.cxxCompiler.empty() || !stored.generator.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
                    ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Builds still use \"%s\", which is no longer detected. Apply to switch to the default toolchain.",
                        stored.cxxCompiler.empty() ? stored.generator.c_str() : stored.cxxCompiler.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
    }
}

void EditorSettingsWindow::drawWebSettings() {
    {
        if (settingsui::beginRow("preferences.emsdk_path")) {
            prefsBeginSettingsRow("Emscripten SDK", "Path to emsdk root. Empty means EMSDK/PATH auto-detect.", !m_emsdkOverride.empty());

            fs::path emsdkDisplay = m_emsdkOverride.empty()
                ? fs::path("<Auto-detect>")
                : fs::path(m_emsdkOverride);
            drawPathPicker(
                "##EditorEmsdkPath", "Browse##editor_emsdk", "Auto##editor_emsdk",
                emsdkDisplay, !m_emsdkOverride.empty(), true,
                [this](const std::string& selectedPath) {
                    m_emsdkOverride = selectedPath;
                    refreshEmsdkStatus();
                },
                [this]() {
                    m_emsdkOverride.clear();
                    refreshEmsdkStatus();
                }
            );

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Status");
            ImGui::TableNextColumn();
            if (m_emsdkInfo.found) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), ICON_FA_CIRCLE_CHECK " Found %s", m_emsdkInfo.description.c_str());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Not found");
            }
        }
    }
}

void EditorSettingsWindow::drawVideoImportSettings() {
    if (settingsui::beginRow("preferences.ffmpeg_path")) {
        prefsBeginSettingsRow("FFmpeg", "ffmpeg executable that converts imported videos to AV1 + Opus WebM (the format <video> plays). "
            "Empty means JASPER_FFMPEG, then PATH. It runs as a separate program; it is never shipped.", !m_ffmpegOverride.empty());
        const fs::path display = m_ffmpegOverride.empty() ? fs::path("<Auto-detect>") : fs::path(m_ffmpegOverride);
        drawPathPicker(
            "##EditorFfmpegPath", "Browse##editor_ffmpeg", "Auto##editor_ffmpeg",
            display, !m_ffmpegOverride.empty(), false,
            [this](const std::string& selectedPath) {
                m_ffmpegOverride = selectedPath;
                refreshFfmpegStatus();
            },
            [this]() {
                m_ffmpegOverride.clear();
                refreshFfmpegStatus();
            }
        );

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Status");
        ImGui::TableNextColumn();
        if (m_ffmpegUsable) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), ICON_FA_CIRCLE_CHECK " %s", m_ffmpegStatus.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", m_ffmpegStatus.c_str());
            ImGui::PopStyleColor();
        }
    }
}

void EditorSettingsWindow::drawUndoSettings() {
    prefsDrawSettingsPanel("##EditorUndoSettingsPanel", [this]() {
        if (settingsui::beginRow("preferences.undo_max_steps")) {
            if (prefsBeginSettingsRow("Max Undo Steps",
                                 "Steps each scene (and the project's asset history) keeps; the oldest go first.",
                                 m_undoMaxSteps != AppSettings::defaultUndoMaxSteps)) {
                m_undoMaxSteps = AppSettings::defaultUndoMaxSteps;
            }
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            ImGui::DragInt("##UndoMaxSteps", &m_undoMaxSteps, 10.0f, AppSettings::minUndoMaxSteps, AppSettings::maxUndoMaxSteps,
                           "%d", ImGuiSliderFlags_AlwaysClamp);
        }

        if (settingsui::beginRow("preferences.undo_memory_limit")) {
            if (prefsBeginSettingsRow("Undo Memory Limit (MB)",
                                 "Memory every undo history together may keep (an estimate); the oldest steps of any scene go "
                                 "first. 0 = unlimited.",
                                 m_undoMemoryLimitMB != AppSettings::defaultUndoMemoryLimitMB)) {
                m_undoMemoryLimitMB = AppSettings::defaultUndoMemoryLimitMB;
            }
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            ImGui::DragInt("##UndoMemoryLimit", &m_undoMemoryLimitMB, 8.0f, 0, AppSettings::maxUndoMemoryLimitMB, "%d",
                           ImGuiSliderFlags_AlwaysClamp);
        }

        if (settingsui::beginRow("preferences.undo_record_selection")) {
            if (prefsBeginSettingsRow("Record Selection in Undo",
                                 "Selecting GameObjects is a step of its own (Ctrl+Z gives the previous selection back). "
                                 "Undoing an edit always restores the selection it had.",
                                 m_undoRecordSelection != true)) {
                m_undoRecordSelection = true;
            }
            ImGui::Checkbox("##UndoRecordSelection", &m_undoRecordSelection);
        }

        if (settingsui::beginRow("preferences.undo_review_play_changes")) {
            if (prefsBeginSettingsRow("Review Play Mode Changes",
                                 "On Stop, list the edits made while playing so chosen ones can be kept "
                                 "(Window > General > Play Mode Changes).",
                                 m_undoReviewPlayChanges != true)) {
                m_undoReviewPlayChanges = true;
            }
            ImGui::Checkbox("##UndoReviewPlayChanges", &m_undoReviewPlayChanges);
        }
    });
}

namespace {
    const char* const prefsLogLevelLabels[] = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal", "Off"};

    // A level combo over the level names of LoggingSettings; true when changed
    bool prefsLogLevelCombo(const char* id, std::string& levelName) {
        LogLevel level = LogLevel::Info;
        parseLogLevel(levelName.c_str(), level);
        int index = static_cast<int>(level);
        ImGui::SetNextItemWidth(Theme::dpi(140.0f));
        if (ImGui::Combo(id, &index, prefsLogLevelLabels, IM_ARRAYSIZE(prefsLogLevelLabels))) {
            levelName = logLevelName(static_cast<LogLevel>(index));
            return true;
        }
        return false;
    }
}

void EditorSettingsWindow::drawLoggingSettings() {
    prefsDrawSettingsPanel("##EditorLoggingSettingsPanel", [this]() {
        const LoggingSettings defaults;
        if (settingsui::beginRow("preferences.logging.file_level")) {
            if (prefsBeginSettingsRow("Log File Level", "The lowest level written to editor.log (next to settings.yaml).",
                                 m_logging.fileLevel != defaults.fileLevel)) {
                m_logging.fileLevel = defaults.fileLevel;
            }
            prefsLogLevelCombo("##LogFileLevel", m_logging.fileLevel);
        }

        if (settingsui::beginRow("preferences.logging.backtrace_on_error")) {
            if (prefsBeginSettingsRow("Capture Backtrace on Errors",
                                 "Errors logged by the engine and editor record their call stack; editor.log shows it.",
                                 m_logging.backtraceOnError != defaults.backtraceOnError)) {
                m_logging.backtraceOnError = defaults.backtraceOnError;
            }
            ImGui::Checkbox("##LogBacktraces", &m_logging.backtraceOnError);
        }

        if (settingsui::beginRow("preferences.logging.show_source")) {
            if (prefsBeginSettingsRow("Show Source Location",
                                 "The file and line a message was logged from, in the Console's details and editor.log.",
                                 m_logging.showSource != defaults.showSource)) {
                m_logging.showSource = defaults.showSource;
            }
            ImGui::Checkbox("##LogShowSource", &m_logging.showSource);
        }

        sectionRow("Category Levels");
        if (settingsui::beginRow("preferences.logging.reset")) {
            prefsBeginSettingsRow("Reset", "Every category back to Info.");
            ImGui::BeginDisabled(m_logging.categoryLevels.empty());
            if (ImGui::Button("Reset All##LogLevels")) {
                m_logging.categoryLevels.clear();
            }
            ImGui::EndDisabled();
        }
        for (size_t i = 0; i < Logger::categoryCount(); i++) {
            if (!settingsui::beginRow("preferences.logging.levels")) break;
            const std::string name = Logger::categoryAt(i).name;
            auto it = m_logging.categoryLevels.find(name);
            std::string level = it != m_logging.categoryLevels.end() ? it->second : std::string("info");
            if (prefsBeginSettingsRow(name.c_str(), "Messages of this category below the level are not logged anywhere.",
                                 level != "info")) {
                level = "info";
            }
            prefsLogLevelCombo(("##LogCategory" + name).c_str(), level);
            if (level == "info") {
                m_logging.categoryLevels.erase(name);
            } else {
                m_logging.categoryLevels[name] = level;
            }
        }
    });
}

void EditorSettingsWindow::drawPixelArtSettings() {
    prefsDrawSettingsPanel("##EditorPixelArtSettingsPanel", [this]() {
        const PixelArtPreferences defaults;
        if (settingsui::beginRow("preferences.pixel_art.agent_edit_highlight")) {
            if (prefsBeginSettingsRow("Highlight Agent Edits", "The Sprite Preview outlines the pixels an agent changed, fading over 3 s.",
                                 m_pixelArt.agentEditHighlight != defaults.agentEditHighlight)) {
                m_pixelArt.agentEditHighlight = defaults.agentEditHighlight;
            }
            ImGui::Checkbox("##AgentEditHighlight", &m_pixelArt.agentEditHighlight);
        }
    });
}

void EditorSettingsWindow::drawDiagnosticsSettings() {
    prefsDrawSettingsPanel("##EditorDiagnosticsSettingsPanel", [this]() {
        const DiagnosticsSettings defaults;
        if (settingsui::beginRow("preferences.diagnostics.hang_watchdog")) {
            if (prefsBeginSettingsRow("Hang Watchdog",
                                 "Writes a hang report when the editor stops responding (docs/crash-diagnostics.md).",
                                 m_diagnostics.hangWatchdog != defaults.hangWatchdog)) {
                m_diagnostics.hangWatchdog = defaults.hangWatchdog;
            }
            ImGui::Checkbox("##HangWatchdog", &m_diagnostics.hangWatchdog);
        }

        if (settingsui::beginRow("preferences.diagnostics.report_after")) {
            if (prefsBeginSettingsRow("Report After (s)",
                                 "How long the editor may not respond before a report. Known long work (loading, imports, "
                                 "native dialogs) gets more time.",
                                 m_diagnostics.reportAfterSeconds != defaults.reportAfterSeconds)) {
                m_diagnostics.reportAfterSeconds = defaults.reportAfterSeconds;
            }
            ImGui::BeginDisabled(!m_diagnostics.hangWatchdog);
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            ImGui::DragInt("##HangReportAfter", &m_diagnostics.reportAfterSeconds, 0.2f, DiagnosticsSettings::minReportAfterSeconds,
                           DiagnosticsSettings::maxReportAfterSeconds, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
        }

        if (settingsui::beginRow("preferences.diagnostics.break_lua_after")) {
            if (prefsBeginSettingsRow("Interrupt Runaway Lua In Play After (s)",
                                 "A Lua function that runs this long in Play without returning gets a \"script interrupted\" "
                                 "error; Play goes on. 0 = never.",
                                 m_diagnostics.breakLuaAfterSeconds != defaults.breakLuaAfterSeconds)) {
                m_diagnostics.breakLuaAfterSeconds = defaults.breakLuaAfterSeconds;
            }
            ImGui::BeginDisabled(!m_diagnostics.hangWatchdog);
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            ImGui::DragInt("##BreakLuaAfter", &m_diagnostics.breakLuaAfterSeconds, 0.2f, 0,
                           DiagnosticsSettings::maxBreakLuaAfterSeconds, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
        }

        if (settingsui::beginRow("preferences.diagnostics.show_crash_notice")) {
            if (prefsBeginSettingsRow("Show Crash Notice On Startup",
                                 "After a session that ended unexpectedly, a notice under the menu bar points to its report.",
                                 m_diagnostics.showCrashNotice != defaults.showCrashNotice)) {
                m_diagnostics.showCrashNotice = defaults.showCrashNotice;
            }
            ImGui::Checkbox("##ShowCrashNotice", &m_diagnostics.showCrashNotice);
        }

        sectionRow("Reports");
        if (settingsui::beginRow("preferences.diagnostics.reports_folder")) {
            prefsBeginSettingsRow("Crash Reports", "Crash and hang reports of this editor, one folder per session.");
            if (ImGui::Button("Open Crash Reports Folder")) {
                const std::filesystem::path folder = diag::crash::reportsDirectory();
                std::error_code ec;
                std::filesystem::create_directories(folder, ec);
                openInFileManager(folder);
            }
        }
    });
}

// S29: version control (docs/editor/version-control.md)
void EditorSettingsWindow::drawVersionControlSettings() {
    prefsDrawSettingsPanel("##EditorVersionControlSettingsPanel", [this]() {
        const VersionControlSettings defaults;
        if (settingsui::beginRow("preferences.vcs.status")) {
            prefsBeginSettingsRow("Version Control", "The project's provider and its tool");
            const vcs::VcsService* service = m_project ? &m_project->getVcs() : nullptr;
            if (!service || service->state() == vcs::VcsState::Off) {
                ImGui::TextDisabled("Off");
            } else if (service->active()) {
                ImGui::TextWrapped("%s", service->stateMessage().c_str());
            } else {
                ImGui::TextWrapped("%s", service->state() == vcs::VcsState::NotAvailable ? service->stateMessage().c_str() :
                                         service->state() == vcs::VcsState::Detecting ? "Looking for a repository..." : service->stateMessage().c_str());
            }
        }
        sectionRow("Tools");
        auto pathRow = [](const char* key, const char* label, const char* tooltip, std::string& value, const char* id) {
            if (!settingsui::beginRow(key)) return;
            if (prefsBeginSettingsRow(label, tooltip, !value.empty())) value.clear();
            char buffer[1024];
            std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint(id, "Found on PATH", buffer, sizeof(buffer))) value = buffer;
        };
        pathRow("editor.vcs.git_path", "Git Path", "The git executable (absolute); empty finds it on PATH, never in the project folder.",
                m_versionControl.gitPath, "##GitPath");
        pathRow("editor.vcs.svn_path", "Subversion Path", "The svn executable (absolute); empty finds it on PATH, never in the project folder.",
                m_versionControl.svnPath, "##SvnPath");
        sectionRow("Locking");
        if (settingsui::beginRow("editor.vcs.lock_on_edit")) {
            if (prefsBeginSettingsRow("Lock on Edit", "The first edit of a lockable file (an LFS binary, svn:needs-lock): off, ask to lock it, or lock it",
                                      m_versionControl.lockOnEdit != defaults.lockOnEdit)) {
                m_versionControl.lockOnEdit = defaults.lockOnEdit;
            }
            int mode = static_cast<int>(m_versionControl.lockOnEdit);
            const char* modes[] = {"Off", "Ask", "Auto"};
            ImGui::SetNextItemWidth(Theme::dpi(140.0f));
            if (ImGui::Combo("##LockOnEdit", &mode, modes, 3)) m_versionControl.lockOnEdit = static_cast<LockOnEdit>(mode);
        }
        sectionRow("Refresh");
        if (settingsui::beginRow("editor.vcs.poll_seconds")) {
            if (prefsBeginSettingsRow("Status Poll (s)", "Seconds between status reads (local: no network). Saves and focus refresh it at once.",
                                      m_versionControl.pollSeconds != defaults.pollSeconds)) {
                m_versionControl.pollSeconds = defaults.pollSeconds;
            }
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            ImGui::DragInt("##VcsPoll", &m_versionControl.pollSeconds, 0.2f, 2, 120, "%d", ImGuiSliderFlags_AlwaysClamp);
        }
        if (settingsui::beginRow("editor.vcs.lock_poll_seconds")) {
            if (prefsBeginSettingsRow("Lock Poll (s)", "Seconds between reads of the locks and the server's changes (network). 0: only after your own locks and commits.",
                                      m_versionControl.lockPollSeconds != defaults.lockPollSeconds)) {
                m_versionControl.lockPollSeconds = defaults.lockPollSeconds;
            }
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            if (ImGui::DragInt("##VcsLockPoll", &m_versionControl.lockPollSeconds, 0.5f, 0, 600, "%d", ImGuiSliderFlags_AlwaysClamp) &&
                m_versionControl.lockPollSeconds > 0 && m_versionControl.lockPollSeconds < 30) {
                m_versionControl.lockPollSeconds = 30;
            }
        }
        if (settingsui::beginRow("editor.vcs.auto_fetch_minutes")) {
            if (prefsBeginSettingsRow("Auto Fetch (min)", "Minutes between automatic fetches (git), so behind shows; 0 off",
                                      m_versionControl.autoFetchMinutes != defaults.autoFetchMinutes)) {
                m_versionControl.autoFetchMinutes = defaults.autoFetchMinutes;
            }
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            ImGui::DragInt("##VcsAutoFetch", &m_versionControl.autoFetchMinutes, 0.2f, 0, 24 * 60, "%d", ImGuiSliderFlags_AlwaysClamp);
        }
        sectionRow("Commit");
        if (settingsui::beginRow("editor.vcs.commit_and_push")) {
            if (prefsBeginSettingsRow("Commit and Push", "The Commit window pushes after each commit (git)",
                                      m_versionControl.commitAndPush != defaults.commitAndPush)) {
                m_versionControl.commitAndPush = defaults.commitAndPush;
            }
            ImGui::Checkbox("##CommitAndPush", &m_versionControl.commitAndPush);
        }
        sectionRow("Code Editor");
        if (settingsui::beginRow("editor.vcs.code_editor_changes")) {
            if (prefsBeginSettingsRow("Show Changes in Code Editor", "Marks in the gutter and on the scrollbar for the lines changed since "
                                      "the last commit, with their diff on hover and Revert Change",
                                      m_versionControl.codeEditorChanges != defaults.codeEditorChanges)) {
                m_versionControl.codeEditorChanges = defaults.codeEditorChanges;
            }
            ImGui::Checkbox("##CodeEditorChanges", &m_versionControl.codeEditorChanges);
        }
        sectionRow("Merge");
        if (settingsui::beginRow("preferences.vcs.smart_merge")) {
            prefsBeginSettingsRow("Smart Merge", "git merges scenes and prefabs GameObject by GameObject with this editor (merge-scene), "
                                                 "instead of its text merge. Writes merge.jasperengine-scene.driver to the repository's config.");
            vcs::VcsService* service = m_project ? &m_project->getVcs() : nullptr;
            const bool canSetUp = service && service->active() && service->caps().mergeDriver;
            ImGui::BeginDisabled(!canSetUp);
            if (ImGui::Button("Set Up Smart Merge")) {
                const std::filesystem::path editor = FileUtils::getExecutablePath();
                m_smartMergeStatus = "Setting up...";
                service->run("Setting up Smart Merge...", [editor](vcs::IVcsProvider& provider, const jobs::CancelToken&) {
                    return provider.setupMergeDriver(editor);
                }, [this](const vcs::VcsResult& result) {
                    m_smartMergeStatus = result.ok ? "Set up: git merges scenes and prefabs with this editor" : "Failed: " + result.message;
                }, true);
            }
            ImGui::EndDisabled();
            if (!canSetUp) ImGui::SetItemTooltip("Needs a Git repository");
            if (!m_smartMergeStatus.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", m_smartMergeStatus.c_str());
            } else if (service && service->snapshot() && service->snapshot()->mergeDriverMissing) {
                ImGui::SameLine();
                ImGui::TextDisabled("Smart merge: not set up");
            }
        }
    });
}

void EditorSettingsWindow::drawAdvancedSettings() {
    prefsDrawSettingsPanel("##EditorAdvancedSettingsPanel", [this]() {
        if (settingsui::beginRow("preferences.shader_cache")) {
            prefsBeginSettingsRow("Shader Cache", "Shared by projects for this editor version. Shaders are rebuilt when needed.");
            ImGui::BeginDisabled(m_project && m_project->isAnyScenePlaying());
            if (ImGui::Button("Clear Shader Cache")) {
                std::error_code ec;
                std::filesystem::remove_all(App::getUserShaderCacheDir(), ec);
                m_cacheStatus = ec ? "Could not clear the shader cache: " + ec.message() : "Shader cache cleared.";
            }
            ImGui::EndDisabled();
            if (!m_cacheStatus.empty()) {
                ImGui::TextWrapped("%s", m_cacheStatus.c_str());
            }
        }
    });
}

bool EditorSettingsWindow::applySettings() {
    AppSettings::setEditorVSyncEnabled(m_editorVSyncEnabled);
    AppSettings::setProjectHubAlways(m_projectHubAlways);
    AppSettings::setAutosaveEnabled(m_autosaveEnabled);
    AppSettings::setAutosaveIntervalSeconds(m_autosaveInterval);
    AppSettings::setNetworkOffline(m_networkOffline);
    AppSettings::setUiScale(m_uiScalePercent / 100.0f);
    const LatticeViewSettings previousLattice = AppSettings::getLatticeViewSettings();
    AppSettings::setLatticeViewSettings(m_lattice);
    const LatticeViewSettings lattice = AppSettings::getLatticeViewSettings();
    if (lattice.falloffCurve != previousLattice.falloffCurve || lattice.falloffRadius != previousLattice.falloffRadius) {
        // new defaults take over the falloff the Scene view uses
        LatticeEditorOps::falloff().curve = static_cast<LatticeFalloffCurve>(lattice.falloffCurve);
        LatticeEditorOps::falloff().radius = lattice.falloffRadius;
    }
    if (lattice != previousLattice && m_project) {
        for (SceneProject& sceneProject : m_project->getScenes()) {
            sceneProject.needUpdateRender = true;
        }
    }
    AppSettings::setSceneViewSettings(m_sceneView);
    AppSettings::setHtmlUiAutoImport(m_htmlUiAutoImport);
    // the watcher starts again on the project when how it watches changed
    static const char* const watcherNames[] = {"auto", "native", "polling"};
    const bool watcherChanged = AppSettings::getFileWatcher() != watcherNames[m_fileWatcher] ||
                                std::fabs(AppSettings::getPollingInterval() - m_pollingInterval) > 1e-4f;
    AppSettings::setAutoRefresh(m_autoRefresh);
    AppSettings::setFileWatcher(watcherNames[m_fileWatcher]);
    AppSettings::setPollingInterval(m_pollingInterval);
    AppSettings::setLogReloads(m_logReloads);
    static const char* const policies[] = {"reload", "after_stop", "stop_and_reload"};
    AppSettings::setScriptChangesWhilePlaying(policies[m_scriptChangesWhilePlaying]);
    AppSettings::setReloadCppDuringPlay(m_reloadCppDuringPlay);
    AppSettings::setExternalScriptEditor(ExternalCodeEditor::choices()[static_cast<size_t>(m_externalScriptEditor)].id);
    AppSettings::setExternalScriptEditorCommand(m_externalScriptEditorCommand);
    HotReloadService& reloads = HotReloadService::get();
    reloads.setAutoRefresh(m_autoRefresh);
    reloads.setLogReloads(m_logReloads);
    if (watcherChanged && m_project && reloads.getProject() == m_project) reloads.startWatching(*m_project);
    AppSettings::setHtmlUiVerboseDiagnostics(m_htmlUiVerboseDiagnostics);
    AppSettings::setHtmlUiPatchReload(m_htmlUiPatchReload);
    AppSettings::setCodeEditorFontSize(m_codeEditorFontSize);
    AppSettings::setUndo(m_undoMaxSteps, m_undoMemoryLimitMB, m_undoRecordSelection, m_undoReviewPlayChanges);
    AppSettings::setLoggingSettings(m_logging);
    AppSettings::setDiagnosticsSettings(m_diagnostics);
    AppSettings::setPixelArtPreferences(m_pixelArt);
    if (m_versionControl != AppSettings::getVersionControlSettings()) {
        AppSettings::setVersionControlSettings(m_versionControl);
        if (m_project) m_project->getVcs().applySettings(m_project->versionControlServiceSettings());
    }
    AppSettings::setDefaultExportDirectory(m_defaultExportDirectory);
    AppSettings::setCMakePath(m_cmakeOverride);
    LocalBuildSettings build;
    if (m_cmakeKitIndex > 0) {
        const auto& kit = m_availableKits[m_cmakeKitIndex - 1];
        build.cCompiler = kit.cCompiler;
        build.cxxCompiler = kit.cxxCompiler;
        build.generator = kit.generator;
    }
    if (m_project) {
        const auto file = m_project->getProjectPath() / "project.yaml";
        build.buildJobs = AppSettings::getBuildSettings(file).buildJobs;
        AppSettings::setBuildSettings(file, build);
    }
    // Also the editor-wide default, so a new project starts from it
    AppSettings::setLastCMakeKit(build.cCompiler, build.cxxCompiler, build.generator);
    AppSettings::setEmsdkPath(m_emsdkOverride);
    AppSettings::setFfmpegPath(m_ffmpegOverride);
    for (const auto& [tool, path] : m_toolPaths) {
        if (AppSettings::getExternalToolPath(tool) != path) AppSettings::setExternalToolPath(tool, path);
    }
    return AppSettings::saveSettings();
}

} // namespace jasperengine::editor
