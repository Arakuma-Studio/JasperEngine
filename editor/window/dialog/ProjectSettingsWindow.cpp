// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectSettingsWindow.h"
#include "widget/ColorMath.h"
#include "widget/ColorField.h"
#include "shortcuts/CommandCatalog.h"
#include "settings/SettingsRegistry.h"
#include "reflect/Reflect.h"
#include "command/type/ShaderCreationSettingsCmd.h"
#include "texture/TextureCookReport.h"
#include "util/FileDialogs.h"
#include "Backend.h"
#include "window/Widgets.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "SettingsCategoryList.h"
#include "asset/AssetMap.h"
#include "input/InputActions.h"
#include "io/UserSettings.h"
#include "organisation/LabelOperations.h"
#include "Out.h"
#include "command/CommandHistory.h"
#include "command/type/HtmlUiRenderSettingsCmd.h"
#include "command/type/ThreadingSettingsCmd.h"
#include "navigation/NavigationSettingsCmd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstring>

namespace jasperengine::editor {

static const char* scalingModeNames[] = { "Fit Width", "Fit Height", "Letterbox", "Crop", "Stretch", "Native" };
static const Scaling scalingModeValues[] = { Scaling::FITWIDTH, Scaling::FITHEIGHT, Scaling::LETTERBOX, Scaling::CROP, Scaling::STRETCH, Scaling::NATIVE };
static const int scalingModeCount = sizeof(scalingModeValues) / sizeof(scalingModeValues[0]);

static const char* textureStrategyNames[] = { "Fit", "Resize", "None" };
static const char* htmlUiMsaaNames[] = { "Off", "2x", "4x", "8x" };
static const int htmlUiMsaaValues[] = { 1, 2, 4, 8 };
static constexpr int htmlUiMsaaCount = 4;

static int findHtmlUiMsaaIndex(int samples) {
    for (int i = 0; i < htmlUiMsaaCount; i++) {
        if (htmlUiMsaaValues[i] == samples) return i;
    }
    return 2;
}
static const TextureStrategy textureStrategyValues[] = { TextureStrategy::FIT, TextureStrategy::RESIZE, TextureStrategy::NONE };
static const int textureStrategyCount = sizeof(textureStrategyValues) / sizeof(textureStrategyValues[0]);
static const char* mobilePbrTierNames[] = { "Full", "Reduced" };
static const PbrTier mobilePbrTierValues[] = { PbrTier::FULL, PbrTier::REDUCED };
static int findMobilePbrTierIndex(PbrTier tier) { return tier == PbrTier::FULL ? 0 : 1; }

static const char* windowModeNames[] = { "Windowed", "Maximized", "Fullscreen" };
static const WindowMode windowModeValues[] = { WindowMode::WINDOWED, WindowMode::MAXIMIZED, WindowMode::FULLSCREEN };
static const int windowModeCount = sizeof(windowModeValues) / sizeof(windowModeValues[0]);

static constexpr int cxxStandardCount = static_cast<int>(std::size(cxxStandards));

// "C++20", "C++23", ...: the combo labels of cxxStandards
static const char* const* cxxStandardNames() {
    static const auto labels = [] {
        std::array<std::string, std::size(cxxStandards)> result;
        for (size_t i = 0; i < result.size(); i++) result[i] = "C++" + std::to_string(cxxStandards[i]);
        return result;
    }();
    static const auto names = [] {
        std::array<const char*, std::size(cxxStandards)> result;
        for (size_t i = 0; i < result.size(); i++) result[i] = labels[i].c_str();
        return result;
    }();
    return names.data();
}

static const char* androidOrientationNames[] = { "Unspecified", "Portrait", "Landscape", "Sensor Portrait", "Sensor Landscape", "Full Sensor" };
static const AndroidOrientation androidOrientationValues[] = {
    AndroidOrientation::Unspecified,
    AndroidOrientation::Portrait,
    AndroidOrientation::Landscape,
    AndroidOrientation::SensorPortrait,
    AndroidOrientation::SensorLandscape,
    AndroidOrientation::FullSensor
};
static const int androidOrientationCount = sizeof(androidOrientationValues) / sizeof(androidOrientationValues[0]);

struct AndroidPermissionInfo {
    const char* key;
    const char* manifestName;
};

static const AndroidPermissionInfo androidPermissionInfos[] = {
    { "internet", "INTERNET" },
    { "access_network_state", "ACCESS_NETWORK_STATE" },
    { "access_wifi_state", "ACCESS_WIFI_STATE" },
    { "change_network_state", "CHANGE_NETWORK_STATE" },
    { "change_wifi_state", "CHANGE_WIFI_STATE" },
    { "vibrate", "VIBRATE" },
    { "wake_lock", "WAKE_LOCK" },
    { "post_notifications", "POST_NOTIFICATIONS" },
    { "camera", "CAMERA" },
    { "record_audio", "RECORD_AUDIO" },
    { "access_coarse_location", "ACCESS_COARSE_LOCATION" },
    { "access_fine_location", "ACCESS_FINE_LOCATION" },
    { "access_location_extra_commands", "ACCESS_LOCATION_EXTRA_COMMANDS" },
    { "access_media_location", "ACCESS_MEDIA_LOCATION" },
    { "read_external_storage", "READ_EXTERNAL_STORAGE" },
    { "write_external_storage", "WRITE_EXTERNAL_STORAGE" },
    { "manage_external_storage", "MANAGE_EXTERNAL_STORAGE" },
    { "read_media_audio", "READ_MEDIA_AUDIO" },
    { "read_media_images", "READ_MEDIA_IMAGES" },
    { "read_media_video", "READ_MEDIA_VIDEO" },
    { "read_media_visual_user_selected", "READ_MEDIA_VISUAL_USER_SELECTED" },
    { "bluetooth", "BLUETOOTH" },
    { "bluetooth_admin", "BLUETOOTH_ADMIN" },
    { "bluetooth_connect", "BLUETOOTH_CONNECT" },
    { "bluetooth_scan", "BLUETOOTH_SCAN" },
    { "nfc", "NFC" },
    { "transmit_ir", "TRANSMIT_IR" },
    { "use_biometric", "USE_BIOMETRIC" },
    { "use_fingerprint", "USE_FINGERPRINT" },
    { "read_contacts", "READ_CONTACTS" },
    { "write_contacts", "WRITE_CONTACTS" },
    { "get_accounts", "GET_ACCOUNTS" },
    { "read_calendar", "READ_CALENDAR" },
    { "write_calendar", "WRITE_CALENDAR" },
    { "read_call_log", "READ_CALL_LOG" },
    { "write_call_log", "WRITE_CALL_LOG" },
    { "read_phone_state", "READ_PHONE_STATE" },
    { "call_phone", "CALL_PHONE" },
    { "read_sms", "READ_SMS" },
    { "write_sms", "WRITE_SMS" },
    { "send_sms", "SEND_SMS" },
    { "receive_sms", "RECEIVE_SMS" },
    { "receive_mms", "RECEIVE_MMS" },
    { "receive_wap_push", "RECEIVE_WAP_PUSH" },
    { "receive_boot_completed", "RECEIVE_BOOT_COMPLETED" },
    { "kill_background_processes", "KILL_BACKGROUND_PROCESSES" },
    { "modify_audio_settings", "MODIFY_AUDIO_SETTINGS" },
    { "set_wallpaper", "SET_WALLPAPER" },
    { "set_wallpaper_hints", "SET_WALLPAPER_HINTS" },
    { "write_settings", "WRITE_SETTINGS" },
};

static const int androidPermissionCount = sizeof(androidPermissionInfos) / sizeof(androidPermissionInfos[0]);

static constexpr float projectDialogWidth = 960.0f;
static constexpr float projectDialogHeight = 660.0f;
static constexpr float projectCategoryListWidth = 200.0f;

enum class ProjectCategory {
    Player,
    Graphics,
    Quality,
    Texture,
    Physics,
    Physics2D,
    Input,
    Scripting,
    TagsAndLayers,
    Editor,
    Navigation,
    Count
};

// ProjectCategory follows the order of the Project Settings pages in the settings registry
// (settings/SettingsRows.cpp), which lists them and their rows for the search
static constexpr int projectCategoryCount = static_cast<int>(ProjectCategory::Count);

static constexpr float projectSettingsLabelWidth = 200.0f;
static constexpr float projectSettingsPanelPadding = 12.0f;
static constexpr float projectSettingsButtonWidth = 120.0f;
static constexpr ImGuiWindowFlags projectNoScrollFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

static int findScalingIndex(Scaling mode) {
    for (int i = 0; i < scalingModeCount; i++) {
        if (scalingModeValues[i] == mode) return i;
    }
    return 0;
}

static int findTextureStrategyIndex(TextureStrategy strategy) {
    for (int i = 0; i < textureStrategyCount; i++) {
        if (textureStrategyValues[i] == strategy) return i;
    }
    return 0;
}

static int findWindowModeIndex(WindowMode mode) {
    for (int i = 0; i < windowModeCount; i++) {
        if (windowModeValues[i] == mode) return i;
    }
    return 0;
}

static int findCxxStandardIndex(int standard) {
    for (int i = 0; i < cxxStandardCount; i++) {
        if (cxxStandards[i] == standard) return i;
    }
    return 0;
}

static int findAndroidOrientationIndex(AndroidOrientation orientation) {
    for (int i = 0; i < androidOrientationCount; i++) {
        if (androidOrientationValues[i] == orientation) return i;
    }
    return 0;
}

// Opening and accepting the dialog must not truncate untouched persisted text.
template <size_t N>
static void applyTextBuffer(std::string& value, const char (&buffer)[N]) {
    if (value.compare(0, N - 1, buffer) != 0) {
        value = buffer;
    }
}

static bool isIdentifierPartValid(const std::string& value, size_t start, size_t end) {
    if (start >= end) return false;
    unsigned char first = static_cast<unsigned char>(value[start]);
    if (!std::isalpha(first) && value[start] != '_') return false;

    for (size_t i = start + 1; i < end; i++) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (!std::isalnum(c) && value[i] != '_') return false;
    }
    return true;
}

static bool isAppleIdentifierPartValid(const std::string& value, size_t start, size_t end) {
    if (start >= end || value[start] == '-' || value[end - 1] == '-') return false;

    for (size_t i = start; i < end; i++) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (!std::isalnum(c) && value[i] != '-') return false;
    }
    return true;
}

static bool isSharedIdentifierPartValid(const std::string& value, size_t start, size_t end) {
    if (start >= end || !std::isalpha(static_cast<unsigned char>(value[start]))) return false;

    for (size_t i = start; i < end; i++) {
        if (!std::isalnum(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

using IdentifierPartValidator = bool (*)(const std::string&, size_t, size_t);

static bool isDottedIdentifierValid(const std::string& value, IdentifierPartValidator partValid) {
    if (value.empty() || value.back() == '.') return false;
    size_t start = 0;
    int parts = 0;
    while (start < value.size()) {
        size_t dot = value.find('.', start);
        size_t end = dot == std::string::npos ? value.size() : dot;
        if (!partValid(value, start, end)) return false;
        parts++;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts >= 2;
}

static bool isJavaPackageNameValid(const std::string& value) {
    return isDottedIdentifierValid(value, isIdentifierPartValid);
}

// Apple allows the hyphens Android rejects, and rejects the underscores it allows.
static bool isAppleBundleIdentifierValid(const std::string& value) {
    return isDottedIdentifierValid(value, isAppleIdentifierPartValid);
}

// The shared id reaches both, so only what Apple and Android accept in common.
static bool isSharedIdentifierValid(const std::string& value) {
    return isDottedIdentifierValid(value, isSharedIdentifierPartValid);
}

// One to maxParts numeric parts, none above maxPart. Windows pads shorter values
// to the four its VERSIONINFO resource needs; Apple takes at most three.
static bool isNumericVersionValid(const std::string& value, int maxParts, unsigned int maxPart) {
    if (value.empty()) return false;

    size_t start = 0;
    int parts = 0;
    bool consumedAll = false;
    while (start <= value.size() && parts < maxParts) {
        size_t dot = value.find('.', start);
        size_t end = dot == std::string::npos ? value.size() : dot;
        if (start >= end) return false;

        unsigned long long part = 0;
        for (size_t i = start; i < end; i++) {
            unsigned char c = static_cast<unsigned char>(value[i]);
            if (!std::isdigit(c)) return false;
            part = part * 10 + static_cast<unsigned>(value[i] - '0');
            if (part > maxPart) return false;
        }

        parts++;
        if (dot == std::string::npos) {
            consumedAll = true;
            break;
        }
        start = dot + 1;
    }

    return parts >= 1 && consumedAll;
}

// The shared version reaches Apple too, so it takes the stricter limit.
static bool isAppleVersionValid(const std::string& value) {
    return isNumericVersionValid(value, 3, 65535);
}

// Build numbers are commonly date stamps, so only the plist part count applies.
static bool isAppleBuildValid(const std::string& value) {
    return isNumericVersionValid(value, 3, 0xFFFFFFFFu);
}

static bool isWindowsVersionValid(const std::string& value) {
    return isNumericVersionValid(value, 4, 65535);
}

// A prefix like "12abc", a zero or an out-of-range value yields 0, read as inherit.
static unsigned int parseAndroidVersionCode(const std::string& value) {
    if (value.empty() || value.size() > 10) return 0;

    unsigned long long parsed = 0;
    for (char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return 0;
        parsed = parsed * 10 + static_cast<unsigned>(c - '0');
    }

    if (parsed < 1 || parsed > 2100000000ull) return 0;  // Google Play's ceiling
    return static_cast<unsigned int>(parsed);
}

static bool isAndroidVersionCodeValid(const std::string& value) {
    return parseAndroidVersionCode(value) > 0;
}

// Typing the hint into an inheriting field keeps it inheriting; an override the
// user had already set stays put even when the shared value catches up with it.
template <size_t N>
static void applyOverride(std::string& value, const char (&buffer)[N], const std::string& inherited) {
    const bool wasInheriting = value.empty();
    applyTextBuffer(value, buffer);
    if (wasInheriting && value == inherited) value.clear();
}

template <size_t N>
static void applyOverride(std::string& value, const char (&buffer)[N], const std::string& inherited, bool (*isValid)(const std::string&)) {
    applyOverride(value, buffer, inherited);
    if (!value.empty() && !isValid(value)) value.clear();
}

// Apple caps its plist versions at three parts, so trim instead of dropping.
template <size_t N>
static void applyAppleVersion(std::string& value, const char (&buffer)[N], const std::string& inherited, bool (*isValid)(const std::string&)) {
    applyOverride(value, buffer, inherited);
    if (!value.empty() && !isValid(value)) value = Project::toAppleVersion(value);
}

static bool hasAndroidAbiSelection(bool armeabiV7a, bool arm64V8a, bool x86, bool x86_64) {
    return armeabiV7a || arm64V8a || x86 || x86_64;
}

// One scrolling child per tab; sections inside it are a collapsing header
// followed by a table, like the Terrain editor panels.
static bool beginSettingsPanel(const char* panelId) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(projectSettingsPanelPadding, projectSettingsPanelPadding)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x, Theme::dpi(6.0f)));

    // Allow vertical scrolling only when content exceeds the fixed dialog;
    // ImGui hides the scrollbar while everything fits.
    return ImGui::BeginChild(panelId, ImVec2(0, 0), ImGuiChildFlags_Borders);
}

static void endSettingsPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
}

static bool beginSettingsTable(const char* tableId) {
    float labelWidth = std::min(Theme::dpi(projectSettingsLabelWidth), ImGui::GetContentRegionAvail().x * 0.4f);
    if (!ImGui::BeginTable(tableId, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

template <typename DrawContents>
static void projectDrawSettingsPanel(const char* panelId, DrawContents drawContents) {
    if (beginSettingsPanel(panelId) && beginSettingsTable("##SettingsTable")) {
        drawContents();
        ImGui::EndTable();
    }
    endSettingsPanel();
}

// A foldout of the page: hidden while the search matches none of its rows, open while it
// matches some (S25 R30)
static bool settingsFoldout(const char* title, ImGuiTreeNodeFlags flags = 0) {
    if (!settingsui::sectionVisible(title)) return false;
    if (settingsui::searching()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    return Theme::foldoutHeader(title, flags);
}

// Section text that belongs outside the row table.
static void drawSectionNote(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

static void helpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Width the value widget gives up so the marker fits beside it.
static float helpMarkerWidth() {
    return ImGui::CalcTextSize("(?)").x + ImGui::GetStyle().ItemSpacing.x;
}

// Returns true when the reset arrow is clicked, like the property rows in Properties.
static bool projectBeginSettingsRow(const char* label, bool defChanged = false, const char* resetTooltip = "Restore default") {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    // the search's matches highlighted, a row the Search palette opened flashing
    settingsui::rowLabel(label);

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
        ImGui::SetItemTooltip("%s", resetTooltip);
    }

    ImGui::TableNextColumn();
    return reset;
}

static void endSettingsRow(const char* tooltip) {
    if (!tooltip) return;
    ImGui::SameLine();
    helpMarker(tooltip);
}

// An empty field inherits, and the hint shows the value it inherits.
template <size_t N>
static void drawOverrideSetting(const char* label, const char* id, char (&buffer)[N], const std::string& inherited, const char* tooltip) {
    if (projectBeginSettingsRow(label, buffer[0] != '\0', "Use the shared value")) {
        buffer[0] = '\0';
    }
    ImGui::SetNextItemWidth(tooltip ? -helpMarkerWidth() : -1.0f);
    ImGui::InputTextWithHint(id, inherited.c_str(), buffer, N);
    endSettingsRow(tooltip);
}

// Shows what the resource carries when the typed value is not already padded.
// Invalid text inherits on apply, so it must not report a padded form here.
template <size_t N>
static void drawWindowsVersionResult(const char (&buffer)[N]) {
    if (buffer[0] == '\0' || !isWindowsVersionValid(buffer)) return;
    const std::string padded = Project::toFourPartVersion(buffer);
    if (padded == buffer) return;
    ImGui::TextDisabled("Exports as %s", padded.c_str());
}

// Only fires on a typed value; an inherited field is validated where it is entered.
template <size_t N>
static void drawOverrideWarning(const char (&buffer)[N], bool (*isValid)(const std::string&), const char* message) {
    if (buffer[0] == '\0' || isValid(buffer)) return;
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s", message);
}

static void drawComboSetting(const char* label, const char* id, const char* const* names, int count, int& selectedIndex, int defaultIndex, const char* tooltip = nullptr) {
    if (projectBeginSettingsRow(label, selectedIndex != defaultIndex)) {
        selectedIndex = defaultIndex;
    }
    if (selectedIndex < 0 || selectedIndex >= count) selectedIndex = 0;

    ImGui::SetNextItemWidth(tooltip ? -helpMarkerWidth() : -1.0f);
    if (ImGui::BeginCombo(id, names[selectedIndex])) {
        for (int i = 0; i < count; i++) {
            bool selected = selectedIndex == i;
            if (ImGui::Selectable(names[i], selected)) selectedIndex = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    endSettingsRow(tooltip);
}

static void drawIntSetting(const char* label, const char* id, int& value, int defaultValue, int minValue = 1, const char* tooltip = nullptr) {
    if (projectBeginSettingsRow(label, value != defaultValue)) {
        value = defaultValue;
    }
    ImGui::SetNextItemWidth(tooltip ? -helpMarkerWidth() : -1.0f);
    ImGui::InputInt(id, &value);
    value = std::max(value, minValue);
    endSettingsRow(tooltip);
}

// A thread count: Auto (-1) or minValue..maxValue.
static void drawCountSetting(const char* label, const char* id, int& value, int minValue, int maxValue, const char* tooltip) {
    if (projectBeginSettingsRow(label, value != -1)) {
        value = -1;
    }
    ImGui::SetNextItemWidth(tooltip ? -helpMarkerWidth() : -1.0f);
    const std::string current = value < 0 ? std::string("Auto") : std::to_string(value);
    if (ImGui::BeginCombo(id, current.c_str())) {
        if (ImGui::Selectable("Auto", value < 0)) value = -1;
        for (int count = minValue; count <= maxValue; count++) {
            const bool selected = value == count;
            if (ImGui::Selectable(std::to_string(count).c_str(), selected)) value = count;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    endSettingsRow(tooltip);
}

static void drawSecondsSetting(const char* label, const char* id, float& value, float defaultValue, const char* tooltip = nullptr) {
    if (projectBeginSettingsRow(label, value != defaultValue)) {
        value = defaultValue;
    }
    ImGui::SetNextItemWidth(tooltip ? -helpMarkerWidth() : -1.0f);
    ImGui::InputFloat(id, &value, 0.1f, 1.0f, "%.2f s");
    value = std::isfinite(value) ? std::max(value, 0.0f) : defaultValue;
    endSettingsRow(tooltip);
}

static void drawDirectorySetting(
    Project* project,
    const char* label,
    const char* tooltip,
    const char* pathId,
    const char* buttonId,
    fs::path& directory,
    const fs::path& defaultDirectory,
    bool keepAbsolutePathOnError = false
) {
    // An empty directory already means the default root
    if (projectBeginSettingsRow(label, !directory.empty() && directory != defaultDirectory)) {
        directory = defaultDirectory;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    float browseWidth = ImGui::CalcTextSize("Browse").x + style.FramePadding.x * 2.0f;
    float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - browseWidth - style.ItemSpacing.x - (tooltip ? helpMarkerWidth() : 0.0f));

    fs::path displayPath = directory.empty() ? defaultDirectory : directory;
    if (displayPath == ".") displayPath = "<Project root>";
    Widgets::pathDisplay(pathId, displayPath, Vector2(pathWidth, ImGui::GetFrameHeight()));

    ImGui::SameLine();
    bool browse = false;
    if (project) {
        browse = ImGui::Button(buttonId);
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(buttonId);
        ImGui::EndDisabled();
    }
    endSettingsRow(tooltip);

    if (!browse) return;

    std::string selectedPath = FileDialogs::openFileDialog(project->getProjectPath().string(), FILE_DIALOG_ALL, true);
    if (selectedPath.empty()) return;

    std::error_code ec;
    fs::path relativePath = fs::relative(fs::path(selectedPath), project->getProjectPath(), ec);
    if (!ec && !relativePath.empty()) {
        directory = relativePath;
    } else {
        directory = keepAbsolutePathOnError ? fs::path(selectedPath) : defaultDirectory;
    }
}

static void drawScriptDirsSetting(Project* project, std::vector<fs::path>& directories) {
    if (projectBeginSettingsRow("Script Directories", !directories.empty())) {
        directories.clear();
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float removeWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + style.FramePadding.x * 2.0f;

    size_t removeIndex = directories.size();
    for (size_t i = 0; i < directories.size(); i++) {
        ImGui::PushID(static_cast<int>(i));

        float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - removeWidth - style.ItemSpacing.x);
        Widgets::pathDisplay("##ScriptDir", directories[i], Vector2(pathWidth, ImGui::GetFrameHeight()));

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button(ICON_FA_TRASH_CAN "##RemoveScriptDir", ImVec2(removeWidth, 0))) {
            removeIndex = i;
        }
        ImGui::PopStyleColor(2);

        ImGui::PopID();
    }

    if (removeIndex < directories.size()) {
        directories.erase(directories.begin() + static_cast<std::ptrdiff_t>(removeIndex));
    }

    bool add = false;
    if (project) {
        add = ImGui::Button(ICON_FA_PLUS " Add##AddScriptDir");
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_PLUS " Add##AddScriptDir");
        ImGui::EndDisabled();
    }
    endSettingsRow("Extra C++ roots. Each one is an include directory, and the sources under it "
        "are compiled without a script component referencing them.");

    if (!add) return;

    std::string selectedPath = FileDialogs::openFileDialog(project->getProjectPath().string(), FILE_DIALOG_ALL, true);
    if (selectedPath.empty()) return;

    // Only a root inside the project survives the export, and fs::relative reaches
    // a sibling through ".."; anything outside is stored whole and builds locally.
    std::error_code ec;
    fs::path relativePath = fs::relative(fs::path(selectedPath), project->getProjectPath(), ec);
    const bool insideProject = !ec && !relativePath.empty() && *relativePath.begin() != "..";
    fs::path directory = insideProject ? relativePath : fs::path(selectedPath);

    if (std::find(directories.begin(), directories.end(), directory) == directories.end()) {
        directories.push_back(std::move(directory));
    }
}

static void drawProjectFilePathSetting(
    Project* project,
    const char* label,
    const char* tooltip,
    const char* pathId,
    const char* browseId,
    const char* clearId,
    fs::path& imagePath,
    int filter = FILE_DIALOG_IMAGE
) {
    projectBeginSettingsRow(label);

    const ImGuiStyle& style = ImGui::GetStyle();
    float browseWidth = ImGui::CalcTextSize("Browse").x + style.FramePadding.x * 2.0f;
    float clearWidth = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f;
    float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - browseWidth - clearWidth - style.ItemSpacing.x * 2.0f);

    fs::path displayPath = imagePath.empty() ? fs::path("<None>") : imagePath;
    Widgets::pathDisplay(pathId, displayPath, Vector2(pathWidth, ImGui::GetFrameHeight()));

    ImGui::SameLine();
    if (ImGui::Button(browseId)) {
        std::string defaultPath = project ? project->getProjectPath().string() : std::string();
        std::string selectedPath = FileDialogs::openFileDialog(defaultPath, filter, false);
        if (!selectedPath.empty()) {
            std::error_code ec;
            fs::path relPath = project ? fs::relative(fs::path(selectedPath), project->getProjectPath(), ec) : fs::path();
            if (!project || ec || relPath.empty() || relPath.string().rfind("..", 0) == 0) {
                imagePath = fs::path(selectedPath);
            } else {
                imagePath = relPath;
            }
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(imagePath.empty());
    if (ImGui::Button(clearId)) {
        imagePath.clear();
    }
    ImGui::EndDisabled();
    endSettingsRow(tooltip);
}

static void drawScalingPreview(Scaling mode, int canvasWidth, int canvasHeight) {
    if (canvasWidth <= 0 || canvasHeight <= 0) return;

    float canvasAspect = (float)canvasWidth / (float)canvasHeight;
    canvasAspect = std::clamp(canvasAspect, 15.0f / 70.0f, 80.0f / 15.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Base canvas size for preview. The bounded aspect ratio keeps the derived
    // height between 15 and 70 pixels.
    float baseW = std::clamp(50.0f * canvasAspect, 15.0f, 80.0f);
    float baseH = baseW / canvasAspect;

    // Three viewport configurations: reference, wider, taller
    float vpW[3] = { baseW * 1.15f, baseW * 1.8f,  baseW * 0.55f };
    float vpH[3] = { baseH * 1.15f, baseH * 0.7f,  baseH * 1.7f  };

    float spacing = 16.0f;
    float totalW = vpW[0] + vpW[1] + vpW[2] + spacing * 2;
    float maxH = 0;
    for (int i = 0; i < 3; i++) { if (vpH[i] > maxH) maxH = vpH[i]; }

    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float offsetX = (availW - totalW) * 0.5f;
    if (offsetX < 0) offsetX = 0;
    float startX = cursor.x + offsetX;
    float startY = cursor.y;

    ImU32 colVpBg        = IM_COL32(215, 228, 243, 255);
    ImU32 colVpBorder    = IM_COL32(155, 165, 180, 255);
    ImU32 colCanvasFill  = IM_COL32(190, 210, 235, 255);
    ImU32 colCanvasBorder= IM_COL32(35, 50, 75, 255);
    ImU32 colTriangle    = IM_COL32(30, 80, 150, 255);
    ImU32 colBlack       = IM_COL32(15, 15, 20, 255);

    float baseTW = baseW * 0.35f;
    float baseTH = baseH * 0.4f;

    float curX = startX;

    for (int i = 0; i < 3; i++) {
        float vw = vpW[i];
        float vh = vpH[i];
        float vx = curX;
        float vy = startY + (maxH - vh) * 0.5f;

        ImVec2 vpMin(vx, vy);
        ImVec2 vpMax(vx + vw, vy + vh);

        // Compute scale factors based on scaling mode
        float scaleX, scaleY;
        switch (mode) {
        case Scaling::FITWIDTH:
            scaleX = scaleY = vw / baseW;
            break;
        case Scaling::FITHEIGHT:
            scaleX = scaleY = vh / baseH;
            break;
        case Scaling::LETTERBOX: {
            float s = (vw / baseW < vh / baseH) ? vw / baseW : vh / baseH;
            scaleX = scaleY = s;
            break;
        }
        case Scaling::CROP: {
            float s = (vw / baseW > vh / baseH) ? vw / baseW : vh / baseH;
            scaleX = scaleY = s;
            break;
        }
        case Scaling::STRETCH:
            scaleX = vw / baseW;
            scaleY = vh / baseH;
            break;
        case Scaling::NATIVE:
        default:
            scaleX = scaleY = 1.0f;
            break;
        }

        float dispW = baseW * scaleX;
        float dispH = baseH * scaleY;

        // Canvas centered in viewport
        float cX = vx + (vw - dispW) * 0.5f;
        float cY = vy + (vh - dispH) * 0.5f;
        ImVec2 cMin(cX, cY);
        ImVec2 cMax(cX + dispW, cY + dispH);

        // Clip drawing to viewport bounds
        dl->PushClipRect(vpMin, vpMax, true);

        // Viewport background
        if (mode == Scaling::LETTERBOX) {
            dl->AddRectFilled(vpMin, vpMax, colBlack);
        } else {
            dl->AddRectFilled(vpMin, vpMax, colVpBg);
        }

        // Canvas fill
        dl->AddRectFilled(cMin, cMax, colCanvasFill);

        // Triangle (content indicator)
        float triW = baseTW * scaleX;
        float triH = baseTH * scaleY;
        float triCX = cX + dispW * 0.5f;
        float triCY = cY + dispH * 0.5f;

        ImVec2 triP1(triCX, triCY - triH * 0.5f);
        ImVec2 triP2(triCX - triW * 0.5f, triCY + triH * 0.5f);
        ImVec2 triP3(triCX + triW * 0.5f, triCY + triH * 0.5f);
        dl->AddTriangleFilled(triP1, triP2, triP3, colTriangle);
        dl->AddTriangle(triP1, triP2, triP3, colCanvasBorder, Theme::dpi(1.0f));

        // Canvas border
        dl->AddRect(cMin, cMax, colCanvasBorder, 0, 0, Theme::dpi(1.5f));

        // Viewport border
        dl->AddRect(vpMin, vpMax, colVpBorder);

        dl->PopClipRect();

        curX += vw + spacing;
    }

    ImGui::Dummy(ImVec2(totalW, maxH));
}

// Same thumbnail flow as InspectorWindow::findThumbnail: load the ProjectWindow-
// generated thumbnail from the pool, requesting async generation when missing.
Texture* ProjectSettingsWindow::findThumbnail(const std::string& path) {
    if (path.empty()) return nullptr;

    std::filesystem::path texPath = path;
    const std::filesystem::path projectPath = m_project->getProjectPath();

    if (texPath.is_relative() && !projectPath.empty()) {
        texPath = projectPath / texPath;
    }
    texPath = texPath.lexically_normal();

    if (!texPath.is_absolute()) return nullptr;

    std::error_code ec;
    if (!std::filesystem::exists(texPath, ec) || ec) {
        return nullptr;
    }

    const std::filesystem::path thumbnailPath = m_project->getThumbnailPath(texPath);
    const std::string thumbPathStr = thumbnailPath.string();

    // Fast path: return from cache if already loaded
    auto thumbIt = m_thumbnailTextures.find(thumbPathStr);
    if (thumbIt != m_thumbnailTextures.end() && !thumbIt->second.empty()) {
        return &thumbIt->second;
    }

    std::error_code thumbEc;
    const bool thumbnailExists = std::filesystem::exists(thumbnailPath, thumbEc) && !thumbEc;

    if (thumbnailExists) {
        TextureData thumbData(thumbnailPath.string().c_str());
        if (thumbData.getData() && thumbData.getSize() > 0) {
            Texture thumbTexture(thumbPathStr, thumbData);
            m_thumbnailTextures[thumbPathStr] = thumbTexture;
            return &m_thumbnailTextures[thumbPathStr];
        }
    }

    // Thumbnail missing or failed to load — request generation
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
        projectWindow->requestThumbnailGeneration(texPath, thumbnailExists);
    }
    return nullptr;
}

void ProjectSettingsWindow::open(Project* project, const char* category) {
    m_isOpen = true;
    if (category) {
        const int index = settingsui::pageIndex(SettingsScope::Project, category);
        if (index >= 0) {
            m_category = index;
            m_categorySearch[0] = '\0';
        }
    }
    m_project = project;
    m_projectNameOriginal = project->getName();
    snprintf(m_projectNameBuffer, sizeof(m_projectNameBuffer), "%s", m_projectNameOriginal.c_str());
    m_canvasWidth = project->getCanvasWidth();
    m_canvasHeight = project->getCanvasHeight();
    m_scalingModeIndex = findScalingIndex(project->getScalingMode());
    m_textureStrategyIndex = findTextureStrategyIndex(project->getTextureStrategy());
    m_vsyncEnabled = project->isVSyncEnabled();
    for (int i = 0; i < 3; i++)
        m_atmosphereQuality[i] = (int)project->getAtmosphereQuality()[i];
    m_textureSettings = project->getTextureSettings();
    m_playerLogging = project->getPlayerLogging();
    m_playerCrash = project->getPlayerCrash();
    m_htmlUiMsaaIndex = findHtmlUiMsaaIndex(project->getHtmlUiMsaaSamples());
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++) {
        m_shadowTiers[platform] = static_cast<int>(project->getShadowTier(platform));
    }
    m_windowModeIndex = findWindowModeIndex(project->getWindowMode());
    m_windowWidth = (int)project->getWindowWidth();
    m_windowHeight = (int)project->getWindowHeight();
    m_windowResizable = project->isWindowResizable();
    m_windowTitleOriginal = project->getWindowTitle();
    snprintf(m_windowTitleBuffer, sizeof(m_windowTitleBuffer), "%s", m_windowTitleOriginal.c_str());
    m_windowIcon = project->getWindowIcon();
    m_assetsDir = project->getAssetsDir();
    m_luaDir = project->getLuaDir();
    m_scriptDirs = project->getScriptDirs();
    m_cxxStandardIndex = findCxxStandardIndex(project->getCxxStandard());
    m_physics2DEnabled = project->isPhysics2DEnabled();
    m_physics3DEnabled = project->isPhysics3DEnabled();
    m_inputActionsAsset = project->getInputActionsAsset();
    m_inputDeadzone = project->getInputDeadzone();
    m_inputStatus.clear();
    m_inputActionsFiles.clear();
    {
        std::error_code ec;
        const fs::path assets = project->getAssetsPath();
        for (auto it = fs::recursive_directory_iterator(assets, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)){
            if (it->is_regular_file(ec) && it->path().extension() == ".inputactions")
                m_inputActionsFiles.push_back(fs::relative(it->path(), project->getProjectPath(), ec));
        }
        std::sort(m_inputActionsFiles.begin(), m_inputActionsFiles.end());
    }
    m_runtimeShadersEnabled = project->isRuntimeShadersEnabled();
    m_mobilePbrTierIndex = findMobilePbrTierIndex(project->getMobilePbrTier());

    const ApplicationSettings& application = project->getApplicationSettings();
    snprintf(m_applicationNameBuffer, sizeof(m_applicationNameBuffer), "%s", application.name.c_str());
    snprintf(m_applicationIdentifierBuffer, sizeof(m_applicationIdentifierBuffer), "%s", application.identifier.c_str());
    snprintf(m_applicationVersionBuffer, sizeof(m_applicationVersionBuffer), "%s", application.version.c_str());
    m_applicationBuild = static_cast<int>(application.build);

    const WebProjectSettings& web = project->getWebProjectSettings();
    snprintf(m_webApplicationNameBuffer, sizeof(m_webApplicationNameBuffer), "%s", web.applicationName.c_str());
    m_webFavicon = web.favicon;
    m_webCustomHtmlShell = web.customHtmlShell;
    snprintf(m_webHeadIncludeBuffer, sizeof(m_webHeadIncludeBuffer), "%s", web.headInclude.c_str());
    m_webResizeCanvasToWindow = web.resizeCanvasToWindow;
    m_webHideEmscriptenUI = web.hideEmscriptenUI;

    const LinuxProjectSettings& linuxSettings = project->getLinuxProjectSettings();
    snprintf(m_linuxApplicationNameBuffer, sizeof(m_linuxApplicationNameBuffer), "%s", linuxSettings.applicationName.c_str());
    snprintf(m_linuxCommentBuffer, sizeof(m_linuxCommentBuffer), "%s", linuxSettings.comment.c_str());
    snprintf(m_linuxCategoriesBuffer, sizeof(m_linuxCategoriesBuffer), "%s", linuxSettings.categories.c_str());

    const WindowsProjectSettings& windows = project->getWindowsProjectSettings();
    snprintf(m_windowsProductNameBuffer, sizeof(m_windowsProductNameBuffer), "%s", windows.productName.c_str());
    snprintf(m_windowsCompanyNameBuffer, sizeof(m_windowsCompanyNameBuffer), "%s", windows.companyName.c_str());
    snprintf(m_windowsFileVersionBuffer, sizeof(m_windowsFileVersionBuffer), "%s", windows.fileVersion.c_str());
    snprintf(m_windowsProductVersionBuffer, sizeof(m_windowsProductVersionBuffer), "%s", windows.productVersion.c_str());

    const MacOSProjectSettings& macOS = project->getMacOSProjectSettings();
    snprintf(m_macOSApplicationNameBuffer, sizeof(m_macOSApplicationNameBuffer), "%s", macOS.applicationName.c_str());
    snprintf(m_macOSBundleIdentifierBuffer, sizeof(m_macOSBundleIdentifierBuffer), "%s", macOS.bundleIdentifier.c_str());
    snprintf(m_macOSVersionNameBuffer, sizeof(m_macOSVersionNameBuffer), "%s", macOS.versionName.c_str());
    snprintf(m_macOSBuildNumberBuffer, sizeof(m_macOSBuildNumberBuffer), "%s", macOS.buildNumber.c_str());
    m_macOSIcon = macOS.icon;
    m_macOSHighDpi = macOS.highDpi;

    const IOSProjectSettings& ios = project->getIOSProjectSettings();
    snprintf(m_iosApplicationNameBuffer, sizeof(m_iosApplicationNameBuffer), "%s", ios.applicationName.c_str());
    snprintf(m_iosBundleIdentifierBuffer, sizeof(m_iosBundleIdentifierBuffer), "%s", ios.bundleIdentifier.c_str());
    snprintf(m_iosVersionNameBuffer, sizeof(m_iosVersionNameBuffer), "%s", ios.versionName.c_str());
    snprintf(m_iosBuildNumberBuffer, sizeof(m_iosBuildNumberBuffer), "%s", ios.buildNumber.c_str());
    m_iosIcon = ios.icon;
    m_iosHideStatusBar = ios.hideStatusBar;
    m_iosHideHomeIndicator = ios.hideHomeIndicator;
    m_iosSupportsHighRefreshRate = ios.supportsHighRefreshRate;
    m_iosAdmobEnabled = ios.admobEnabled;
    snprintf(m_iosAdmobAppIdBuffer, sizeof(m_iosAdmobAppIdBuffer), "%s", ios.admobAppId.c_str());

    const AndroidProjectSettings& android = project->getAndroidProjectSettings();
    snprintf(m_androidApplicationNameBuffer, sizeof(m_androidApplicationNameBuffer), "%s", android.applicationName.c_str());
    snprintf(m_androidPackageNameBuffer, sizeof(m_androidPackageNameBuffer), "%s", android.packageName.c_str());
    snprintf(m_androidVersionNameBuffer, sizeof(m_androidVersionNameBuffer), "%s", android.versionName.c_str());
    m_androidLauncherIcon = android.launcherIcon;
    m_androidAdaptiveIconForeground = android.adaptiveIconForeground;
    m_androidAdaptiveIconBackground = android.adaptiveIconBackground;
    if (android.versionCode > 0) {
        snprintf(m_androidVersionCodeBuffer, sizeof(m_androidVersionCodeBuffer), "%u", android.versionCode);
    } else {
        m_androidVersionCodeBuffer[0] = '\0';
    }
    m_androidMinSdk = static_cast<int>(android.minSdk);
    m_androidTargetSdk = static_cast<int>(android.targetSdk);
    m_androidOrientationIndex = findAndroidOrientationIndex(android.orientation);
    m_androidAbiArmeabiV7a = android.abiArmeabiV7a;
    m_androidAbiArm64V8a = android.abiArm64V8a;
    m_androidAbiX86 = android.abiX86;
    m_androidAbiX86_64 = android.abiX86_64;
    m_androidPermissions = android.permissions;
    m_androidAllowBackup = android.allowBackup;
    m_androidFullscreen = android.fullscreen;
    m_androidKeepScreenOn = android.keepScreenOn;
    m_androidAdmobEnabled = android.admobEnabled;
    snprintf(m_androidAdmobAppIdBuffer, sizeof(m_androidAdmobAppIdBuffer), "%s", android.admobAppId.c_str());

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

    m_loadingSettings = project->getLoadingSettings();
    m_threading = project->getThreadingSettings();
    m_shaderCreation = project->getShaderCreationSettings();
    m_htmlUiRender = project->getHtmlUiRenderSettings();
    m_navigation = project->getNavigationSettings();

    m_packNativeResources = project->shouldPackNativeResources();
    m_versionControlMetadata = project->hasVersionControlMetadata();
    m_vcs = project->getVcsProjectSettings();

    m_tags = project->getTags();
    m_layerNames = project->getLayerNames();
    m_layerNames.resize(MAX_LAYERS);
    m_newTagBuffer[0] = '\0';

    m_layerCollision3D = project->getLayerCollisionMatrix3D();
    m_layerCollision2D = project->getLayerCollisionMatrix2D();
}

void ProjectSettingsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Project Settings##ProjectSettingsModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = viewport->GetWorkCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImVec2 size(
        std::min(Theme::dpi(projectDialogWidth), viewport->WorkSize.x * 0.9f),
        std::min(Theme::dpi(projectDialogHeight), viewport->WorkSize.y * 0.9f)
    );
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoResize |
                             projectNoScrollFlags;

    bool popupOpen = ImGui::BeginPopupModal("Project Settings##ProjectSettingsModal", &m_isOpen, flags);

    if (popupOpen) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            drawSettings();
        }
        ImGui::EndPopup();
    }
}

void ProjectSettingsWindow::drawSettings() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float footerY = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight();
    float regionHeight = std::max(1.0f, footerY - style.ItemSpacing.y * 2.0f - ImGui::GetCursorPosY());

    ImGui::BeginChild("##ProjectSettingsRegion", ImVec2(0, regionHeight), ImGuiChildFlags_None, projectNoScrollFlags);

    const float listWidth = std::min(Theme::dpi(projectCategoryListWidth), ImGui::GetContentRegionAvail().x * 0.3f);
    // the Search palette opened a row: its page
    if (const std::string revealPage = settingsui::pendingRevealPage(); !revealPage.empty()) {
        const int index = settingsui::pageIndex(SettingsScope::Project, revealPage);
        if (index >= 0) m_category = index;
    }
    settingsui::pageList("##ProjectSettingsCategories", SettingsScope::Project, m_category,
                         m_categorySearch, sizeof(m_categorySearch), ImVec2(listWidth, ImGui::GetContentRegionAvail().y));
    ImGui::SameLine();

    ImGui::BeginGroup();
    const SettingsPage* page = settingsui::pageAt(SettingsScope::Project, m_category);
    settingsui::categoryTitle(page ? page->displayName().c_str() : "");
    settingsui::beginPage(page ? page->name : std::string(), m_categorySearch);
    switch (static_cast<ProjectCategory>(m_category)) {
        case ProjectCategory::Player: drawPlayerSettings(); break;
        case ProjectCategory::Graphics: drawGraphicsSettings(); break;
        case ProjectCategory::Quality: drawQualitySettings(); break;
        case ProjectCategory::Texture: drawTextureSettings(); break;
        case ProjectCategory::Physics: drawPhysicsSettings(); break;
        case ProjectCategory::Physics2D: drawPhysics2DSettings(); break;
        case ProjectCategory::Input: drawInputSettings(); break;
        case ProjectCategory::Scripting: drawScriptingSettings(); break;
        case ProjectCategory::TagsAndLayers: drawTagsAndLayersSettings(); break;
        case ProjectCategory::Editor: drawEditorSettings(); break;
        case ProjectCategory::Navigation: drawNavigationSettings(); break;
        default: break;
    }
    settingsui::endPage();
    ImGui::EndGroup();

    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, footerY - style.ItemSpacing.y));
    ImGui::Separator();

    // Unity-style footer: buttons on the right, primary first.
    const float buttonWidth = Theme::dpi(projectSettingsButtonWidth);
    const float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    ImGui::SetCursorPos(ImVec2(std::max(style.WindowPadding.x, ImGui::GetWindowContentRegionMax().x - buttonsWidth), footerY));

    const bool hasAbi = hasAndroidAbiSelection(m_androidAbiArmeabiV7a, m_androidAbiArm64V8a, m_androidAbiX86, m_androidAbiX86_64);
    const std::string tagsAndLayersProblem = tagsAndLayersError();
    const bool canSave = hasAbi && tagsAndLayersProblem.empty();

    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
        if (applySettings()) {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!hasAbi) {
            ImGui::SetTooltip("Select at least one Android architecture (Player > Android).");
        } else {
            ImGui::SetTooltip("%s (Tags and Layers).", tagsAndLayersProblem.c_str());
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }

    // Per-frame lifecycle, matching Properties: the pool keeps the underlying
    // thumbnail textures cached, so re-creating the wrappers next frame is cheap.
    m_thumbnailTextures.clear();
}

std::string ProjectSettingsWindow::inheritedApplicationName() const {
    if (m_applicationNameBuffer[0]) return m_applicationNameBuffer;
    if (m_projectNameBuffer[0]) return m_projectNameBuffer;
    return "JasperEngine";
}

std::string ProjectSettingsWindow::inheritedApplicationIdentifier() const {
    return m_applicationIdentifierBuffer[0] ? m_applicationIdentifierBuffer : ApplicationSettings{}.identifier;
}

std::string ProjectSettingsWindow::inheritedApplicationVersion() const {
    return m_applicationVersionBuffer[0] ? m_applicationVersionBuffer : ApplicationSettings{}.version;
}

std::string ProjectSettingsWindow::inheritedApplicationBuild() const {
    return std::to_string(std::max(1, m_applicationBuild));
}

void ProjectSettingsWindow::drawPlayerSettings() {
    if (beginSettingsPanel("##PlayerSettingsPanel")) {
        if (beginSettingsTable("##PlayerProductTable")) {
            if (settingsui::beginRow("player.product_name")) {
                projectBeginSettingsRow("Project Name");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##ProjectName", m_projectNameBuffer, sizeof(m_projectNameBuffer));
            }


            const ApplicationSettings defaults;

            if (settingsui::beginRow("player.application_name")) {
                projectBeginSettingsRow("Product Name");
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                std::string nameHint = m_projectNameBuffer[0] ? m_projectNameBuffer : "JasperEngine";
                ImGui::InputTextWithHint("##ApplicationName", nameHint.c_str(), m_applicationNameBuffer, sizeof(m_applicationNameBuffer));
                endSettingsRow("Name every platform shows to the player. Empty means the project name.");
            }

            if (settingsui::beginRow("player.identifier")) {
                if (projectBeginSettingsRow("Application Identifier", strcmp(m_applicationIdentifierBuffer, defaults.identifier.c_str()) != 0)) {
                    snprintf(m_applicationIdentifierBuffer, sizeof(m_applicationIdentifierBuffer), "%s", defaults.identifier.c_str());
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::InputText("##ApplicationIdentifier", m_applicationIdentifierBuffer, sizeof(m_applicationIdentifierBuffer));
                endSettingsRow("Reverse-DNS id behind the Apple bundle identifiers and the Android package name.");
                if (!isSharedIdentifierValid(m_applicationIdentifierBuffer)) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Use letters and digits only, for example com.company.game.");
                }
            }

            if (settingsui::beginRow("player.version")) {
                if (projectBeginSettingsRow("Version", strcmp(m_applicationVersionBuffer, defaults.version.c_str()) != 0)) {
                    snprintf(m_applicationVersionBuffer, sizeof(m_applicationVersionBuffer), "%s", defaults.version.c_str());
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::InputText("##ApplicationVersion", m_applicationVersionBuffer, sizeof(m_applicationVersionBuffer));
                endSettingsRow("Version shown to the player. Apple takes at most three parts, Windows pads to four.");
                if (!isAppleVersionValid(m_applicationVersionBuffer)) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Use up to three numbers from 0 to 65535, for example 1.0.");
                }
            }

            if (settingsui::beginRow("player.build")) {
                drawIntSetting("Build", "##ApplicationBuild", m_applicationBuild, static_cast<int>(defaults.build), 1,
                    "Apple build number and Android version code. Increase it for every release.");
            }

            ImGui::EndTable();
        }

        if (settingsFoldout("Icon", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##PlayerIconTable")) {
            if (settingsui::beginRow("player.window_icon")) {
                projectBeginSettingsRow("Default Icon");
                {
                    const ImGuiStyle& style = ImGui::GetStyle();
                    float browseWidth = ImGui::CalcTextSize("Browse").x + style.FramePadding.x * 2.0f;
                    float clearWidth = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f;
                    float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - browseWidth - clearWidth - style.ItemSpacing.x * 2.0f - helpMarkerWidth());

                    fs::path iconDisplay = m_windowIcon.empty() ? fs::path("<None>") : m_windowIcon;
                    Widgets::pathDisplay("##WindowIconPath", iconDisplay, Vector2(pathWidth, ImGui::GetFrameHeight()));

                    ImGui::SameLine();
                    if (ImGui::Button("Browse##windowicon")) {
                        std::string defaultPath = m_project->getProjectPath().string();
                        std::string selectedPath = FileDialogs::openFileDialog(defaultPath, FILE_DIALOG_IMAGE, false);
                        if (!selectedPath.empty()) {
                            // Store project-relative when the file is inside the project
                            // so the setting survives moving the project folder.
                            std::error_code ec;
                            fs::path relPath = fs::relative(fs::path(selectedPath), m_project->getProjectPath(), ec);
                            if (ec || relPath.empty() || relPath.string().rfind("..", 0) == 0) {
                                m_windowIcon = fs::path(selectedPath);
                            } else {
                                m_windowIcon = relPath;
                            }
                        }
                    }

                    ImGui::SameLine();
                    ImGui::BeginDisabled(m_windowIcon.empty());
                    if (ImGui::Button("Clear##windowicon")) {
                        m_windowIcon.clear();
                    }
                    ImGui::EndDisabled();
                    endSettingsRow("Application icon for desktop builds: embedded into the Windows executable and used as the window/taskbar icon. Square PNG recommended (256x256 or larger).");

                    if (!m_windowIcon.empty()) {
                        if (Texture* thumb = findThumbnail(m_windowIcon.string())) {
                            ImGui::Spacing();

                            const float previewSize = Theme::dpi(64.0f);
                            float thumbWidth = (float)thumb->getWidth();
                            float thumbHeight = (float)thumb->getHeight();
                            float scale = previewSize / std::max(1.0f, std::max(thumbWidth, thumbHeight));
                            ImVec2 imageSize(std::max(1.0f, thumbWidth * scale), std::max(1.0f, thumbHeight * scale));

                            ImVec2 imagePos = ImGui::GetCursorScreenPos();
                            Widgets::image(Backend::getImGuiTexture(thumb->getRender()), imageSize);
                            ImGui::GetWindowDrawList()->AddRect(
                                imagePos, ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y),
                                ImGui::GetColorU32(ImGuiCol_Border));
                        }
                    }
                }
            }

            ImGui::EndTable();
        }

        if (settingsFoldout("Resolution and Presentation", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##PlayerResolutionTable")) {
            if (settingsui::beginRow("player.window_mode")) {
                drawComboSetting("Fullscreen Mode", "##WindowMode", windowModeNames, windowModeCount, m_windowModeIndex,
                    findWindowModeIndex(Project::defaultWindowMode),
                    "Initial window state of desktop builds. Web and mobile ignore window settings.");
            }

            if (settingsui::beginRow("player.window_width")) {
                drawIntSetting("Default Screen Width", "##WindowWidth", m_windowWidth, (int)Project::defaultWindowWidth);
            }
            if (settingsui::beginRow("player.window_height")) {
                drawIntSetting("Default Screen Height", "##WindowHeight", m_windowHeight, (int)Project::defaultWindowHeight);
            }

            if (settingsui::beginRow("player.window_resizable")) {
                if (projectBeginSettingsRow("Resizable Window", m_windowResizable != Project::defaultWindowResizable)) {
                    m_windowResizable = Project::defaultWindowResizable;
                }
                ImGui::Checkbox("##WindowResizable", &m_windowResizable);
                endSettingsRow("Applies to desktop builds. Exported Windows and macOS builds are always resizable.");
            }

            if (settingsui::beginRow("player.window_title")) {
                if (projectBeginSettingsRow("Window Title", strcmp(m_windowTitleBuffer, Project::defaultWindowTitle) != 0)) {
                    snprintf(m_windowTitleBuffer, sizeof(m_windowTitleBuffer), "%s", Project::defaultWindowTitle);
                }
                std::string titleHint = m_project->getName().empty() ? "JasperEngine" : m_project->getName();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##WindowTitle", titleHint.c_str(), m_windowTitleBuffer, sizeof(m_windowTitleBuffer));
            }

            ImGui::EndTable();
        }

        if (settingsFoldout("Splash and Loading", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##PlayerLoadingTable")) {
            if (settingsui::beginRow("player.start_scene")) {
                projectBeginSettingsRow("Start Scene");

                const auto& scenes = m_project->getScenes();
                const SceneProject* selectedScene = m_project->getScene(m_startSceneId);
                if (!selectedScene || selectedScene->filepath.empty()) {
                    selectedScene = nullptr;
                    m_startSceneId = NULL_PROJECT_SCENE;
                    for (const auto& scene : scenes) {
                        if (!scene.filepath.empty()) {
                            selectedScene = &scene;
                            m_startSceneId = scene.id;
                            break;
                        }
                    }
                }

                if (!selectedScene) {
                    ImGui::TextDisabled("No saved scenes");
                } else {
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##StartScene", selectedScene->name.c_str())) {
                        for (const auto& scene : scenes) {
                            if (scene.filepath.empty()) continue;

                            bool selected = m_startSceneId == scene.id;
                            if (ImGui::Selectable(scene.name.c_str(), selected)) m_startSceneId = scene.id;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
            }


            const LoadingSettings defaults;
            LoadingSettings& loading = m_loadingSettings;

            if (settingsui::beginRow("player.loading_scene")) {
                if (projectBeginSettingsRow("Loading Scene", loading.sceneId != defaults.sceneId)) {
                    loading.sceneId = defaults.sceneId;
                }
                const SceneProject* loadingScene = m_project->getScene(loading.sceneId);
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::BeginCombo("##LoadingScene", loadingScene ? loadingScene->name.c_str() : "None")) {
                    if (ImGui::Selectable("None", !loadingScene)) loading.sceneId = NULL_PROJECT_SCENE;
                    for (const auto& scene : m_project->getScenes()) {
                        if (scene.filepath.empty()) continue;

                        bool selected = loading.sceneId == scene.id;
                        if (ImGui::Selectable(scene.name.c_str(), selected)) loading.sceneId = scene.id;
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                endSettingsRow("Shown over scene changes until the new scene has loaded.");
            }

            if (settingsui::beginRow("player.loading_delay")) {
                drawSecondsSetting("Loading Delay", "##LoadingDelay", loading.delay, defaults.delay,
                    "Time the loading scene covers the old scene before replacing it.");
            }
            if (settingsui::beginRow("player.loading_timeout")) {
                drawSecondsSetting("Loading Timeout", "##LoadingTimeout", loading.timeout, defaults.timeout,
                    "Time a load waits without progress before giving up.");
            }

            if (settingsui::beginRow("player.async_loading")) {
                if (projectBeginSettingsRow("Async Loading", loading.asyncLoading != defaults.asyncLoading)) {
                    loading.asyncLoading = defaults.asyncLoading;
                }
                ImGui::Checkbox("##AsyncLoading", &loading.asyncLoading);
                endSettingsRow("Exported games load resources on worker threads. The editor always does.");
            }

            ImGui::EndTable();
        }

        if (settingsFoldout("Logging") && beginSettingsTable("##PlayerLoggingTable")) {
            const PlayerLoggingSettings defaults;
            if (settingsui::beginRow("player.logging.level")) {
                if (projectBeginSettingsRow("Log Level", m_playerLogging.level != defaults.level)) {
                    m_playerLogging.level = defaults.level;
                }
                static const char* const levelNames[] = {"trace", "debug", "info", "warn", "error", "off"};
                static const char* const levelLabels[] = {"Trace", "Debug", "Info", "Warning", "Error", "Off"};
                int levelIndex = 2;
                for (int i = 0; i < IM_ARRAYSIZE(levelNames); i++) {
                    if (m_playerLogging.level == levelNames[i]) levelIndex = i;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::Combo("##PlayerLogLevel", &levelIndex, levelLabels, IM_ARRAYSIZE(levelLabels))) {
                    m_playerLogging.level = levelNames[levelIndex];
                }
                endSettingsRow("The lowest level the built game logs (the console, logcat, the browser console). "
                    "JASPER_LOG_LEVEL and --log-level override it when the game starts.");
            }

            if (settingsui::beginRow("player.logging.file")) {
                if (projectBeginSettingsRow("Log File", m_playerLogging.fileLog != defaults.fileLog)) {
                    m_playerLogging.fileLog = defaults.fileLog;
                }
                ImGui::Checkbox("##PlayerLogFile", &m_playerLogging.fileLog);
                endSettingsRow("Desktop games also write logs/player.log in their user data folder (1 MB, the previous one kept).");
            }

            if (settingsui::beginRow("player.logging.backtrace")) {
                if (projectBeginSettingsRow("Capture Backtrace on Errors", m_playerLogging.backtrace != defaults.backtrace)) {
                    m_playerLogging.backtrace = defaults.backtrace;
                }
                ImGui::Checkbox("##PlayerLogBacktrace", &m_playerLogging.backtrace);
                endSettingsRow("Errors record their call stack (written to the log file). Debug builds always do.");
            }

            ImGui::EndTable();
        }

        if (settingsFoldout("Crash Handling") && beginSettingsTable("##PlayerCrashTable")) {
            const PlayerCrashSettings defaults;
            if (settingsui::beginRow("player.crash.enabled")) {
                if (projectBeginSettingsRow("Enabled", m_playerCrash.enabled != defaults.enabled)) {
                    m_playerCrash.enabled = defaults.enabled;
                }
                ImGui::Checkbox("##PlayerCrashEnabled", &m_playerCrash.enabled);
                endSettingsRow("A crash writes a report folder in the game's user data (crash/reports). Nothing is sent "
                    "anywhere: Lua Crash.setReportHook gets the pending reports at the next start.");
            }

            ImGui::BeginDisabled(!m_playerCrash.enabled);
            if (settingsui::beginRow("player.crash.log_tail")) {
                if (projectBeginSettingsRow("Include Log Tail", m_playerCrash.includeLogTail != defaults.includeLogTail)) {
                    m_playerCrash.includeLogTail = defaults.includeLogTail;
                }
                ImGui::Checkbox("##PlayerCrashLogTail", &m_playerCrash.includeLogTail);
                endSettingsRow("The report keeps the last log lines (log-tail.txt). They may hold player data.");
            }

            if (settingsui::beginRow("player.crash.dump_with_heap")) {
                if (projectBeginSettingsRow("Windows Dump Type", m_playerCrash.dumpWithHeap != defaults.dumpWithHeap)) {
                    m_playerCrash.dumpWithHeap = defaults.dumpWithHeap;
                }
                static const char* const dumpLabels[] = {"Small", "With Heap"};
                int dumpIndex = m_playerCrash.dumpWithHeap ? 1 : 0;
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::Combo("##PlayerCrashDump", &dumpIndex, dumpLabels, IM_ARRAYSIZE(dumpLabels))) {
                    m_playerCrash.dumpWithHeap = dumpIndex == 1;
                }
                endSettingsRow("What a Windows minidump holds: threads, stacks and modules (Small), or also the memory the "
                    "stacks point to (With Heap, larger, more player data).");
            }
            ImGui::EndDisabled();

            if (settingsui::beginRow("player.crash.hang_watchdog")) {
                if (projectBeginSettingsRow("Hang Watchdog In Development Builds", m_playerCrash.hangWatchdog != defaults.hangWatchdog)) {
                    m_playerCrash.hangWatchdog = defaults.hangWatchdog;
                }
                ImGui::Checkbox("##PlayerHangWatchdog", &m_playerCrash.hangWatchdog);
                endSettingsRow("A Development Build reports when its main thread stops responding (hang-NNN.txt). "
                    "Release builds never run it.");
            }

            ImGui::BeginDisabled(!m_playerCrash.hangWatchdog);
            if (settingsui::beginRow("player.crash.hang_seconds")) {
                if (projectBeginSettingsRow("Hang Threshold (s)", m_playerCrash.hangSeconds != defaults.hangSeconds)) {
                    m_playerCrash.hangSeconds = defaults.hangSeconds;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::DragInt("##PlayerHangSeconds", &m_playerCrash.hangSeconds, 0.2f, PlayerCrashSettings::minHangSeconds,
                               PlayerCrashSettings::maxHangSeconds, "%d", ImGuiSliderFlags_AlwaysClamp);
                endSettingsRow("How long the main thread may not respond before a report.");
            }
            ImGui::EndDisabled();

            if (settingsui::beginRow("player.crash.web_overlay")) {
                if (projectBeginSettingsRow("Web > Show Crash Overlay", m_playerCrash.webCrashOverlay != defaults.webCrashOverlay)) {
                    m_playerCrash.webCrashOverlay = defaults.webCrashOverlay;
                }
                ImGui::Checkbox("##PlayerCrashWebOverlay", &m_playerCrash.webCrashOverlay);
                endSettingsRow("A web game that crashes shows \"The game stopped working. [Reload]\" over the page "
                    "(window.jasperengine.onCrash gets the report either way).");
            }

            ImGui::EndTable();
        }

        if (settingsFoldout("Threading") && beginSettingsTable("##PlayerThreadingTable")) {
            const ThreadingProjectSettings defaults;
            ThreadingProjectSettings& threading = m_threading;
            if (settingsui::beginRow("player.threading.workers")) {
                drawCountSetting("Worker Threads", "##WorkerThreads", threading.workers, 1, 15,
                    "Job system workers. Auto: one per logical core minus one (at most 15; 4 on mobile and web). "
                    "Applies on the next launch.");
            }
            if (settingsui::beginRow("player.threading.reserved_frame_workers")) {
                drawCountSetting("Reserved Frame Workers", "##ReservedFrameWorkers", threading.reservedFrameWorkers, 0, 4,
                    "Workers Background work (asset decoding, cooks) never takes, so the frame always has them. "
                    "Auto: 2 on desktop, 1 on mobile and web. Applies immediately.");
            }
            if (settingsui::beginRow("player.threading.io_threads")) {
                drawCountSetting("Asset I/O Threads", "##AssetIOThreads", threading.ioThreads, 0, 4,
                    "Threads that read asset files. Auto: 2 on desktop, 1 on mobile, none on web (the decode job reads). "
                    "Applies on the next launch.");
            }

            if (settingsui::beginRow("player.threading.web_pump_budget_ms")) {
                if (projectBeginSettingsRow("Web Pump Budget", threading.webPumpBudgetMs != defaults.webPumpBudgetMs)) {
                    threading.webPumpBudgetMs = defaults.webPumpBudgetMs;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::InputFloat("##WebPumpBudget", &threading.webPumpBudgetMs, 0.5f, 1.0f, "%.1f ms");
                threading.webPumpBudgetMs = std::isfinite(threading.webPumpBudgetMs)
                    ? std::min(std::max(threading.webPumpBudgetMs, 0.5f), 50.0f) : defaults.webPumpBudgetMs;
                endSettingsRow("Web builds without threads run Background jobs for this long each frame.");
            }

            if (settingsui::beginRow("player.threading.web_multithreaded")) {
                if (projectBeginSettingsRow("Web: Multithreaded", threading.webMultithreaded != defaults.webMultithreaded)) {
                    threading.webMultithreaded = defaults.webMultithreaded;
                }
                ImGui::Checkbox("##WebMultithreaded", &threading.webMultithreaded);
                endSettingsRow("Also builds the web player with threads (requires cross-origin isolation): the page "
                               "picks it when the browser allows SharedArrayBuffer, else the single-thread build.");
            }

            ImGui::EndTable();
        }

        if (settingsFoldout("Publishing Settings") && beginSettingsTable("##PlayerPublishingTable")) {
            if (settingsui::beginRow("player.pack_native_resources")) {
                if (projectBeginSettingsRow("Native Resource Pack", m_packNativeResources != Project::defaultPackNativeResources)) {
                    m_packNativeResources = Project::defaultPackNativeResources;
                }
                ImGui::Checkbox("##PackNativeResources", &m_packNativeResources);
                endSettingsRow("Experimental. Packs exported assets and Lua files into resources.pak for Desktop and Android source exports. "
                    "Packed resources are read through Data; direct File handles cannot open them.");
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        drawPlatformsSettings();
    }
    endSettingsPanel();
}

void ProjectSettingsWindow::drawGraphicsSettings() {
    projectDrawSettingsPanel("##GraphicsSettingsPanel", [this]() {
        if (settingsui::beginRow("graphics.canvas_width")) {
            drawIntSetting("Canvas Width", "##CanvasWidth", m_canvasWidth, (int)Project::defaultCanvasWidth);
        }
        if (settingsui::beginRow("graphics.canvas_height")) {
            drawIntSetting("Canvas Height", "##CanvasHeight", m_canvasHeight, (int)Project::defaultCanvasHeight);
        }

        if (settingsui::beginRow("graphics.scaling_mode")) {
            drawComboSetting("Scaling Mode", "##ScalingMode", scalingModeNames, scalingModeCount, m_scalingModeIndex, findScalingIndex(Project::defaultScalingMode));
            ImGui::Spacing();
            drawScalingPreview(scalingModeValues[m_scalingModeIndex], m_canvasWidth, m_canvasHeight);
        }

        if (settingsui::beginRow("graphics.texture_strategy")) {
            drawComboSetting("Texture Strategy", "##TextureStrategy", textureStrategyNames, textureStrategyCount, m_textureStrategyIndex, findTextureStrategyIndex(Project::defaultTextureStrategy),
                "Only very old GPUs need power-of-two textures. Resize blurs the edges of atlas tiles. The editor applies a change when the project is reopened.");
        }

        if (settingsui::beginRow("graphics.html_ui_msaa_samples")) {
            drawComboSetting("HTML UI MSAA", "##HtmlUiMsaa", htmlUiMsaaNames, htmlUiMsaaCount, m_htmlUiMsaaIndex, findHtmlUiMsaaIndex(Project::defaultHtmlUiMsaaSamples),
                "Multisampling of the HTML/CSS UI documents: smoother rounded corners, thin borders and rotated edges. "
                "Devices without MSAA draw them without it.");
        }
        {
            // the renderer's cost and text options (S16)
            const HtmlUiRenderSettings defaults;
            if (settingsui::beginRow("graphics.html_ui_glyph_atlas_mib")) {
                if (projectBeginSettingsRow("HTML UI Glyph Atlas (MiB)", m_htmlUiRender.glyphAtlasMiB != defaults.glyphAtlasMiB)) {
                    m_htmlUiRender.glyphAtlasMiB = defaults.glyphAtlasMiB;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::InputInt("##HtmlUiGlyphAtlas", &m_htmlUiRender.glyphAtlasMiB);
                if (m_htmlUiRender.glyphAtlasMiB != 0)
                    m_htmlUiRender.glyphAtlasMiB = std::clamp(m_htmlUiRender.glyphAtlasMiB, Project::minHtmlUiGlyphAtlasMiB, Project::maxHtmlUiGlyphAtlasMiB);
                endSettingsRow("GPU memory of the HTML UI's glyph atlas: two thirds bitmap glyphs, one third distance-field glyphs. "
                               "0 is the platform's default (24 MiB on desktop, 12 on Android, iOS and the web). When text needs "
                               "more, pages not drawn lately are cleared and their glyphs made again.");
            }
            if (settingsui::beginRow("graphics.html_ui_sdf_primitives")) {
                if (projectBeginSettingsRow("HTML UI SDF Primitives", m_htmlUiRender.sdfPrimitives != defaults.sdfPrimitives)) {
                    m_htmlUiRender.sdfPrimitives = defaults.sdfPrimitives;
                }
                ImGui::Checkbox("##HtmlUiSdfPrimitives", &m_htmlUiRender.sdfPrimitives);
                endSettingsRow("Draws solid backgrounds, borders, outlines and box shadows as antialiased shapes in batched draws, "
                               "without shadow textures or layers. Off draws them as meshes, as before.");
            }
            if (settingsui::beginRow("graphics.html_ui_transform_aware_text")) {
                if (projectBeginSettingsRow("HTML UI Transform-Aware Text", m_htmlUiRender.transformAwareText != defaults.transformAwareText)) {
                    m_htmlUiRender.transformAwareText = defaults.transformAwareText;
                }
                ImGui::Checkbox("##HtmlUiTransformAwareText", &m_htmlUiRender.transformAwareText);
                endSettingsRow("Rasterizes text at its on-screen size under scale transforms and odd pixel ratios, and draws it "
                               "from distance fields while that size animates. Off keeps text a bitmap at the document's scale.");
            }
        }

        if (settingsui::beginRow("graphics.mobile_pbr_tier")) {
            drawComboSetting("Mobile PBR Tier", "##MobilePbrTier", mobilePbrTierNames, 2, m_mobilePbrTierIndex, findMobilePbrTierIndex(Project::defaultMobilePbrTier),
                "Android, iOS and HTML5 exports with Reduced leave out iridescence, anisotropy direction maps and clear coat normal maps "
                "of the built-in Lit's extended lobes (fewer and lighter shader variants). Desktop exports and the editor draw everything.");
        }

        // Shadows: the quality tier a scene of Quality Auto renders with, per platform
        if (settingsui::sectionVisible("Shadows")) {
            ImGui::SeparatorText("Shadows");
            static const char* const tierNames[] = {"Low", "Medium", "High", "Ultra"};
            static const char* const platformLabels[] = {"macOS", "iOS", "Web", "Android", "Linux", "Windows"};
            static const int platformOrder[] = {5, 4, 0, 2, 3, 1};   // Windows, Linux, macOS, Web, Android, iOS
            for (int platform : platformOrder) {
                const std::string key = std::string("graphics.shadow_tier.") + Project::getShadowTierPlatformName(platform);
                if (!settingsui::beginRow(key.c_str())) continue;
                const std::string label = std::string(platformLabels[platform]) + " Shadow Tier";
                const std::string id = std::string("##ShadowTier") + platformLabels[platform];
                drawComboSetting(label.c_str(), id.c_str(), tierNames, 4, m_shadowTiers[platform],
                    static_cast<int>(ShadowTiers::defaultTier(platform)),
                    "The shadow quality tier of this platform's builds (and of the editor running on it) for scenes whose shadow "
                    "Quality is Auto: cascades, resolution, distance, filter and the memory budget (docs/rendering/shadows.md). "
                    "Ultra adds PCSS contact hardening.");
            }
        }

        drawShaderCreationSettings();

        if (settingsui::beginRow("graphics.runtime_shaders")) {
            if (projectBeginSettingsRow("Runtime Shader Compilation", m_runtimeShadersEnabled != Project::defaultRuntimeShadersEnabled)) {
                m_runtimeShadersEnabled = Project::defaultRuntimeShadersEnabled;
            }
            ImGui::Checkbox("##RuntimeShaders", &m_runtimeShadersEnabled);
            endSettingsRow("Exported builds compile the shader variants they do not ship precompiled, such as those of meshes and materials "
                "that scripts build or change, in the background and cache them on disk. When off, the build carries only the "
                "variants in Build Settings > Shaders and leaves out what draws with any other. Play always compiles.");
        }
    });
}

// "mainThreadBudgetMs" -> "main_thread_budget_ms": the field's settings key
static std::string shaderCreationKey(const char* name) {
    std::string out;
    for (const char* c = name; *c; c++) {
        if (std::isupper(static_cast<unsigned char>(*c))) {
            if (!out.empty()) out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        } else {
            out += *c;
        }
    }
    return out;
}

// Graphics > Shader Creation and Shader Warm-up (spec S32): the rows of the reflection table
// (labels, tooltips and the budget's range are its), and the warm-up list's count with Clear
void ProjectSettingsWindow::drawShaderCreationSettings() {
    const ShaderCreationProjectSettings defaults;
    for (const char* section : {"Shader Creation", "Shader Warm-up"}) {
        if (!settingsui::sectionVisible(section)) continue;
        ImGui::SeparatorText(section);
        for (const reflect::FieldDesc& field : reflect::shaderCreationStruct().fields) {
            const std::string key = std::string("shader_creation.") + shaderCreationKey(field.name);
            const SettingRow* row = SettingsRegistry::get().find(key);
            if (!row || row->section != section || !settingsui::beginRow(key.c_str())) continue;
            void* value = field.ref(&m_shaderCreation);
            const void* base = field.ref(const_cast<ShaderCreationProjectSettings*>(&defaults));
            const std::string id = std::string("##") + field.name;
            if (field.kind == reflect::Kind::Bool) {
                bool& b = *static_cast<bool*>(value);
                if (projectBeginSettingsRow(field.label, b != *static_cast<const bool*>(base))) b = *static_cast<const bool*>(base);
                ImGui::Checkbox(id.c_str(), &b);
            } else {
                float& f = *static_cast<float*>(value);
                if (projectBeginSettingsRow(field.label, f != *static_cast<const float*>(base))) f = *static_cast<const float*>(base);
                ImGui::SliderFloat(id.c_str(), &f, field.min, field.max, field.format ? field.format : "%.1f");
                f = std::clamp(f, field.min, field.max);
            }
            endSettingsRow(field.tooltip ? field.tooltip : "");
        }
        if (std::string(section) == "Shader Warm-up" && settingsui::beginRow("shader_creation.warmup_entries")) {
            const SettingRow* row = SettingsRegistry::get().find("shader_creation.warmup_entries");
            SettingsContext ctx;
            ctx.projectPtr = m_project;
            const mcp::Json counts = row && row->get ? row->get(ctx) : mcp::Json();
            projectBeginSettingsRow("Warm-up List");
            ImGui::Text("%d shaders, %d pipelines", counts.value("shaders", 0), counts.value("pipelines", 0));
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##ShaderWarmup")) {
                const CommandDef* clear = CommandCatalog::editor().find("assets.clearShaderWarmup");
                if (clear && clear->run) clear->run(CommandArgs{});
            }
            endSettingsRow(row ? row->tooltip.c_str() : "");
        }
    }
}

void ProjectSettingsWindow::drawQualitySettings() {
    projectDrawSettingsPanel("##QualitySettingsPanel", [this]() {
        if (settingsui::beginRow("quality.vsync")) {
            if (projectBeginSettingsRow("VSync", m_vsyncEnabled != Project::defaultVSyncEnabled)) {
                m_vsyncEnabled = Project::defaultVSyncEnabled;
            }
            ImGui::Checkbox("##VSync", &m_vsyncEnabled);
            endSettingsRow("Synchronize Play mode and supported desktop builds to the display refresh rate. "
                "The editor window follows Preferences > General > Editor VSync. "
                "macOS Metal exports remain synchronized.");
        }

        // the tiers of volumetric fog and clouds (docs/mcp/guide/atmosphere.md)
        static const char* const qualityNames[] = {"Off", "Low", "Medium", "High"};
        static const char* const rows[3] = {"Atmosphere Quality (Desktop)", "Atmosphere Quality (Mobile)", "Atmosphere Quality (Web)"};
        static const char* const ids[3] = {"##AtmosphereDesktop", "##AtmosphereMobile", "##AtmosphereWeb"};
        static const char* const tooltips[3] = {
            "Volumetric fog and volumetric clouds on Windows, macOS and Linux (and the editor's views). Off: neither; "
            "Low, Medium, High: their resolution, steps, light and volume counts. Height fog and the sky fill are never tiered.",
            "Volumetric fog and volumetric clouds on Android and iOS.",
            "Volumetric fog and volumetric clouds in HTML5 builds."};
        static const char* const keys[3] = {"quality.atmosphere_quality_desktop", "quality.atmosphere_quality_mobile", "quality.atmosphere_quality_web"};
        for (int i = 0; i < 3; i++) {
            if (!settingsui::beginRow(keys[i])) continue;
            drawComboSetting(rows[i], ids[i], qualityNames, 4, m_atmosphereQuality[i], (int)Project::defaultAtmosphereQuality[i], tooltips[i]);
        }
    });
}

void ProjectSettingsWindow::drawTextureSettings() {
    projectDrawSettingsPanel("##TextureSettingsPanel", [this]() {
        const TextureProjectSettings defaults;
        TextureProjectSettings& t = m_textureSettings;
        // the cook defaults of each family (a texture's own settings win)
        static const char* const qualityLabels[] = {"None (uncompressed)", "Low", "Normal", "High"};
        static const char* const sizeLabels[] = {"32", "64", "128", "256", "512", "1024", "2048", "4096", "8192", "16384"};
        for (size_t f = 0; f < kTextureFamilyCount; f++) {
            const std::string family = textureFamilyLabel((TextureFamily)f);
            const std::string familyKey = textureFamilyName((TextureFamily)f);
            if (settingsui::beginRow(("texture.default_quality." + familyKey).c_str())) {
            const std::string qualityRow = "Default Quality (" + family + ")";
            const std::string qualityId = "##TextureQuality" + family;
            int quality = (int)t.defaultQuality[f];
            drawComboSetting(qualityRow.c_str(), qualityId.c_str(), qualityLabels, 4, quality, (int)defaults.defaultQuality[f],
                "The compression of this family's textures without an override: None keeps them uncompressed; Low, "
                "Normal, High pick the formats of the spec's table (docs/textures.md).");
            t.defaultQuality[f] = (TextureCookQuality)quality;
            }
            if (!settingsui::beginRow(("texture.default_max_size." + familyKey).c_str())) continue;
            const std::string sizeRow = "Default Max Size (" + family + ")";
            const std::string sizeId = "##TextureMaxSize" + family;
            int sizeIndex = 0, defaultIndex = 0;
            for (int i = 0; i < 10; i++) {
                if ((32 << i) == t.defaultMaxSize[f]) sizeIndex = i;
                if ((32 << i) == defaults.defaultMaxSize[f]) defaultIndex = i;
            }
            drawComboSetting(sizeRow.c_str(), sizeId.c_str(), sizeLabels, 10, sizeIndex, defaultIndex,
                             "A larger source is shrunk to this on its longest side at cook time.");
            t.defaultMaxSize[f] = 32 << sizeIndex;
        }

        if (settingsui::beginRow("texture.android_etc2_fallback")) {
            if (projectBeginSettingsRow("Android ETC2 Fallback", t.androidEtc2Fallback != defaults.androidEtc2Fallback)) {
                t.androidEtc2Fallback = defaults.androidEtc2Fallback;
            }
            ImGui::Checkbox("##AndroidEtc2Fallback", &t.androidEtc2Fallback);
            endSettingsRow("Android exports also carry an ETC2 / EAC cook, loaded by devices without ASTC, and the manifest no "
                           "longer asks the store for ASTC.");
        }

        // the runtime per platform group: Engine::setTextureQuality / setTextureMemoryBudget
        static const char* const groups[3] = {"Desktop", "Mobile", "Web"};
        static const char* const mipLabels[] = {"Full (0)", "Half (1)", "Quarter (2)", "Eighth (3)"};
        for (int g = 0; g < 3; g++) {
            const std::string groupKey = texturePlatformGroupName((size_t)g);
            if (settingsui::beginRow(("texture.texture_quality." + groupKey).c_str())) {
            const std::string qualityRow = std::string("Texture Quality (") + groups[g] + ")";
            const std::string qualityId = std::string("##TextureRuntimeQuality") + groups[g];
            drawComboSetting(qualityRow.c_str(), qualityId.c_str(), mipLabels, 4, t.textureQuality[g], defaults.textureQuality[g],
                "Top mips of streamable textures dropped at load (Unity's Global Mipmap Limit); Lua: Engine.setTextureQuality.");
            }
            if (settingsui::beginRow(("texture.memory_budget_mb." + groupKey).c_str())) {
            const std::string budgetRow = std::string("Texture Memory Budget (") + groups[g] + ")";
            const std::string budgetId = std::string("##TextureBudget") + groups[g];
            if (projectBeginSettingsRow(budgetRow.c_str(), t.memoryBudgetMB[g] != defaults.memoryBudgetMB[g])) {
                t.memoryBudgetMB[g] = defaults.memoryBudgetMB[g];
            }
            ImGui::SetNextItemWidth(-helpMarkerWidth());
            ImGui::InputInt(budgetId.c_str(), &t.memoryBudgetMB[g], 32, 256);
            t.memoryBudgetMB[g] = std::clamp(t.memoryBudgetMB[g], 0, 1 << 20);
            endSettingsRow("MB of resident textures; 0 = unlimited. Over it, the largest streamable textures drop mips "
                           "(a texture keeps at least 64 px).");
            }
            if (!settingsui::beginRow(("texture.low_memory_fallback." + groupKey).c_str())) continue;
            const std::string lowRow = std::string("Low-Memory Fallback (") + groups[g] + ")";
            const std::string lowId = std::string("##TextureLowMemory") + groups[g];
            if (projectBeginSettingsRow(lowRow.c_str(), t.lowMemoryFallback[g] != defaults.lowMemoryFallback[g])) {
                t.lowMemoryFallback[g] = defaults.lowMemoryFallback[g];
            }
            bool low = t.lowMemoryFallback[g];
            ImGui::Checkbox(lowId.c_str(), &low);
            t.lowMemoryFallback[g] = low;
            endSettingsRow("When a GL-family device samples none of a texture's formats: RGB565 / RGBA4444 instead of RGBA8.");
        }

        // the cook cache
        if (settingsui::beginRow("texture.cache_size_mb")) {
        if (projectBeginSettingsRow("Cache Size", t.cacheSizeMB != defaults.cacheSizeMB)) {
            t.cacheSizeMB = defaults.cacheSizeMB;
        }
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::InputInt("##TextureCacheSize", &t.cacheSizeMB, 256, 1024);
        t.cacheSizeMB = std::clamp(t.cacheSizeMB, 64, 1 << 20);
        endSettingsRow("MB the cooked texture cache keeps (least recently used files go first).");
        }
        if (settingsui::beginRow("texture.global_cache_dir")) {
        if (projectBeginSettingsRow("Global Cache Directory", !t.globalCacheDir.empty())) {
            t.globalCacheDir.clear();
        }
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "%s", t.globalCacheDir.c_str());
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        if (ImGui::InputTextWithHint("##TextureGlobalCache", ".jasperengine/cache/textures (this project)", buffer, sizeof(buffer))) {
            t.globalCacheDir = buffer;
        }
        endSettingsRow("A directory every project shares (the keys are content addresses, so sharing is safe).");
        }
        if (!settingsui::beginRow("texture.clear_cache")) return;
        projectBeginSettingsRow("Texture Cache");
        TextureCookQueue& cooks = TextureCookQueue::get();
        const bool attached = cooks.getProject() == m_project;
        ImGui::BeginDisabled(!attached);
        if (ImGui::Button("Clear Texture Cache")) {
            cooks.cancelAll();
            cooks.getCache().clear();
            cooks.invalidateAll();
        }
        ImGui::EndDisabled();
        if (attached) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TextureCookReport::formatBytes(cooks.getCache().totalBytes()).c_str());
        }
    });
}

void ProjectSettingsWindow::drawPhysicsSettings() {
    if (beginSettingsPanel("##PhysicsSettingsPanel")) {
        if (beginSettingsTable("##SettingsTable")) {
            if (settingsui::beginRow("physics.enabled")) {
                if (projectBeginSettingsRow("3D Physics", m_physics3DEnabled != Project::defaultPhysics3DEnabled)) {
                    m_physics3DEnabled = Project::defaultPhysics3DEnabled;
                }
                ImGui::Checkbox("##Physics3D", &m_physics3DEnabled);
                endSettingsRow("Jolt-based 3D physics. When off, Jolt is left out of exported builds, along with any Rigidbodies and joints the scenes still use. Play always runs with both backends available.");
            }
            ImGui::EndTable();
        }

        if (settingsFoldout("Layer Collision Matrix", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawSectionNote("Rigidbodies on layers whose box is cleared pass through each other and report no contacts. "
                "Scripts change it with Physics.ignoreLayerCollision; collider category and mask bits still apply to the checked pairs.");
            if (settingsui::beginRow("physics.layer_collision_matrix")) drawLayerCollisionMatrix("##LayerCollisionMatrix3D", m_layerCollision3D);
        }
    }
    endSettingsPanel();
}

void ProjectSettingsWindow::drawPhysics2DSettings() {
    if (beginSettingsPanel("##Physics2DSettingsPanel")) {
        if (beginSettingsTable("##SettingsTable")) {
            if (settingsui::beginRow("physics2d.enabled")) {
                if (projectBeginSettingsRow("2D Physics", m_physics2DEnabled != Project::defaultPhysics2DEnabled)) {
                    m_physics2DEnabled = Project::defaultPhysics2DEnabled;
                }
                ImGui::Checkbox("##Physics2D", &m_physics2DEnabled);
                endSettingsRow("Box2D-based 2D physics. When off, Box2D is left out of exported builds, along with any Rigidbody 2D and joints the scenes still use. Play always runs with both backends available.");
            }
            ImGui::EndTable();
        }

        if (settingsFoldout("Layer Collision Matrix", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawSectionNote("Rigidbody 2D colliders on layers whose box is cleared pass through each other and report no contacts or triggers. "
                "Scripts change it with Physics2D.ignoreLayerCollision; collider category, mask and group still apply to the checked pairs.");
            if (settingsui::beginRow("physics2d.layer_collision_matrix")) drawLayerCollisionMatrix("##LayerCollisionMatrix2D", m_layerCollision2D);
        }
    }
    endSettingsPanel();
}

// Text turned a quarter left, read bottom to top, from bottomLeft upwards
static void addVerticalText(ImDrawList* drawList, const ImVec2& bottomLeft, ImU32 color, const char* text) {
    const int firstVertex = drawList->VtxBuffer.Size;
    // laid out unclipped, as the clip test would run on the unrotated glyphs; the
    // draw command still clips to the window
    const ImVec4 noClip(-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX);
    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), bottomLeft, color, text, nullptr, 0.0f, &noClip);
    for (int i = firstVertex; i < drawList->VtxBuffer.Size; i++) {
        ImDrawVert& vertex = drawList->VtxBuffer[i];
        const float dx = vertex.pos.x - bottomLeft.x;
        const float dy = vertex.pos.y - bottomLeft.y;
        vertex.pos = ImVec2(bottomLeft.x + dy, bottomLeft.y - dx);
    }
}

void ProjectSettingsWindow::drawInputSettings() {
    if (beginSettingsPanel("##InputSettingsPanel")) {
        if (beginSettingsTable("##SettingsTable")) {
            if (settingsui::beginRow("input.actions")) {
                if (projectBeginSettingsRow("Default Actions Asset", !m_inputActionsAsset.empty())) {
                    m_inputActionsAsset.clear();
                }
                const std::string current = m_inputActionsAsset.empty() ? std::string("None") : m_inputActionsAsset.generic_string();
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::BeginCombo("##InputActionsAsset", current.c_str())) {
                    if (ImGui::Selectable("None", m_inputActionsAsset.empty())) m_inputActionsAsset.clear();
                    for (const fs::path& file : m_inputActionsFiles) {
                        if (ImGui::Selectable(file.generic_string().c_str(), file == m_inputActionsAsset)) m_inputActionsAsset = file;
                    }
                    ImGui::EndCombo();
                }
                endSettingsRow("The Input Actions asset Play and the built game load at start (InputActions.loadDefault). "
                    "Window > Input Actions makes and edits one.");
            }

            if (settingsui::beginRow("input.deadzone")) {
                if (projectBeginSettingsRow("Default Deadzone", m_inputDeadzone != Project::defaultInputDeadzone)) {
                    m_inputDeadzone = Project::defaultInputDeadzone;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::SliderFloat("##InputDeadzone", &m_inputDeadzone, 0.0f, 0.95f, "%.2f");
                endSettingsRow("The radial deadzone of stick and axis bindings of AXIS and VECTOR2 actions that set none of their own.");
            }

            if (settingsui::beginRow("input.rebinding_overrides")) {
                projectBeginSettingsRow("Rebinding Overrides");
                ImGui::BeginDisabled(m_inputActionsAsset.empty());
                if (ImGui::Button("Reset rebinding overrides")) {
                    // the key the game keeps them under (InputActions::getOverridesKey), in the editor's settings
                    std::error_code ec;
                    const fs::path relative = fs::relative(m_project->getProjectPath() / m_inputActionsAsset, m_project->getAssetsPath(), ec);
                    const std::string path = relative.generic_string();
                    const Guid guid = AssetMap::findGuid(path);
                    const std::string key = "jasperengine.input." + (guid.isNull() ? path : guid.str());
                    UserSettings::removeKey(key.c_str());
                    if (InputActions::isLoaded() && InputActions::getAssetPath() == path)
                        InputActions::resetOverrides();
                    m_inputStatus = "The rebinding saved for " + m_inputActionsAsset.generic_string() + " in this editor is cleared.";
                }
                ImGui::EndDisabled();
                endSettingsRow("Players' rebinding (InputActions.saveOverrides) is kept per asset in the user settings; "
                    "this clears the editor's copy, which Play uses.");
            }
            ImGui::EndTable();
        }
        if (!m_inputStatus.empty()) drawSectionNote(m_inputStatus.c_str());
    }
    endSettingsPanel();
}

void ProjectSettingsWindow::drawNavigationSettings() {
    const nav::NavigationSettings defaults;
    nav::NavigationSettings& settings = m_navigation.settings;
    if (beginSettingsPanel("##NavigationSettingsPanel")) {
        if (beginSettingsTable("##SettingsTable")) {
            if (settingsui::beginRow("navigation.include_in_builds")) {
                if (projectBeginSettingsRow("Include in Builds", !m_navigation.includeInBuilds)) {
                    m_navigation.includeInBuilds = true;
                }
                ImGui::Checkbox("##NavIncludeInBuilds", &m_navigation.includeInBuilds);
                endSettingsRow("Recast/Detour in exported builds. Off strips the library: navmesh bakes, queries and agents "
                    "on navmeshes fail (logged once); NavGrids keep working.");
            }
            if (settingsui::beginRow("navigation.crowd_max_agents")) {
                if (projectBeginSettingsRow("Crowd Max Agents", settings.crowdMaxAgents != defaults.crowdMaxAgents)) {
                    settings.crowdMaxAgents = defaults.crowdMaxAgents;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::InputInt("##NavCrowdMaxAgents", &settings.crowdMaxAgents)) {
                    settings.crowdMaxAgents = std::clamp(settings.crowdMaxAgents, 1, 4096);
                }
                endSettingsRow("Agents of one crowd (one surface and agent type). A crowd grows past it, with a warning.");
            }
            if (settingsui::beginRow("navigation.max_grid_solves_per_tick")) {
                if (projectBeginSettingsRow("Grid Solves per Tick", settings.maxGridSolvesPerTick != defaults.maxGridSolvesPerTick)) {
                    settings.maxGridSolvesPerTick = defaults.maxGridSolvesPerTick;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::InputInt("##NavGridSolves", &settings.maxGridSolvesPerTick)) {
                    settings.maxGridSolvesPerTick = std::clamp(settings.maxGridSolvesPerTick, 1, 4096);
                }
                endSettingsRow("NavGrid path requests solved per fixed tick (in parallel); the rest wait for the next tick.");
            }
            if (settingsui::beginRow("navigation.web_bake_budget_ms")) {
                if (projectBeginSettingsRow("Web Bake Budget", settings.webBakeBudgetMs != defaults.webBakeBudgetMs)) {
                    settings.webBakeBudgetMs = defaults.webBakeBudgetMs;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::SliderFloat("##NavWebBakeBudget", &settings.webBakeBudgetMs, 0.5f, 50.0f, "%.1f ms");
                endSettingsRow("Without worker threads (single-threaded web), a runtime bake runs on the main thread this long per frame.");
            }
            ImGui::EndTable();
        }
        drawSectionNote("Agent types and areas: Window > AI > Navigation (Agents and Areas tabs).");
    }
    endSettingsPanel();
}

void ProjectSettingsWindow::drawLayerCollisionMatrix(const char* id, LayerCollisionRows& rows) {
    // named layers only, lowest first, as Unity lists them
    std::vector<int> layers;
    std::vector<std::string> names;
    for (int layer = 0; layer < MAX_LAYERS; layer++) {
        const std::string name = TagManager::isBuiltinLayer(layer)
            ? std::string(TagManager::getBuiltinLayerName(layer))
            : TagManager::trimName(m_layerNames[layer]);
        if (!name.empty()) {
            layers.push_back(layer);
            names.push_back(name);
        }
    }
    const int count = static_cast<int>(layers.size());

    ImGui::PushID(id);

    // every layer, named or not, like Unity's buttons
    if (ImGui::Button("Disable All")) {
        rows.fill(LAYER_MASK_NOTHING);
    }
    ImGui::SameLine();
    if (ImGui::Button("Enable All")) {
        rows.fill(LAYER_MASK_EVERYTHING);
    }
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    float labelWidth = 0.0f;
    for (const std::string& name : names) {
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(name.c_str()).x);
    }
    const float cell = ImGui::GetFrameHeight() + Theme::dpi(2.0f);
    const float headerHeight = labelWidth + style.ItemInnerSpacing.y * 2.0f;
    const float labelColumn = labelWidth + style.ItemSpacing.x * 2.0f;
    const ImVec2 contentSize(labelColumn + count * cell, headerHeight + count * cell);

    // scrolls sideways when 32 named layers do not fit the page
    const float childHeight = contentSize.y + style.ScrollbarSize + style.WindowPadding.y;
    if (ImGui::BeginChild("##Matrix", ImVec2(0, childHeight), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float gridX = origin.x + labelColumn;
        const float gridY = origin.y + headerHeight;
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 highlightColor = ImGui::GetColorU32(ImGuiCol_CheckMark);

        // Row and column of the box under the mouse, highlighted like Unity
        int hoveredRow = -1;
        int hoveredColumn = -1;
        if (ImGui::IsWindowHovered()) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int column = static_cast<int>(std::floor((mouse.x - gridX) / cell));
            const int row = static_cast<int>(std::floor((mouse.y - gridY) / cell));
            if (mouse.x >= gridX && mouse.y >= gridY && row < count && column >= 0 && column <= count - 1 - row) {
                hoveredRow = row;
                hoveredColumn = column;
            }
        }
        if (hoveredRow >= 0) {
            const ImU32 bandColor = ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.5f);
            drawList->AddRectFilled(ImVec2(origin.x, gridY + hoveredRow * cell),
                                    ImVec2(gridX + (count - hoveredRow) * cell, gridY + (hoveredRow + 1) * cell), bandColor);
            drawList->AddRectFilled(ImVec2(gridX + hoveredColumn * cell, origin.y),
                                    ImVec2(gridX + (hoveredColumn + 1) * cell, gridY + (count - hoveredColumn) * cell), bandColor);
        }

        // Columns run from the highest layer on the left down to the lowest
        for (int column = 0; column < count; column++) {
            const int index = count - 1 - column;
            const float x = gridX + column * cell + (cell - ImGui::GetFontSize()) * 0.5f;
            addVerticalText(drawList, ImVec2(x, gridY - style.ItemInnerSpacing.y), column == hoveredColumn ? highlightColor : textColor, names[index].c_str());
        }

        for (int row = 0; row < count; row++) {
            const float y = gridY + row * cell;
            const ImVec2 labelSize = ImGui::CalcTextSize(names[row].c_str());
            drawList->AddText(ImVec2(gridX - style.ItemSpacing.x - labelSize.x, y + (cell - labelSize.y) * 0.5f),
                              row == hoveredRow ? highlightColor : textColor, names[row].c_str());

            // row 'row' pairs its layer with itself and every higher layer
            for (int column = 0; column <= count - 1 - row; column++) {
                const int layerA = layers[row];
                const int layerB = layers[count - 1 - column];

                ImGui::SetCursorScreenPos(ImVec2(gridX + column * cell, y + (cell - ImGui::GetFrameHeight()) * 0.5f));
                ImGui::PushID(row * MAX_LAYERS + column);
                bool collide = ((rows[layerA] >> layerB) & 1u) != 0;
                if (ImGui::Checkbox("##Pair", &collide)) {
                    if (collide) {
                        rows[layerA] |= 1u << layerB;
                        rows[layerB] |= 1u << layerA;
                    } else {
                        rows[layerA] &= ~(1u << layerB);
                        rows[layerB] &= ~(1u << layerA);
                    }
                }
                ImGui::SetItemTooltip("%s / %s", names[row].c_str(), names[count - 1 - column].c_str());
                ImGui::PopID();
            }
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(contentSize);
    }
    ImGui::EndChild();

    ImGui::PopID();
}

void ProjectSettingsWindow::drawScriptingSettings() {
    projectDrawSettingsPanel("##ScriptingSettingsPanel", [this]() {
        if (settingsui::beginRow("scripting.lua_dir")) {
            drawDirectorySetting(
                m_project, "Lua Directory", nullptr, "##LuaPath", "Browse##lua",
                m_luaDir, fs::path(Project::defaultLuaDir), true
            );
        }
        if (settingsui::beginRow("scripting.script_dirs")) {
            drawScriptDirsSetting(m_project, m_scriptDirs);
        }

        if (settingsui::beginRow("scripting.cxx_standard")) {
            drawComboSetting("C++ Standard", "##CxxStandard", cxxStandardNames(), cxxStandardCount, m_cxxStandardIndex,
                findCxxStandardIndex(Project::defaultCxxStandard),
                "Language standard for C++ scripts in Play and exported games. Saved with the project. "
                "Exports also compile the engine with this standard. "
                "Choose a compatible compiler in Preferences > External Tools: GCC " JASPERENGINE_CXX_FLOOR_GNU
                ", Clang " JASPERENGINE_CXX_FLOOR_CLANG ", Apple Clang " JASPERENGINE_CXX_FLOOR_APPLECLANG
                " or MSVC " JASPERENGINE_CXX_FLOOR_MSVC
                " (Visual Studio 2022) or newer. C++23 exports to Xcode need Xcode 16 or newer.");
        }
    });
}

std::string ProjectSettingsWindow::newTagError() const {
    const std::string tag = TagManager::trimName(m_newTagBuffer);
    if (tag.empty()) return "Type a tag name";
    if (TagManager::isBuiltinTag(tag) || std::find(m_tags.begin(), m_tags.end(), tag) != m_tags.end()) {
        return "Tag \"" + tag + "\" is already defined";
    }
    return "";
}

int ProjectSettingsWindow::findLayerNameClash(int layer, const std::string& name) const {
    if (name.empty()) return -1;
    for (int other = 0; other < MAX_LAYERS; other++) {
        if (other == layer) continue;
        const std::string otherName = TagManager::isBuiltinLayer(other)
            ? std::string(TagManager::getBuiltinLayerName(other))
            : TagManager::trimName(m_layerNames[other]);
        if (otherName == name) return other;
    }
    return -1;
}

std::string ProjectSettingsWindow::tagsAndLayersError() const {
    for (int layer = 0; layer < MAX_LAYERS; layer++) {
        if (TagManager::isBuiltinLayer(layer)) continue;
        const int clash = findLayerNameClash(layer, TagManager::trimName(m_layerNames[layer]));
        if (clash >= 0) {
            return "User Layer " + std::to_string(layer) + " repeats the name of layer " + std::to_string(clash);
        }
    }
    return "";
}

void ProjectSettingsWindow::drawTagsAndLayersSettings() {
    if (beginSettingsPanel("##TagsAndLayersSettingsPanel")) {
        drawSectionNote("Saved with the project. A GameObject keeps a tag that is removed here, but scripts "
            "can no longer set or compare it. Scripts read layer names through LayerMask.");

        const ImVec4 errorColor(1.0f, 0.45f, 0.35f, 1.0f);
        const float buttonWidth = ImGui::GetFrameHeight();

        if (settingsFoldout("Tags", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##TagsTable")) {
            for (const std::string& tag : TagManager::getBuiltinTags()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Built-in");
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tag.c_str());
            }

            // Unity's list: user tags by index, - removes one, + adds the name typed below
            int removeIndex = -1;
            for (size_t i = 0; i < m_tags.size(); i++) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Tag %zu", i);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(m_tags[i].c_str());
                // right-aligned in the cell, like the + below
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - buttonWidth));
                if (ImGui::Button(ICON_FA_MINUS, ImVec2(buttonWidth, 0))) {
                    removeIndex = static_cast<int>(i);
                }
                ImGui::SetItemTooltip("Remove tag \"%s\"", m_tags[i].c_str());
                ImGui::PopID();
            }
            if (removeIndex >= 0) {
                m_tags.erase(m_tags.begin() + removeIndex);
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("New Tag Name");
            ImGui::TableNextColumn();
            const std::string addError = newTagError();
            ImGui::SetNextItemWidth(-(buttonWidth + ImGui::GetStyle().ItemSpacing.x));
            bool add = ImGui::InputTextWithHint("##NewTagName", "Tag name", m_newTagBuffer, sizeof(m_newTagBuffer),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
            if (add) {
                // keep typing tags one after another
                ImGui::SetKeyboardFocusHere(-1);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!addError.empty());
            add = ImGui::Button(ICON_FA_PLUS "##AddTag", ImVec2(buttonWidth, 0)) || add;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", addError.empty() ? "Add the tag" : addError.c_str());
            if (add && addError.empty()) {
                m_tags.push_back(TagManager::trimName(m_newTagBuffer));
                m_newTagBuffer[0] = '\0';
            }
            if (m_newTagBuffer[0] != '\0' && !addError.empty()) {
                ImGui::TextColored(errorColor, "%s", addError.c_str());
            }
            ImGui::EndTable();
        }

        if (settingsFoldout("Layers", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##LayersTable")) {
            for (int layer = 0; layer < MAX_LAYERS; layer++) {
                ImGui::PushID(layer);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                const bool builtin = TagManager::isBuiltinLayer(layer);
                ImGui::Text(builtin ? "Builtin Layer %d" : "User Layer %d", layer);
                ImGui::TableNextColumn();
                if (builtin) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", TagManager::getBuiltinLayerName(layer));
                } else {
                    char buffer[128];
                    snprintf(buffer, sizeof(buffer), "%s", m_layerNames[layer].c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText("##LayerName", buffer, sizeof(buffer))) {
                        m_layerNames[layer] = buffer;
                    }
                    const int clash = findLayerNameClash(layer, TagManager::trimName(m_layerNames[layer]));
                    if (clash >= 0) {
                        ImGui::TextColored(errorColor, "Layer %d already has this name", clash);
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    endSettingsPanel();
}

void ProjectSettingsWindow::drawEditorSettings() {
    projectDrawSettingsPanel("##EditorSettingsPanel", [this]() {
        if (settingsui::beginRow("editor.version_control_files")) {
            if (projectBeginSettingsRow("Version Control Files", m_versionControlMetadata != Project::defaultVersionControlMetadata)) {
                m_versionControlMetadata = Project::defaultVersionControlMetadata;
            }
            ImGui::Checkbox("##VersionControlMetadata", &m_versionControlMetadata);
            endSettingsRow("Keep .gitignore and .gitattributes in the project up to date, so the editor's "
                "working directory, this machine's build settings and each developer's own layout stay out "
                "of the repository. A file the editor did not generate is never replaced.");
        }
        // S29: the project's version control (project.yaml versionControl)
        const VcsProjectSettings vcsDefaults;
        if (settingsui::beginRow("editor.vcs.provider")) {
            if (projectBeginSettingsRow("Version Control", m_vcs.provider != vcsDefaults.provider)) m_vcs.provider = vcsDefaults.provider;
            int provider = static_cast<int>(m_vcs.provider);
            const char* providers[] = {"Auto", "Git", "Subversion", "None"};
            ImGui::SetNextItemWidth(Theme::dpi(160.0f));
            if (ImGui::Combo("##VcsProvider", &provider, providers, 4)) m_vcs.provider = static_cast<vcs::ProviderChoice>(provider);
            endSettingsRow("Auto finds the nearest .git or .svn above the project. None turns the editor's version control off.");
        }
        if (settingsui::beginRow("editor.vcs.lfs")) {
            if (projectBeginSettingsRow("Git LFS", m_vcs.lfs != vcsDefaults.lfs)) m_vcs.lfs = vcsDefaults.lfs;
            ImGui::Checkbox("##VcsLfs", &m_vcs.lfs);
            endSettingsRow("The generated .gitattributes stores images, models, audio and fonts in Git LFS and makes them "
                "lockable, so two people never edit the same binary. Scenes, prefabs and .meta files stay text.");
        }
        if (settingsui::beginRow("editor.vcs.lock_scenes")) {
            if (projectBeginSettingsRow("Lockable Scenes", m_vcs.lockScenes != vcsDefaults.lockScenes)) m_vcs.lockScenes = vcsDefaults.lockScenes;
            ImGui::Checkbox("##VcsLockScenes", &m_vcs.lockScenes);
            endSettingsRow("Scenes and prefabs lockable too. They are no LFS objects: whether a lock on them works depends on "
                "the server (Smart Merge usually makes locks on scenes unnecessary).");
        }


        if (settingsui::beginRow("editor.assets_dir")) {
            drawDirectorySetting(
                m_project, "Assets Directory", nullptr, "##AssetsPath", "Browse##assets",
                m_assetsDir, fs::path(Project::defaultAssetsDir)
            );
        }
    });
    drawAssetLabelsSettings();
}

// Asset Labels (S25 R23): each label's colour, rename and delete. These apply at once, each one
// undo step of the project history (they rewrite the assets' metas and in-file keys), not with OK.
void ProjectSettingsWindow::drawAssetLabelsSettings() {
    if (!settingsFoldout("Asset Labels", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!settingsui::beginRow("editor.asset_labels")) return;
    drawSectionNote("Colours, names and removal apply at once and are undoable (Edit > Undo). Label assets with "
                    "the Project window's Labels menu; find them with l:Name in its search.");
    const std::vector<AssetLabels::LabelInfo> labels = m_project->getAssetLabels().labels();
    if (labels.empty()) {
        ImGui::TextDisabled("No labels yet.");
        return;
    }
    if (!beginSettingsTable("##AssetLabelsTable")) return;
    for (const AssetLabels::LabelInfo& label : labels) {
        ImGui::PushID(label.name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        Vector3 color(0.5f, 0.5f, 0.5f);
        if (AssetLabels::isColor(label.color)) {
            const unsigned int value = static_cast<unsigned int>(std::strtoul(label.color.c_str() + 1, nullptr, 16));
            color = Vector3(((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f);
        }
        const auto preview = m_labelColorPreview.find(label.name);
        if (preview != m_labelColorPreview.end()) color = preview->second;
        ui::ColorFieldOptions colorOptions;
        colorOptions.alpha = false;
        colorOptions.linearStorage = false;
        colorOptions.label = "Label Color";
        colorOptions.width = ImGui::GetFrameHeight() * 1.5f;
        const ui::EditPhase colorPhase = ui::colorField("##LabelColor", color, colorOptions);
        if (colorPhase == ui::EditPhase::Changing) {
            m_labelColorPreview[label.name] = color;
        } else if (colorPhase == ui::EditPhase::Cancelled) {
            m_labelColorPreview.erase(label.name);
        } else if (colorPhase == ui::EditPhase::Committed && preview != m_labelColorPreview.end()) {
            // the label file is written once, with the colour the window closed on
            const std::string hex = "#" + colormath::toHex(Vector4(preview->second, 1.0f), false);
            m_labelColorPreview.erase(preview);
            const LabelOperations::Result result = LabelOperations::setColor(m_project, label.name, hex);
            if (!result.ok) Out::warning("Asset Labels: %s", result.error.c_str());
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  (%zu)", label.name.c_str(), label.assets);
        ImGui::SetItemTooltip("%zu asset(s) have this label", label.assets);
        ImGui::TableNextColumn();
        const bool editing = m_renamingLabel == label.name;
        if (editing) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            const bool enter = ImGui::InputText("##LabelName", m_labelNameBuffer, sizeof(m_labelNameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Rename") || enter) {
                const LabelOperations::Result result = LabelOperations::rename(m_project, label.name, m_labelNameBuffer);
                if (!result.ok) Out::warning("Asset Labels: %s", result.error.c_str());
                m_renamingLabel.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) m_renamingLabel.clear();
        } else {
            if (ImGui::Button("Rename...")) {
                m_renamingLabel = label.name;
                std::snprintf(m_labelNameBuffer, sizeof(m_labelNameBuffer), "%s", label.name.c_str());
            }
            ImGui::SetItemTooltip("Rename it on every asset");
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                const LabelOperations::Result result = LabelOperations::remove(m_project, label.name);
                if (!result.ok) Out::warning("Asset Labels: %s", result.error.c_str());
            }
            ImGui::SetItemTooltip("Take it off every asset (undoable)");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void ProjectSettingsWindow::drawPlatformsSettings() {
    // Unity's per-platform Player tabs: each adds what its platform needs on top
    // of the shared product settings above.
    // while searching, only the platforms with a matching row
    static const char* const platforms[] = {"Windows", "macOS", "Linux", "iOS", "Android", "Web"};
    if (std::none_of(std::begin(platforms), std::end(platforms), [](const char* name) { return settingsui::sectionVisible(name); })) return;
    settingsui::boldText("Settings for Platform");
    ImGui::Spacing();
    if (!ImGui::BeginTabBar("##PlayerPlatformTabs", ImGuiTabBarFlags_FittingPolicyShrink)) return;

    if (settingsui::sectionVisible("Windows") && ImGui::BeginTabItem(ICON_FA_WINDOW_MAXIMIZE " Windows")) {
        drawSectionNote("The Default Icon above is embedded into Windows players.");
        if (beginSettingsTable("##WindowsSettingsTable")) {
            drawWindowsSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (settingsui::sectionVisible("macOS") && ImGui::BeginTabItem(ICON_FA_LAPTOP " macOS")) {
        drawSectionNote("Bundle settings apply to Apple projects exported as source code. "
            "An Executable Player is a standalone executable, not an .app bundle.");
        if (beginSettingsTable("##MacOSSettingsTable")) {
            drawMacOSSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (settingsui::sectionVisible("Linux") && ImGui::BeginTabItem(ICON_FA_TERMINAL " Linux")) {
        if (beginSettingsTable("##LinuxSettingsTable")) {
            drawLinuxSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (settingsui::sectionVisible("iOS") && ImGui::BeginTabItem(ICON_FA_MOBILE_BUTTON " iOS")) {
        if (beginSettingsTable("##IOSSettingsTable")) {
            drawIOSSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (settingsui::sectionVisible("Android") && ImGui::BeginTabItem(ICON_FA_MOBILE_SCREEN_BUTTON " Android")) {
        if (beginSettingsTable("##AndroidSettingsTable")) {
            drawAndroidSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (settingsui::sectionVisible("Web") && ImGui::BeginTabItem(ICON_FA_GLOBE " Web")) {
        if (beginSettingsTable("##WebSettingsTable")) {
            drawWebSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

void ProjectSettingsWindow::drawWebSettings() {
    if (settingsui::beginRow("web.application_name")) {
        drawOverrideSetting("App Name", "##WebAppName", m_webApplicationNameBuffer, inheritedApplicationName(),
            "Title used by the exported web page. Empty means the shared application name.");
    }

    if (settingsui::beginRow("web.favicon")) {
        drawProjectFilePathSetting(m_project, "Favicon", "Browser tab icon for web export.", "##WebFaviconPath",
            "Browse##webfavicon", "Clear##webfavicon", m_webFavicon);
    }
    if (settingsui::beginRow("web.custom_html_shell")) {
        drawProjectFilePathSetting(m_project, "Custom HTML Shell", "Optional web HTML wrapper. The file must contain {{JASPERENGINE_DEFAULT_HTML}} marker.", "##WebHtmlShellPath",
            "Browse##webhtmlshell", "Clear##webhtmlshell", m_webCustomHtmlShell, FILE_DIALOG_ALL);
    }

    if (settingsui::beginRow("web.head_include")) {
        projectBeginSettingsRow("Head Include");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextMultiline("##WebHeadInclude", m_webHeadIncludeBuffer, sizeof(m_webHeadIncludeBuffer), ImVec2(-1, Theme::dpi(80.0f)));
        endSettingsRow("HTML inserted into the <head> section by web export when supported.");
    }

    if (settingsui::beginRow("web.resize_canvas_to_window")) {
        if (projectBeginSettingsRow("Resize Canvas To Window", !m_webResizeCanvasToWindow)) {
            m_webResizeCanvasToWindow = true;
        }
        ImGui::Checkbox("##WebResizeCanvasToWindow", &m_webResizeCanvasToWindow);
        endSettingsRow("Resize the rendering surface to the browser window. Canvas Scaling Mode controls aspect ratio and stretching.");
    }

    if (settingsui::beginRow("web.hide_emscripten_ui")) {
        if (projectBeginSettingsRow("Hide Emscripten UI", m_webHideEmscriptenUI)) {
            m_webHideEmscriptenUI = false;
        }
        ImGui::Checkbox("##WebHideEmscriptenUI", &m_webHideEmscriptenUI);
        endSettingsRow("Hide the standard logo, status, controls and output console. Runtime scripts remain active. Custom HTML elements are not removed.");
    }
}

void ProjectSettingsWindow::drawLinuxSettings() {
    if (settingsui::beginRow("linux.application_name")) {
        drawOverrideSetting("App Name", "##LinuxAppName", m_linuxApplicationNameBuffer, inheritedApplicationName(),
            "Linux desktop launcher name. Empty means the shared application name.");
    }

    if (settingsui::beginRow("linux.comment")) {
        projectBeginSettingsRow("Comment");
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::InputText("##LinuxComment", m_linuxCommentBuffer, sizeof(m_linuxCommentBuffer));
        endSettingsRow("Short description written to the .desktop launcher file.");
    }

    if (settingsui::beginRow("linux.categories")) {
        if (projectBeginSettingsRow("Categories", strcmp(m_linuxCategoriesBuffer, LinuxProjectSettings{}.categories.c_str()) != 0)) {
            snprintf(m_linuxCategoriesBuffer, sizeof(m_linuxCategoriesBuffer), "%s", LinuxProjectSettings{}.categories.c_str());
        }
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::InputText("##LinuxCategories", m_linuxCategoriesBuffer, sizeof(m_linuxCategoriesBuffer));
        endSettingsRow("Desktop Entry categories. Example: Game;ArcadeGame;");
    }
}

void ProjectSettingsWindow::drawWindowsSettings() {
    if (settingsui::beginRow("windows.product_name")) {
        drawOverrideSetting("Product Name", "##WindowsProductName", m_windowsProductNameBuffer, inheritedApplicationName(),
            "Windows executable metadata. Empty means the shared application name.");
    }

    if (settingsui::beginRow("windows.company_name")) {
        projectBeginSettingsRow("Company Name");
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::InputText("##WindowsCompanyName", m_windowsCompanyNameBuffer, sizeof(m_windowsCompanyNameBuffer));
        endSettingsRow("Windows executable metadata company name.");
    }

    const std::string inheritedWindowsVersion = Project::toFourPartVersion(inheritedApplicationVersion());

    if (settingsui::beginRow("windows.file_version")) {
        drawOverrideSetting("File Version", "##WindowsFileVersion", m_windowsFileVersionBuffer, inheritedWindowsVersion,
            "Windows numeric file version. Empty means the shared version, padded to four parts.");
        drawOverrideWarning(m_windowsFileVersionBuffer, isWindowsVersionValid,
            "Use up to four numbers from 0 to 65535, for example 1.0.0.0.");
        drawWindowsVersionResult(m_windowsFileVersionBuffer);
    }

    if (settingsui::beginRow("windows.product_version")) {
        drawOverrideSetting("Product Version", "##WindowsProductVersion", m_windowsProductVersionBuffer, inheritedWindowsVersion,
            "Windows product version. Empty means the shared version, padded to four parts.");
        drawOverrideWarning(m_windowsProductVersionBuffer, isWindowsVersionValid,
            "Use up to four numbers from 0 to 65535, for example 1.0.0.0.");
        drawWindowsVersionResult(m_windowsProductVersionBuffer);
    }

}

void ProjectSettingsWindow::drawMacOSSettings() {
    if (settingsui::beginRow("macos.application_name")) {
        drawOverrideSetting("App Name", "##MacOSAppName", m_macOSApplicationNameBuffer, inheritedApplicationName(),
            "macOS bundle display name. Empty means the shared application name.");
    }

    if (settingsui::beginRow("macos.bundle_identifier")) {
        drawOverrideSetting("Bundle Identifier", "##MacOSBundleIdentifier", m_macOSBundleIdentifierBuffer, inheritedApplicationIdentifier(),
            "CFBundleIdentifier. Empty means the shared identifier.");
        drawOverrideWarning(m_macOSBundleIdentifierBuffer, isAppleBundleIdentifierValid,
            "Use letters, digits and hyphens, for example com.company.my-game.");
    }

    if (settingsui::beginRow("macos.version_name")) {
        drawOverrideSetting("Version Name", "##MacOSVersionName", m_macOSVersionNameBuffer, inheritedApplicationVersion(),
            "CFBundleShortVersionString. Empty means the shared version.");
        drawOverrideWarning(m_macOSVersionNameBuffer, isAppleVersionValid,
            "Apple accepts at most three numbers, for example 1.0.2.");
    }
    if (settingsui::beginRow("macos.build_number")) {
        drawOverrideSetting("Build Number", "##MacOSBuildNumber", m_macOSBuildNumberBuffer, inheritedApplicationBuild(),
            "CFBundleVersion. Empty means the shared build.");
        drawOverrideWarning(m_macOSBuildNumberBuffer, isAppleBuildValid,
            "Apple accepts at most three numbers, for example 42.");
    }

    if (settingsui::beginRow("macos.icon")) {
        drawProjectFilePathSetting(m_project, "Icon", "macOS application icon source image. Export support depends on the generated Xcode project.", "##MacOSIconPath",
            "Browse##macosicon", "Clear##macosicon", m_macOSIcon);
    }

    if (settingsui::beginRow("macos.high_dpi")) {
        if (projectBeginSettingsRow("High DPI", !m_macOSHighDpi)) {
            m_macOSHighDpi = true;
        }
        ImGui::Checkbox("##MacOSHighDpi", &m_macOSHighDpi);
        endSettingsRow("Allow high-DPI rendering on macOS.");
    }
}

void ProjectSettingsWindow::drawIOSSettings() {
    if (settingsui::beginRow("ios.application_name")) {
        drawOverrideSetting("App Name", "##IOSAppName", m_iosApplicationNameBuffer, inheritedApplicationName(),
            "iOS bundle display name. Empty means the shared application name.");
    }

    if (settingsui::beginRow("ios.bundle_identifier")) {
        drawOverrideSetting("Bundle Identifier", "##IOSBundleIdentifier", m_iosBundleIdentifierBuffer, inheritedApplicationIdentifier(),
            "CFBundleIdentifier. Empty means the shared identifier.");
        drawOverrideWarning(m_iosBundleIdentifierBuffer, isAppleBundleIdentifierValid,
            "Use letters, digits and hyphens, for example com.company.my-game.");
    }

    if (settingsui::beginRow("ios.version_name")) {
        drawOverrideSetting("Version Name", "##IOSVersionName", m_iosVersionNameBuffer, inheritedApplicationVersion(),
            "CFBundleShortVersionString. Empty means the shared version.");
        drawOverrideWarning(m_iosVersionNameBuffer, isAppleVersionValid,
            "Apple accepts at most three numbers, for example 1.0.2.");
    }
    if (settingsui::beginRow("ios.build_number")) {
        drawOverrideSetting("Build Number", "##IOSBuildNumber", m_iosBuildNumberBuffer, inheritedApplicationBuild(),
            "CFBundleVersion. Empty means the shared build.");
        drawOverrideWarning(m_iosBuildNumberBuffer, isAppleBuildValid,
            "Apple accepts at most three numbers, for example 42.");
    }

    if (settingsui::beginRow("ios.icon")) {
        drawProjectFilePathSetting(m_project, "Icon", "iOS application icon source image. Export support depends on the generated Xcode project.", "##IOSIconPath",
            "Browse##iosicon", "Clear##iosicon", m_iosIcon);
    }

    if (settingsui::beginRow("ios.hide_status_bar")) {
        if (projectBeginSettingsRow("Hide Status Bar", !m_iosHideStatusBar)) {
            m_iosHideStatusBar = true;
        }
        ImGui::Checkbox("##IOSHideStatusBar", &m_iosHideStatusBar);
        endSettingsRow("Hide the iOS status bar while the app runs.");
    }

    if (settingsui::beginRow("ios.hide_home_indicator")) {
        if (projectBeginSettingsRow("Hide Home Indicator", !m_iosHideHomeIndicator)) {
            m_iosHideHomeIndicator = true;
        }
        ImGui::Checkbox("##IOSHideHomeIndicator", &m_iosHideHomeIndicator);
        endSettingsRow("Request hiding the iOS home indicator.");
    }

    if (settingsui::beginRow("ios.supports_high_refresh_rate")) {
        if (projectBeginSettingsRow("High Refresh Rate", !m_iosSupportsHighRefreshRate)) {
            m_iosSupportsHighRefreshRate = true;
        }
        ImGui::Checkbox("##IOSHighRefreshRate", &m_iosSupportsHighRefreshRate);
        endSettingsRow("Allow 120 Hz displays when supported.");
    }

    if (settingsui::beginRow("ios.admob_enabled")) {
        if (projectBeginSettingsRow("Google AdMob", m_iosAdmobEnabled)) {
            m_iosAdmobEnabled = false;
        }
        ImGui::Checkbox("##IOSAdMob", &m_iosAdmobEnabled);
        endSettingsRow("Links Google Mobile Ads for System.initializeAdMob and the interstitial ad calls. Without it the export leaves the SDK out and those calls do nothing.");
    }

    if (settingsui::beginRow("ios.admob_app_id")) {
        ImGui::BeginDisabled(!m_iosAdmobEnabled);
        projectBeginSettingsRow("AdMob App ID");
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::InputTextWithHint("##IOSAdMobAppId", "ca-app-pub-0000000000000000~0000000000", m_iosAdmobAppIdBuffer, sizeof(m_iosAdmobAppIdBuffer));
        endSettingsRow((std::string("GADApplicationIdentifier from the AdMob console, required with AdMob. Google's sample app ") +
            iosSampleAdMobAppId + " only serves test ads.").c_str());
        drawOverrideWarning(m_iosAdmobAppIdBuffer, isAdMobAppIdValid,
            "Use the app ID (with ~), not an ad unit ID (with /).");
        if (m_iosAdmobEnabled && m_iosAdmobAppIdBuffer[0] == '\0') {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Exports need the AdMob App ID.");
        }
        ImGui::EndDisabled();
    }
}

void ProjectSettingsWindow::drawAndroidSettings() {
    AndroidProjectSettings defaults;

    if (settingsui::beginRow("android.application_name")) {
        drawOverrideSetting("App Name", "##AndroidAppName", m_androidApplicationNameBuffer, inheritedApplicationName(),
            "Android launcher label. Empty means the shared application name.");
    }

    if (settingsui::beginRow("android.package_name")) {
        drawOverrideSetting("Package Name", "##AndroidPackageName", m_androidPackageNameBuffer, inheritedApplicationIdentifier(),
            "Android applicationId. Empty means the shared identifier.");
        drawOverrideWarning(m_androidPackageNameBuffer, isJavaPackageNameValid,
            "Use a Java package name, for example com.company.game.");
    }

    if (settingsui::beginRow("android.version_code")) {
        drawOverrideSetting("Version Code", "##AndroidVersionCode", m_androidVersionCodeBuffer, inheritedApplicationBuild(),
            "Integer version used by Android and stores. Empty means the shared build.");
        drawOverrideWarning(m_androidVersionCodeBuffer, isAndroidVersionCodeValid,
            "Use a whole number from 1 to 2100000000.");
    }

    if (settingsui::beginRow("android.version_name")) {
        drawOverrideSetting("Version Name", "##AndroidVersionName", m_androidVersionNameBuffer, inheritedApplicationVersion(),
            "Visible version string. Empty means the shared version.");
    }

    if (settingsui::beginRow({"android.launcher_icon", "android.adaptive_icon_foreground", "android.adaptive_icon_background"})) {
        projectBeginSettingsRow("Launcher Icons");
        ImGui::TextWrapped("PNG recommended. Adaptive icon requires foreground and background images.");
        endSettingsRow("Android launcher icon resources. If adaptive foreground and background are set, the export creates an adaptive icon. Otherwise Launcher Icon is used.");
    }

    if (settingsui::beginRow("android.launcher_icon")) {
        drawProjectFilePathSetting(m_project, "Launcher Icon", "Fallback launcher icon. Square PNG recommended.", "##AndroidLauncherIconPath",
            "Browse##androidlaunchericon", "Clear##androidlaunchericon", m_androidLauncherIcon);
    }
    if (settingsui::beginRow("android.adaptive_icon_foreground")) {
        drawProjectFilePathSetting(m_project, "Adaptive Foreground", "Adaptive icon foreground image.", "##AndroidAdaptiveIconForegroundPath",
            "Browse##androidadaptiveforeground", "Clear##androidadaptiveforeground", m_androidAdaptiveIconForeground);
    }
    if (settingsui::beginRow("android.adaptive_icon_background")) {
        drawProjectFilePathSetting(m_project, "Adaptive Background", "Adaptive icon background image.", "##AndroidAdaptiveIconBackgroundPath",
            "Browse##androidadaptivebackground", "Clear##androidadaptivebackground", m_androidAdaptiveIconBackground);
    }

    if (settingsui::beginRow("android.min_sdk")) {
        drawIntSetting("Min SDK", "##AndroidMinSdk", m_androidMinSdk, static_cast<int>(defaults.minSdk), 1,
            "Lowest Android API level the exported project supports.");
    }
    if (settingsui::beginRow("android.target_sdk")) {
        drawIntSetting("Target SDK", "##AndroidTargetSdk", m_androidTargetSdk, static_cast<int>(defaults.targetSdk), 1,
            "Android API level the app declares as its target.");
        m_androidTargetSdk = std::max(m_androidTargetSdk, m_androidMinSdk);
    }

    if (settingsui::beginRow("android.orientation")) {
        drawComboSetting("Orientation", "##AndroidOrientation", androidOrientationNames, androidOrientationCount,
            m_androidOrientationIndex, findAndroidOrientationIndex(defaults.orientation),
            "Screen orientation requested by the Android activity.");
    }

    if (settingsui::beginRow("android.abis")) {
        projectBeginSettingsRow("Architectures");
        ImGui::Checkbox("armeabi-v7a", &m_androidAbiArmeabiV7a);
        ImGui::Checkbox("arm64-v8a", &m_androidAbiArm64V8a);
        ImGui::Checkbox("x86", &m_androidAbiX86);
        ImGui::Checkbox("x86_64", &m_androidAbiX86_64);
        endSettingsRow("Native CPU architectures included in the APK.");
        if (!hasAndroidAbiSelection(m_androidAbiArmeabiV7a, m_androidAbiArm64V8a, m_androidAbiX86, m_androidAbiX86_64)) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Select at least one architecture.");
        }
    }

    if (settingsui::beginRow("android.permissions")) {
        projectBeginSettingsRow("Permissions");
        if (ImGui::SmallButton("None##AndroidPermissionsNone")) {
            m_androidPermissions.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Game Defaults##AndroidPermissionsGameDefaults")) {
            m_androidPermissions.clear();
            m_androidPermissions.insert("internet");
            m_androidPermissions.insert("access_network_state");
            m_androidPermissions.insert("vibrate");
        }

        ImGui::BeginChild("##AndroidPermissionsList", ImVec2(0, Theme::dpi(170.0f)), ImGuiChildFlags_Borders);
        for (int i = 0; i < androidPermissionCount; i++) {
            const AndroidPermissionInfo& permission = androidPermissionInfos[i];
            bool enabled = m_androidPermissions.count(permission.key) > 0;
            std::string label = std::string(permission.manifestName) + "##AndroidPermission_" + permission.key;
            if (ImGui::Checkbox(label.c_str(), &enabled)) {
                if (enabled) {
                    m_androidPermissions.insert(permission.key);
                } else {
                    m_androidPermissions.erase(permission.key);
                }
            }
        }
        ImGui::EndChild();
        endSettingsRow("Android permissions written to AndroidManifest.xml. Some permissions still require runtime approval in your Android code.");
    }

    if (settingsui::beginRow({"android.allow_backup", "android.keep_screen_on"})) {
        if (projectBeginSettingsRow("Application Options", m_androidAllowBackup != defaults.allowBackup || m_androidKeepScreenOn != defaults.keepScreenOn)) {
            m_androidAllowBackup = defaults.allowBackup;
            m_androidKeepScreenOn = defaults.keepScreenOn;
        }

        ImGui::Checkbox("Allow Backup", &m_androidAllowBackup);
        ImGui::SetItemTooltip("Maps to android:allowBackup.");

        ImGui::Checkbox("Keep Screen On", &m_androidKeepScreenOn);
        ImGui::SetItemTooltip("Adds FLAG_KEEP_SCREEN_ON to the Android activity.");
        endSettingsRow("Android application/activity behavior.");
    }

    if (settingsui::beginRow("android.fullscreen")) {
        if (projectBeginSettingsRow("Screen Options", m_androidFullscreen != defaults.fullscreen)) {
            m_androidFullscreen = defaults.fullscreen;
        }

        ImGui::Checkbox("Fullscreen", &m_androidFullscreen);
        ImGui::SetItemTooltip("Uses the fullscreen Android theme and hides system bars in MainActivity.");
        endSettingsRow("Android screen/window behavior.");
    }

    if (settingsui::beginRow("android.admob_enabled")) {
        if (projectBeginSettingsRow("Google AdMob", m_androidAdmobEnabled != defaults.admobEnabled)) {
            m_androidAdmobEnabled = defaults.admobEnabled;
        }
        ImGui::Checkbox("##AndroidAdMob", &m_androidAdmobEnabled);
        endSettingsRow("Compiles in Google Mobile Ads for System.initializeAdMob and the interstitial ad calls. Without it the export leaves the SDK out and those calls do nothing. Declare ads and Ad ID use in the Play Console.");
    }

    if (settingsui::beginRow("android.admob_app_id")) {
        ImGui::BeginDisabled(!m_androidAdmobEnabled);
        projectBeginSettingsRow("AdMob App ID");
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::InputTextWithHint("##AndroidAdMobAppId", "ca-app-pub-0000000000000000~0000000000", m_androidAdmobAppIdBuffer, sizeof(m_androidAdmobAppIdBuffer));
        endSettingsRow((std::string("APPLICATION_ID from the AdMob console, required with AdMob. Google's sample app ") +
            androidSampleAdMobAppId + " only serves test ads.").c_str());
        drawOverrideWarning(m_androidAdmobAppIdBuffer, isAdMobAppIdValid,
            "Use the app ID (with ~), not an ad unit ID (with /).");
        if (m_androidAdmobEnabled && m_androidAdmobAppIdBuffer[0] == '\0') {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Exports need the AdMob App ID.");
        }
        ImGui::EndDisabled();
    }
}

bool ProjectSettingsWindow::applySettings() {
    if (!hasAndroidAbiSelection(m_androidAbiArmeabiV7a, m_androidAbiArm64V8a, m_androidAbiX86, m_androidAbiX86_64)) {
        return false;
    }
    if (!tagsAndLayersError().empty()) {
        return false;
    }
    // The edit buffer truncates long names. Only write back when the value
    // changed, since setName also rebuilds libName and the window title.
    std::string projectName = m_projectNameBuffer;
    if (!projectName.empty() && projectName != m_projectNameOriginal.substr(0, sizeof(m_projectNameBuffer) - 1)) {
        m_project->setName(projectName);
    }

    m_project->setCanvasSize(m_canvasWidth, m_canvasHeight);
    m_project->setScalingMode(scalingModeValues[m_scalingModeIndex]);
    m_project->setTextureStrategy(textureStrategyValues[m_textureStrategyIndex]);
    m_project->setVSyncEnabled(m_vsyncEnabled);
    m_project->setAtmosphereQuality({(AtmosphereQuality)m_atmosphereQuality[0], (AtmosphereQuality)m_atmosphereQuality[1],
                                     (AtmosphereQuality)m_atmosphereQuality[2]});
    m_project->setTextureSettings(m_textureSettings);
    m_project->setPlayerLogging(m_playerLogging);
    m_project->setPlayerCrash(m_playerCrash);
    m_project->setHtmlUiMsaaSamples(htmlUiMsaaValues[m_htmlUiMsaaIndex]);
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++) {
        m_project->setShadowTier(platform, static_cast<ShadowTier>(m_shadowTiers[platform]));
    }
    m_project->setWindowMode(windowModeValues[m_windowModeIndex]);
    m_project->setWindowSize(m_windowWidth, m_windowHeight);
    m_project->setWindowResizable(m_windowResizable);

    // The edit buffer truncates long titles. Preserve an untouched title that
    // is longer than the buffer instead of writing back the truncated value.
    if (m_windowTitleBuffer != m_windowTitleOriginal.substr(0, sizeof(m_windowTitleBuffer) - 1)) {
        m_project->setWindowTitle(m_windowTitleBuffer);
    }
    m_project->setWindowIcon(m_windowIcon);

    ApplicationSettings& application = m_project->getApplicationSettings();
    applyTextBuffer(application.name, m_applicationNameBuffer);
    applyTextBuffer(application.identifier, m_applicationIdentifierBuffer);
    if (!isSharedIdentifierValid(application.identifier)) {
        application.identifier = ApplicationSettings{}.identifier;
    }
    applyTextBuffer(application.version, m_applicationVersionBuffer);
    if (!isAppleVersionValid(application.version)) {
        application.version = Project::toAppleVersion(application.version);  // trim, do not discard
    }
    application.build = static_cast<unsigned int>(std::max(1, m_applicationBuild));

    // Moves the referenced files in and rewrites every reference to the new roots
    m_project->changeAssetRoots(m_assetsDir, m_luaDir);
    m_project->setScriptDirs(m_scriptDirs);
    if (m_cxxStandardIndex < 0 || m_cxxStandardIndex >= cxxStandardCount) {
        m_cxxStandardIndex = 0;
    }
    m_project->setCxxStandard(cxxStandards[m_cxxStandardIndex]);
    m_project->setPhysics2DEnabled(m_physics2DEnabled);
    m_project->setPhysics3DEnabled(m_physics3DEnabled);
    m_project->setInputActionsAsset(m_inputActionsAsset);
    m_project->setInputDeadzone(m_inputDeadzone);
    m_project->setRuntimeShadersEnabled(m_runtimeShadersEnabled);
    m_project->setMobilePbrTier(mobilePbrTierValues[m_mobilePbrTierIndex]);

    // The project already carries the values applied above
    const std::string inheritedName = m_project->getApplicationName();
    const std::string inheritedIdentifier = m_project->getApplicationIdentifier();
    const std::string inheritedVersion = m_project->getApplicationVersion();
    const std::string inheritedBuild = m_project->getApplicationBuild();

    WebProjectSettings& web = m_project->getWebProjectSettings();
    applyOverride(web.applicationName, m_webApplicationNameBuffer, inheritedName);
    web.favicon = m_webFavicon;
    web.customHtmlShell = m_webCustomHtmlShell;
    applyTextBuffer(web.headInclude, m_webHeadIncludeBuffer);
    web.resizeCanvasToWindow = m_webResizeCanvasToWindow;
    web.hideEmscriptenUI = m_webHideEmscriptenUI;

    LinuxProjectSettings& linuxSettings = m_project->getLinuxProjectSettings();
    applyOverride(linuxSettings.applicationName, m_linuxApplicationNameBuffer, inheritedName);
    applyTextBuffer(linuxSettings.comment, m_linuxCommentBuffer);
    applyTextBuffer(linuxSettings.categories, m_linuxCategoriesBuffer);
    if (linuxSettings.categories.empty()) {
        linuxSettings.categories = LinuxProjectSettings{}.categories;
    }

    WindowsProjectSettings& windows = m_project->getWindowsProjectSettings();
    applyOverride(windows.productName, m_windowsProductNameBuffer, inheritedName);
    applyTextBuffer(windows.companyName, m_windowsCompanyNameBuffer);
    // Compared through the padding export applies, so "2.5" and "2.5.0.0" both read as shared
    const std::string inheritedWindowsVersion = m_project->getApplicationFileVersion();
    auto applyWindowsVersion = [&inheritedWindowsVersion](std::string& value, const auto& buffer) {
        const bool wasInheriting = value.empty();
        applyTextBuffer(value, buffer);
        if (value.empty()) return;
        if (!isWindowsVersionValid(value)
                || (wasInheriting && Project::toFourPartVersion(value) == inheritedWindowsVersion)) {
            value.clear();
        }
    };

    applyWindowsVersion(windows.fileVersion, m_windowsFileVersionBuffer);
    applyWindowsVersion(windows.productVersion, m_windowsProductVersionBuffer);

    MacOSProjectSettings& macOS = m_project->getMacOSProjectSettings();
    applyOverride(macOS.applicationName, m_macOSApplicationNameBuffer, inheritedName);
    applyOverride(macOS.bundleIdentifier, m_macOSBundleIdentifierBuffer, inheritedIdentifier, isAppleBundleIdentifierValid);
    applyAppleVersion(macOS.versionName, m_macOSVersionNameBuffer, inheritedVersion, isAppleVersionValid);
    applyAppleVersion(macOS.buildNumber, m_macOSBuildNumberBuffer, inheritedBuild, isAppleBuildValid);
    macOS.icon = m_macOSIcon;
    macOS.highDpi = m_macOSHighDpi;

    IOSProjectSettings& ios = m_project->getIOSProjectSettings();
    applyOverride(ios.applicationName, m_iosApplicationNameBuffer, inheritedName);
    applyOverride(ios.bundleIdentifier, m_iosBundleIdentifierBuffer, inheritedIdentifier, isAppleBundleIdentifierValid);
    applyAppleVersion(ios.versionName, m_iosVersionNameBuffer, inheritedVersion, isAppleVersionValid);
    applyAppleVersion(ios.buildNumber, m_iosBuildNumberBuffer, inheritedBuild, isAppleBuildValid);
    ios.icon = m_iosIcon;
    ios.hideStatusBar = m_iosHideStatusBar;
    ios.hideHomeIndicator = m_iosHideHomeIndicator;
    ios.supportsHighRefreshRate = m_iosSupportsHighRefreshRate;
    ios.admobEnabled = m_iosAdmobEnabled;
    applyTextBuffer(ios.admobAppId, m_iosAdmobAppIdBuffer);

    AndroidProjectSettings& android = m_project->getAndroidProjectSettings();
    applyOverride(android.applicationName, m_androidApplicationNameBuffer, inheritedName);
    applyOverride(android.packageName, m_androidPackageNameBuffer, inheritedIdentifier, isJavaPackageNameValid);
    // Anything but a positive integer goes back to inheriting the shared build.
    const bool versionCodeWasInheriting = android.versionCode == 0;
    android.versionCode = parseAndroidVersionCode(m_androidVersionCodeBuffer);
    if (versionCodeWasInheriting && android.versionCode == m_project->getApplicationVersionCode()) {
        android.versionCode = 0;
    }
    applyOverride(android.versionName, m_androidVersionNameBuffer, inheritedVersion);
    android.launcherIcon = m_androidLauncherIcon;
    android.adaptiveIconForeground = m_androidAdaptiveIconForeground;
    android.adaptiveIconBackground = m_androidAdaptiveIconBackground;
    android.minSdk = static_cast<unsigned int>(std::max(1, m_androidMinSdk));
    android.targetSdk = static_cast<unsigned int>(std::max(m_androidMinSdk, m_androidTargetSdk));
    android.orientation = androidOrientationValues[std::clamp(m_androidOrientationIndex, 0, androidOrientationCount - 1)];
    android.abiArmeabiV7a = m_androidAbiArmeabiV7a;
    android.abiArm64V8a = m_androidAbiArm64V8a;
    android.abiX86 = m_androidAbiX86;
    android.abiX86_64 = m_androidAbiX86_64;
    android.permissions = m_androidPermissions;
    android.allowBackup = m_androidAllowBackup;
    android.fullscreen = m_androidFullscreen;
    android.keepScreenOn = m_androidKeepScreenOn;
    android.admobEnabled = m_androidAdmobEnabled;
    applyTextBuffer(android.admobAppId, m_androidAdmobAppIdBuffer);

    const SceneProject* startScene = m_project->getScene(m_startSceneId);
    if (startScene && !startScene->filepath.empty()) {
        m_project->setStartSceneId(startScene->id);
    } else {
        m_project->setStartSceneId(NULL_PROJECT_SCENE);
    }

    m_project->getLoadingSettings() = m_loadingSettings;
    // undoable: the reserve applies to the editor's job system at once
    if (m_threading != m_project->getThreadingSettings()) {
        m_project->getProjectCommandHistory()->addCommand(new ThreadingSettingsCmd(m_project, m_threading));
    }
    // undoable: the budget and warm draws apply to the editor at once
    if (m_shaderCreation != m_project->getShaderCreationSettings()) {
        m_project->getProjectCommandHistory()->addCommand(new ShaderCreationSettingsCmd(m_project, m_shaderCreation));
    }
    // undoable: the editor's documents follow at once
    if (m_htmlUiRender != m_project->getHtmlUiRenderSettings()) {
        m_project->getProjectCommandHistory()->addCommand(new HtmlUiRenderSettingsCmd(m_project, m_htmlUiRender));
    }
    // undoable: every scene's crowds and grids read them at their next fixed tick
    if (m_navigation != m_project->getNavigationSettings()) {
        m_project->getProjectCommandHistory()->addCommand(new NavigationSettingsCmd(m_project, m_navigation));
    }

    m_project->setPackNativeResources(m_packNativeResources);
    m_project->setVersionControlMetadata(m_versionControlMetadata);
    m_project->setVcsProjectSettings(m_vcs);

    // a name typed into New Tag Name but not added yet is kept rather than lost
    std::vector<std::string> tags = m_tags;
    if (m_newTagBuffer[0] != '\0' && newTagError().empty()) {
        tags.push_back(TagManager::trimName(m_newTagBuffer));
    }
    // GameObjects keep a removed tag, as in Unity; scripts then see it as not defined
    m_project->setTagsAndLayers(tags, m_layerNames);
    m_project->setLayerCollisionMatrices(m_layerCollision3D, m_layerCollision2D);

    // the standalone build's main.cpp bakes these settings in
    m_project->markGeneratedSourcesDirty();

    return m_project->saveProjectFile();
}

} // namespace jasperengine::editor
