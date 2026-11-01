// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Debug (feature debug-overlay) on a reflection table (S08): the project
// file's debug block (only the values away from their defaults, so older projects re-save
// unchanged), its schema in jasper://reflection, the names of its values (DebugSettingsNames,
// MCP's debug.* rows) and the labels, tooltips and choices of the settings rows and the
// Project Settings window.

#include "reflect/Reflect.h"
#include "reflect/Field.h"
#include "reflect/TypeOps.h"

#include "DebugSettings.h"

#include <cctype>

namespace jasperengine::editor::reflect {

namespace {

    // case-insensitive, '-' or ' ' for '_' ("Three-Finger Hold" reads as three_finger_hold)
    std::string normalizeDebugToken(std::string_view token) {
        std::string out;
        out.reserve(token.size());
        for (char c : token) {
            if (c == '-' || c == ' ') out += '_';
            else out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    // the defaults are the fallbacks (DebugSettings' member defaults; DebugReflectTest checks
    // they agree): a value that does not read keeps its default (reported)
    constexpr EnumItem kDebugModeItems[] = {
        {static_cast<int>(DebugSettings::Mode::Never), "never", "Never", "DebugSettings::Mode::Never"},
        {static_cast<int>(DebugSettings::Mode::Development), "development", "Development Builds", "DebugSettings::Mode::Development"},
        {static_cast<int>(DebugSettings::Mode::AllBuilds), "always", "Always", "DebugSettings::Mode::AllBuilds"},
    };
    constexpr EnumItem kDebugGestureItems[] = {
        {static_cast<int>(DebugOverlaySettings::Gesture::ThreeFingerHold), "three_finger_hold", "Three Fingers Held", "DebugOverlaySettings::Gesture::ThreeFingerHold"},
        {static_cast<int>(DebugOverlaySettings::Gesture::FourFingerTap), "four_finger_tap", "Four Finger Tap", "DebugOverlaySettings::Gesture::FourFingerTap"},
        {static_cast<int>(DebugOverlaySettings::Gesture::Off), "none", "None", "DebugOverlaySettings::Gesture::Off"},
    };
    constexpr EnumItem kDebugPopupItems[] = {
        {static_cast<int>(DebugOverlaySettings::Popup::Never), "never", "Never", "DebugOverlaySettings::Popup::Never"},
        {static_cast<int>(DebugOverlaySettings::Popup::TouchDevices), "touch_devices", "Touch Devices", "DebugOverlaySettings::Popup::TouchDevices"},
        {static_cast<int>(DebugOverlaySettings::Popup::AllDevices), "always", "Always", "DebugOverlaySettings::Popup::AllDevices"},
    };
    constexpr EnumItem kDebugStatsItems[] = {
        {static_cast<int>(DebugOverlaySettings::Stats::Off), "off", "Off", "DebugOverlaySettings::Stats::Off"},
        {static_cast<int>(DebugOverlaySettings::Stats::Fps), "fps", "FPS", "DebugOverlaySettings::Stats::Fps"},
        {static_cast<int>(DebugOverlaySettings::Stats::Full), "full", "Full", "DebugOverlaySettings::Stats::Full"},
    };
    constexpr EnumItem kDebugCornerItems[] = {
        {static_cast<int>(DebugOverlaySettings::Corner::TopLeft), "top_left", "Top Left", "DebugOverlaySettings::Corner::TopLeft"},
        {static_cast<int>(DebugOverlaySettings::Corner::TopRight), "top_right", "Top Right", "DebugOverlaySettings::Corner::TopRight"},
        {static_cast<int>(DebugOverlaySettings::Corner::BottomLeft), "bottom_left", "Bottom Left", "DebugOverlaySettings::Corner::BottomLeft"},
        {static_cast<int>(DebugOverlaySettings::Corner::BottomRight), "bottom_right", "Bottom Right", "DebugOverlaySettings::Corner::BottomRight"},
    };
    constexpr EnumItem kDebugAllowLuaItems[] = {
        {static_cast<int>(DebugSettings::AllowLua::Development), "development", "Development Builds", "DebugSettings::AllowLua::Development"},
        {static_cast<int>(DebugSettings::AllowLua::AllBuilds), "always", "Always", "DebugSettings::AllowLua::AllBuilds"},
        {static_cast<int>(DebugSettings::AllowLua::Never), "never", "Never", "DebugSettings::AllowLua::Never"},
    };
    constexpr EnumItem kDebugStackTraceItems[] = {
        {static_cast<int>(LogStackTraces::Off), "none", "None", "LogStackTraces::Off"},
        {static_cast<int>(LogStackTraces::Errors), "errors", "Errors", "LogStackTraces::Errors"},
        {static_cast<int>(LogStackTraces::Warnings), "warnings", "Warnings and Errors", "LogStackTraces::Warnings"},
        {static_cast<int>(LogStackTraces::All), "all", "All", "LogStackTraces::All"},
    };

    template<size_t N>
    constexpr EnumDesc debugSettingsEnum(const char* cppType, const EnumItem (&items)[N], int fallback) {
        return EnumDesc{cppType, std::span<const EnumItem>(items), fallback, &normalizeDebugToken};
    }

}

constexpr EnumDesc kDebugModeEnum = debugSettingsEnum("DebugSettings::Mode", kDebugModeItems, static_cast<int>(DebugSettings::Mode::Development));
constexpr EnumDesc kDebugGestureEnum = debugSettingsEnum("DebugOverlaySettings::Gesture", kDebugGestureItems, static_cast<int>(DebugOverlaySettings::Gesture::ThreeFingerHold));
constexpr EnumDesc kDebugPopupEnum = debugSettingsEnum("DebugOverlaySettings::Popup", kDebugPopupItems, static_cast<int>(DebugOverlaySettings::Popup::TouchDevices));
constexpr EnumDesc kDebugStatsEnum = debugSettingsEnum("DebugOverlaySettings::Stats", kDebugStatsItems, static_cast<int>(DebugOverlaySettings::Stats::Off));
constexpr EnumDesc kDebugCornerEnum = debugSettingsEnum("DebugOverlaySettings::Corner", kDebugCornerItems, static_cast<int>(DebugOverlaySettings::Corner::TopLeft));
constexpr EnumDesc kDebugAllowLuaEnum = debugSettingsEnum("DebugSettings::AllowLua", kDebugAllowLuaItems, static_cast<int>(DebugSettings::AllowLua::Development));
constexpr EnumDesc kDebugStackTracesEnum = debugSettingsEnum("LogStackTraces", kDebugStackTraceItems, static_cast<int>(LogStackTraces::Warnings));

namespace {

    // the YAML keys are the members' names; drawer "key" is a key name (InputControlNames) or ""
    constexpr FieldDesc kDebugFields[] = {
        JE_FIELD(DebugSettings, overlay, .enumDesc = &kDebugModeEnum, .flags = OmitIfDefault, .label = "Overlay",
                 .tooltip = "Development Builds: the editor's Play and exports built with the Debug build type (Engine.isDevelopmentBuild). "
                            "Always: every build. Never: off unless a script sets DebugOverlay.enabled.",
                 .group = "Overlay"),
        JE_FIELD(DebugSettings, toggleKey, .flags = OmitIfDefault, .label = "Toggle Key",
                 .tooltip = "Opens and closes the console (the backquote key, as Unity's IngameDebugConsole). The game still gets the key.",
                 .group = "Overlay", .drawer = "key"),
        JE_FIELD(DebugSettings, statsKey, .flags = OmitIfDefault, .label = "Stats Key",
                 .tooltip = "Cycles the stats panel: off, FPS, full.", .group = "Overlay", .drawer = "key"),
        JE_FIELD(DebugSettings, gesture, .enumDesc = &kDebugGestureEnum, .flags = OmitIfDefault, .label = "Gesture",
                 .tooltip = "The touch gesture that opens and closes the console: three fingers held 0.75 s, or four fingers tapped.",
                 .group = "Overlay"),
        JE_FIELD(DebugSettings, popup, .enumDesc = &kDebugPopupEnum, .flags = OmitIfDefault, .label = "Popup",
                 .tooltip = "A small bubble at the right edge with the warning and error counts; tap it to open the console.",
                 .group = "Overlay"),
        JE_FIELD(DebugSettings, openOnError, .flags = OmitIfDefault, .label = "Open on Error",
                 .tooltip = "The first error opens the console.", .group = "Overlay"),
        JE_FIELD(DebugSettings, stats, .enumDesc = &kDebugStatsEnum, .flags = OmitIfDefault, .label = "Initial Stats",
                 .tooltip = "The stats panel at start: FPS and a frame-time graph, or full (draw calls, memory, audio, physics, entities...).",
                 .group = "Overlay"),
        JE_FIELD(DebugSettings, statsCorner, .enumDesc = &kDebugCornerEnum, .flags = OmitIfDefault, .label = "Stats Corner",
                 .tooltip = "Where the stats panel sits. Taps go through it to the game.", .group = "Overlay"),
        JE_FIELD(DebugSettings, allowLua, .enumDesc = &kDebugAllowLuaEnum, .flags = OmitIfDefault, .label = "Allow Lua Commands",
                 .tooltip = "The console's `lua <code>` command runs Lua typed on the device: in development builds, always or never.",
                 .group = "Overlay"),
        JE_FIELD(DebugSettings, logBufferSize, .flags = OmitIfDefault | Clamped, .min = 0.0f,
                 .max = static_cast<float>(DebugSettings::maxLogBufferSize), .label = "Log Buffer Size",
                 .tooltip = "Lines the log keeps for the console and Log.getEntries (0 keeps none): every record of the logger, "
                            "from the first line, in every build.",
                 .group = "Log"),
        JE_FIELD(DebugSettings, logStackTraces, .enumDesc = &kDebugStackTracesEnum, .flags = OmitIfDefault, .label = "Log Stack Traces",
                 .tooltip = "Which Log.* lines of Lua scripts carry a Lua stack trace (Lua errors always do).", .group = "Log"),
    };

}

JE_REFLECT_STRUCT_TRAITS(DebugSettings, kDebugStruct);
const StructDesc kDebugStruct = JE_STRUCT_DESC(DebugSettings, "DebugSettings", kDebugFields);

const StructDesc& debugSettingsStruct() {
    return kDebugStruct;
}

std::vector<const EnumDesc*> debugSettingsEnums() {
    return {&kDebugModeEnum, &kDebugGestureEnum, &kDebugPopupEnum, &kDebugStatsEnum, &kDebugCornerEnum,
            &kDebugAllowLuaEnum, &kDebugStackTracesEnum};
}

const FieldDesc* debugSettingsField(std::string_view name) {
    for (const FieldDesc& field : kDebugStruct.fields) {
        if (name == field.name) return &field;
    }
    return nullptr;
}

}
