// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorSettingsWindow.h"
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
#include "htmlui/VideoImporter.h"
#include "lattice/LatticeEditorOps.h"

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
        Undo,
        Lattice,
        ExternalTools,
        Logging,
        ShaderCache,
        Count
    };

    // Order matches PreferencesCategory. Keywords are the page's labels, for the search field.
    const settingsui::Category preferencesCategories[] = {
        {"General", "", "editor vsync ui scale zoom size default build directory output auto save autosave backup interval"},
        {"Code Editor", "", "font size"},
        {"Undo", "", "undo redo history max steps memory limit record selection review play mode changes keep values"},
        {"Lattice", "", "lattice cage line colour color thickness behind geometry opacity point size selected stretch squash "
                        "gradient falloff curve radius proportional"},
        {"External Tools", "", "cmake compiler kit emscripten sdk emsdk web desktop ffmpeg video import av1"},
        {"Logging", "", "log level category console file backtrace errors source location debug trace"},
        {"Shader Cache", "", "clear shader cache"},
    };
    constexpr int preferencesCategoryCount = static_cast<int>(PreferencesCategory::Count);
    static_assert(std::size(preferencesCategories) == static_cast<size_t>(PreferencesCategory::Count));

    // Sub-heading row inside a settings table.
    void sectionRow(const char* title) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Spacing();
        settingsui::boldText(title);
        ImGui::TableNextColumn();
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
        ImGui::TextUnformatted(label);
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
    m_editorVSyncEnabled = AppSettings::getEditorVSyncEnabled();
    m_uiScalePercent = static_cast<int>(std::lround(AppSettings::getUiScale() * 100.0f));
    m_autosaveEnabled = AppSettings::isAutosaveEnabled();
    m_autosaveInterval = AppSettings::getAutosaveIntervalSeconds();
    m_htmlUiAutoImport = AppSettings::getHtmlUiAutoImport();
    m_htmlUiVerboseDiagnostics = AppSettings::getHtmlUiVerboseDiagnostics();
    m_codeEditorFontSize = AppSettings::getCodeEditorFontSize();
    m_undoMaxSteps = AppSettings::getUndoMaxSteps();
    m_undoMemoryLimitMB = AppSettings::getUndoMemoryLimitMB();
    m_undoRecordSelection = AppSettings::getUndoRecordSelection();
    m_undoReviewPlayChanges = AppSettings::getUndoReviewPlayChanges();
    m_logging = AppSettings::getLoggingSettings();
    m_lattice = AppSettings::getLatticeViewSettings();
    m_defaultExportDirectory = AppSettings::getDefaultExportDirectory();
    refreshEmsdkStatus();
}

bool EditorSettingsWindow::showPage(const std::string& name) {
    for (int i = 0; i < preferencesCategoryCount; i++) {
        if (name == preferencesCategories[i].name) {
            m_category = i;
            return true;
        }
    }
    return false;
}

const char* EditorSettingsWindow::pageName() const {
    return preferencesCategories[m_category].name;
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

    bool popupOpen = ImGui::BeginPopupModal("Preferences##EditorSettingsModal", &m_isOpen, flags);

    if (popupOpen) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
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
    settingsui::categoryList("##PreferencesCategories", preferencesCategories, preferencesCategoryCount, m_category,
                             m_categorySearch, sizeof(m_categorySearch), ImVec2(listWidth, ImGui::GetContentRegionAvail().y));
    ImGui::SameLine();

    ImGui::BeginGroup();
    settingsui::categoryTitle(preferencesCategories[m_category].name);
    switch (static_cast<PreferencesCategory>(m_category)) {
        case PreferencesCategory::General: drawGeneralSettings(); break;
        case PreferencesCategory::CodeEditor: drawCodeEditorSettings(); break;
        case PreferencesCategory::Undo: drawUndoSettings(); break;
        case PreferencesCategory::Lattice: drawLatticeSettings(); break;
        case PreferencesCategory::ExternalTools: drawExternalToolsSettings(); break;
        case PreferencesCategory::Logging: drawLoggingSettings(); break;
        case PreferencesCategory::ShaderCache: drawAdvancedSettings(); break;
        default: break;
    }
    ImGui::EndGroup();

    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, footerY - style.ItemSpacing.y));
    ImGui::Separator();

    // Unity-style footer: buttons on the right, primary first.
    const float buttonWidth = Theme::dpi(prefsSettingsButtonWidth);
    const float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    ImGui::SetCursorPos(ImVec2(std::max(style.WindowPadding.x, ImGui::GetWindowContentRegionMax().x - buttonsWidth), footerY));

    if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
        if (applySettings()) {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)) ||
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

        if (prefsBeginSettingsRow("Editor VSync",
                             "VSync for the editor UI. Play mode and exported builds use the project VSync setting.",
                             m_editorVSyncEnabled != true)) {
            m_editorVSyncEnabled = true;
        }
        ImGui::Checkbox("##EditorVSync", &m_editorVSyncEnabled);

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

        sectionRow("Auto Save");
        if (prefsBeginSettingsRow("Auto Save Scenes",
                             "Keep a backup of each modified scene in .jasperengine/backup (the scene file itself is only written "
                             "by Save). After a crash, opening the scene offers to restore it.",
                             m_autosaveEnabled != true)) {
            m_autosaveEnabled = true;
        }
        ImGui::Checkbox("##AutosaveEnabled", &m_autosaveEnabled);
        if (prefsBeginSettingsRow("Auto Save Interval (s)", "Seconds between the backups of a modified scene (30 to 300).",
                             m_autosaveInterval != AppSettings::defaultAutosaveInterval)) {
            m_autosaveInterval = AppSettings::defaultAutosaveInterval;
        }
        ImGui::BeginDisabled(!m_autosaveEnabled);
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::DragInt("##AutosaveInterval", &m_autosaveInterval, 1.0f, AppSettings::minAutosaveInterval,
                       AppSettings::maxAutosaveInterval, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        prefsBeginSettingsRow("Backups", "The project's auto-saved backups, and what a crash left half-written (orphans).");
        ImGui::BeginDisabled(!m_project || m_project->getProjectPath().empty());
        if (ImGui::Button("Open Backup Folder")) {
            const fs::path folder = SceneBackups::root(m_project->getProjectPath());
            std::error_code ec;
            fs::create_directories(folder, ec);
            openInFileManager(folder);
        }
        ImGui::EndDisabled();

        sectionRow("HTML UI");
        if (prefsBeginSettingsRow("Import Remote Resources",
                             "When a document loads, download the Google Fonts, stylesheets, images and Lottie animations it "
                             "references by URL into assets/htmlui-imports (the game never uses the network).",
                             m_htmlUiAutoImport != true)) {
            m_htmlUiAutoImport = true;
        }
        ImGui::Checkbox("##HtmlUiAutoImport", &m_htmlUiAutoImport);
        if (prefsBeginSettingsRow("Verbose Diagnostics",
                             "Log every note of the HTML/CSS compatibility layer (rewrites, approximations) to the Console, "
                             "not only errors, warnings and the per-document summary.",
                             m_htmlUiVerboseDiagnostics != false)) {
            m_htmlUiVerboseDiagnostics = false;
        }
        ImGui::Checkbox("##HtmlUiVerboseDiagnostics", &m_htmlUiVerboseDiagnostics);
    });
}

void EditorSettingsWindow::drawCodeEditorSettings() {
    prefsDrawSettingsPanel("##EditorCodeEditorSettingsPanel", [this]() {
        const bool fontChanged = std::fabs(m_codeEditorFontSize - AppSettings::defaultCodeEditorFontSize) > 1e-4f;
        if (prefsBeginSettingsRow("Font Size", "Font size of the code editor. Ctrl + mouse wheel zooms it too.", fontChanged)) {
            m_codeEditorFontSize = AppSettings::defaultCodeEditorFontSize;
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::DragFloat("##CodeEditorFontSize", &m_codeEditorFontSize, 0.2f, AppSettings::minCodeEditorFontSize,
                         AppSettings::maxCodeEditorFontSize, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    });
}

void EditorSettingsWindow::drawLatticeSettings() {
    prefsDrawSettingsPanel("##EditorLatticeSettingsPanel", [this]() {
        const LatticeViewSettings defaults;
        const ImGuiColorEditFlags colorFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;

        auto colorRow = [&](const char* label, const char* tooltip, std::array<float, 4>& color, const std::array<float, 4>& defaultColor) {
            if (prefsBeginSettingsRow(label, tooltip, color != defaultColor)) {
                color = defaultColor;
            }
            ImGui::ColorEdit4((std::string("##Lattice") + label).c_str(), color.data(), colorFlags);
        };

        sectionRow("Cage");
        colorRow("Line Colour", "Edges of the cage at their rest length (the middle of the stretch gradient).",
                 m_lattice.lineColor, defaults.lineColor);
        if (prefsBeginSettingsRow("Line Thickness", "Width of the cage's edges in pixels.", m_lattice.lineThickness != defaults.lineThickness)) {
            m_lattice.lineThickness = defaults.lineThickness;
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::SliderFloat("##LatticeLineThickness", &m_lattice.lineThickness, 1.0f, 4.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
        if (prefsBeginSettingsRow("Behind Geometry", "Opacity of the parts of the cage that meshes hide (3D views).",
                             m_lattice.behindOpacity != defaults.behindOpacity)) {
            m_lattice.behindOpacity = defaults.behindOpacity;
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::SliderFloat("##LatticeBehindOpacity", &m_lattice.behindOpacity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

        sectionRow("Points");
        if (prefsBeginSettingsRow("Point Size", "Size of the point squares in pixels.", m_lattice.pointSize != defaults.pointSize)) {
            m_lattice.pointSize = defaults.pointSize;
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::SliderFloat("##LatticePointSize", &m_lattice.pointSize, 3.0f, 20.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
        colorRow("Point Colour", nullptr, m_lattice.pointColor, defaults.pointColor);
        colorRow("Selected Point Colour", nullptr, m_lattice.selectedPointColor, defaults.selectedPointColor);

        sectionRow("Stretch");
        if (prefsBeginSettingsRow("Stretch Colouring", "Colour the edges by their length over their rest length: squashed edges "
                             "towards the squash colour (at half), stretched ones towards the stretch colour (at twice).",
                             m_lattice.stretchColoring != defaults.stretchColoring)) {
            m_lattice.stretchColoring = defaults.stretchColoring;
        }
        ImGui::Checkbox("##LatticeStretchColoring", &m_lattice.stretchColoring);
        ImGui::BeginDisabled(!m_lattice.stretchColoring);
        colorRow("Squash Colour", nullptr, m_lattice.squashColor, defaults.squashColor);
        colorRow("Stretch Colour", nullptr, m_lattice.stretchColor, defaults.stretchColor);
        ImGui::EndDisabled();

        sectionRow("Proportional Falloff");
        static const char* curves[] = {"Smooth", "Linear", "Sharp", "Constant"};
        if (prefsBeginSettingsRow("Falloff Curve", "How the points around the moved ones follow them (O toggles the falloff).",
                             m_lattice.falloffCurve != defaults.falloffCurve)) {
            m_lattice.falloffCurve = defaults.falloffCurve;
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::Combo("##LatticeFalloffCurve", &m_lattice.falloffCurve, curves, IM_ARRAYSIZE(curves));
        if (prefsBeginSettingsRow("Default Radius", "Falloff radius in lattice local units (the cage is 1 across); "
                             "the mouse wheel changes it during a drag.", m_lattice.falloffRadius != defaults.falloffRadius)) {
            m_lattice.falloffRadius = defaults.falloffRadius;
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::DragFloat("##LatticeFalloffRadius", &m_lattice.falloffRadius, 0.01f, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    });
}

void EditorSettingsWindow::drawExternalToolsSettings() {
    prefsDrawSettingsPanel("##EditorExternalToolsPanel", [this]() {
        sectionRow("Desktop Build");
        drawCMakeSettings();
        sectionRow("Web Build");
        drawWebSettings();
        sectionRow("Video Import");
        drawVideoImportSettings();
    });
}

void EditorSettingsWindow::drawCMakeSettings() {
    {
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

void EditorSettingsWindow::drawWebSettings() {
    {
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

void EditorSettingsWindow::drawVideoImportSettings() {
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

void EditorSettingsWindow::drawUndoSettings() {
    prefsDrawSettingsPanel("##EditorUndoSettingsPanel", [this]() {
        if (prefsBeginSettingsRow("Max Undo Steps",
                             "Steps each scene (and the project's asset history) keeps; the oldest go first.",
                             m_undoMaxSteps != AppSettings::defaultUndoMaxSteps)) {
            m_undoMaxSteps = AppSettings::defaultUndoMaxSteps;
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::DragInt("##UndoMaxSteps", &m_undoMaxSteps, 10.0f, AppSettings::minUndoMaxSteps, AppSettings::maxUndoMaxSteps,
                       "%d", ImGuiSliderFlags_AlwaysClamp);

        if (prefsBeginSettingsRow("Undo Memory Limit (MB)",
                             "Memory every undo history together may keep (an estimate); the oldest steps of any scene go "
                             "first. 0 = unlimited.",
                             m_undoMemoryLimitMB != AppSettings::defaultUndoMemoryLimitMB)) {
            m_undoMemoryLimitMB = AppSettings::defaultUndoMemoryLimitMB;
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::DragInt("##UndoMemoryLimit", &m_undoMemoryLimitMB, 8.0f, 0, AppSettings::maxUndoMemoryLimitMB, "%d",
                       ImGuiSliderFlags_AlwaysClamp);

        if (prefsBeginSettingsRow("Record Selection in Undo",
                             "Selecting GameObjects is a step of its own (Ctrl+Z gives the previous selection back). "
                             "Undoing an edit always restores the selection it had.",
                             m_undoRecordSelection != true)) {
            m_undoRecordSelection = true;
        }
        ImGui::Checkbox("##UndoRecordSelection", &m_undoRecordSelection);

        if (prefsBeginSettingsRow("Review Play Mode Changes",
                             "On Stop, list the edits made while playing so chosen ones can be kept "
                             "(Window > General > Play Mode Changes).",
                             m_undoReviewPlayChanges != true)) {
            m_undoReviewPlayChanges = true;
        }
        ImGui::Checkbox("##UndoReviewPlayChanges", &m_undoReviewPlayChanges);
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
        if (prefsBeginSettingsRow("Log File Level", "The lowest level written to editor.log (next to settings.yaml).",
                             m_logging.fileLevel != defaults.fileLevel)) {
            m_logging.fileLevel = defaults.fileLevel;
        }
        prefsLogLevelCombo("##LogFileLevel", m_logging.fileLevel);

        if (prefsBeginSettingsRow("Capture Backtrace on Errors",
                             "Errors logged by the engine and editor record their call stack; editor.log shows it.",
                             m_logging.backtraceOnError != defaults.backtraceOnError)) {
            m_logging.backtraceOnError = defaults.backtraceOnError;
        }
        ImGui::Checkbox("##LogBacktraces", &m_logging.backtraceOnError);

        if (prefsBeginSettingsRow("Show Source Location",
                             "The file and line a message was logged from, in the Console's details and editor.log.",
                             m_logging.showSource != defaults.showSource)) {
            m_logging.showSource = defaults.showSource;
        }
        ImGui::Checkbox("##LogShowSource", &m_logging.showSource);

        sectionRow("Category Levels");
        prefsBeginSettingsRow("Reset", "Every category back to Info.");
        ImGui::BeginDisabled(m_logging.categoryLevels.empty());
        if (ImGui::Button("Reset All##LogLevels")) {
            m_logging.categoryLevels.clear();
        }
        ImGui::EndDisabled();
        for (size_t i = 0; i < Logger::categoryCount(); i++) {
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

void EditorSettingsWindow::drawAdvancedSettings() {
    prefsDrawSettingsPanel("##EditorAdvancedSettingsPanel", [this]() {
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
    });
}

bool EditorSettingsWindow::applySettings() {
    AppSettings::setEditorVSyncEnabled(m_editorVSyncEnabled);
    AppSettings::setAutosaveEnabled(m_autosaveEnabled);
    AppSettings::setAutosaveIntervalSeconds(m_autosaveInterval);
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
    AppSettings::setHtmlUiAutoImport(m_htmlUiAutoImport);
    AppSettings::setHtmlUiVerboseDiagnostics(m_htmlUiVerboseDiagnostics);
    AppSettings::setCodeEditorFontSize(m_codeEditorFontSize);
    AppSettings::setUndo(m_undoMaxSteps, m_undoMemoryLimitMB, m_undoRecordSelection, m_undoReviewPlayChanges);
    AppSettings::setLoggingSettings(m_logging);
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
    return AppSettings::saveSettings();
}

} // namespace jasperengine::editor
