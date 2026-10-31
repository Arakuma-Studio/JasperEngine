// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectSettingsWindow.h"
#include "util/FileDialogs.h"
#include "Backend.h"
#include "window/Widgets.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "SettingsCategoryList.h"
#include "window/CanvasScalerFields.h"
#include "window/PlayInput.h"
#include "imgui_internal.h"
#include "command/type/InputActionsCmd.h"
#include "input/InputActions.h"
#include "input/InputControlNames.h"

#include <algorithm>
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

static const char* windowModeNames[] = { "Windowed", "Maximized", "Fullscreen" };
static const WindowMode windowModeValues[] = { WindowMode::WINDOWED, WindowMode::MAXIMIZED, WindowMode::FULLSCREEN };
static const int windowModeCount = sizeof(windowModeValues) / sizeof(windowModeValues[0]);

static const char* cxxStandardNames[] = { "C++17", "C++20", "C++23" };
static const int cxxStandardCount = sizeof(cxxStandardNames) / sizeof(cxxStandardNames[0]);
static_assert(std::size(cxxStandardNames) == std::size(cxxStandards));

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

static constexpr float dialogWidth = 960.0f;
static constexpr float dialogHeight = 660.0f;
static constexpr float categoryListWidth = 200.0f;

enum class ProjectCategory {
    Player,
    Graphics,
    Quality,
    Physics,
    Physics2D,
    Input,
    Scripting,
    TagsAndLayers,
    Editor,
    Count
};

// Order matches ProjectCategory. Keywords are the page's setting labels, for the search field.
static const settingsui::Category projectCategories[] = {
    {"Player", "", "project product name application identifier version build default icon resolution presentation "
                   "fullscreen mode screen width height resizable window title orientation auto rotation portrait landscape "
                   "keep screen on sleep target frame rate fps start scene splash loading async publishing "
                   "native resource pack windows macos linux ios android web company bundle identifier package favicon "
                   "google admob ads app id other settings network allow insecure http downloads over http cleartext "
                   "internet access permission"},
    {"Graphics", "", "canvas width height scaling mode texture strategy reference resolution runtime shader compilation variants "
                     "html ui msaa anti-aliasing antialiasing"},
    {"Quality", "", "vsync"},
    {"Physics", "", "3d physics jolt rigidbody"},
    {"Physics 2D", "", "2d physics box2d rigidbody 2d"},
    {"Input", "", "input manager axes actions buttons keys keyboard mouse gamepad joystick bindings horizontal vertical "
                  "jump fire submit cancel gravity sensitivity dead snap invert rebinding"},
    {"Scripting", "", "lua directory script directories c++ standard"},
    {"Tags and Layers", "", "tags layers"},
    {"Editor", "", "version control files assets directory"},
};
static constexpr int projectCategoryCount = static_cast<int>(ProjectCategory::Count);
static_assert(std::size(projectCategories) == static_cast<size_t>(ProjectCategory::Count));

static constexpr float settingsLabelWidth = 200.0f;
static constexpr float settingsPanelPadding = 12.0f;
static constexpr float settingsButtonWidth = 120.0f;
static constexpr ImGuiWindowFlags noScrollFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(settingsPanelPadding, settingsPanelPadding)));
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
    float labelWidth = std::min(Theme::dpi(settingsLabelWidth), ImGui::GetContentRegionAvail().x * 0.4f);
    if (!ImGui::BeginTable(tableId, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

template <typename DrawContents>
static void drawSettingsPanel(const char* panelId, DrawContents drawContents) {
    if (beginSettingsPanel(panelId) && beginSettingsTable("##SettingsTable")) {
        drawContents();
        ImGui::EndTable();
    }
    endSettingsPanel();
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
static bool beginSettingsRow(const char* label, bool defChanged = false, const char* resetTooltip = "Restore default") {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);

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
    if (beginSettingsRow(label, buffer[0] != '\0', "Use the shared value")) {
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
    if (beginSettingsRow(label, selectedIndex != defaultIndex)) {
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
    if (beginSettingsRow(label, value != defaultValue)) {
        value = defaultValue;
    }
    ImGui::SetNextItemWidth(tooltip ? -helpMarkerWidth() : -1.0f);
    ImGui::InputInt(id, &value);
    value = std::max(value, minValue);
    endSettingsRow(tooltip);
}

static void drawSecondsSetting(const char* label, const char* id, float& value, float defaultValue, const char* tooltip = nullptr) {
    if (beginSettingsRow(label, value != defaultValue)) {
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
    if (beginSettingsRow(label, !directory.empty() && directory != defaultDirectory)) {
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
    if (beginSettingsRow("Script Directories", !directories.empty())) {
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
    beginSettingsRow(label);

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
        for (int i = 0; i < projectCategoryCount; i++) {
            if (strcmp(projectCategories[i].name, category) == 0) {
                m_category = i;
                m_categorySearch[0] = '\0';
                break;
            }
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
    m_htmlUiMsaaIndex = findHtmlUiMsaaIndex(project->getHtmlUiMsaaSamples());
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
    m_physics3DWorld = project->getPhysics3DWorldSettings();
    m_runtimeShadersEnabled = project->isRuntimeShadersEnabled();

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

    m_packNativeResources = project->shouldPackNativeResources();
    m_versionControlMetadata = project->hasVersionControlMetadata();

    m_tags = project->getTags();
    m_layerNames = project->getLayerNames();
    m_layerNames.resize(MAX_LAYERS);
    m_newTagBuffer[0] = '\0';

    m_layerCollision3D = project->getLayerCollisionMatrix3D();
    m_layerCollision2D = project->getLayerCollisionMatrix2D();

    m_inputActions = project->getInputActions();
    m_inputCommitted = m_inputActions;
    m_inputCommittedJson = InputActions::actionsToJson(m_inputActions);
    m_inputUndo.clear();
    m_inputRedo.clear();
    m_inputSelected = std::clamp(m_inputSelected, 0, std::max(0, static_cast<int>(m_inputActions.size()) - 1));
    m_inputSearch[0] = '\0';
    m_inputListen = InputListen();
}

void ProjectSettingsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Project Settings##ProjectSettingsModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = viewport->GetWorkCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImVec2 size(
        std::min(Theme::dpi(dialogWidth), viewport->WorkSize.x * 0.9f),
        std::min(Theme::dpi(dialogHeight), viewport->WorkSize.y * 0.9f)
    );
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoResize |
                             noScrollFlags;

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
    // the Input page's Listen takes Escape: it must not close the dialog in the frame it ends
    const bool listening = m_inputListen.action >= 0;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float footerY = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight();
    float regionHeight = std::max(1.0f, footerY - style.ItemSpacing.y * 2.0f - ImGui::GetCursorPosY());

    ImGui::BeginChild("##ProjectSettingsRegion", ImVec2(0, regionHeight), ImGuiChildFlags_None, noScrollFlags);

    const float listWidth = std::min(Theme::dpi(categoryListWidth), ImGui::GetContentRegionAvail().x * 0.3f);
    settingsui::categoryList("##ProjectSettingsCategories", projectCategories, projectCategoryCount, m_category,
                             m_categorySearch, sizeof(m_categorySearch), ImVec2(listWidth, ImGui::GetContentRegionAvail().y));
    ImGui::SameLine();

    ImGui::BeginGroup();
    settingsui::categoryTitle(projectCategories[m_category].name);
    switch (static_cast<ProjectCategory>(m_category)) {
        case ProjectCategory::Player: drawPlayerSettings(); break;
        case ProjectCategory::Graphics: drawGraphicsSettings(); break;
        case ProjectCategory::Quality: drawQualitySettings(); break;
        case ProjectCategory::Physics: drawPhysicsSettings(); break;
        case ProjectCategory::Physics2D: drawPhysics2DSettings(); break;
        case ProjectCategory::Input: drawInputSettings(); break;
        case ProjectCategory::Scripting: drawScriptingSettings(); break;
        case ProjectCategory::TagsAndLayers: drawTagsAndLayersSettings(); break;
        case ProjectCategory::Editor: drawEditorSettings(); break;
        default: break;
    }
    ImGui::EndGroup();

    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, footerY - style.ItemSpacing.y));
    ImGui::Separator();

    // Unity-style footer: buttons on the right, primary first.
    const float buttonWidth = Theme::dpi(settingsButtonWidth);
    const float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    ImGui::SetCursorPos(ImVec2(std::max(style.WindowPadding.x, ImGui::GetWindowContentRegionMax().x - buttonsWidth), footerY));

    const bool hasAbi = hasAndroidAbiSelection(m_androidAbiArmeabiV7a, m_androidAbiArm64V8a, m_androidAbiX86, m_androidAbiX86_64);
    const std::string tagsAndLayersProblem = tagsAndLayersError();
    const std::string inputProblem = inputError();
    const bool canSave = hasAbi && tagsAndLayersProblem.empty() && inputProblem.empty();

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
        } else if (!tagsAndLayersProblem.empty()) {
            ImGui::SetTooltip("%s (Tags and Layers).", tagsAndLayersProblem.c_str());
        } else {
            ImGui::SetTooltip("%s (Input).", inputProblem.c_str());
        }
    }

    ImGui::SameLine();
    // Escape cancels a Listen of the Input page first
    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)) ||
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
            beginSettingsRow("Project Name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ProjectName", m_projectNameBuffer, sizeof(m_projectNameBuffer));


            const ApplicationSettings defaults;

            beginSettingsRow("Product Name");
            ImGui::SetNextItemWidth(-helpMarkerWidth());
            std::string nameHint = m_projectNameBuffer[0] ? m_projectNameBuffer : "JasperEngine";
            ImGui::InputTextWithHint("##ApplicationName", nameHint.c_str(), m_applicationNameBuffer, sizeof(m_applicationNameBuffer));
            endSettingsRow("Name every platform shows to the player. Empty means the project name.");

            if (beginSettingsRow("Application Identifier", strcmp(m_applicationIdentifierBuffer, defaults.identifier.c_str()) != 0)) {
                snprintf(m_applicationIdentifierBuffer, sizeof(m_applicationIdentifierBuffer), "%s", defaults.identifier.c_str());
            }
            ImGui::SetNextItemWidth(-helpMarkerWidth());
            ImGui::InputText("##ApplicationIdentifier", m_applicationIdentifierBuffer, sizeof(m_applicationIdentifierBuffer));
            endSettingsRow("Reverse-DNS id behind the Apple bundle identifiers and the Android package name.");
            if (!isSharedIdentifierValid(m_applicationIdentifierBuffer)) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Use letters and digits only, for example com.company.game.");
            }

            if (beginSettingsRow("Version", strcmp(m_applicationVersionBuffer, defaults.version.c_str()) != 0)) {
                snprintf(m_applicationVersionBuffer, sizeof(m_applicationVersionBuffer), "%s", defaults.version.c_str());
            }
            ImGui::SetNextItemWidth(-helpMarkerWidth());
            ImGui::InputText("##ApplicationVersion", m_applicationVersionBuffer, sizeof(m_applicationVersionBuffer));
            endSettingsRow("Version shown to the player. Apple takes at most three parts, Windows pads to four.");
            if (!isAppleVersionValid(m_applicationVersionBuffer)) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Use up to three numbers from 0 to 65535, for example 1.0.");
            }

            drawIntSetting("Build", "##ApplicationBuild", m_applicationBuild, static_cast<int>(defaults.build), 1,
                "Apple build number and Android version code. Increase it for every release.");

            ImGui::EndTable();
        }

        if (Theme::foldoutHeader("Icon", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##PlayerIconTable")) {
            beginSettingsRow("Default Icon");
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

            ImGui::EndTable();
        }

        if (Theme::foldoutHeader("Resolution and Presentation", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##PlayerResolutionTable")) {
            drawComboSetting("Fullscreen Mode", "##WindowMode", windowModeNames, windowModeCount, m_windowModeIndex,
                findWindowModeIndex(Project::defaultWindowMode),
                "Initial window state of desktop builds. Web and mobile ignore window settings.");

            drawIntSetting("Default Screen Width", "##WindowWidth", m_windowWidth, (int)Project::defaultWindowWidth);
            drawIntSetting("Default Screen Height", "##WindowHeight", m_windowHeight, (int)Project::defaultWindowHeight);

            if (beginSettingsRow("Resizable Window", m_windowResizable != Project::defaultWindowResizable)) {
                m_windowResizable = Project::defaultWindowResizable;
            }
            ImGui::Checkbox("##WindowResizable", &m_windowResizable);
            endSettingsRow("Applies to desktop builds. Exported Windows and macOS builds are always resizable.");

            if (beginSettingsRow("Window Title", strcmp(m_windowTitleBuffer, Project::defaultWindowTitle) != 0)) {
                snprintf(m_windowTitleBuffer, sizeof(m_windowTitleBuffer), "%s", Project::defaultWindowTitle);
            }
            std::string titleHint = m_project->getName().empty() ? "JasperEngine" : m_project->getName();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##WindowTitle", titleHint.c_str(), m_windowTitleBuffer, sizeof(m_windowTitleBuffer));

            drawPresentationSettings();

            ImGui::EndTable();
        }

        if (Theme::foldoutHeader("Splash and Loading", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##PlayerLoadingTable")) {
            beginSettingsRow("Start Scene");

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


            const LoadingSettings defaults;
            LoadingSettings& loading = m_loadingSettings;

            if (beginSettingsRow("Loading Scene", loading.sceneId != defaults.sceneId)) {
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

            drawSecondsSetting("Loading Delay", "##LoadingDelay", loading.delay, defaults.delay,
                "Time the loading scene covers the old scene before replacing it.");
            drawSecondsSetting("Loading Timeout", "##LoadingTimeout", loading.timeout, defaults.timeout,
                "Time a load waits without progress before giving up.");

            if (beginSettingsRow("Async Loading", loading.asyncLoading != defaults.asyncLoading)) {
                loading.asyncLoading = defaults.asyncLoading;
            }
            ImGui::Checkbox("##AsyncLoading", &loading.asyncLoading);
            endSettingsRow("Exported games load resources on worker threads. The editor always does.");

            ImGui::EndTable();
        }

        if (Theme::foldoutHeader("Other Settings") && beginSettingsTable("##PlayerOtherTable")) {
            const bool defaultInsecureHttp = ApplicationSettings{}.allowInsecureHttp;
            if (beginSettingsRow("Allow Insecure HTTP", m_allowInsecureHttp != defaultInsecureHttp)) {
                m_allowInsecureHttp = defaultInsecureHttp;
            }
            ImGui::Checkbox("##AllowInsecureHttp", &m_allowInsecureHttp);
            endSettingsRow("Plain http:// URLs other than localhost. Off: they fail with insecure_http on every platform; "
                "exports set Android cleartext and iOS ATS to match.");

            ImGui::EndTable();
        }

        if (Theme::foldoutHeader("Publishing Settings") && beginSettingsTable("##PlayerPublishingTable")) {
            if (beginSettingsRow("Native Resource Pack", m_packNativeResources != Project::defaultPackNativeResources)) {
                m_packNativeResources = Project::defaultPackNativeResources;
            }
            ImGui::Checkbox("##PackNativeResources", &m_packNativeResources);
            endSettingsRow("Experimental. Packs exported assets and Lua files into resources.pak for Desktop and Android source exports. "
                "Packed resources are read through Data; direct File handles cannot open them.");

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
    if (beginSettingsRow("Allowed Orientations", m_allowedOrientations != defaults.allowedOrientations)) {
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

    if (beginSettingsRow("Keep Screen On", m_keepScreenOn != defaults.keepScreenOn)) {
        m_keepScreenOn = defaults.keepScreenOn;
    }
    ImGui::Checkbox("##KeepScreenOn", &m_keepScreenOn);
    endSettingsRow("Android and iOS: the screen never dims or locks while the game runs (Screen.sleepTimeout = "
        "SleepTimeout.NeverSleep). Off: the device's own setting. Desktops and the web follow the system unless a "
        "script sets NeverSleep (as Unity).");

    if (beginSettingsRow("Target Frame Rate", m_targetFrameRate != defaults.targetFrameRate)) {
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
    drawSettingsPanel("##GraphicsSettingsPanel", [this]() {
        drawIntSetting("Canvas Width", "##CanvasWidth", m_canvasWidth, (int)Project::defaultCanvasWidth);
        drawIntSetting("Canvas Height", "##CanvasHeight", m_canvasHeight, (int)Project::defaultCanvasHeight);

        drawComboSetting("Scaling Mode", "##ScalingMode", scalingModeNames, scalingModeCount, m_scalingModeIndex, findScalingIndex(Project::defaultScalingMode));
        ImGui::Spacing();
        drawScalingPreview(scalingModeValues[m_scalingModeIndex], m_canvasWidth, m_canvasHeight);

        // the Canvas Scaler's project default: the UI's own space, apart from the game canvas
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Spacing();
        ImGui::TextUnformatted("UI Scale");
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

        drawComboSetting("Texture Strategy", "##TextureStrategy", textureStrategyNames, textureStrategyCount, m_textureStrategyIndex, findTextureStrategyIndex(Project::defaultTextureStrategy),
            "Only very old GPUs need power-of-two textures. Resize blurs the edges of atlas tiles. The editor applies a change when the project is reopened.");

        drawComboSetting("HTML UI MSAA", "##HtmlUiMsaa", htmlUiMsaaNames, htmlUiMsaaCount, m_htmlUiMsaaIndex, findHtmlUiMsaaIndex(Project::defaultHtmlUiMsaaSamples),
            "Multisampling of the HTML/CSS UI documents: smoother rounded corners, thin borders and rotated edges. "
            "Devices without MSAA draw them without it.");

        if (beginSettingsRow("Runtime Shader Compilation", m_runtimeShadersEnabled != Project::defaultRuntimeShadersEnabled)) {
            m_runtimeShadersEnabled = Project::defaultRuntimeShadersEnabled;
        }
        ImGui::Checkbox("##RuntimeShaders", &m_runtimeShadersEnabled);
        endSettingsRow("Exported builds compile the shader variants they do not ship precompiled, such as those of meshes and materials "
            "that scripts build or change, in the background and cache them on disk. When off, the build carries only the "
            "variants in Build Settings > Shaders and leaves out what draws with any other. Play always compiles.");
    });
}

void ProjectSettingsWindow::drawQualitySettings() {
    drawSettingsPanel("##QualitySettingsPanel", [this]() {
        if (beginSettingsRow("VSync", m_vsyncEnabled != Project::defaultVSyncEnabled)) {
            m_vsyncEnabled = Project::defaultVSyncEnabled;
        }
        ImGui::Checkbox("##VSync", &m_vsyncEnabled);
        endSettingsRow("Synchronize Play mode and supported desktop builds to the display refresh rate. "
            "The editor window follows Preferences > General > Editor VSync. "
            "macOS Metal exports remain synchronized.");
    });
}

void ProjectSettingsWindow::drawPhysicsSettings() {
    if (beginSettingsPanel("##PhysicsSettingsPanel")) {
        if (beginSettingsTable("##SettingsTable")) {
            if (beginSettingsRow("3D Physics", m_physics3DEnabled != Project::defaultPhysics3DEnabled)) {
                m_physics3DEnabled = Project::defaultPhysics3DEnabled;
            }
            ImGui::Checkbox("##Physics3D", &m_physics3DEnabled);
            endSettingsRow("Jolt-based 3D physics. When off, Jolt is left out of exported builds, along with any Rigidbodies and joints the scenes still use. Play always runs with both backends available.");
            ImGui::EndTable();
        }

        if (Theme::foldoutHeader("3D World Capacity", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawSectionNote("Limits of each scene's 3D physics world, reserved when the scene starts playing. "
                "Past Max Bodies no more Rigidbodies are made (the console names the setting); "
                "past the pairs or contact constraints some contacts are dropped for that step.");
            if (beginSettingsTable("##WorldCapacityTable")) {
                const Physics3DWorldSettings defaults;
                const int maxIndex = 8388607;
                auto capacityRow = [&](const char* label, const char* id, unsigned int& field, unsigned int defaultValue, const char* tooltip) {
                    int value = (int)std::min<unsigned int>(field, (unsigned int)maxIndex);
                    drawIntSetting(label, id, value, (int)defaultValue, 16, tooltip);
                    field = (unsigned int)std::min(value, maxIndex);
                };
                capacityRow("Max Bodies", "##Physics3DMaxBodies", m_physics3DWorld.maxBodies, defaults.maxBodies,
                    "The most Rigidbodies a scene can have at once (Jolt's maxBodies).");
                capacityRow("Max Body Pairs", "##Physics3DMaxBodyPairs", m_physics3DWorld.maxBodyPairs, defaults.maxBodyPairs,
                    "The most pairs of bodies whose bounds overlap in one step (Jolt's maxBodyPairs).");
                capacityRow("Max Contact Constraints", "##Physics3DMaxContacts", m_physics3DWorld.maxContactConstraints, defaults.maxContactConstraints,
                    "The most touching body pairs solved in one step (Jolt's maxContactConstraints).");
                ImGui::EndTable();
            }
        }

        if (Theme::foldoutHeader("Layer Collision Matrix", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawSectionNote("Rigidbodies on layers whose box is cleared pass through each other and report no contacts. "
                "Scripts change it with Physics.ignoreLayerCollision; collider category and mask bits still apply to the checked pairs.");
            drawLayerCollisionMatrix("##LayerCollisionMatrix3D", m_layerCollision3D);
        }
    }
    endSettingsPanel();
}

void ProjectSettingsWindow::drawPhysics2DSettings() {
    if (beginSettingsPanel("##Physics2DSettingsPanel")) {
        if (beginSettingsTable("##SettingsTable")) {
            if (beginSettingsRow("2D Physics", m_physics2DEnabled != Project::defaultPhysics2DEnabled)) {
                m_physics2DEnabled = Project::defaultPhysics2DEnabled;
            }
            ImGui::Checkbox("##Physics2D", &m_physics2DEnabled);
            endSettingsRow("Box2D-based 2D physics. When off, Box2D is left out of exported builds, along with any Rigidbody 2D and joints the scenes still use. Play always runs with both backends available.");
            ImGui::EndTable();
        }

        if (Theme::foldoutHeader("Layer Collision Matrix", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawSectionNote("Rigidbody 2D colliders on layers whose box is cleared pass through each other and report no contacts or triggers. "
                "Scripts change it with Physics2D.ignoreLayerCollision; collider category, mask and group still apply to the checked pairs.");
            drawLayerCollisionMatrix("##LayerCollisionMatrix2D", m_layerCollision2D);
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
    drawSettingsPanel("##ScriptingSettingsPanel", [this]() {
        drawDirectorySetting(
            m_project, "Lua Directory", nullptr, "##LuaPath", "Browse##lua",
            m_luaDir, fs::path(Project::defaultLuaDir), true
        );
        drawScriptDirsSetting(m_project, m_scriptDirs);

        drawComboSetting("C++ Standard", "##CxxStandard", cxxStandardNames, cxxStandardCount, m_cxxStandardIndex,
            findCxxStandardIndex(Project::defaultCxxStandard),
            "Language standard for C++ scripts in Play and exported games. Saved with the project. "
            "Exports also compile the engine with this standard. "
            "Choose a compatible compiler in Preferences > External Tools.");
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

        if (Theme::foldoutHeader("Tags", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##TagsTable")) {
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

        if (Theme::foldoutHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen) && beginSettingsTable("##LayersTable")) {
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
    drawSettingsPanel("##EditorSettingsPanel", [this]() {
        if (beginSettingsRow("Version Control Files", m_versionControlMetadata != Project::defaultVersionControlMetadata)) {
            m_versionControlMetadata = Project::defaultVersionControlMetadata;
        }
        ImGui::Checkbox("##VersionControlMetadata", &m_versionControlMetadata);
        endSettingsRow("Keep .gitignore and .gitattributes in the project up to date, so the editor's "
            "working directory, this machine's build settings and each developer's own layout stay out "
            "of the repository. A file the editor did not generate is never replaced.");


        drawDirectorySetting(
            m_project, "Assets Directory", nullptr, "##AssetsPath", "Browse##assets",
            m_assetsDir, fs::path(Project::defaultAssetsDir)
        );
    });
}

void ProjectSettingsWindow::drawPlatformsSettings() {
    // Unity's per-platform Player tabs: each adds what its platform needs on top
    // of the shared product settings above.
    settingsui::boldText("Settings for Platform");
    ImGui::Spacing();
    if (!ImGui::BeginTabBar("##PlayerPlatformTabs", ImGuiTabBarFlags_FittingPolicyShrink)) return;

    if (ImGui::BeginTabItem(ICON_FA_WINDOW_MAXIMIZE " Windows")) {
        drawSectionNote("The Default Icon above is embedded into Windows players.");
        if (beginSettingsTable("##WindowsSettingsTable")) {
            drawWindowsSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem(ICON_FA_LAPTOP " macOS")) {
        drawSectionNote("Bundle settings apply to Apple projects exported as source code. "
            "An Executable Player is a standalone executable, not an .app bundle.");
        if (beginSettingsTable("##MacOSSettingsTable")) {
            drawMacOSSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem(ICON_FA_TERMINAL " Linux")) {
        if (beginSettingsTable("##LinuxSettingsTable")) {
            drawLinuxSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem(ICON_FA_MOBILE_BUTTON " iOS")) {
        if (beginSettingsTable("##IOSSettingsTable")) {
            drawIOSSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem(ICON_FA_MOBILE_SCREEN_BUTTON " Android")) {
        if (beginSettingsTable("##AndroidSettingsTable")) {
            drawAndroidSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem(ICON_FA_GLOBE " Web")) {
        if (beginSettingsTable("##WebSettingsTable")) {
            drawWebSettings();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

void ProjectSettingsWindow::drawWebSettings() {
    drawOverrideSetting("App Name", "##WebAppName", m_webApplicationNameBuffer, inheritedApplicationName(),
        "Title used by the exported web page. Empty means the shared application name.");

    drawProjectFilePathSetting(m_project, "Favicon", "Browser tab icon for web export.", "##WebFaviconPath",
        "Browse##webfavicon", "Clear##webfavicon", m_webFavicon);
    drawProjectFilePathSetting(m_project, "Custom HTML Shell", "Optional web HTML wrapper. The file must contain {{JASPERENGINE_DEFAULT_HTML}} marker.", "##WebHtmlShellPath",
        "Browse##webhtmlshell", "Clear##webhtmlshell", m_webCustomHtmlShell, FILE_DIALOG_ALL);

    beginSettingsRow("Head Include");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextMultiline("##WebHeadInclude", m_webHeadIncludeBuffer, sizeof(m_webHeadIncludeBuffer), ImVec2(-1, Theme::dpi(80.0f)));
    endSettingsRow("HTML inserted into the <head> section by web export when supported.");

    if (beginSettingsRow("Resize Canvas To Window", !m_webResizeCanvasToWindow)) {
        m_webResizeCanvasToWindow = true;
    }
    ImGui::Checkbox("##WebResizeCanvasToWindow", &m_webResizeCanvasToWindow);
    endSettingsRow("Resize the rendering surface to the browser window. Canvas Scaling Mode controls aspect ratio and stretching.");

    if (beginSettingsRow("Hide Emscripten UI", m_webHideEmscriptenUI)) {
        m_webHideEmscriptenUI = false;
    }
    ImGui::Checkbox("##WebHideEmscriptenUI", &m_webHideEmscriptenUI);
    endSettingsRow("Hide the standard logo, status, controls and output console. Runtime scripts remain active. Custom HTML elements are not removed.");
}

void ProjectSettingsWindow::drawLinuxSettings() {
    drawOverrideSetting("App Name", "##LinuxAppName", m_linuxApplicationNameBuffer, inheritedApplicationName(),
        "Linux desktop launcher name. Empty means the shared application name.");

    beginSettingsRow("Comment");
    ImGui::SetNextItemWidth(-helpMarkerWidth());
    ImGui::InputText("##LinuxComment", m_linuxCommentBuffer, sizeof(m_linuxCommentBuffer));
    endSettingsRow("Short description written to the .desktop launcher file.");

    if (beginSettingsRow("Categories", strcmp(m_linuxCategoriesBuffer, LinuxProjectSettings{}.categories.c_str()) != 0)) {
        snprintf(m_linuxCategoriesBuffer, sizeof(m_linuxCategoriesBuffer), "%s", LinuxProjectSettings{}.categories.c_str());
    }
    ImGui::SetNextItemWidth(-helpMarkerWidth());
    ImGui::InputText("##LinuxCategories", m_linuxCategoriesBuffer, sizeof(m_linuxCategoriesBuffer));
    endSettingsRow("Desktop Entry categories. Example: Game;ArcadeGame;");
}

void ProjectSettingsWindow::drawWindowsSettings() {
    drawOverrideSetting("Product Name", "##WindowsProductName", m_windowsProductNameBuffer, inheritedApplicationName(),
        "Windows executable metadata. Empty means the shared application name.");

    beginSettingsRow("Company Name");
    ImGui::SetNextItemWidth(-helpMarkerWidth());
    ImGui::InputText("##WindowsCompanyName", m_windowsCompanyNameBuffer, sizeof(m_windowsCompanyNameBuffer));
    endSettingsRow("Windows executable metadata company name.");

    const std::string inheritedWindowsVersion = Project::toFourPartVersion(inheritedApplicationVersion());

    drawOverrideSetting("File Version", "##WindowsFileVersion", m_windowsFileVersionBuffer, inheritedWindowsVersion,
        "Windows numeric file version. Empty means the shared version, padded to four parts.");
    drawOverrideWarning(m_windowsFileVersionBuffer, isWindowsVersionValid,
        "Use up to four numbers from 0 to 65535, for example 1.0.0.0.");
    drawWindowsVersionResult(m_windowsFileVersionBuffer);

    drawOverrideSetting("Product Version", "##WindowsProductVersion", m_windowsProductVersionBuffer, inheritedWindowsVersion,
        "Windows product version. Empty means the shared version, padded to four parts.");
    drawOverrideWarning(m_windowsProductVersionBuffer, isWindowsVersionValid,
        "Use up to four numbers from 0 to 65535, for example 1.0.0.0.");
    drawWindowsVersionResult(m_windowsProductVersionBuffer);

}

void ProjectSettingsWindow::drawMacOSSettings() {
    drawOverrideSetting("App Name", "##MacOSAppName", m_macOSApplicationNameBuffer, inheritedApplicationName(),
        "macOS bundle display name. Empty means the shared application name.");

    drawOverrideSetting("Bundle Identifier", "##MacOSBundleIdentifier", m_macOSBundleIdentifierBuffer, inheritedApplicationIdentifier(),
        "CFBundleIdentifier. Empty means the shared identifier.");
    drawOverrideWarning(m_macOSBundleIdentifierBuffer, isAppleBundleIdentifierValid,
        "Use letters, digits and hyphens, for example com.company.my-game.");

    drawOverrideSetting("Version Name", "##MacOSVersionName", m_macOSVersionNameBuffer, inheritedApplicationVersion(),
        "CFBundleShortVersionString. Empty means the shared version.");
    drawOverrideWarning(m_macOSVersionNameBuffer, isAppleVersionValid,
        "Apple accepts at most three numbers, for example 1.0.2.");
    drawOverrideSetting("Build Number", "##MacOSBuildNumber", m_macOSBuildNumberBuffer, inheritedApplicationBuild(),
        "CFBundleVersion. Empty means the shared build.");
    drawOverrideWarning(m_macOSBuildNumberBuffer, isAppleBuildValid,
        "Apple accepts at most three numbers, for example 42.");

    drawProjectFilePathSetting(m_project, "Icon", "macOS application icon source image. Export support depends on the generated Xcode project.", "##MacOSIconPath",
        "Browse##macosicon", "Clear##macosicon", m_macOSIcon);

    if (beginSettingsRow("High DPI", !m_macOSHighDpi)) {
        m_macOSHighDpi = true;
    }
    ImGui::Checkbox("##MacOSHighDpi", &m_macOSHighDpi);
    endSettingsRow("Allow high-DPI rendering on macOS.");
}

void ProjectSettingsWindow::drawIOSSettings() {
    beginSettingsRow("Orientation");
    ImGui::TextDisabled("From Player > Resolution and Presentation");
    endSettingsRow("Default Orientation and Allowed Orientations for Auto Rotation are shared by Android and iOS.");

    drawOverrideSetting("App Name", "##IOSAppName", m_iosApplicationNameBuffer, inheritedApplicationName(),
        "iOS bundle display name. Empty means the shared application name.");

    drawOverrideSetting("Bundle Identifier", "##IOSBundleIdentifier", m_iosBundleIdentifierBuffer, inheritedApplicationIdentifier(),
        "CFBundleIdentifier. Empty means the shared identifier.");
    drawOverrideWarning(m_iosBundleIdentifierBuffer, isAppleBundleIdentifierValid,
        "Use letters, digits and hyphens, for example com.company.my-game.");

    drawOverrideSetting("Version Name", "##IOSVersionName", m_iosVersionNameBuffer, inheritedApplicationVersion(),
        "CFBundleShortVersionString. Empty means the shared version.");
    drawOverrideWarning(m_iosVersionNameBuffer, isAppleVersionValid,
        "Apple accepts at most three numbers, for example 1.0.2.");
    drawOverrideSetting("Build Number", "##IOSBuildNumber", m_iosBuildNumberBuffer, inheritedApplicationBuild(),
        "CFBundleVersion. Empty means the shared build.");
    drawOverrideWarning(m_iosBuildNumberBuffer, isAppleBuildValid,
        "Apple accepts at most three numbers, for example 42.");

    drawProjectFilePathSetting(m_project, "Icon", "iOS application icon source image. Export support depends on the generated Xcode project.", "##IOSIconPath",
        "Browse##iosicon", "Clear##iosicon", m_iosIcon);

    if (beginSettingsRow("Hide Status Bar", !m_iosHideStatusBar)) {
        m_iosHideStatusBar = true;
    }
    ImGui::Checkbox("##IOSHideStatusBar", &m_iosHideStatusBar);
    endSettingsRow("Hide the iOS status bar while the app runs.");

    if (beginSettingsRow("Hide Home Indicator", !m_iosHideHomeIndicator)) {
        m_iosHideHomeIndicator = true;
    }
    ImGui::Checkbox("##IOSHideHomeIndicator", &m_iosHideHomeIndicator);
    endSettingsRow("Request hiding the iOS home indicator.");

    if (beginSettingsRow("High Refresh Rate", !m_iosSupportsHighRefreshRate)) {
        m_iosSupportsHighRefreshRate = true;
    }
    ImGui::Checkbox("##IOSHighRefreshRate", &m_iosSupportsHighRefreshRate);
    endSettingsRow("Allow 120 Hz displays when supported.");

    if (beginSettingsRow("Google AdMob", m_iosAdmobEnabled)) {
        m_iosAdmobEnabled = false;
    }
    ImGui::Checkbox("##IOSAdMob", &m_iosAdmobEnabled);
    endSettingsRow("Links Google Mobile Ads for System.initializeAdMob and the interstitial ad calls. Without it the export leaves the SDK out and those calls do nothing.");

    ImGui::BeginDisabled(!m_iosAdmobEnabled);
    beginSettingsRow("AdMob App ID");
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

void ProjectSettingsWindow::drawAndroidSettings() {
    AndroidProjectSettings defaults;

    drawOverrideSetting("App Name", "##AndroidAppName", m_androidApplicationNameBuffer, inheritedApplicationName(),
        "Android launcher label. Empty means the shared application name.");

    drawOverrideSetting("Package Name", "##AndroidPackageName", m_androidPackageNameBuffer, inheritedApplicationIdentifier(),
        "Android applicationId. Empty means the shared identifier.");
    drawOverrideWarning(m_androidPackageNameBuffer, isJavaPackageNameValid,
        "Use a Java package name, for example com.company.game.");

    drawOverrideSetting("Version Code", "##AndroidVersionCode", m_androidVersionCodeBuffer, inheritedApplicationBuild(),
        "Integer version used by Android and stores. Empty means the shared build.");
    drawOverrideWarning(m_androidVersionCodeBuffer, isAndroidVersionCodeValid,
        "Use a whole number from 1 to 2100000000.");

    drawOverrideSetting("Version Name", "##AndroidVersionName", m_androidVersionNameBuffer, inheritedApplicationVersion(),
        "Visible version string. Empty means the shared version.");

    beginSettingsRow("Launcher Icons");
    ImGui::TextWrapped("PNG recommended. Adaptive icon requires foreground and background images.");
    endSettingsRow("Android launcher icon resources. If adaptive foreground and background are set, the export creates an adaptive icon. Otherwise Launcher Icon is used.");

    drawProjectFilePathSetting(m_project, "Launcher Icon", "Fallback launcher icon. Square PNG recommended.", "##AndroidLauncherIconPath",
        "Browse##androidlaunchericon", "Clear##androidlaunchericon", m_androidLauncherIcon);
    drawProjectFilePathSetting(m_project, "Adaptive Foreground", "Adaptive icon foreground image.", "##AndroidAdaptiveIconForegroundPath",
        "Browse##androidadaptiveforeground", "Clear##androidadaptiveforeground", m_androidAdaptiveIconForeground);
    drawProjectFilePathSetting(m_project, "Adaptive Background", "Adaptive icon background image.", "##AndroidAdaptiveIconBackgroundPath",
        "Browse##androidadaptivebackground", "Clear##androidadaptivebackground", m_androidAdaptiveIconBackground);

    drawIntSetting("Min SDK", "##AndroidMinSdk", m_androidMinSdk, static_cast<int>(defaults.minSdk), 1,
        "Lowest Android API level the exported project supports.");
    drawIntSetting("Target SDK", "##AndroidTargetSdk", m_androidTargetSdk, static_cast<int>(defaults.targetSdk), 1,
        "Android API level the app declares as its target.");
    m_androidTargetSdk = std::max(m_androidTargetSdk, m_androidMinSdk);

    beginSettingsRow("Orientation");
    ImGui::TextDisabled("From Player > Resolution and Presentation");
    endSettingsRow("Default Orientation and Allowed Orientations for Auto Rotation are shared by Android and iOS.");

    beginSettingsRow("Architectures");
    ImGui::Checkbox("armeabi-v7a", &m_androidAbiArmeabiV7a);
    ImGui::Checkbox("arm64-v8a", &m_androidAbiArm64V8a);
    ImGui::Checkbox("x86", &m_androidAbiX86);
    ImGui::Checkbox("x86_64", &m_androidAbiX86_64);
    endSettingsRow("Native CPU architectures included in the APK.");
    if (!hasAndroidAbiSelection(m_androidAbiArmeabiV7a, m_androidAbiArm64V8a, m_androidAbiX86, m_androidAbiX86_64)) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Select at least one architecture.");
    }

    drawComboSetting("Internet Access", "##AndroidInternetAccess", androidInternetAccessNames, androidInternetAccessCount,
        m_androidInternetAccessIndex, defaults.internetAccess == AndroidInternetAccess::Require ? 1 : 0,
        "Auto adds INTERNET when Lua or C++ code uses Http (checked at export). Require always adds it.");

    beginSettingsRow("Permissions");
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

    if (beginSettingsRow("Application Options", m_androidAllowBackup != defaults.allowBackup)) {
        m_androidAllowBackup = defaults.allowBackup;
    }

    ImGui::Checkbox("Allow Backup", &m_androidAllowBackup);
    ImGui::SetItemTooltip("Maps to android:allowBackup.");
    endSettingsRow("Android application/activity behavior. Keep Screen On is in Player > Resolution and Presentation.");

    if (beginSettingsRow("Screen Options", m_androidFullscreen != defaults.fullscreen)) {
        m_androidFullscreen = defaults.fullscreen;
    }

    ImGui::Checkbox("Fullscreen", &m_androidFullscreen);
    ImGui::SetItemTooltip("Uses the fullscreen Android theme and hides system bars in MainActivity.");
    endSettingsRow("Android screen/window behavior.");

    if (beginSettingsRow("Google AdMob", m_androidAdmobEnabled != defaults.admobEnabled)) {
        m_androidAdmobEnabled = defaults.admobEnabled;
    }
    ImGui::Checkbox("##AndroidAdMob", &m_androidAdmobEnabled);
    endSettingsRow("Compiles in Google Mobile Ads for System.initializeAdMob and the interstitial ad calls. Without it the export leaves the SDK out and those calls do nothing. Declare ads and Ad ID use in the Play Console.");

    ImGui::BeginDisabled(!m_androidAdmobEnabled);
    beginSettingsRow("AdMob App ID");
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

// ---- Input (Project Settings > Input): the input actions

namespace {

    const char* inputKindLabels[] = { "Key or Mouse Button", "Gamepad Axis", "Mouse Movement" };
    const char* mouseAxisLabels[] = { "X (right)", "Y (up)", "Wheel (up)" };

    std::string inputUniqueName(const std::vector<InputAction>& actions, const std::string& base) {
        auto taken = [&actions](const std::string& name) {
            return std::any_of(actions.begin(), actions.end(), [&name](const InputAction& a) { return a.name == name; });
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
    std::string inputBindingSummary(const InputAction& action, const InputBinding& binding) {
        switch (binding.kind) {
            case InputBindingKind::Buttons: {
                std::string text = std::string(inputKindLabels[0]) + ": " + inputControlsText(binding.positive);
                if (action.type == InputActionType::Axis && !binding.negative.empty())
                    text += " / " + inputControlsText(binding.negative);
                return text;
            }
            case InputBindingKind::GamepadAxis:
                return std::string(inputKindLabels[1]) + ": " + InputControlNames::gamepadAxisName(binding.axis) +
                       (binding.invert ? " (inverted)" : "");
            case InputBindingKind::MouseDelta:
                return std::string(inputKindLabels[2]) + ": " + InputActions::mouseAxisName(binding.mouseAxis);
        }
        return "";
    }

    bool inputPlaying(Project* project) {
        return project && project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
    }

    // a non-negative number field with Unity's label
    bool inputNumberRow(const char* label, const char* id, float& value, float defaultValue, float speed, float maximum,
                        const char* tooltip) {
        if (beginSettingsRow(label, value != defaultValue)) value = defaultValue;
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        const bool changed = ImGui::DragFloat(id, &value, speed, 0.0f, maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        endSettingsRow(tooltip);
        return changed;
    }

}

std::string ProjectSettingsWindow::inputError() const {
    // names are trimmed when their field is left (and on OK): no error while one is typed
    std::vector<InputAction> actions = m_inputActions;
    for (InputAction& action : actions) action.name = TagManager::trimName(action.name);
    return InputActions::validate(actions);
}

void ProjectSettingsWindow::commitInputEdit() {
    // a drag or a text edit is one step, taken when it ends
    if (ImGui::IsAnyItemActive()) return;
    const std::string json = InputActions::actionsToJson(m_inputActions);
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
    m_inputCommittedJson = InputActions::actionsToJson(m_inputActions);
    m_inputListen = InputListen();
}

void ProjectSettingsWindow::redoInputEdit() {
    if (m_inputRedo.empty()) return;
    m_inputUndo.push_back(m_inputActions);
    m_inputActions = m_inputRedo.back();
    m_inputRedo.pop_back();
    m_inputCommitted = m_inputActions;
    m_inputCommittedJson = InputActions::actionsToJson(m_inputActions);
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

    InputBinding& binding = m_inputActions[m_inputListen.action].bindings[m_inputListen.binding];
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
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) continue;
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

void ProjectSettingsWindow::drawInputSettings() {
    ImGuiIO& io = ImGui::GetIO();
    if (m_inputListen.action < 0 && !io.WantTextInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))) {
            redoInputEdit();
        } else if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            undoInputEdit();
        }
    }

    if (beginSettingsPanel("##InputSettingsPanel")) {
        drawSectionNote("Input actions are Unity's virtual axes and buttons: scripts read Input.getAxis(\"Horizontal\") and "
            "Input.getButtonDown(\"Jump\"), and the keyboard, gamepad and on-screen controls bound to an action add up. "
            "Saved with the project; a game can rebind them at run time (Input.setBindings).");

        ImGui::BeginDisabled(m_inputUndo.empty() && InputActions::actionsToJson(m_inputActions) == m_inputCommittedJson);
        if (ImGui::Button(ICON_FA_ROTATE_LEFT " Undo")) undoInputEdit();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Undo the last Input change (Ctrl+Z)");
        ImGui::SameLine();
        ImGui::BeginDisabled(m_inputRedo.empty());
        if (ImGui::Button(ICON_FA_ROTATE_RIGHT " Redo")) redoInputEdit();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Redo (Ctrl+Y)");
        ImGui::SameLine();
        if (ImGui::Button("Reset to Unity Defaults")) {
            m_inputActions = InputActions::defaults();
            m_inputSelected = 0;
            m_inputListen = InputListen();
        }
        ImGui::SetItemTooltip("Horizontal, Vertical, Fire1-3, Jump, Mouse X/Y, Mouse ScrollWheel, Submit and Cancel "
                              "as Unity's Input Manager has them");

        const std::string problem = inputError();
        if (!problem.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", problem.c_str());
        }

        const float listWidth = std::min(Theme::dpi(220.0f), ImGui::GetContentRegionAvail().x * 0.35f);
        ImGui::BeginChild("##InputActionList", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
        drawInputActionList();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##InputActionDetails", ImVec2(0, 0), ImGuiChildFlags_None);
        if (m_inputSelected >= 0 && m_inputSelected < static_cast<int>(m_inputActions.size())) {
            drawInputActionDetails(m_inputActions[m_inputSelected], m_inputSelected);
        } else {
            ImGui::TextDisabled("No action selected. Add one with +.");
        }
        ImGui::EndChild();
    }
    endSettingsPanel();

    pollInputListen();
    commitInputEdit();
}

void ProjectSettingsWindow::drawInputActionList() {
    const float buttonWidth = ImGui::GetFrameHeight();
    if (ImGui::Button(ICON_FA_PLUS "##AddInputAction", ImVec2(buttonWidth, 0))) {
        InputAction action;
        action.name = inputUniqueName(m_inputActions, "New Action");
        action.bindings.push_back(InputActions::makeBinding(InputBindingKind::Buttons));
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
        InputAction copy = m_inputActions[m_inputSelected];
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
        const InputAction& action = m_inputActions[i];
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
        InputAction moved = m_inputActions[moveFrom];
        m_inputActions.erase(m_inputActions.begin() + moveFrom);
        m_inputActions.insert(m_inputActions.begin() + moveTo, moved);
        m_inputSelected = moveTo;
        m_inputListen = InputListen();
    }
}

void ProjectSettingsWindow::drawInputActionDetails(InputAction& action, int actionIndex) {
    ImGui::PushID(actionIndex);
    if (beginSettingsTable("##InputActionTable")) {
        beginSettingsRow("Name");
        char name[128];
        snprintf(name, sizeof(name), "%s", action.name.c_str());
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        if (ImGui::InputText("##InputActionName", name, sizeof(name))) action.name = name;
        if (ImGui::IsItemDeactivatedAfterEdit()) action.name = TagManager::trimName(action.name);
        endSettingsRow("What scripts pass to Input.getAxis / getButton. Unique, case-sensitive.");

        if (beginSettingsRow("Type", action.type != InputActionType::Axis)) action.type = InputActionType::Axis;
        int type = action.type == InputActionType::Button ? 1 : 0;
        const char* typeLabels[] = { "Axis", "Button" };
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        if (ImGui::Combo("##InputActionType", &type, typeLabels, 2)) action.type = type == 1 ? InputActionType::Button : InputActionType::Axis;
        endSettingsRow("Axis: -1..1 from a positive and a negative side. Button: 0..1, negative buttons are not read.");

        beginSettingsRow("Description");
        char description[256];
        snprintf(description, sizeof(description), "%s", action.description.c_str());
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        if (ImGui::InputText("##InputActionDescription", description, sizeof(description))) action.description = description;
        endSettingsRow("For you and settings screens; the game does not read it.");

        if (beginSettingsRow("Press Point", action.pressPoint != 0.5f)) action.pressPoint = 0.5f;
        ImGui::SetNextItemWidth(-helpMarkerWidth());
        ImGui::DragFloat("##InputActionPressPoint", &action.pressPoint, 0.01f, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        endSettingsRow("How far a gamepad axis or an on-screen stick goes before getButton is true.");

        if (inputPlaying(m_project)) {
            beginSettingsRow("Live");
            InputActions::State state;
            if (InputActions::getState(action.name, state)) {
                const float low = action.type == InputActionType::Button ? 0.0f : -1.0f;
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
    if (ImGui::BeginPopup("##AddInputBinding")) {
        for (int kind = 0; kind < 3; kind++) {
            if (ImGui::MenuItem(inputKindLabels[kind])) {
                action.bindings.push_back(InputActions::makeBinding(static_cast<InputBindingKind>(kind)));
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
        group("Mouse", InputControlNames::mouseButtonNames());
        group("Gamepad", InputControlNames::gamepadButtonNames());
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

void ProjectSettingsWindow::drawInputBinding(InputAction& action, int actionIndex, int bindingIndex) {
    InputBinding& binding = action.bindings[bindingIndex];
    if (!beginSettingsTable("##InputBindingTable")) return;

    beginSettingsRow("Kind");
    int kind = static_cast<int>(binding.kind);
    ImGui::SetNextItemWidth(-helpMarkerWidth());
    if (ImGui::Combo("##BindingKind", &kind, inputKindLabels, 3) && kind != static_cast<int>(binding.kind)) {
        binding = InputActions::makeBinding(static_cast<InputBindingKind>(kind));
        m_inputListen = InputListen();
    }
    endSettingsRow("Unity's Input Manager types: Key or Mouse Button, Joystick Axis, Mouse Movement.");

    const InputBinding defaults = InputActions::makeBinding(binding.kind);
    auto gamepadRow = [&binding]() {
        if (beginSettingsRow("Gamepad", binding.gamepad != -1)) binding.gamepad = -1;
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
        case InputBindingKind::Buttons: {
            beginSettingsRow(action.type == InputActionType::Axis ? "Positive Buttons" : "Buttons");
            drawInputControls("##Positive", binding.positive, actionIndex, bindingIndex, 0);
            if (action.type == InputActionType::Axis) {
                beginSettingsRow("Negative Buttons");
                drawInputControls("##Negative", binding.negative, actionIndex, bindingIndex, 1);
            }
            gamepadRow();
            inputNumberRow("Gravity", "##Gravity", binding.gravity, defaults.gravity, 0.1f, 1000.0f,
                "Speed in units per second that the value falls back to 0 when no button is pressed (0: at once).");
            inputNumberRow("Dead", "##Dead", binding.dead, defaults.dead, 0.001f, 0.999f,
                "Values smaller than this read 0.");
            inputNumberRow("Sensitivity", "##Sensitivity", binding.sensitivity, defaults.sensitivity, 0.1f, 1000.0f,
                "Speed in units per second that the value moves to the target (0: at once).");
            if (beginSettingsRow("Snap", binding.snap)) binding.snap = false;
            ImGui::Checkbox("##Snap", &binding.snap);
            endSettingsRow("Pressing the opposite side jumps to 0 first instead of sliding through it.");
            break;
        }
        case InputBindingKind::GamepadAxis: {
            beginSettingsRow("Axis");
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
        case InputBindingKind::MouseDelta: {
            beginSettingsRow("Axis");
            int axis = static_cast<int>(binding.mouseAxis);
            ImGui::SetNextItemWidth(-helpMarkerWidth());
            if (ImGui::Combo("##BindingMouseAxis", &axis, mouseAxisLabels, 3)) binding.mouseAxis = static_cast<MouseDeltaAxis>(axis);
            endSettingsRow("Mouse movement this frame in canvas px (wheel: steps); not clamped to -1..1.");
            inputNumberRow("Sensitivity", "##Sensitivity", binding.sensitivity, defaults.sensitivity, 0.01f, 100.0f,
                "Multiplies the movement (Unity's default 0.1).");
            break;
        }
    }
    if (beginSettingsRow("Invert", binding.invert)) binding.invert = false;
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
    m_project->setHtmlUiMsaaSamples(htmlUiMsaaValues[m_htmlUiMsaaIndex]);
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
    m_project->setPhysics3DWorldSettings(m_physics3DWorld);
    m_project->setRuntimeShadersEnabled(m_runtimeShadersEnabled);

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
    android.abiArmeabiV7a = m_androidAbiArmeabiV7a;
    android.abiArm64V8a = m_androidAbiArm64V8a;
    android.abiX86 = m_androidAbiX86;
    android.abiX86_64 = m_androidAbiX86_64;
    android.permissions = m_androidPermissions;
    android.internetAccess = m_androidInternetAccessIndex == 1 ? AndroidInternetAccess::Require : AndroidInternetAccess::Auto;
    android.allowBackup = m_androidAllowBackup;
    android.fullscreen = m_androidFullscreen;
    android.admobEnabled = m_androidAdmobEnabled;
    applyTextBuffer(android.admobAppId, m_androidAdmobAppIdBuffer);

    const SceneProject* startScene = m_project->getScene(m_startSceneId);
    if (startScene && !startScene->filepath.empty()) {
        m_project->setStartSceneId(startScene->id);
    } else {
        m_project->setStartSceneId(NULL_PROJECT_SCENE);
    }

    m_project->getLoadingSettings() = m_loadingSettings;

    m_project->setPackNativeResources(m_packNativeResources);
    m_project->setVersionControlMetadata(m_versionControlMetadata);

    // a name typed into New Tag Name but not added yet is kept rather than lost
    std::vector<std::string> tags = m_tags;
    if (m_newTagBuffer[0] != '\0' && newTagError().empty()) {
        tags.push_back(TagManager::trimName(m_newTagBuffer));
    }
    // GameObjects keep a removed tag, as in Unity; scripts then see it as not defined
    m_project->setTagsAndLayers(tags, m_layerNames);
    m_project->setLayerCollisionMatrices(m_layerCollision3D, m_layerCollision2D);

    // Input: one undoable step of the project history (Edit > Undo with the Project window
    // focused, MCP undo scope project)
    for (InputAction& action : m_inputActions) action.name = TagManager::trimName(action.name);
    if (InputActions::actionsToJson(m_inputActions) != InputActions::actionsToJson(m_project->getInputActions())) {
        auto* command = new InputActionsCmd(m_project, m_project->getInputActions(), m_inputActions);
        command->setNoMerge();
        m_project->getProjectCommandHistory()->addCommand(command);
    }

    // the standalone build's main.cpp bakes these settings in
    m_project->markGeneratedSourcesDirty();

    return m_project->saveProjectFile();
}

} // namespace jasperengine::editor
