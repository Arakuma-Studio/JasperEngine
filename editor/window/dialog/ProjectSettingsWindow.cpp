// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectSettingsWindow.h"
#include "window/ProjectWindow.h"
#include "io/watch/LinkedFolders.h"
#include "window/EditorModal.h"
#include "widget/ColorMath.h"
#include "window/PlayInput.h"
#include "widget/ColorField.h"
#include "shortcuts/CommandCatalog.h"
#include "settings/SettingsRegistry.h"
#include "reflect/Reflect.h"
#include "command/type/ShaderCreationSettingsCmd.h"
#include "texture/TextureCookReport.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/NativePlugins.h"
#include "Backend.h"
#include "App.h"
#include "util/Util.h"
#include "window/Widgets.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "SettingsCategoryList.h"
#include "asset/AssetMap.h"
#include "window/CanvasScalerFields.h"
#include "audio/LoudnessMonitor.h"
#include "command/type/AudioSettingsCmd.h"
#include "audio/AudioMixer.h"
#include "subsystem/AudioSystem.h"
#include "EditorHost.h"
#include "command/type/DebugSettingsCmd.h"
#include "input/InputActions.h"
#include "input/InputControlNames.h"
#include "input/InputManagerFormat.h"
#include "io/UserSettings.h"
#include "organisation/LabelOperations.h"
#include "Out.h"
#include "command/CommandHistory.h"
#include "command/type/InputActionsCmd.h"
#include "command/type/HtmlUiRenderSettingsCmd.h"
#include "command/type/ThreadingSettingsCmd.h"
#include "navigation/NavigationSettingsCmd.h"
#include "command/type/ShaderGlobalsCmd.h"
#include "util/Color.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <set>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstdlib>
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

static const char* androidInternetAccessNames[] = { "Auto", "Require" };
static constexpr int androidInternetAccessCount = 2;
// Player > Resolution and Presentation, Unity's names and order
static const char* defaultOrientationNames[] = { "Portrait", "Portrait Upside Down", "Landscape Left", "Landscape Right", "Auto Rotation" };
static const ScreenOrientation defaultOrientationValues[] = {
    ScreenOrientation::Portrait,
    ScreenOrientation::PortraitUpsideDown,
    ScreenOrientation::LandscapeLeft,
    ScreenOrientation::LandscapeRight,
    ScreenOrientation::AutoRotation
};
static const int defaultOrientationCount = sizeof(defaultOrientationValues) / sizeof(defaultOrientationValues[0]);

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
    Audio,
    Scripting,
    Debug,
    TagsAndLayers,
    NativePlugins,
    Editor,
    Navigation,
    Count
};

// ProjectCategory follows the order of the Project Settings pages in the settings registry
// (settings/SettingsRows.cpp), which lists them and their rows for the search
// (Audio, Debug and Native Plugins are pages there too, in this order)

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

static int findDefaultOrientationIndex(ScreenOrientation orientation) {
    for (int i = 0; i < defaultOrientationCount; i++) {
        if (defaultOrientationValues[i] == orientation) return i;
    }
    return defaultOrientationCount - 1;
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

// S30 (docs/shipping.md)
static const char* const buildMethodNames[] = {"auto", "prebuilt", "compile"};
static const char* const buildMethodLabels[] = {"Auto", "Prebuilt Player", "Compile From Source"};
// the default first (AndroidFormat: aab), the universal APK last (an explicit choice)
static const char* const androidPackageFormats[] = {"aab", "apk-per-abi", "apk"};
static const char* const androidPackageFormatLabels[] = {"App Bundle (AAB, Google Play)", "One APK per ABI", "Universal APK (every ABI)"};
static const char* const iosExportMethods[] = {"development", "ad-hoc", "app-store", "enterprise"};

template<size_t N>
static int indexOfName(const char* const (&names)[N], const std::string& value) {
    for (size_t i = 0; i < N; i++)
        if (value == names[i]) return static_cast<int>(i);
    return 0;
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
    m_uiScale = project->getUIScale();
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
        for (watch::LinkedDirectoryIterator it(assets, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)){
            if (it->is_regular_file(ec) && it->path().extension() == ".inputactions")
                m_inputActionsFiles.push_back(project->normalizeToProjectRelative(it->path()));
        }
        std::sort(m_inputActionsFiles.begin(), m_inputActionsFiles.end());
    }
    m_physics3DWorld = project->getPhysics3DWorldSettings();
    m_runtimeShadersEnabled = project->isRuntimeShadersEnabled();
    m_mobilePbrTierIndex = findMobilePbrTierIndex(project->getMobilePbrTier());

    const PresentationSettings& presentation = project->getPresentationSettings();
    m_defaultOrientationIndex = findDefaultOrientationIndex(presentation.defaultOrientation);
    m_allowedOrientations = presentation.allowedOrientations;
    m_keepScreenOn = presentation.keepScreenOn;
    m_targetFrameRate = presentation.targetFrameRate;

    const ApplicationSettings& application = project->getApplicationSettings();
    snprintf(m_applicationNameBuffer, sizeof(m_applicationNameBuffer), "%s", application.name.c_str());
    snprintf(m_applicationIdentifierBuffer, sizeof(m_applicationIdentifierBuffer), "%s", application.identifier.c_str());
    snprintf(m_applicationVersionBuffer, sizeof(m_applicationVersionBuffer), "%s", application.version.c_str());
    m_applicationBuild = static_cast<int>(application.build);
    m_allowInsecureHttp = application.allowInsecureHttp;

    const WebProjectSettings& web = project->getWebProjectSettings();
    snprintf(m_webApplicationNameBuffer, sizeof(m_webApplicationNameBuffer), "%s", web.applicationName.c_str());
    m_webFavicon = web.favicon;
    m_webCustomHtmlShell = web.customHtmlShell;
    snprintf(m_webHeadIncludeBuffer, sizeof(m_webHeadIncludeBuffer), "%s", web.headInclude.c_str());
    m_webResizeCanvasToWindow = web.resizeCanvasToWindow;
    m_webHideEmscriptenUI = web.hideEmscriptenUI;
    m_webViewportFitCover = web.viewportFitCover;

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
    snprintf(m_iosTeamIdBuffer, sizeof(m_iosTeamIdBuffer), "%s", ios.teamId.c_str());
    m_iosExportMethodIndex = indexOfName(iosExportMethods, ios.exportMethod);

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
    m_androidAbiArmeabiV7a = android.abiArmeabiV7a;
    m_androidAbiArm64V8a = android.abiArm64V8a;
    m_androidAbiX86 = android.abiX86;
    m_androidAbiX86_64 = android.abiX86_64;
    m_androidPermissions = android.permissions;
    m_androidInternetAccessIndex = android.internetAccess == AndroidInternetAccess::Require ? 1 : 0;
    m_androidAllowBackup = android.allowBackup;
    m_androidFullscreen = android.fullscreen;
    m_androidAdmobEnabled = android.admobEnabled;
    snprintf(m_androidAdmobAppIdBuffer, sizeof(m_androidAdmobAppIdBuffer), "%s", android.admobAppId.c_str());
    m_androidPackageFormatIndex = indexOfName(androidPackageFormats, android.packageFormat);
    m_androidKeystorePath = android.keystorePath;
    snprintf(m_androidKeystoreAliasBuffer, sizeof(m_androidKeystoreAliasBuffer), "%s", android.keystoreAlias.c_str());

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
    m_audioSettings = project->getAudioSettings();
    snprintf(m_audioMixerBuffer, sizeof(m_audioMixerBuffer), "%s", m_audioSettings.mixer.c_str());
    snprintf(m_audioDefaultGroupBuffer, sizeof(m_audioDefaultGroupBuffer), "%s", m_audioSettings.defaultOutputGroup.c_str());
    m_debugSettings = project->getDebugSettings();

    m_packNativeResources = project->shouldPackNativeResources();
    m_versionControlMetadata = project->hasVersionControlMetadata();
    m_buildMethodIndex = indexOfName(buildMethodNames, project->getBuildMethod());
    m_packContent = project->isPackContent();
    m_stripEngineCode = project->isStripEngineCode();
    m_vcs = project->getVcsProjectSettings();

    m_tags = project->getTags();
    m_layerNames = project->getLayerNames();
    m_layerNames.resize(MAX_LAYERS);
    m_newTagBuffer[0] = '\0';

    m_layerCollision3D = project->getLayerCollisionMatrix3D();
    m_layerCollision2D = project->getLayerCollisionMatrix2D();

    const AndroidProjectSettings& nativeAndroid = project->getAndroidProjectSettings();
    m_gradleDependencies = nativeAndroid.gradleDependencies;
    m_gradleRepositories = nativeAndroid.gradleRepositories;
    m_gradleProperties = nativeAndroid.gradleProperties;
    m_extraPermissions = nativeAndroid.extraPermissions;
    m_manifestXml = nativeAndroid.manifestXml;
    m_manifestApplicationXml = nativeAndroid.manifestApplicationXml;
    const IOSProjectSettings& nativeIOS = project->getIOSProjectSettings();
    m_iosPlistEntries.assign(nativeIOS.plistEntries.begin(), nativeIOS.plistEntries.end());
    m_iosPlistXml = nativeIOS.plistXml;
    m_iosFrameworks = nativeIOS.frameworks;
    m_iosCocoapods = nativeIOS.cocoapods;
    m_iosSwiftPackages = nativeIOS.swiftPackages;
    m_iosEntitlementsXml = nativeIOS.entitlementsXml;
    m_iosDeploymentTarget = nativeIOS.deploymentTarget;
    const MacOSProjectSettings& nativeMacOS = project->getMacOSProjectSettings();
    m_macosPlistEntries.assign(nativeMacOS.plistEntries.begin(), nativeMacOS.plistEntries.end());
    m_macosFrameworks = nativeMacOS.frameworks;
    m_webScriptUrls = project->getWebProjectSettings().scriptUrls;
    m_nativeSummary.clear();

    loadInputManagerActions();
    m_inputSearch[0] = '\0';

    m_globalsEdit.begin(project);
    m_shaderGlobalUsesScanned = false;
    m_shaderGlobalRemove = -1;
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

    // OK applies only what the page changed, over the project's settings as they are then, so an
    // agent's concurrent set_project_settings survives it (phase 2 M4): no refusal of edits meanwhile
    bool popupOpen = EditorModal::begin("Project Settings##ProjectSettingsModal", &m_isOpen, flags, EditorModal::Role::MergesEdits);

    if (popupOpen) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            drawSettings();
        }
        ImGui::EndPopup();
    }

    // closed without OK: the engine declares the project's shader globals again
    if (!m_isOpen && m_globalsEdit.isPreviewed())
        m_globalsEdit.cancel();
}

void ProjectSettingsWindow::drawSettings() {
    // the Input page's Listen takes Escape: it must not close the dialog in the frame it ends
    const bool listening = m_inputListen.action >= 0;
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
        case ProjectCategory::Audio: drawAudioSettings(); break;
        case ProjectCategory::Scripting: drawScriptingSettings(); break;
        case ProjectCategory::Debug: drawDebugSettings(); break;
        case ProjectCategory::TagsAndLayers: drawTagsAndLayersSettings(); break;
        case ProjectCategory::NativePlugins: drawNativePluginsSettings(); break;
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
    const std::string inputProblem = inputError();
    const std::string shaderGlobalsProblem = m_globalsEdit.error();
    const bool canSave = hasAbi && tagsAndLayersProblem.empty() && inputProblem.empty() && shaderGlobalsProblem.empty();

    ImGui::BeginDisabled(!canSave);
    if (EditorModal::button("OK", "ok", ImVec2(buttonWidth, 0))) {
        if (applySettings()) {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!hasAbi) {
            ImGui::SetTooltip("Select at least one Android architecture (Player > Android).");
        } else if (!tagsAndLayersProblem.empty()) {
            ImGui::SetTooltip("%s (Tags and Layers).", tagsAndLayersProblem.c_str());
        } else if (!inputProblem.empty()) {
            ImGui::SetTooltip("%s (Input).", inputProblem.c_str());
        } else {
            ImGui::SetTooltip("%s (Graphics > Shader Globals).", shaderGlobalsProblem.c_str());
        }
    }

    ImGui::SameLine();
    // Escape cancels a Listen of the Input page first
    if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0)) ||
        (!listening && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
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

            drawPresentationSettings();

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

        if (settingsFoldout("Other Settings") && beginSettingsTable("##PlayerOtherTable")) {
            if (settingsui::beginRow("player.allow_insecure_http")) {
                const bool defaultInsecureHttp = ApplicationSettings{}.allowInsecureHttp;
                if (projectBeginSettingsRow("Allow Insecure HTTP", m_allowInsecureHttp != defaultInsecureHttp)) {
                    m_allowInsecureHttp = defaultInsecureHttp;
                }
                ImGui::Checkbox("##AllowInsecureHttp", &m_allowInsecureHttp);
                endSettingsRow("Plain http:// URLs other than localhost. Off: they fail with insecure_http on every platform; "
                    "exports set Android cleartext and iOS ATS to match.");
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
            if (settingsui::beginRow("player.build_method")) {
                if (projectBeginSettingsRow("Build Method", m_buildMethodIndex != 0)) m_buildMethodIndex = 0;
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::Combo("##BuildMethod", &m_buildMethodIndex, buildMethodLabels, IM_ARRAYSIZE(buildMethodLabels));
                endSettingsRow("How Build makes the game. Auto: a prebuilt player (no compiler needed) unless the project needs native code "
                               "(C++ scripts, ProjectBuild.cmake, capacities above the player's, AdMob), then compiled from source.");
            }
            if (settingsui::beginRow("player.pack_content")) {
                if (projectBeginSettingsRow("Content Pack", m_packContent)) m_packContent = false;
                ImGui::Checkbox("##PackContent", &m_packContent);
                endSettingsRow("Prebuilt builds keep their content in one content.jpak next to the player instead of loose files "
                               "(the web always packs it).");
            }
            if (settingsui::beginRow("player.strip_engine_code")) {
                if (projectBeginSettingsRow("Strip Engine Code", !m_stripEngineCode)) m_stripEngineCode = true;
                ImGui::Checkbox("##StripEngineCode", &m_stripEngineCode);
                endSettingsRow("Builds leave out the engine modules nothing in the project uses (HTML UI, video, the runtime shader "
                               "compiler, 3D or 2D physics): a smaller player that behaves the same. Build Settings shows what is kept and "
                               "why. Turn it off for a script that builds engine names at runtime from pieces.");
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        drawPlatformsSettings();
    }
    endSettingsPanel();
}

void ProjectSettingsWindow::drawPresentationSettings() {
    const PresentationSettings defaults;

    drawComboSetting("Default Orientation", "##DefaultOrientation", defaultOrientationNames, defaultOrientationCount,
        m_defaultOrientationIndex, findDefaultOrientationIndex(defaults.defaultOrientation),
        "Orientation the game starts in on phones and tablets (Android and iOS). Auto Rotation follows the device "
        "within the allowed orientations below. Scripts change it with Screen.orientation.");

    const bool autoRotation = defaultOrientationValues[std::clamp(m_defaultOrientationIndex, 0, defaultOrientationCount - 1)] ==
        ScreenOrientation::AutoRotation;
    if (projectBeginSettingsRow("Allowed Orientations", m_allowedOrientations != defaults.allowedOrientations)) {
        m_allowedOrientations = defaults.allowedOrientations;
    }
    ImGui::BeginDisabled(!autoRotation);
    const struct { const char* label; ScreenOrientation orientation; } choices[] = {
        {"Portrait##Allowed", ScreenOrientation::Portrait},
        {"Portrait Upside Down##Allowed", ScreenOrientation::PortraitUpsideDown},
        {"Landscape Left##Allowed", ScreenOrientation::LandscapeLeft},
        {"Landscape Right##Allowed", ScreenOrientation::LandscapeRight},
    };
    for (const auto& choice : choices) {
        const uint8_t bit = PlatformRules::orientationBit(choice.orientation);
        bool on = (m_allowedOrientations & bit) != 0;
        if (ImGui::Checkbox(choice.label, &on)) {
            const uint8_t mask = on ? (uint8_t)(m_allowedOrientations | bit) : (uint8_t)(m_allowedOrientations & ~bit);
            // at least one stays (Unity refuses the last one too)
            if (mask & PlatformRules::ALL_ORIENTATIONS) m_allowedOrientations = mask;
        }
    }
    ImGui::EndDisabled();
    endSettingsRow("Allowed Orientations for Auto Rotation (Screen.autorotateTo*): where the device may turn the game. "
        "Landscape Left is the device turned counter-clockwise from portrait.");
    if (autoRotation) {
        bool androidUnsupported = false;
        PlatformRules::androidRequestedOrientation(ScreenOrientation::AutoRotation, m_allowedOrientations, &androidUnsupported);
        if (androidUnsupported) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Android cannot limit rotation to this set; it allows all four.");
        }
    }

    if (projectBeginSettingsRow("Keep Screen On", m_keepScreenOn != defaults.keepScreenOn)) {
        m_keepScreenOn = defaults.keepScreenOn;
    }
    ImGui::Checkbox("##KeepScreenOn", &m_keepScreenOn);
    endSettingsRow("Android and iOS: the screen never dims or locks while the game runs (Screen.sleepTimeout = "
        "SleepTimeout.NeverSleep). Off: the device's own setting. Desktops and the web follow the system unless a "
        "script sets NeverSleep (as Unity).");

    if (projectBeginSettingsRow("Target Frame Rate", m_targetFrameRate != defaults.targetFrameRate)) {
        m_targetFrameRate = defaults.targetFrameRate;
    }
    ImGui::SetNextItemWidth(-helpMarkerWidth());
    const int before = m_targetFrameRate;
    ImGui::InputInt("##TargetFrameRate", &m_targetFrameRate);
    // 0 is no rate: the step buttons go from -1 to 1 and back
    if (m_targetFrameRate == 0) m_targetFrameRate = before < 0 ? 1 : -1;
    if (m_targetFrameRate < 0) m_targetFrameRate = -1;
    m_targetFrameRate = std::min(m_targetFrameRate, 1000);
    endSettingsRow("Initial Engine.targetFrameRate. -1 = the platform's rate: 60 on mobile, every browser frame on the web, "
        "as fast as vsync allows on desktops (which ignore it while VSync is on, as Unity).");
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

        // the Canvas Scaler's project default: the UI's own space, apart from the game canvas
        if (settingsui::beginRow("graphics.ui_scale")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Spacing();
            settingsui::rowLabel("UI Scale");
            ImGui::SameLine();
            if (m_uiScale != CanvasScalerSettings()) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT "##resetUIScale"))
                    m_uiScale = CanvasScalerSettings();
                ImGui::SetItemTooltip("Restore default (Follow Game Canvas)");
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Spacing();
            ImGui::TextDisabled("Canvas Scaler of the UI scenes and HTML UI documents (a scene can override it)");
            drawCanvasScalerFields("##UIScale", m_uiScale);
            if (m_uiScale.mode != UIScaleMode::FollowGameCanvas && m_canvasWidth > 0 && m_canvasHeight > 0) {
                float viewWidth = 0.0f, viewHeight = 0.0f, dpi = 0.0f;
                m_project->getEditViewSize(viewWidth, viewHeight, dpi);
                int gameCanvasWidth = m_canvasWidth, gameCanvasHeight = m_canvasHeight;
                Rect viewRect;
                Engine::computeViewLayout((int)viewWidth, (int)viewHeight, m_canvasWidth, m_canvasHeight, scalingModeValues[m_scalingModeIndex],
                                          gameCanvasWidth, gameCanvasHeight, viewRect);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", canvasScalerPreview(m_uiScale, viewRect.getWidth(), viewRect.getHeight(),
                                                              (float)gameCanvasWidth, (float)gameCanvasHeight, dpi).c_str());
                ImGui::SetItemTooltip("For the Game view's current resolution; pick another in its resolution menu.");
            }
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
        if (settingsFoldout("Shader Globals", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawShaderGlobals();
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
            ("Volumetric fog and volumetric clouds on Windows, macOS and Linux (and the editor's views). Off: neither; "
             "Low, Medium, High: their resolution, steps, light and volume counts. Height fog and the sky fill are never tiered."),
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

        if (settingsFoldout("3D World Capacity", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawSectionNote("Limits of each scene's 3D physics world, reserved when the scene starts playing. "
                "Past Max Bodies no more Rigidbodies are made (the console names the setting); "
                "past the pairs or contact constraints some contacts are dropped for that step.");
            if (beginSettingsTable("##WorldCapacityTable")) {
                const Physics3DWorldSettings defaults;
                const int maxIndex = 8388607;
                auto capacityRow = [&](const char* key, const char* label, const char* id, unsigned int& field, unsigned int defaultValue, const char* tooltip) {
                    // a registry row (physics.max_*: MCP and the settings search)
                    if (!settingsui::beginRow(key)) return;
                    int value = (int)std::min<unsigned int>(field, (unsigned int)maxIndex);
                    drawIntSetting(label, id, value, (int)defaultValue, 16, tooltip);
                    field = (unsigned int)std::min(value, maxIndex);
                };
                capacityRow("physics.max_bodies", "Max Bodies", "##Physics3DMaxBodies", m_physics3DWorld.maxBodies, defaults.maxBodies,
                    "The most Rigidbodies a scene can have at once (Jolt's maxBodies).");
                capacityRow("physics.max_body_pairs", "Max Body Pairs", "##Physics3DMaxBodyPairs", m_physics3DWorld.maxBodyPairs, defaults.maxBodyPairs,
                    "The most pairs of bodies whose bounds overlap in one step (Jolt's maxBodyPairs).");
                capacityRow("physics.max_contact_constraints", "Max Contact Constraints", "##Physics3DMaxContacts", m_physics3DWorld.maxContactConstraints, defaults.maxContactConstraints,
                    "The most touching body pairs solved in one step (Jolt's maxContactConstraints).");
                ImGui::EndTable();
            }
        }

        if (settingsFoldout("Layer Collision Matrix", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawSectionNote("Rigidbodies on layers whose box is cleared pass through each other and report no contacts. "
                "Scripts change it with Physics.ignoreLayerCollision; collider category and mask bits still apply to the checked pairs.");
            if (settingsui::beginRow("physics.layer_collision_matrix")) drawLayerCollisionMatrix("##LayerCollisionMatrix3D", m_layerCollision3D);
        }
    }
    endSettingsPanel();
}

void ProjectSettingsWindow::drawAudioSettings() {
    if (beginSettingsPanel("##AudioSettingsPanel")) {
        if (beginSettingsTable("##AudioSettingsTable")) {
            const AudioSettings defaults;
            AudioSettings& audio = m_audioSettings;

            if (settingsui::beginRow("audio.mixer")) {
                if (projectBeginSettingsRow("Audio Mixer", m_audioMixerBuffer[0] != '\0')) {
                    m_audioMixerBuffer[0] = '\0';
                }
                const float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SetNextItemWidth(-helpMarkerWidth() - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
                ImGui::InputTextWithHint("##AudioMixer", "None (an implicit Master)", m_audioMixerBuffer, sizeof(m_audioMixerBuffer));
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                        std::vector<std::string> files = Util::getStringsFromPayload(payload);
                        if (!files.empty()) {
                            const fs::path relative = m_project->normalizeToAssetsRelative(m_project->resolveAssetPath(FileUtils::pathFromUtf8(files[0])));
                            if (relative.extension() == ".mixer") {
                                snprintf(m_audioMixerBuffer, sizeof(m_audioMixerBuffer), "%s", FileUtils::pathToGenericUtf8(relative).c_str());
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FOLDER_OPEN "##AudioMixerPick")) {
                    const std::string picked = FileDialogs::openFileDialog(m_project->getAssetsPath().string(), FILE_DIALOG_AUDIO_MIXER);
                    if (!picked.empty()) {
                        const fs::path relative = m_project->normalizeToAssetsRelative(FileUtils::pathFromUtf8(picked));
                        snprintf(m_audioMixerBuffer, sizeof(m_audioMixerBuffer), "%s", FileUtils::pathToGenericUtf8(relative).c_str());
                    }
                }
                endSettingsRow("The .mixer the game plays through (assets-relative). Window > Audio > Audio Mixer edits it and "
                               "makes audio/Main.mixer when none is set. Empty: every sound plays through an implicit Master at 0 dB.");
            }

            if (settingsui::beginRow("audio.default_output_group")) {
                if (projectBeginSettingsRow("Default Output Group", m_audioDefaultGroupBuffer[0] != '\0')) {
                    m_audioDefaultGroupBuffer[0] = '\0';
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                const std::vector<std::string> groups = AudioMixer::getGroupNames();
                if (ImGui::BeginCombo("##AudioDefaultGroup", m_audioDefaultGroupBuffer[0] ? m_audioDefaultGroupBuffer : (groups.empty() ? "Master" : groups[0].c_str()))) {
                    for (size_t i = 0; i < groups.size(); i++) {
                        const char* value = i == 0 ? "" : groups[i].c_str();
                        if (ImGui::Selectable(groups[i].c_str(), strcmp(value, m_audioDefaultGroupBuffer) == 0)) {
                            snprintf(m_audioDefaultGroupBuffer, sizeof(m_audioDefaultGroupBuffer), "%s", value);
                        }
                    }
                    ImGui::EndCombo();
                }
                endSettingsRow("The mixer group new Audio Sources play through.");
            }

            if (settingsui::beginRow("audio.max_real_voices")) {
                if (projectBeginSettingsRow("Max Real Voices", audio.maxRealVoices != defaults.maxRealVoices)) {
                    audio.maxRealVoices = defaults.maxRealVoices;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                // SoLoud mixes at most 255 voices at once, the mixer's group buses among them
                const int realVoicesLimit = std::max(1, (int)AudioMixer::maxActiveVoices - (int)AudioMixer::getGroupNames().size());
                ImGui::DragInt("##AudioMaxRealVoices", &audio.maxRealVoices, 1.0f, 1, realVoicesLimit);
                audio.maxRealVoices = std::clamp(audio.maxRealVoices, 1, realVoicesLimit);
                endSettingsRow("Voices mixed at once. Past it the quietest are virtualised (silent until they are loud enough again). "
                               "At most 255 minus the mixer's groups (each group is a voice too).");
            }

            if (settingsui::beginRow("audio.max_voices")) {
                if (projectBeginSettingsRow("Max Voices", audio.maxVoices != defaults.maxVoices)) {
                    audio.maxVoices = defaults.maxVoices;
                }
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::DragInt("##AudioMaxVoices", &audio.maxVoices, 1.0f, 1, 1000);
                audio.maxVoices = std::clamp(audio.maxVoices, 1, 1000);
                endSettingsRow("Voices playing at all. Past it the least important (the highest Audio Source Priority, the oldest first) "
                               "is stopped, or the new one does not start when it is the least important.");
            }

            if (settingsui::beginRow("audio.sample_rate")) {
                if (projectBeginSettingsRow("Sample Rate", audio.sampleRate != defaults.sampleRate)) {
                    audio.sampleRate = defaults.sampleRate;
                }
                static const int rates[] = {0, 22050, 44100, 48000};
                static const char* rateNames[] = {"Auto (the device's)", "22050 Hz", "44100 Hz", "48000 Hz"};
                int rateIndex = 0;
                for (int i = 0; i < 4; i++) if (rates[i] == audio.sampleRate) rateIndex = i;
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::Combo("##AudioSampleRate", &rateIndex, rateNames, 4)) audio.sampleRate = rates[rateIndex];
                endSettingsRow("The output's sample rate. It applies the next time the audio opens (exports: at start).");
            }

            const float loudnessHelpWidth = -helpMarkerWidth();
            auto measurement = [&](const char* key, const char* label, const char* id, float& value, float reset, float minimum, float maximum,
                                   const char* format, const char* help) {
                if (!settingsui::beginRow(key)) return;
                if (projectBeginSettingsRow(label, value != reset)) value = reset;
                ImGui::SetNextItemWidth(loudnessHelpWidth);
                ImGui::DragFloat(id, &value, 0.1f, minimum, maximum, format);
                value = std::clamp(value, minimum, maximum);
                endSettingsRow(help);
            };
            measurement("audio.loudness_target_lufs", "Loudness Target", "##AudioLoudnessTarget", audio.loudnessTargetLufs, defaults.loudnessTargetLufs,
                        -40.0f, -6.0f, "%.1f LUFS", "Measurement only: changing the target changes no sound. The Audio Mixer and the agents read against it.");
            measurement("audio.loudness_tolerance_lu", "Loudness Tolerance", "##AudioLoudnessTolerance", audio.loudnessToleranceLu, defaults.loudnessToleranceLu,
                        0.5f, 10.0f, "%.1f LU", "Measurement only: how far from the target still passes.");
            measurement("audio.true_peak_ceiling_dbtp", "True Peak Ceiling", "##AudioTruePeakCeiling", audio.truePeakCeilingDbtp, defaults.truePeakCeilingDbtp,
                        -12.0f, 0.0f, "%.1f dBTP", "Measurement only: a limiter's Threshold on the Master is what holds it.");

            if (settingsui::beginRow("audio.output_stage")) {
                if (projectBeginSettingsRow("Output Stage", audio.outputStage != defaults.outputStage)) audio.outputStage = defaults.outputStage;
                int stage = audio.outputStage == "linear" ? 1 : 0;
                static const char* stageNames[] = {"Round-off (soft clip, -1.66 dB)", "Linear (hard clip at 0 dBFS)"};
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::Combo("##AudioOutputStage", &stage, stageNames, 2)) audio.outputStage = stage == 1 ? "linear" : "roundoff";
                endSettingsRow("What the output does to the mix. Linear lets a Master limiter's ceiling hold at the device; "
                               "without a limiter it clips when Master goes over 0 dBFS.");
            }

            if (settingsui::beginRow("audio.ios_session_category")) {
                if (projectBeginSettingsRow("iOS Audio Session", audio.iosSessionCategory != defaults.iosSessionCategory)) audio.iosSessionCategory = defaults.iosSessionCategory;
                static const char* sessionValues[] = {"default", "ambient", "solo_ambient", "playback"};
                static const char* sessionNames[] = {"Default (miniaudio's)", "Ambient", "Solo Ambient", "Playback"};
                int session = 0;
                for (int i = 0; i < 4; i++) if (audio.iosSessionCategory == sessionValues[i]) session = i;
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                if (ImGui::Combo("##AudioIosSession", &session, sessionNames, 4)) audio.iosSessionCategory = sessionValues[session];
                endSettingsRow("iOS only. Ambient respects the silent switch and mixes with the player's music. "
                               "Default ignores the switch and interrupts the music.");
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_SLIDERS " Open Audio Mixer")) {
            editor::getEditorHost().openAudioMixerWindow(m_audioMixerBuffer[0] ? m_project->resolveAssetPath(FileUtils::pathFromUtf8(m_audioMixerBuffer)) : fs::path());
        }
        if (m_project && m_project->isAnyScenePlaying()) {
            LoudnessMonitor::get().markVisible();
            const LoudnessMonitor::Reading reading = LoudnessMonitor::get().reading();
            if (reading.momentary) {
                ImGui::Text("Master: %.1f LUFS (target %.1f)", *reading.momentary, m_audioSettings.loudnessTargetLufs);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", AudioSystem::isInited() ? (std::string("Output: ") + (AudioSystem::isOutputAvailable() ? AudioSystem::getBackendName() : "none (" + AudioSystem::getBackendName() + ")")).c_str() : "");
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
        drawInputManagerSection();
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

// a key of the debug overlay: none or a key name (InputControlNames)
static void drawDebugKeySetting(const char* label, const char* id, std::string& key, const std::string& defaultKey, const char* tooltip) {
    if (projectBeginSettingsRow(label, key != defaultKey)) {
        key = defaultKey;
    }
    const int code = key.empty() ? D_KEY_UNKNOWN : InputControlNames::parseKey(key);
    const std::string shown = key.empty() ? std::string("None") : (code != D_KEY_UNKNOWN ? InputControlNames::keyName(code) : key);
    ImGui::SetNextItemWidth(-helpMarkerWidth());
    if (ImGui::BeginCombo(id, shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("None", key.empty())) key.clear();
        for (const std::string& name : InputControlNames::keyNames()) {
            const bool selected = code != D_KEY_UNKNOWN && InputControlNames::parseKey(name) == code;
            if (ImGui::Selectable(name.c_str(), selected)) key = name;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    endSettingsRow(tooltip);
}

// Project Settings > Debug: the rows of its reflection table (editor/reflect/DebugReflect.cpp:
// labels, tooltips, the choices of the enums, the buffer size's range), in its groups
void ProjectSettingsWindow::drawDebugSettings() {
    if (beginSettingsPanel("##DebugSettingsPanel")) {
        drawSectionNote("The on-device debug overlay: a log console (filters, search, collapse, stack traces, copy, save, "
            "commands) and an FPS / stats panel, in the editor's Play and in exported games. Scripts turn it on in any "
            "build with DebugOverlay.enabled = true (a remote switch for one tester).");

        DebugSettings defaults;
        DebugSettings& debug = m_debugSettings;

        for (const char* group : {"Overlay", "Log"}) {
            const std::string tableId = std::string("##Debug") + group + "Table";
            if (!settingsFoldout(group, ImGuiTreeNodeFlags_DefaultOpen) || !beginSettingsTable(tableId.c_str())) continue;
            for (const DebugSettingsNames::SettingKey& entry : DebugSettingsNames::settingKeys()) {
                const reflect::FieldDesc* field = reflect::debugSettingsField(entry.field);
                if (!field || !field->group || std::string(field->group) != group || !settingsui::beginRow(entry.key)) continue;
                const std::string id = std::string("##Debug_") + field->name;
                const char* tooltip = field->tooltip ? field->tooltip : "";
                void* value = field->ref(&debug);
                void* base = field->ref(&defaults);
                if (field->enumDesc) {
                    std::vector<const char*> labels;
                    for (const reflect::EnumItem& item : field->enumDesc->items) labels.push_back(item.label);
                    int index = reflect::readEnum(*field, value);
                    drawComboSetting(field->label, id.c_str(), labels.data(), static_cast<int>(labels.size()), index,
                                     reflect::readEnum(*field, base), tooltip);
                    reflect::writeEnum(*field, value, std::clamp(index, 0, static_cast<int>(labels.size()) - 1));
                } else if (field->drawer && std::string(field->drawer) == "key") {
                    drawDebugKeySetting(field->label, id.c_str(), *static_cast<std::string*>(value), *static_cast<std::string*>(base), tooltip);
                } else if (field->kind == reflect::Kind::Bool) {
                    bool& b = *static_cast<bool*>(value);
                    if (projectBeginSettingsRow(field->label, b != *static_cast<bool*>(base))) b = *static_cast<bool*>(base);
                    ImGui::Checkbox(id.c_str(), &b);
                    endSettingsRow(tooltip);
                } else {
                    unsigned int& u = *static_cast<unsigned int*>(value);
                    int number = static_cast<int>(u);
                    drawIntSetting(field->label, id.c_str(), number, static_cast<int>(*static_cast<unsigned int*>(base)),
                                   static_cast<int>(field->min), tooltip);
                    u = static_cast<unsigned int>(std::clamp(number, static_cast<int>(field->min), static_cast<int>(field->max)));
                }
            }
            ImGui::EndTable();
        }
    }
    endSettingsPanel();
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

    if (settingsui::beginRow("web.viewport_fit_cover")) {
        if (projectBeginSettingsRow("Viewport Fit Cover", !m_webViewportFitCover)) {
            m_webViewportFitCover = true;
        }
        ImGui::Checkbox("##WebViewportFitCover", &m_webViewportFitCover);
        endSettingsRow("Adds viewport-fit=cover to the page's viewport: on phones the page reaches under the notch and the "
                       "safe area comes from env(safe-area-inset-*) (Screen.safeArea, the Safe Area component).");
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
    projectBeginSettingsRow("Orientation");
    ImGui::TextDisabled("From Player > Resolution and Presentation");
    endSettingsRow("Default Orientation and Allowed Orientations for Auto Rotation are shared by Android and iOS.");

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

    if (settingsui::beginRow("ios.team_id")) {
        projectBeginSettingsRow("Team ID");
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::InputTextWithHint("##IOSTeamId", "ABCDE12345", m_iosTeamIdBuffer, sizeof(m_iosTeamIdBuffer));
        endSettingsRow("The Apple Developer team that signs device builds (Xcode's DEVELOPMENT_TEAM). The simulator needs none.");
    }

    if (settingsui::beginRow("ios.export_method")) {
        if (projectBeginSettingsRow("Export Method", m_iosExportMethodIndex != 0)) m_iosExportMethodIndex = 0;
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::Combo("##IOSExportMethod", &m_iosExportMethodIndex, iosExportMethods, IM_ARRAYSIZE(iosExportMethods));
        endSettingsRow("How xcodebuild -exportArchive makes the .ipa on a Mac (development, ad-hoc, app-store, enterprise).");
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

    projectBeginSettingsRow("Orientation");
    ImGui::TextDisabled("From Player > Resolution and Presentation");
    endSettingsRow("Default Orientation and Allowed Orientations for Auto Rotation are shared by Android and iOS.");

    if (settingsui::beginRow("android.internet_access")) {
        drawComboSetting("Internet Access", "##AndroidInternetAccess", androidInternetAccessNames, androidInternetAccessCount,
            m_androidInternetAccessIndex, defaults.internetAccess == AndroidInternetAccess::Require ? 1 : 0,
            "Auto adds INTERNET when Lua or C++ code uses Http (checked at export). Require always adds it.");
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

    if (settingsui::beginRow("android.allow_backup")) {
        if (projectBeginSettingsRow("Application Options", m_androidAllowBackup != defaults.allowBackup)) {
            m_androidAllowBackup = defaults.allowBackup;
        }

        ImGui::Checkbox("Allow Backup", &m_androidAllowBackup);
        ImGui::SetItemTooltip("Maps to android:allowBackup.");
        endSettingsRow("Android application/activity behavior. Keep Screen On is in Player > Resolution and Presentation.");
    }

    if (settingsui::beginRow("android.fullscreen")) {
        if (projectBeginSettingsRow("Screen Options", m_androidFullscreen != defaults.fullscreen)) {
            m_androidFullscreen = defaults.fullscreen;
        }

        ImGui::Checkbox("Fullscreen", &m_androidFullscreen);
        ImGui::SetItemTooltip("Uses the fullscreen Android theme and hides system bars in MainActivity.");
        endSettingsRow("Android screen/window behavior.");
    }

    if (settingsui::beginRow("android.package_format")) {
        if (projectBeginSettingsRow("Package Format", m_androidPackageFormatIndex != 0)) m_androidPackageFormatIndex = 0;
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::Combo("##AndroidPackageFormat", &m_androidPackageFormatIndex, androidPackageFormatLabels, IM_ARRAYSIZE(androidPackageFormatLabels));
        endSettingsRow("What Build makes. App Bundle: for Google Play, which serves each device only its ABI's libraries "
                       "(bundletool is downloaded once). One APK per ABI: installable files, each with one ABI's libraries. "
                       "Universal APK: one installable file with every selected ABI's libraries (several times the download).");
    }

    if (settingsui::beginRow("android.keystore_path")) {
        drawProjectFilePathSetting(m_project, "Keystore", "The keystore that signs release builds. Its passwords are asked when you build "
            "(or JASPER_ANDROID_KS_PASS / JASPER_ANDROID_KEY_PASS) and never saved. None: the debug keystore signs it.",
            "##AndroidKeystorePath", "Browse##androidkeystore", "Clear##androidkeystore", m_androidKeystorePath, FILE_DIALOG_ALL);
    }

    if (settingsui::beginRow("android.keystore_alias")) {
        ImGui::BeginDisabled(m_androidKeystorePath.empty());
        projectBeginSettingsRow("Key Alias");
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::InputTextWithHint("##AndroidKeystoreAlias", "upload", m_androidKeystoreAliasBuffer, sizeof(m_androidKeystoreAliasBuffer));
        endSettingsRow("The key of the keystore that signs the build.");
        ImGui::EndDisabled();
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

// ---- Native Plugins

namespace {

    // A text field over a std::string (grows with the text)
    int growString(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* text = static_cast<std::string*>(data->UserData);
            text->resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = text->data();
        }
        return 0;
    }

    bool inputString(const char* id, std::string& text, const char* hint = nullptr) {
        if (hint) return ImGui::InputTextWithHint(id, hint, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, growString, &text);
        return ImGui::InputText(id, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, growString, &text);
    }

    bool inputMultiline(const char* id, std::string& text, float height) {
        return ImGui::InputTextMultiline(id, text.data(), text.capacity() + 1, ImVec2(-1.0f, Theme::dpi(height)),
                                         ImGuiInputTextFlags_CallbackResize, growString, &text);
    }

    std::string trimmed(const std::string& text) {
        const size_t start = text.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return std::string();
        return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
    }

    std::vector<std::string> splitList(const std::string& text) {
        std::vector<std::string> items;
        size_t start = 0;
        while (start <= text.size()) {
            const size_t comma = text.find(',', start);
            const std::string item = trimmed(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!item.empty()) items.push_back(item);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return items;
    }

    std::string joinList(const std::vector<std::string>& items) {
        std::string text;
        for (const std::string& item : items) text += (text.empty() ? "" : ", ") + item;
        return text;
    }

    void xmlWarning(const std::string& xml) {
        const std::string error = NativePlugins::xmlFragmentError(xml);
        if (!error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s (the export stops on it)", error.c_str());
    }

    // one text row per entry, - removes, + adds
    void drawStringList(const char* label, const char* id, std::vector<std::string>& items, const char* hint, const char* tooltip) {
        projectBeginSettingsRow(label);
        ImGui::PushID(id);
        int remove = -1;
        for (size_t i = 0; i < items.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            inputString("##item", items[i], hint);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_MINUS, ImVec2(ImGui::GetFrameHeight(), 0.0f))) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) items.erase(items.begin() + remove);
        if (ImGui::SmallButton(ICON_FA_PLUS)) items.emplace_back();
        ImGui::PopID();
        endSettingsRow(tooltip);
    }

    const char* const plistKinds[] = {"String", "Boolean", "Integer", "Real", "String list"};

    void drawPlistEntries(const char* id, std::vector<std::pair<std::string, NativePlistValue>>& entries, const char* tooltip) {
        projectBeginSettingsRow("Info.plist Entries");
        ImGui::PushID(id);
        int remove = -1;
        std::set<std::string> keys;
        bool duplicate = false;
        for (size_t i = 0; i < entries.size(); i++) {
            auto& [key, value] = entries[i];
            ImGui::PushID(static_cast<int>(i));
            const float keyWidth = ImGui::GetContentRegionAvail().x * 0.4f;
            ImGui::SetNextItemWidth(keyWidth);
            inputString("##key", key, "NSUserTrackingUsageDescription");
            if (!trimmed(key).empty() && !keys.insert(trimmed(key)).second) duplicate = true;
            ImGui::SameLine();
            int kind = static_cast<int>(value.kind);
            ImGui::SetNextItemWidth(Theme::dpi(100.0f));
            if (ImGui::Combo("##kind", &kind, plistKinds, 5)) value.kind = static_cast<NativePlistValue::Kind>(kind);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            switch (value.kind) {
                case NativePlistValue::Kind::String: inputString("##value", value.text); break;
                case NativePlistValue::Kind::Bool: ImGui::Checkbox("##value", &value.flag); break;
                case NativePlistValue::Kind::Integer: {
                    ImS64 v = static_cast<ImS64>(value.integer);
                    if (ImGui::InputScalar("##value", ImGuiDataType_S64, &v)) value.integer = static_cast<long long>(v);
                    break;
                }
                case NativePlistValue::Kind::Real: ImGui::InputDouble("##value", &value.real); break;
                case NativePlistValue::Kind::StringArray: {
                    std::string text = joinList(value.items);
                    if (inputString("##value", text, "fetch, remote-notification")) value.items = splitList(text);
                    break;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_MINUS, ImVec2(ImGui::GetFrameHeight(), 0.0f))) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) entries.erase(entries.begin() + remove);
        if (ImGui::SmallButton(ICON_FA_PLUS)) entries.emplace_back();
        if (duplicate) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "A key is listed twice: the last one wins.");
        ImGui::PopID();
        endSettingsRow(tooltip);
    }

    void drawXmlSetting(const char* label, const char* id, std::string& xml, float height, const char* tooltip) {
        projectBeginSettingsRow(label);
        ImGui::PushID(id);
        inputMultiline("##xml", xml, height);
        xmlWarning(xml);
        ImGui::PopID();
        endSettingsRow(tooltip);
    }

    std::map<std::string, NativePlistValue> plistMap(const std::vector<std::pair<std::string, NativePlistValue>>& entries) {
        std::map<std::string, NativePlistValue> map;
        for (const auto& [key, value] : entries) {
            if (!trimmed(key).empty()) map[trimmed(key)] = value;
        }
        return map;
    }

    std::vector<std::string> nonEmpty(const std::vector<std::string>& items) {
        std::vector<std::string> out;
        for (const std::string& item : items) {
            if (!trimmed(item).empty()) out.push_back(trimmed(item));
        }
        return out;
    }

}

// ---- Input (Project Settings > Input): the Input Manager's actions (the asset's "Input Manager" map)

namespace {

    const char* inputKindLabels[] = { "Key or Mouse Button", "Gamepad Axis", "Mouse Movement", "Input Actions Binding" };
    const char* mouseAxisLabels[] = { "X (right)", "Y (up)", "Wheel (up)" };

    std::string inputUniqueName(const std::vector<InputManagerAction>& actions, const std::string& base) {
        auto taken = [&actions](const std::string& name) {
            return std::any_of(actions.begin(), actions.end(), [&name](const InputManagerAction& a) { return a.name == name; });
        };
        if (!taken(base)) return base;
        for (int n = 1;; n++) {
            const std::string name = base + " (" + std::to_string(n) + ")";
            if (!taken(name)) return name;
        }
    }

    std::string inputControlsText(const std::vector<InputControl>& controls) {
        std::string text;
        for (const InputControl& control : controls) text += (text.empty() ? "" : ", ") + InputControlNames::name(control);
        return text.empty() ? std::string("none") : text;
    }

    // "Key or Mouse Button: right, d / left, a"
    std::string inputBindingSummary(const InputManagerAction& action, const InputManagerBinding& binding) {
        switch (binding.kind) {
            case InputManagerBindingKind::Buttons: {
                std::string text = std::string(inputKindLabels[0]) + ": " + inputControlsText(binding.positive);
                if (action.type != InputManagerActionType::Button && !binding.negative.empty())
                    text += " / " + inputControlsText(binding.negative);
                return text;
            }
            case InputManagerBindingKind::GamepadAxis:
                return std::string(inputKindLabels[1]) + ": " + InputControlNames::gamepadAxisName(binding.axis) +
                       (binding.invert ? " (inverted)" : "");
            case InputManagerBindingKind::MouseDelta:
                return std::string(inputKindLabels[2]) + ": " + InputManagerFormat::mouseAxisName(binding.mouseAxis);
            case InputManagerBindingKind::Binding:
                return std::string(inputKindLabels[3]) + ": " + binding.binding;
        }
        return "";
    }

    bool inputPlaying(Project* project) {
        return project && project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
    }

    // a non-negative number field with Unity's label
    bool inputNumberRow(const char* label, const char* id, float& value, float defaultValue, float speed, float maximum,
                        const char* tooltip) {
        if (projectBeginSettingsRow(label, value != defaultValue)) value = defaultValue;
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        const bool changed = ImGui::DragFloat(id, &value, speed, 0.0f, maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        endSettingsRow(tooltip);
        return changed;
    }

}

void ProjectSettingsWindow::refreshNativeSummary() {
    // what plugins/native holds
    const NativePlugins::Scan scan = NativePlugins::scan(m_project->getProjectPath());
    std::string summary;
    static const char* const labels[] = {"Android", "Apple", "iOS", "macOS", "Web", "C++"};
    const auto& platforms = NativePlugins::platformNames();
    for (size_t i = 0; i < platforms.size(); i++) {
        const size_t count = scan.count(platforms[i]);
        if (count) summary += (summary.empty() ? "" : ", ") + std::string(labels[i]) + " " + std::to_string(count);
    }
    const size_t plugins = NativePlugins::listPlugins(scan).size();
    const size_t mocks = NativePlugins::listMocks(m_project->getLuaPath()).size();
    m_nativeSummary = scan.exists
        ? "plugins/native: " + std::to_string(plugins) + " plugin(s) (" + (summary.empty() ? std::string("no files") : summary + " files") +
          "), " + std::to_string(mocks) + " Lua mock(s). Lua calls them with Native.call / callAsync."
        : "No plugins/native folder yet. Create Plugin writes one plugin for the chosen platforms and its Lua mock; "
          "Lua calls it with Native.call / callAsync (jasper://docs/native-bridge).";
}

void ProjectSettingsWindow::drawNativePluginsSettings() {
    if (beginSettingsPanel("##NativePluginsPanel")) {
        if (m_nativeSummary.empty()) refreshNativeSummary();
        drawSectionNote(m_nativeSummary.c_str());
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open Folder")) {
            const fs::path root = NativePlugins::root(m_project->getProjectPath());
            std::error_code ec;
            fs::create_directories(root, ec);
            auto openInShell = ImGui::GetPlatformIO().Platform_OpenInShellFn;
            const std::string rootUtf8 = FileUtils::pathToUtf8(root);
            if (!openInShell || !openInShell(ImGui::GetCurrentContext(), rootUtf8.c_str())) {
                Backend::getApp().registerAlert("Open in File Manager", "Could not open " + rootUtf8);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Create Plugin...")) m_nativePluginDialog.open(m_project);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE " Refresh")) refreshNativeSummary();
        const bool wasOpen = m_nativePluginDialog.isOpen();
        m_nativePluginDialog.show();
        if (wasOpen && !m_nativePluginDialog.isOpen()) refreshNativeSummary();
        ImGui::Spacing();

        if (settingsFoldout("Android", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##NativeAndroidTable")) {
            if (settingsui::beginRow("android.gradle_dependencies")) {
                drawStringList("Gradle Dependencies", "##GradleDependencies", m_gradleDependencies, "com.example:sdk:1.2.3",
                    "group:name:version becomes api \"...\" in the nativeplugins module; other text is a dependencies line as is "
                    "(implementation platform('...bom:33.0.0')).");
            }
            if (settingsui::beginRow("android.gradle_repositories")) {
                drawStringList("Maven Repositories", "##GradleRepositories", m_gradleRepositories, "https://maven.example.com/releases",
                    "maven { url \"...\" } in allprojects.repositories.");
            }
            if (settingsui::beginRow("android.gradle_properties")) {
                drawStringList("Gradle Properties", "##GradleProperties", m_gradleProperties, "android.enableJetifier=true",
                    "key=value lines of gradle.properties (replacing a key already there).");
            }
            if (settingsui::beginRow("android.extra_permissions")) {
                drawStringList("Extra Permissions", "##ExtraPermissions", m_extraPermissions, "com.google.android.gms.permission.AD_ID",
                    "<uses-permission> by full name (the Player permissions list takes the common ones).");
            }
            if (settingsui::beginRow("android.manifest_xml")) {
                drawXmlSetting("Manifest XML", "##ManifestXml", m_manifestXml, 70.0f,
                    "Raw XML inside <manifest>, before <application> (<queries>, <uses-feature>, tools:node=\"remove\" entries).");
            }
            if (settingsui::beginRow("android.manifest_application_xml")) {
                drawXmlSetting("Application XML", "##ManifestApplicationXml", m_manifestApplicationXml, 70.0f,
                    "Raw XML inside <application> (an SDK's <meta-data>, <activity>, <provider>).");
            }
            ImGui::EndTable();
        }

        if (settingsFoldout("iOS", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##NativeIOSTable")) {
            if (settingsui::beginRow("ios.plist_entries")) {
                drawPlistEntries("##IOSPlist", m_iosPlistEntries, "Info.plist keys, replacing the export's (NSUserTrackingUsageDescription, GADApplicationIdentifier...).");
            }
            if (settingsui::beginRow("ios.plist_xml")) {
                drawXmlSetting("Info.plist XML", "##IOSPlistXml", m_iosPlistXml, 70.0f,
                    "Raw <key>..</key><value> pairs appended to the top dictionary (dictionaries, SKAdNetworkItems).");
            }
            if (settingsui::beginRow("ios.frameworks")) {
                drawStringList("Frameworks", "##IOSFrameworks", m_iosFrameworks, "StoreKit", "System frameworks linked; Name? links it weakly.");
            }

            if (settingsui::beginRow("ios.cocoapods")) {
                projectBeginSettingsRow("CocoaPods");
                {
                    ImGui::PushID("##Pods");
                    int remove = -1;
                    for (size_t i = 0; i < m_iosCocoapods.size(); i++) {
                        ImGui::PushID(static_cast<int>(i));
                        const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight()) / 3.0f - ImGui::GetStyle().ItemSpacing.x;
                        ImGui::SetNextItemWidth(width);
                        inputString("##name", m_iosCocoapods[i].name, "Pod name");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(width);
                        inputString("##version", m_iosCocoapods[i].version, "~> 1.0");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(width);
                        inputString("##source", m_iosCocoapods[i].source, "spec repository (optional)");
                        ImGui::SameLine();
                        if (ImGui::Button(ICON_FA_MINUS, ImVec2(ImGui::GetFrameHeight(), 0.0f))) remove = static_cast<int>(i);
                        ImGui::PopID();
                    }
                    if (remove >= 0) m_iosCocoapods.erase(m_iosCocoapods.begin() + remove);
                    if (ImGui::SmallButton(ICON_FA_PLUS)) m_iosCocoapods.emplace_back();
                    ImGui::PopID();
                }
                endSettingsRow("workspaces/xcode/Podfile: run pod install there and open JasperEngine.xcworkspace.");
            }

            if (settingsui::beginRow("ios.swift_packages")) {
                projectBeginSettingsRow("Swift Packages");
                {
                    ImGui::PushID("##Packages");
                    int remove = -1;
                    for (size_t i = 0; i < m_iosSwiftPackages.size(); i++) {
                        NativeSwiftPackage& package = m_iosSwiftPackages[i];
                        ImGui::PushID(static_cast<int>(i));
                        const float avail = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x * 3.0f;
                        ImGui::SetNextItemWidth(avail * 0.5f);
                        inputString("##url", package.url, "https://github.com/...");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(avail * 0.2f);
                        inputString("##version", package.version, "1.0.0");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(avail * 0.3f);
                        std::string products = joinList(package.products);
                        if (inputString("##products", products, "Product, Other")) package.products = splitList(products);
                        ImGui::SameLine();
                        if (ImGui::Button(ICON_FA_MINUS, ImVec2(ImGui::GetFrameHeight(), 0.0f))) remove = static_cast<int>(i);
                        ImGui::PopID();
                    }
                    if (remove >= 0) m_iosSwiftPackages.erase(m_iosSwiftPackages.begin() + remove);
                    if (ImGui::SmallButton(ICON_FA_PLUS)) m_iosSwiftPackages.emplace_back();
                    ImGui::PopID();
                }
                endSettingsRow("Repository URL, minimum version (up to the next major), the products linked (comma-separated).");
            }

            if (settingsui::beginRow("ios.entitlements_xml")) {
                drawXmlSetting("Entitlements", "##IOSEntitlements", m_iosEntitlementsXml, 60.0f,
                    "The <key>..</key><value> pairs of ios/JasperEngine.entitlements (CODE_SIGN_ENTITLEMENTS).");
            }
            const IOSProjectSettings iosDefaults;
            if (settingsui::beginRow("ios.deployment_target")) {
                if (projectBeginSettingsRow("Deployment Target", m_iosDeploymentTarget != iosDefaults.deploymentTarget)) {
                    m_iosDeploymentTarget = iosDefaults.deploymentTarget;
                }
                ImGui::SetNextItemWidth(Theme::dpi(100.0f));
                inputString("##IOSDeploymentTarget", m_iosDeploymentTarget);
                endSettingsRow("IPHONEOS_DEPLOYMENT_TARGET of every target: 15.0 or later.");
                const bool validTarget = m_iosDeploymentTarget.find_first_not_of("0123456789.") == std::string::npos &&
                                         std::count(m_iosDeploymentTarget.begin(), m_iosDeploymentTarget.end(), '.') == 1 &&
                                         m_iosDeploymentTarget.front() != '.' && m_iosDeploymentTarget.back() != '.' &&
                                         std::atof(m_iosDeploymentTarget.c_str()) >= 15.0;
                if (!validTarget) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Use a version like 15.0 or 16.4 (at least 15.0); the export uses 15.0.");
                }
            }
            ImGui::EndTable();
        }

        if (settingsFoldout("macOS") && beginSettingsTable("##NativeMacOSTable")) {
            if (settingsui::beginRow("macos.plist_entries")) {
                drawPlistEntries("##MacOSPlist", m_macosPlistEntries, "Info.plist keys of the macOS app, replacing the export's.");
            }
            if (settingsui::beginRow("macos.frameworks")) {
                drawStringList("Frameworks", "##MacOSFrameworks", m_macosFrameworks, "GameKit", "System frameworks linked; Name? links it weakly.");
            }
            ImGui::EndTable();
        }

        if (settingsFoldout("Web") && beginSettingsTable("##NativeWebTable")) {
            if (settingsui::beginRow("web.script_urls")) {
                drawStringList("Script URLs", "##WebScriptUrls", m_webScriptUrls, "https://sdk.example.com/sdk.js",
                    "Each a <script src> in the page <head>, before plugins/native/web/*.js.");
            }
            ImGui::EndTable();
        }
    }
    endSettingsPanel();
}

std::string ProjectSettingsWindow::inputError() const {
    // names are trimmed when their field is left (and on OK): no error while one is typed
    std::vector<InputManagerAction> actions = m_inputActions;
    for (InputManagerAction& action : actions) action.name = TagManager::trimName(action.name);
    return InputManagerFormat::validate(actions);
}

void ProjectSettingsWindow::commitInputEdit() {
    // a drag or a text edit is one step, taken when it ends
    if (ImGui::IsAnyItemActive()) return;
    const std::string json = InputManagerFormat::actionsToJson(m_inputActions);
    if (json == m_inputCommittedJson) return;
    m_inputUndo.push_back(m_inputCommitted);
    if (m_inputUndo.size() > 200) m_inputUndo.erase(m_inputUndo.begin());
    m_inputRedo.clear();
    m_inputCommitted = m_inputActions;
    m_inputCommittedJson = json;
}

void ProjectSettingsWindow::undoInputEdit() {
    commitInputEdit();
    if (m_inputUndo.empty()) return;
    m_inputRedo.push_back(m_inputActions);
    m_inputActions = m_inputUndo.back();
    m_inputUndo.pop_back();
    m_inputCommitted = m_inputActions;
    m_inputCommittedJson = InputManagerFormat::actionsToJson(m_inputActions);
    m_inputListen = InputListen();
}

void ProjectSettingsWindow::redoInputEdit() {
    if (m_inputRedo.empty()) return;
    m_inputUndo.push_back(m_inputActions);
    m_inputActions = m_inputRedo.back();
    m_inputRedo.pop_back();
    m_inputCommitted = m_inputActions;
    m_inputCommittedJson = InputManagerFormat::actionsToJson(m_inputActions);
    m_inputListen = InputListen();
}

void ProjectSettingsWindow::pollInputListen() {
    if (m_inputListen.action < 0) return;
    if (m_inputListen.action >= static_cast<int>(m_inputActions.size()) ||
        m_inputListen.binding >= static_cast<int>(m_inputActions[m_inputListen.action].bindings.size())) {
        m_inputListen = InputListen();
        return;
    }
    // the click that started it is not the control
    if (++m_inputListen.frames < 3) return;

    InputManagerBinding& binding = m_inputActions[m_inputListen.action].bindings[m_inputListen.binding];
    auto finish = [this]() { m_inputListen = InputListen(); };
    auto addControl = [&binding, this, &finish](InputControl control) {
        std::vector<InputControl>& list = m_inputListen.list == 1 ? binding.negative : binding.positive;
        if (std::find(list.begin(), list.end(), control) == list.end()) list.push_back(control);
        finish();
    };

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        finish();
        return;
    }

    // a gamepad axis binding takes an axis: the stick or trigger moved past half way
    if (m_inputListen.list == 2) {
        struct AxisKey { ImGuiKey key; int axis; };
        const AxisKey axisKeys[] = {
            {ImGuiKey_GamepadLStickLeft, D_GAMEPAD_AXIS_LEFT_X}, {ImGuiKey_GamepadLStickRight, D_GAMEPAD_AXIS_LEFT_X},
            {ImGuiKey_GamepadLStickUp, D_GAMEPAD_AXIS_LEFT_Y}, {ImGuiKey_GamepadLStickDown, D_GAMEPAD_AXIS_LEFT_Y},
            {ImGuiKey_GamepadRStickLeft, D_GAMEPAD_AXIS_RIGHT_X}, {ImGuiKey_GamepadRStickRight, D_GAMEPAD_AXIS_RIGHT_X},
            {ImGuiKey_GamepadRStickUp, D_GAMEPAD_AXIS_RIGHT_Y}, {ImGuiKey_GamepadRStickDown, D_GAMEPAD_AXIS_RIGHT_Y},
            {ImGuiKey_GamepadL2, D_GAMEPAD_AXIS_LEFT_TRIGGER}, {ImGuiKey_GamepadR2, D_GAMEPAD_AXIS_RIGHT_TRIGGER},
        };
        for (const AxisKey& entry : axisKeys) {
            if (ImGui::GetKeyData(entry.key)->AnalogValue >= 0.5f) {
                binding.axis = entry.axis;
                finish();
                return;
            }
        }
        // a pad the running game reads (Input.getPressedControl names axes "left_x+")
        std::string pressed = Input::getPressedControl();
        if (!pressed.empty() && (pressed.back() == '+' || pressed.back() == '-')) pressed.pop_back();
        const int axis = InputControlNames::parseGamepadAxis(pressed);
        if (axis >= 0) {
            binding.axis = axis;
            finish();
        }
        return;
    }

    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) continue;   // shortcut-lint: allow Listen in this modal captures any key for a game binding
        const int engineKey = PlayInput::engineKey(static_cast<ImGuiKey>(key));
        if (engineKey != D_KEY_UNKNOWN) {
            addControl({InputDevice::Key, engineKey});
            return;
        }
        const int button = PlayInput::engineGamepadButton(static_cast<ImGuiKey>(key));
        if (button >= 0) {
            addControl({InputDevice::GamepadButton, button});
            return;
        }
    }
    // a click on the dialog (Listen again, another field) is not a binding: click empty space
    for (int button = 0; button < 5 && !ImGui::IsAnyItemHovered(); button++) {
        if (ImGui::IsMouseClicked(button)) {
            addControl({InputDevice::Mouse, button});
            return;
        }
    }
    // a pad the running game reads
    InputControl control;
    if (InputControlNames::parseButton(Input::getPressedControl(), control) && control.device == InputDevice::GamepadButton)
        addControl(control);
}

void ProjectSettingsWindow::loadInputManagerActions() {
    m_inputActionsSource = m_inputActionsAsset;
    m_inputManagerReadable = true;
    std::vector<InputManagerAction> actions;
    if (m_inputActionsAsset.empty()) {
        // no asset: the game has Unity's defaults
        actions = InputManagerFormat::defaults();
    } else {
        std::string text;
        const InputActionsMigration& migration = m_project->getInputActionsMigration();
        if (migration.pending && migration.asset == m_inputActionsAsset) {
            text = migration.text;
        } else {
            std::ifstream file(m_project->getProjectPath() / m_inputActionsAsset, std::ios::binary);
            m_inputManagerReadable = file.good();
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        InputActionAssetDesc asset;
        std::string error;
        if (m_inputManagerReadable && InputActionAssetDesc::parse(text, asset, error)) {
            if (const InputActionMapDesc* map = asset.findMap(InputManagerFormat::MAP_NAME))
                actions = InputManagerFormat::fromMap(*map);
        } else {
            m_inputManagerReadable = false;
        }
    }
    m_inputOriginal = actions;
    m_inputActions = actions;
    m_inputCommitted = actions;
    m_inputCommittedJson = InputManagerFormat::actionsToJson(actions);
    m_inputUndo.clear();
    m_inputRedo.clear();
    m_inputSelected = std::clamp(m_inputSelected, 0, std::max(0, static_cast<int>(m_inputActions.size()) - 1));
    m_inputListen = InputListen();
}

void ProjectSettingsWindow::drawInputManagerSection() {
    // another Default Actions Asset picked above: its Input Manager map
    if (m_inputActionsAsset != m_inputActionsSource) loadInputManagerActions();

    ImGuiIO& io = ImGui::GetIO();
    // the section's own undo: the modal dialog keeps the editor's shortcuts off while it is open
    if (m_inputListen.action < 0 && !io.WantTextInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))) {  // shortcut-lint: allow the modal Project Settings dialog's own Input undo (the dispatcher is off while a popup is open)
            redoInputEdit();
        } else if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {  // shortcut-lint: allow the modal Project Settings dialog's own Input undo (the dispatcher is off while a popup is open)
            undoInputEdit();
        }
    }

    if (!settingsui::beginRow("input.manager_actions")) return;
    ImGui::Spacing();
    ImGui::SeparatorText("Input Manager");
    if (m_inputActionsAsset.empty()) {
        drawSectionNote("Unity's virtual axes and buttons: scripts read Input.getAxis(\"Horizontal\") and "
            "Input.getButtonDown(\"Jump\"), and the keyboard, gamepad and on-screen controls bound to an action add up. "
            "Without an actions asset the game has Unity's eleven defaults; OK with an edit here makes "
            "InputManager.inputactions (its \"Input Manager\" map) the Default Actions Asset. A game can rebind them at "
            "run time (Input.setBindings).");
    } else if (!m_inputManagerReadable) {
        drawSectionNote((m_inputActionsAsset.generic_string() + " cannot be read as an actions asset: fix it in Window > Input "
            "Actions (or pick another asset) to edit its Input Manager map here.").c_str());
        return;
    } else {
        drawSectionNote(("The \"Input Manager\" map of " + m_inputActionsAsset.generic_string() + ": Unity's virtual axes and "
            "buttons, read by Input.getAxis(\"Horizontal\") and Input.getButtonDown(\"Jump\") (names the map does not have "
            "are looked up in the asset's other maps). Window > Input Actions edits the same map with every Input Actions "
            "feature; a game can rebind them at run time (Input.setBindings).").c_str());
    }

    ImGui::BeginDisabled(m_inputUndo.empty() && InputManagerFormat::actionsToJson(m_inputActions) == m_inputCommittedJson);
    if (ImGui::Button(ICON_FA_ROTATE_LEFT " Undo")) undoInputEdit();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Undo the last Input Manager change (Ctrl+Z)");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_inputRedo.empty());
    if (ImGui::Button(ICON_FA_ROTATE_RIGHT " Redo")) redoInputEdit();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Redo (Ctrl+Y)");
    ImGui::SameLine();
    if (ImGui::Button("Reset to Unity Defaults")) {
        m_inputActions = InputManagerFormat::defaults();
        m_inputSelected = 0;
        m_inputListen = InputListen();
    }
    ImGui::SetItemTooltip("Horizontal, Vertical, Fire1-3, Jump, Mouse X/Y, Mouse ScrollWheel, Submit and Cancel "
                          "as Unity's Input Manager has them");

    const std::string problem = inputError();
    if (!problem.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", problem.c_str());
    }

    const float height = std::max(Theme::dpi(320.0f), ImGui::GetContentRegionAvail().y);
    const float listWidth = std::min(Theme::dpi(220.0f), ImGui::GetContentRegionAvail().x * 0.35f);
    ImGui::BeginChild("##InputActionList", ImVec2(listWidth, height), ImGuiChildFlags_Borders);
    drawInputActionList();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##InputActionDetails", ImVec2(0, height), ImGuiChildFlags_None);
    if (m_inputSelected >= 0 && m_inputSelected < static_cast<int>(m_inputActions.size())) {
        drawInputActionDetails(m_inputActions[m_inputSelected], m_inputSelected);
    } else {
        ImGui::TextDisabled("No action selected. Add one with +.");
    }
    ImGui::EndChild();

    pollInputListen();
    commitInputEdit();
}

void ProjectSettingsWindow::drawInputActionList() {
    const float buttonWidth = ImGui::GetFrameHeight();
    if (ImGui::Button(ICON_FA_PLUS "##AddInputAction", ImVec2(buttonWidth, 0))) {
        InputManagerAction action;
        action.name = inputUniqueName(m_inputActions, "New Action");
        action.bindings.push_back(InputManagerFormat::makeBinding(InputManagerBindingKind::Buttons));
        const int at = std::clamp(m_inputSelected + 1, 0, static_cast<int>(m_inputActions.size()));
        m_inputActions.insert(m_inputActions.begin() + at, action);
        m_inputSelected = at;
        m_inputSearch[0] = '\0';
    }
    ImGui::SetItemTooltip("Add an action");
    ImGui::SameLine();
    const bool hasSelection = m_inputSelected >= 0 && m_inputSelected < static_cast<int>(m_inputActions.size());
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button(ICON_FA_COPY "##DuplicateInputAction", ImVec2(buttonWidth, 0)) && hasSelection) {
        InputManagerAction copy = m_inputActions[m_inputSelected];
        copy.name = inputUniqueName(m_inputActions, copy.name);
        m_inputActions.insert(m_inputActions.begin() + m_inputSelected + 1, copy);
        m_inputSelected++;
    }
    ImGui::SetItemTooltip("Duplicate the action");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH "##DeleteInputAction", ImVec2(buttonWidth, 0)) && hasSelection) {
        m_inputActions.erase(m_inputActions.begin() + m_inputSelected);
        m_inputSelected = std::min(m_inputSelected, static_cast<int>(m_inputActions.size()) - 1);
        m_inputListen = InputListen();
    }
    ImGui::SetItemTooltip("Delete the action (scripts reading it then log an error and read 0)");
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##InputActionSearch", ICON_FA_MAGNIFYING_GLASS " Search", m_inputSearch, sizeof(m_inputSearch));
    std::string search = m_inputSearch;
    std::transform(search.begin(), search.end(), search.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const bool playing = inputPlaying(m_project);
    int moveFrom = -1, moveTo = -1;
    for (int i = 0; i < static_cast<int>(m_inputActions.size()); i++) {
        const InputManagerAction& action = m_inputActions[i];
        std::string lower = action.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!search.empty() && lower.find(search) == std::string::npos) continue;

        ImGui::PushID(i);
        // a live dot while the game plays: pressed (green) or moved off rest (dim)
        std::string label = action.name.empty() ? std::string("(no name)") : action.name;
        if (playing) {
            InputActions::State state;
            if (InputActions::getState(action.name, state)) {
                ImVec4 color = state.pressed ? ImVec4(0.35f, 0.85f, 0.4f, 1.0f)
                             : (std::fabs(state.value) > 0.0005f ? ImVec4(0.9f, 0.75f, 0.3f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::TextColored(color, ICON_FA_CIRCLE);
                ImGui::SameLine();
            }
        }
        if (ImGui::Selectable(label.c_str(), i == m_inputSelected)) {
            m_inputSelected = i;
        }
        // drag to reorder (the order is the one getActionNames lists)
        if (search.empty() && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("INPUT_ACTION_INDEX", &i, sizeof(int));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (search.empty() && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INPUT_ACTION_INDEX")) {
                moveFrom = *static_cast<const int*>(payload->Data);
                moveTo = i;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
        InputManagerAction moved = m_inputActions[moveFrom];
        m_inputActions.erase(m_inputActions.begin() + moveFrom);
        m_inputActions.insert(m_inputActions.begin() + moveTo, moved);
        m_inputSelected = moveTo;
        m_inputListen = InputListen();
    }
}

void ProjectSettingsWindow::drawInputActionDetails(InputManagerAction& action, int actionIndex) {
    ImGui::PushID(actionIndex);
    if (beginSettingsTable("##InputActionTable")) {
        projectBeginSettingsRow("Name");
        char name[128];
        snprintf(name, sizeof(name), "%s", action.name.c_str());
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        if (ImGui::InputText("##InputActionName", name, sizeof(name))) action.name = name;
        if (ImGui::IsItemDeactivatedAfterEdit()) action.name = TagManager::trimName(action.name);
        endSettingsRow("What scripts pass to Input.getAxis / getButton. Unique, case-sensitive.");

        if (projectBeginSettingsRow("Type", action.type != InputManagerActionType::Axis)) action.type = InputManagerActionType::Axis;
        int type = static_cast<int>(action.type);
        const char* typeLabels[] = { "Axis", "Button", "Vector2" };
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        if (ImGui::Combo("##InputActionType", &type, typeLabels, 3)) action.type = static_cast<InputManagerActionType>(type);
        endSettingsRow("Axis: -1..1 from a positive and a negative side. Button: 0..1, negative buttons are not read. "
            "Vector2: an Input Actions VECTOR2 (a stick, a 2D composite; Window > Input Actions edits its bindings).");

        projectBeginSettingsRow("Description");
        char description[256];
        snprintf(description, sizeof(description), "%s", action.description.c_str());
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        if (ImGui::InputText("##InputActionDescription", description, sizeof(description))) action.description = description;
        endSettingsRow("For you and settings screens; the game does not read it.");

        if (projectBeginSettingsRow("Press Point", action.pressPoint != 0.5f)) action.pressPoint = 0.5f;
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::DragFloat("##InputActionPressPoint", &action.pressPoint, 0.01f, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        endSettingsRow("How far a gamepad axis or an on-screen stick goes before getButton is true.");

        if (inputPlaying(m_project)) {
            projectBeginSettingsRow("Live");
            InputActions::State state;
            if (InputActions::getState(action.name, state)) {
                const float low = action.type == InputManagerActionType::Axis ? -1.0f : 0.0f;
                const float fraction = std::clamp((state.value - low) / (1.0f - low), 0.0f, 1.0f);
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%.3f", state.value);
                ImGui::TextColored(state.pressed ? ImVec4(0.35f, 0.85f, 0.4f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], ICON_FA_CIRCLE);
                ImGui::SetItemTooltip(state.pressed ? "Pressed (getButton)" : "Not pressed");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-helpMarkerWidth());
                ImGui::ProgressBar(fraction, ImVec2(-helpMarkerWidth(), 0), overlay);
                endSettingsRow("Input.getAxis of the running game (its definition: what OK last applied).");
            } else {
                ImGui::TextDisabled("Not in the running game yet: OK applies it");
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    int removeBinding = -1, moveUp = -1, moveDown = -1;
    for (int b = 0; b < static_cast<int>(action.bindings.size()); b++) {
        ImGui::PushID(b);
        const std::string header = "Binding " + std::to_string(b + 1) + ": " + inputBindingSummary(action, action.bindings[b]) +
                                   "###InputBinding";
        const bool open = Theme::foldoutHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (open) {
            const float buttonWidth = ImGui::GetFrameHeight();
            ImGui::BeginDisabled(b == 0);
            if (ImGui::Button(ICON_FA_ARROW_UP, ImVec2(buttonWidth, 0))) moveUp = b;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Move up");
            ImGui::SameLine();
            ImGui::BeginDisabled(b + 1 >= static_cast<int>(action.bindings.size()));
            if (ImGui::Button(ICON_FA_ARROW_DOWN, ImVec2(buttonWidth, 0))) moveDown = b;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Move down");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH " Remove")) removeBinding = b;
            drawInputBinding(action, actionIndex, b);
        }
        ImGui::PopID();
    }
    if (removeBinding >= 0) {
        action.bindings.erase(action.bindings.begin() + removeBinding);
        m_inputListen = InputListen();
    } else if (moveUp > 0) {
        std::swap(action.bindings[moveUp], action.bindings[moveUp - 1]);
        m_inputListen = InputListen();
    } else if (moveDown >= 0) {
        std::swap(action.bindings[moveDown], action.bindings[moveDown + 1]);
        m_inputListen = InputListen();
    }

    if (ImGui::Button(ICON_FA_PLUS " Add Binding")) ImGui::OpenPopup("##AddInputBinding");
    // the Input Manager's three kinds (other bindings: Window > Input Actions)
    if (ImGui::BeginPopup("##AddInputBinding")) {
        for (int kind = 0; kind < 3; kind++) {
            if (ImGui::MenuItem(inputKindLabels[kind])) {
                action.bindings.push_back(InputManagerFormat::makeBinding(static_cast<InputManagerBindingKind>(kind)));
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void ProjectSettingsWindow::drawInputControls(const char* id, std::vector<InputControl>& controls, int actionIndex, int bindingIndex, int list) {
    ImGui::PushID(id);
    int remove = -1;
    for (size_t c = 0; c < controls.size(); c++) {
        ImGui::PushID(static_cast<int>(c));
        const std::string label = InputControlNames::name(controls[c]) + " " ICON_FA_XMARK;
        if (ImGui::SmallButton(label.c_str())) remove = static_cast<int>(c);
        ImGui::SetItemTooltip("Remove");
        ImGui::SameLine();
        ImGui::PopID();
    }
    if (remove >= 0) controls.erase(controls.begin() + remove);

    if (ImGui::SmallButton(ICON_FA_PLUS)) {
        m_inputControlFilter[0] = '\0';
        ImGui::OpenPopup("##PickControl");
    }
    ImGui::SetItemTooltip("Add a key, mouse button or gamepad button");
    if (ImGui::BeginPopup("##PickControl")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Theme::dpi(200.0f));
        ImGui::InputTextWithHint("##ControlFilter", "Filter (space, mouse, gamepad...)", m_inputControlFilter, sizeof(m_inputControlFilter));
        const std::string filter = m_inputControlFilter;
        ImGui::BeginChild("##ControlNames", ImVec2(Theme::dpi(220.0f), Theme::dpi(260.0f)));
        auto group = [&](const char* title, const std::vector<std::string>& names) {
            bool shownTitle = false;
            for (const std::string& name : names) {
                if (!filter.empty() && name.find(filter) == std::string::npos) continue;
                if (!shownTitle) {
                    ImGui::TextDisabled("%s", title);
                    shownTitle = true;
                }
                if (ImGui::Selectable(name.c_str())) {
                    InputControl control;
                    if (InputControlNames::parseButton(name, control) &&
                            std::find(controls.begin(), controls.end(), control) == controls.end())
                        controls.push_back(control);
                    ImGui::CloseCurrentPopup();
                }
            }
        };
        group("Keys", InputControlNames::keyNames());
        group("Mouse", InputControlNames::mouseButtonControlNames());
        group("Gamepad", InputControlNames::gamepadButtonControlNames());
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool listening = m_inputListen.action == actionIndex && m_inputListen.binding == bindingIndex && m_inputListen.list == list;
    if (listening) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    if (ImGui::SmallButton(listening ? "Press a key or button... (Esc)" : ICON_FA_EAR_LISTEN " Listen")) {
        if (listening) {
            m_inputListen = InputListen();
        } else {
            m_inputListen = InputListen();
            m_inputListen.action = actionIndex;
            m_inputListen.binding = bindingIndex;
            m_inputListen.list = list;
        }
    }
    if (listening) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Adds the next key or gamepad button you press, or a mouse button clicked on empty space "
                          "(a pad the playing game reads too). Esc or Listen again cancels.");
    ImGui::PopID();
}

void ProjectSettingsWindow::drawInputBinding(InputManagerAction& action, int actionIndex, int bindingIndex) {
    InputManagerBinding& binding = action.bindings[bindingIndex];
    if (!beginSettingsTable("##InputBindingTable")) return;

    // another Input Actions binding (a path, a 2D composite, modifiers...): Window > Input Actions edits it
    if (binding.kind == InputManagerBindingKind::Binding) {
        projectBeginSettingsRow("Binding");
        ImGui::TextWrapped("%s", binding.binding.c_str());
        endSettingsRow("An Input Actions binding the Input Manager's kinds cannot show: Window > Input Actions edits it.");
        ImGui::EndTable();
        return;
    }

    projectBeginSettingsRow("Kind");
    int kind = static_cast<int>(binding.kind);
    ImGui::SetNextItemWidth(-helpMarkerWidth());
    if (ImGui::Combo("##BindingKind", &kind, inputKindLabels, 3) && kind != static_cast<int>(binding.kind)) {
        binding = InputManagerFormat::makeBinding(static_cast<InputManagerBindingKind>(kind));
        m_inputListen = InputListen();
    }
    endSettingsRow("Unity's Input Manager types: Key or Mouse Button, Joystick Axis, Mouse Movement.");

    const InputManagerBinding defaults = InputManagerFormat::makeBinding(binding.kind);
    auto gamepadRow = [&binding]() {
        if (projectBeginSettingsRow("Gamepad", binding.gamepad != -1)) binding.gamepad = -1;
        std::string preview = binding.gamepad < 0 ? std::string("Any") : "Gamepad " + std::to_string(binding.gamepad);
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        if (ImGui::BeginCombo("##BindingGamepad", preview.c_str())) {
            if (ImGui::Selectable("Any", binding.gamepad < 0)) binding.gamepad = -1;
            for (int pad = 0; pad < 16; pad++) {
                if (ImGui::Selectable(("Gamepad " + std::to_string(pad)).c_str(), binding.gamepad == pad)) binding.gamepad = pad;
            }
            ImGui::EndCombo();
        }
        endSettingsRow("The gamepad id (0-15) whose buttons and axes this binding reads; Any reads every connected pad.");
    };

    switch (binding.kind) {
        case InputManagerBindingKind::Buttons: {
            projectBeginSettingsRow(action.type != InputManagerActionType::Button ? "Positive Buttons" : "Buttons");
            drawInputControls("##Positive", binding.positive, actionIndex, bindingIndex, 0);
            if (action.type != InputManagerActionType::Button) {
                projectBeginSettingsRow("Negative Buttons");
                drawInputControls("##Negative", binding.negative, actionIndex, bindingIndex, 1);
            }
            gamepadRow();
            inputNumberRow("Gravity", "##Gravity", binding.gravity, defaults.gravity, 0.1f, 1000.0f,
                "Speed in units per second that the value falls back to 0 when no button is pressed (0: at once).");
            inputNumberRow("Dead", "##Dead", binding.dead, defaults.dead, 0.001f, 0.999f,
                "Values smaller than this read 0.");
            inputNumberRow("Sensitivity", "##Sensitivity", binding.sensitivity, defaults.sensitivity, 0.1f, 1000.0f,
                "Speed in units per second that the value moves to the target (0: at once).");
            if (projectBeginSettingsRow("Snap", binding.snap)) binding.snap = false;
            ImGui::Checkbox("##Snap", &binding.snap);
            endSettingsRow("Pressing the opposite side jumps to 0 first instead of sliding through it.");
            break;
        }
        case InputManagerBindingKind::GamepadAxis: {
            projectBeginSettingsRow("Axis");
            int axis = binding.axis;
            const std::vector<std::string>& axes = InputControlNames::gamepadAxisNames();
            ImGui::SetNextItemWidth(Theme::dpi(160.0f));
            if (ImGui::BeginCombo("##BindingAxis", InputControlNames::gamepadAxisName(axis).c_str())) {
                for (int a = 0; a < static_cast<int>(axes.size()); a++) {
                    if (ImGui::Selectable(axes[a].c_str(), a == axis)) binding.axis = a;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const bool listening = m_inputListen.action == actionIndex && m_inputListen.binding == bindingIndex && m_inputListen.list == 2;
            if (ImGui::SmallButton(listening ? "Move a stick or trigger... (Esc)" : ICON_FA_EAR_LISTEN " Listen##Axis")) {
                m_inputListen = InputListen();
                if (!listening) {
                    m_inputListen.action = actionIndex;
                    m_inputListen.binding = bindingIndex;
                    m_inputListen.list = 2;
                }
            }
            endSettingsRow("Sticks read -1..1 (up is -1 on the Y axes: Invert makes it positive), triggers 0..1.");
            gamepadRow();
            inputNumberRow("Dead", "##Dead", binding.dead, defaults.dead, 0.01f, 0.999f,
                "The dead zone: below it the axis reads 0, above it the value is rescaled to start at 0.");
            inputNumberRow("Sensitivity", "##Sensitivity", binding.sensitivity, defaults.sensitivity, 0.01f, 100.0f,
                "Multiplies the value.");
            break;
        }
        case InputManagerBindingKind::MouseDelta: {
            projectBeginSettingsRow("Axis");
            int axis = static_cast<int>(binding.mouseAxis);
            ImGui::SetNextItemWidth(-helpMarkerWidth());
            if (ImGui::Combo("##BindingMouseAxis", &axis, mouseAxisLabels, 3)) binding.mouseAxis = static_cast<MouseDeltaAxis>(axis);
            endSettingsRow("Mouse movement this frame in canvas px (wheel: steps); not clamped to -1..1.");
            inputNumberRow("Sensitivity", "##Sensitivity", binding.sensitivity, defaults.sensitivity, 0.01f, 100.0f,
                "Multiplies the movement (Unity's default 0.1).");
            break;
        }
        case InputManagerBindingKind::Binding:
            // shown read-only above
            break;
    }
    if (projectBeginSettingsRow("Invert", binding.invert)) binding.invert = false;
    ImGui::Checkbox("##Invert", &binding.invert);
    endSettingsRow("Flips the sign.");
    ImGui::EndTable();
}

bool ProjectSettingsWindow::applySettings() {
    if (!hasAndroidAbiSelection(m_androidAbiArmeabiV7a, m_androidAbiArm64V8a, m_androidAbiX86, m_androidAbiX86_64)) {
        return false;
    }
    if (!tagsAndLayersError().empty()) {
        return false;
    }
    if (!inputError().empty()) {
        return false;
    }
    if (!m_globalsEdit.error().empty()) {
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
    if (m_project->getUIScale() != m_uiScale)
        m_project->setUIScale(m_uiScale);
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

    PresentationSettings& presentation = m_project->getPresentationSettings();
    presentation.defaultOrientation = defaultOrientationValues[std::clamp(m_defaultOrientationIndex, 0, defaultOrientationCount - 1)];
    presentation.allowedOrientations = m_allowedOrientations ? m_allowedOrientations : PresentationSettings{}.allowedOrientations;
    presentation.keepScreenOn = m_keepScreenOn;
    presentation.targetFrameRate = m_targetFrameRate <= 0 ? -1 : std::min(m_targetFrameRate, 1000);

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
    application.allowInsecureHttp = m_allowInsecureHttp;

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
    m_project->setPhysics3DWorldSettings(m_physics3DWorld);
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
    web.scriptUrls = nonEmpty(m_webScriptUrls);
    web.viewportFitCover = m_webViewportFitCover;

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
    macOS.plistEntries = plistMap(m_macosPlistEntries);
    macOS.frameworks = nonEmpty(m_macosFrameworks);

    IOSProjectSettings& ios = m_project->getIOSProjectSettings();
    applyOverride(ios.applicationName, m_iosApplicationNameBuffer, inheritedName);
    applyOverride(ios.bundleIdentifier, m_iosBundleIdentifierBuffer, inheritedIdentifier, isAppleBundleIdentifierValid);
    applyAppleVersion(ios.versionName, m_iosVersionNameBuffer, inheritedVersion, isAppleVersionValid);
    applyAppleVersion(ios.buildNumber, m_iosBuildNumberBuffer, inheritedBuild, isAppleBuildValid);
    ios.icon = m_iosIcon;
    ios.plistEntries = plistMap(m_iosPlistEntries);
    ios.plistXml = m_iosPlistXml;
    ios.frameworks = nonEmpty(m_iosFrameworks);
    ios.cocoapods.clear();
    for (NativeCocoaPod pod : m_iosCocoapods) {
        pod.name = trimmed(pod.name);
        pod.version = trimmed(pod.version);
        pod.source = trimmed(pod.source);
        if (!pod.name.empty()) ios.cocoapods.push_back(pod);
    }
    ios.swiftPackages.clear();
    for (NativeSwiftPackage package : m_iosSwiftPackages) {
        package.url = trimmed(package.url);
        package.version = trimmed(package.version);
        if (!package.url.empty()) ios.swiftPackages.push_back(package);
    }
    ios.entitlementsXml = m_iosEntitlementsXml;
    ios.deploymentTarget = trimmed(m_iosDeploymentTarget).empty() ? IOSProjectSettings().deploymentTarget : trimmed(m_iosDeploymentTarget);
    ios.hideStatusBar = m_iosHideStatusBar;
    ios.hideHomeIndicator = m_iosHideHomeIndicator;
    ios.supportsHighRefreshRate = m_iosSupportsHighRefreshRate;
    ios.admobEnabled = m_iosAdmobEnabled;
    applyTextBuffer(ios.admobAppId, m_iosAdmobAppIdBuffer);
    applyTextBuffer(ios.teamId, m_iosTeamIdBuffer);
    ios.exportMethod = iosExportMethods[std::clamp(m_iosExportMethodIndex, 0, static_cast<int>(IM_ARRAYSIZE(iosExportMethods)) - 1)];

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
    android.abiArmeabiV7a = m_androidAbiArmeabiV7a;
    android.abiArm64V8a = m_androidAbiArm64V8a;
    android.abiX86 = m_androidAbiX86;
    android.abiX86_64 = m_androidAbiX86_64;
    android.permissions = m_androidPermissions;
    android.internetAccess = m_androidInternetAccessIndex == 1 ? AndroidInternetAccess::Require : AndroidInternetAccess::Auto;
    android.gradleDependencies = nonEmpty(m_gradleDependencies);
    android.gradleRepositories = nonEmpty(m_gradleRepositories);
    android.gradleProperties = nonEmpty(m_gradleProperties);
    android.extraPermissions = nonEmpty(m_extraPermissions);
    android.manifestXml = m_manifestXml;
    android.manifestApplicationXml = m_manifestApplicationXml;
    android.allowBackup = m_androidAllowBackup;
    android.fullscreen = m_androidFullscreen;
    android.admobEnabled = m_androidAdmobEnabled;
    applyTextBuffer(android.admobAppId, m_androidAdmobAppIdBuffer);
    android.packageFormat = androidPackageFormats[std::clamp(m_androidPackageFormatIndex, 0, static_cast<int>(IM_ARRAYSIZE(androidPackageFormats)) - 1)];
    android.keystorePath = m_androidKeystorePath;
    applyTextBuffer(android.keystoreAlias, m_androidKeystoreAliasBuffer);

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

    // Debug: one undoable step of the project history
    if (m_debugSettings != m_project->getDebugSettings()) {
        auto* command = new DebugSettingsCmd(m_project, m_project->getDebugSettings(), m_debugSettings);
        command->setNoMerge();
        m_project->getProjectCommandHistory()->addCommand(command);
    }

    m_project->setPackNativeResources(m_packNativeResources);
    m_project->setVersionControlMetadata(m_versionControlMetadata);
    m_project->setBuildMethod(buildMethodNames[std::clamp(m_buildMethodIndex, 0, 2)]);
    m_project->setPackContent(m_packContent);
    m_project->setStripEngineCode(m_stripEngineCode);
    m_project->setVcsProjectSettings(m_vcs);

    // a name typed into New Tag Name but not added yet is kept rather than lost
    std::vector<std::string> tags = m_tags;
    if (m_newTagBuffer[0] != '\0' && newTagError().empty()) {
        tags.push_back(TagManager::trimName(m_newTagBuffer));
    }
    // GameObjects keep a removed tag, as in Unity; scripts then see it as not defined
    m_project->setTagsAndLayers(tags, m_layerNames);
    m_project->setLayerCollisionMatrices(m_layerCollision3D, m_layerCollision2D);

    // Audio: one undoable step too
    {
        AudioSettings audio = m_audioSettings;
        audio.mixer = FileUtils::pathToGenericUtf8(FileUtils::pathFromUtf8(m_audioMixerBuffer));
        while (!audio.mixer.empty() && audio.mixer.back() == ' ') audio.mixer.pop_back();
        // "assets/audio/Main.mixer" typed as the Project window shows it
        if (audio.mixer.rfind("assets/", 0) == 0 && !std::filesystem::exists(m_project->resolveAssetPath(FileUtils::pathFromUtf8(audio.mixer)))) {
            audio.mixer = audio.mixer.substr(7);
        }
        audio.defaultOutputGroup = m_audioDefaultGroupBuffer;
        if (audio != m_project->getAudioSettings()) {
            auto* command = new AudioSettingsCmd(m_project, m_project->getAudioSettings(), audio);
            command->setNoMerge();
            m_project->getProjectCommandHistory()->addCommand(command);
        }
    }

    // Input Manager: one undoable step of the project history (Edit > Undo with the Project
    // window focused, MCP undo scope project), into the asset just set (made when there is none)
    for (InputManagerAction& action : m_inputActions) action.name = TagManager::trimName(action.name);
    if (m_inputManagerReadable && InputManagerFormat::actionsToJson(m_inputActions) != InputManagerFormat::actionsToJson(m_inputOriginal)) {
        auto* command = new InputActionsCmd(m_project, m_inputActions);
        command->setNoMerge();
        m_project->getProjectCommandHistory()->addCommand(command);
    }

    // Shader Globals: one undoable step too; a renamed global's bindings follow it
    m_globalsEdit.commit();

    // the standalone build's main.cpp bakes these settings in
    m_project->markGeneratedSourcesDirty();

    return m_project->saveProjectFile();
}


// ---------------------------------------------------------------------------- Shader Globals

void ProjectSettingsWindow::refreshShaderGlobalUses() {
    m_shaderGlobalUses = m_project ? ShaderGlobalsUsage::find(m_project) : std::vector<ShaderGlobalsUsage::Use>();
    m_shaderGlobalUsesScanned = true;
}

static const ShaderGlobalType shaderGlobalTypes[] = {
    ShaderGlobalType::Float, ShaderGlobalType::Vector2, ShaderGlobalType::Vector3, ShaderGlobalType::Vector4,
    ShaderGlobalType::Color, ShaderGlobalType::Matrix4, ShaderGlobalType::FloatArray, ShaderGlobalType::Vector4Array,
    ShaderGlobalType::Texture2D, ShaderGlobalType::TextureCube};
static const char* const shaderGlobalTypeLabels[] = {
    "Float", "Vector2", "Vector3", "Vector4", "Color", "Matrix4", "Float Array", "Vector4 Array", "Texture 2D", "Texture Cube"};

bool ProjectSettingsWindow::drawShaderGlobalValue(const char* id, ShaderGlobalType type, unsigned int arraySize, std::vector<Vector4>& values) {
    const unsigned int slots = ShaderGlobals::getSlotCount(type, arraySize);
    if (values.size() < slots) values.resize(slots, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    bool changed = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-1);
    switch (type) {
        case ShaderGlobalType::Float:
            changed = ImGui::DragFloat("##value", &values[0].x, 0.01f);
            break;
        case ShaderGlobalType::Vector2:
        case ShaderGlobalType::Vector3:
        case ShaderGlobalType::Vector4: {
            float fields[4] = {values[0].x, values[0].y, values[0].z, values[0].w};
            if (ImGui::DragScalarN("##value", ImGuiDataType_Float, fields, ShaderGlobals::getComponentCount(type), 0.01f)) {
                values[0] = Vector4(fields[0], fields[1], fields[2], fields[3]);
                changed = true;
            }
            break;
        }
        case ShaderGlobalType::Color: {
            // edited as sRGB, stored linear (alpha as is): the Color window (ui::colorField)
            ui::ColorFieldOptions colorOptions;
            colorOptions.label = "Shader Global Color";
            Vector4 color = values[0];
            const ui::EditPhase phase = ui::colorField("##value", color, colorOptions);
            if (phase == ui::EditPhase::Changing || phase == ui::EditPhase::Cancelled || phase == ui::EditPhase::Applied) {
                values[0] = color;
                changed = true;
            }
            break;
        }
        case ShaderGlobalType::Matrix4:
        case ShaderGlobalType::FloatArray:
        case ShaderGlobalType::Vector4Array: {
            // a summary that opens the grid / element list
            std::string summary;
            if (type == ShaderGlobalType::Matrix4) {
                const bool identity = values[0] == Vector4(1, 0, 0, 0) && values[1] == Vector4(0, 1, 0, 0) &&
                                      values[2] == Vector4(0, 0, 1, 0) && values[3] == Vector4(0, 0, 0, 1);
                summary = identity ? "Identity" : "Matrix";
            } else {
                summary = std::to_string(slots) + (type == ShaderGlobalType::FloatArray ? " floats" : " vectors");
            }
            if (ImGui::Button((summary + "  " ICON_FA_PEN "##edit").c_str(), ImVec2(-1, 0)))
                ImGui::OpenPopup("##values_popup");
            if (ImGui::BeginPopup("##values_popup")) {
                if (type == ShaderGlobalType::Matrix4) {
                    // rows on screen, stored column-major (values[c] is column c)
                    ImGui::TextDisabled("Rows of the matrix (stored by column)");
                    auto at = [&values](int column, int row) -> float& {
                        Vector4& v = values[column];
                        return row == 0 ? v.x : row == 1 ? v.y : row == 2 ? v.z : v.w;
                    };
                    for (int r = 0; r < 4; r++) {
                        float row[4] = {at(0, r), at(1, r), at(2, r), at(3, r)};
                        ImGui::SetNextItemWidth(Theme::dpi(320.0f));
                        if (ImGui::DragFloat4(("##row" + std::to_string(r)).c_str(), row, 0.01f)) {
                            for (int c = 0; c < 4; c++) at(c, r) = row[c];
                            changed = true;
                        }
                    }
                    if (ImGui::Button("Identity")) {
                        values = {Vector4(1, 0, 0, 0), Vector4(0, 1, 0, 0), Vector4(0, 0, 1, 0), Vector4(0, 0, 0, 1)};
                        changed = true;
                    }
                } else {
                    ImGui::BeginChild("##elements", ImVec2(Theme::dpi(360.0f), std::min(Theme::dpi(300.0f), ImGui::GetFrameHeightWithSpacing() * slots + 4)));
                    for (unsigned int e = 0; e < slots; e++) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("[%u]", e);
                        ImGui::SameLine(Theme::dpi(40.0f));
                        ImGui::SetNextItemWidth(-1);
                        if (type == ShaderGlobalType::FloatArray) {
                            changed |= ImGui::DragFloat(("##e" + std::to_string(e)).c_str(), &values[e].x, 0.01f);
                        } else {
                            float fields[4] = {values[e].x, values[e].y, values[e].z, values[e].w};
                            if (ImGui::DragFloat4(("##e" + std::to_string(e)).c_str(), fields, 0.01f)) {
                                values[e] = Vector4(fields[0], fields[1], fields[2], fields[3]);
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndChild();
                }
                ImGui::EndPopup();
            }
            break;
        }
        default:
            break;
    }
    ImGui::PopID();
    return changed;
}

bool ProjectSettingsWindow::drawShaderGlobalTexture(const char* id, ShaderGlobalType type, std::string& path) {
    bool changed = false;
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + style.FramePadding.x * 2.0f;
    const std::string shown = path.empty() ? (type == ShaderGlobalType::TextureCube ? "Black" : "White") : fs::path(path).filename().string();
    ImGui::PushStyleColor(ImGuiCol_Text, path.empty() ? style.Colors[ImGuiCol_TextDisabled] : style.Colors[ImGuiCol_Text]);
    ImGui::Button((shown + "##slot").c_str(), ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - (buttonWidth + style.ItemSpacing.x) * 2.0f), 0));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nDrop an image here%s", path.empty() ? "No texture: the shader reads the fallback" : path.c_str(),
                          type == ShaderGlobalType::TextureCube ? " (a cube map: one image with the six faces)" : "");
    auto assign = [&](const std::string& absolutePath) {
        if (!m_project->isInsideAssetsPath(absolutePath)) {
            Backend::getApp().registerOutsideAssetsAlert(absolutePath);
            return;
        }
        path = m_project->normalizeToAssetsRelative(absolutePath).generic_string();
        changed = true;
    };
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            std::vector<std::string> received = Util::getStringsFromPayload(payload);
            if (!received.empty() && Util::isImageFile(received[0])) assign(received[0]);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "##pick")) {
        const std::string selected = FileDialogs::openFileDialog(m_project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
        if (!selected.empty()) assign(selected);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(path.empty());
    if (ImGui::Button(ICON_FA_XMARK "##clear")) {
        path.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

void ProjectSettingsWindow::drawShaderGlobals() {
    if (!m_shaderGlobalUsesScanned) refreshShaderGlobalUses();
    const bool playing = m_project && m_project->isAnyScenePlaying();
    const ImGuiStyle& style = ImGui::GetStyle();

    drawSectionNote("Values every shader can read: a member of the same name in a u_vs_globals / u_fs_globals block (or a texture "
                    "uniform of the name), or a library shader parameter bound to it with the link button in the Inspector. Scripts set "
                    "them with Shader.setGlobal*; Play starts and stops with these defaults.");

    bool edited = false;
    std::vector<ShaderGlobalsEdit::Row>& shaderGlobalRows = m_globalsEdit.rows();
    if (ImGui::Button(ICON_FA_PLUS " Add")) {
        m_globalsEdit.add();
        edited = true;
    }
    ImGui::SameLine();
    // the declarations to paste into a shader: the selected rows, else all
    std::vector<std::string> selected;
    for (const ShaderGlobalsEdit::Row& row : shaderGlobalRows) {
        if (row.selected) selected.push_back(row.decl.name);
    }
    const std::vector<ShaderGlobalDecl> decls = m_globalsEdit.decls();
    ImGui::BeginDisabled(decls.empty());
    if (ImGui::Button(ICON_FA_COPY " Copy GLSL: Vertex")) ImGui::SetClipboardText(ShaderGlobals::getGlslBlock(decls, ShaderStageType::VERTEX, selected).c_str());
    ImGui::SetItemTooltip("The u_vs_globals block of %s, for a .vert", selected.empty() ? "every global" : "the checked globals");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY " Fragment")) ImGui::SetClipboardText(ShaderGlobals::getGlslBlock(decls, ShaderStageType::FRAGMENT, selected).c_str());
    ImGui::SetItemTooltip("The u_fs_globals block and texture uniforms of %s, for a .frag", selected.empty() ? "every global" : "the checked globals");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE "##refresh_uses")) refreshShaderGlobalUses();
    ImGui::SetItemTooltip("Scan the open scenes again for what uses the globals (Used by)");
    if (playing) {
        ImGui::SameLine();
        if (ImGui::Button("Copy Live Values to Defaults")) {
            for (ShaderGlobalsEdit::Row& row : shaderGlobalRows) {
                ShaderGlobalValue live;
                if (ShaderGlobals::isTexture(row.decl.type) || !ShaderGlobals::get(ShaderGlobals::findID(row.decl.name), live)) continue;
                if (live.type == row.decl.type && live.values.size() == row.decl.defaultValue.size()) row.decl.defaultValue = live.values;
            }
            edited = true;
        }
        ImGui::SetItemTooltip("The values scripts or the Live column set now become the defaults (saved with OK)");
    }

    const int columns = playing ? 7 : 6;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!shaderGlobalRows.empty() && ImGui::BeginTable("##ShaderGlobalsTable", columns, flags)) {
        ImGui::TableSetupColumn("##select", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        if (playing) ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Used by").x + style.CellPadding.x * 2.0f);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();

        int removeRow = -1;
        for (int i = 0; i < (int)shaderGlobalRows.size(); i++) {
            ShaderGlobalsEdit::Row& row = shaderGlobalRows[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Checkbox("##selected", &row.selected);
            ImGui::SetItemTooltip("In Copy GLSL (none checked: every global)");

            // the name, checked as typed (a GLSL identifier, not u_ / gl_, no duplicates)
            ImGui::TableNextColumn();
            std::string nameError;
            ShaderGlobals::isValidName(row.nameBuffer, &nameError);
            for (int j = 0; j < (int)shaderGlobalRows.size() && nameError.empty(); j++) {
                if (j != i && shaderGlobalRows[j].decl.name == row.nameBuffer) nameError = "another global is named '" + std::string(row.nameBuffer) + "'";
            }
            if (!nameError.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.35f, 1.0f));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", row.nameBuffer, sizeof(row.nameBuffer), ImGuiInputTextFlags_CharsNoBlank)) {
                row.decl.name = row.nameBuffer;
                edited = true;
            }
            if (!nameError.empty()) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                if (!nameError.empty())
                    ImGui::SetTooltip("%s", nameError.c_str());
                else if (!row.originalName.empty() && row.originalName != row.decl.name)
                    ImGui::SetTooltip("Renamed from %s: bindings follow it on OK. Shaders keep their member names: rename them in the GLSL too.",
                                      row.originalName.c_str());
                else
                    ImGui::SetTooltip("The GLSL member (or texture uniform) name shaders declare; Unity's leading _ is fine");
            }

            ImGui::TableNextColumn();
            int typeIndex = 0;
            for (int t = 0; t < (int)std::size(shaderGlobalTypes); t++) {
                if (shaderGlobalTypes[t] == row.decl.type) typeIndex = t;
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##type", &typeIndex, shaderGlobalTypeLabels, (int)std::size(shaderGlobalTypeLabels)) &&
                    shaderGlobalTypes[typeIndex] != row.decl.type) {
                // another type starts from its own default
                row.decl = ShaderGlobals::makeDecl(row.decl.name, shaderGlobalTypes[typeIndex], 4);
                edited = true;
            }
            if (ShaderGlobals::isArray(row.decl.type)) {
                int size = (int)row.decl.arraySize;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt("##size", &size)) {
                    size = std::clamp(size, 1, (int)ShaderGlobals::MAX_ARRAY_SIZE);
                    if (size != (int)row.decl.arraySize) {
                        row.decl.arraySize = (unsigned int)size;
                        row.decl.defaultValue.resize(size, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
                        edited = true;
                    }
                }
                ImGui::SetItemTooltip("Elements (1 to %u)", ShaderGlobals::MAX_ARRAY_SIZE);
            }

            ImGui::TableNextColumn();
            if (ShaderGlobals::isTexture(row.decl.type))
                edited |= drawShaderGlobalTexture("##default", row.decl.type, row.decl.defaultTexture);
            else
                edited |= drawShaderGlobalValue("##default", row.decl.type, row.decl.arraySize, row.decl.defaultValue);

            if (playing) {
                // what scripts set now; an edit here lasts until Play stops
                ImGui::TableNextColumn();
                const int id = ShaderGlobals::findID(row.decl.name);
                ShaderGlobalValue live;
                if (!ShaderGlobals::get(id, live)) {
                    ImGui::TextDisabled("(not declared yet)");
                } else if (ShaderGlobals::isTexture(live.type)) {
                    const std::string path = live.texture.isFramebuffer() ? std::string("render target") : live.texture.getPath(0);
                    ImGui::TextDisabled("%s", path.empty() ? "(default)" : path.c_str());
                } else if (live.type == row.decl.type) {
                    std::vector<Vector4> values = live.values;
                    if (drawShaderGlobalValue("##live", live.type, live.arraySize, values)) ShaderGlobals::setValues(id, values);
                } else {
                    ImGui::TextDisabled("(set as %s)", ShaderGlobals::getTypeName(live.type));
                }
            }

            // what uses it: bindings and loaded shaders
            ImGui::TableNextColumn();
            int bindings = 0;
            int shaders = 0;
            std::string uses;
            for (const ShaderGlobalsUsage::Use& use : m_shaderGlobalUses) {
                if (use.global != row.decl.name && !(use.global == row.originalName && use.binding)) continue;
                (use.binding ? bindings : shaders)++;
                if (uses.size() < 2000) uses += "\n" + ShaderGlobalsUsage::describe(use);
            }
            if (bindings + shaders == 0) {
                ImGui::TextDisabled("-");
            } else {
                ImGui::Text("%d", bindings + shaders);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", (bindings + shaders == 0) ? "No binding or loaded shader of the open scenes uses it"
                                         : (std::to_string(bindings) + " binding(s), " + std::to_string(shaders) + " shader(s):" + uses).c_str());

            ImGui::TableNextColumn();
            if (ImGui::Button(ICON_FA_TRASH "##remove")) {
                if (bindings > 0) {
                    m_shaderGlobalRemove = i;
                    ImGui::OpenPopup("##remove_global");
                } else {
                    removeRow = i;
                }
            }
            ImGui::SetItemTooltip("Remove");
            if (ImGui::BeginPopup("##remove_global")) {
                ImGui::Text("%d binding(s) use %s:", bindings, row.decl.name.c_str());
                for (const ShaderGlobalsUsage::Use& use : m_shaderGlobalUses) {
                    if (use.binding && (use.global == row.decl.name || use.global == row.originalName))
                        ImGui::BulletText("%s", ShaderGlobalsUsage::describe(use).c_str());
                }
                ImGui::TextDisabled("They stay, reading 0 until a script sets the global (check_shader reports them).");
                if (ImGui::Button("Remove")) {
                    removeRow = m_shaderGlobalRemove;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (removeRow >= 0 && removeRow < (int)shaderGlobalRows.size()) {
            shaderGlobalRows.erase(shaderGlobalRows.begin() + removeRow);
            m_shaderGlobalRemove = -1;
            edited = true;
        }
    } else if (shaderGlobalRows.empty()) {
        ImGui::TextDisabled("No shader globals. Add one, or let scripts make them (Shader.setGlobalColor(\"_Tint\", c)).");
    }

    const std::string problem = m_globalsEdit.error();
    if (!problem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", problem.c_str());
        ImGui::PopStyleColor();
    }
    bool renamed = false;
    for (const ShaderGlobalsEdit::Row& row : shaderGlobalRows) renamed |= !row.originalName.empty() && row.originalName != row.decl.name;
    if (renamed) {
        drawSectionNote("Renamed globals take their Inspector bindings along on OK; shaders are not rewritten: rename the "
                        "u_vs_globals / u_fs_globals members (and texture uniforms) too.");
    }
    if (edited) m_globalsEdit.preview();
}

} // namespace jasperengine::editor
