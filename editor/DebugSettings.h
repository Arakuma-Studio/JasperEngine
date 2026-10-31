// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Log.h"
#include "debug/DebugOverlay.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // Project Settings > Debug: the on-device debug overlay and the log (project.yaml `debug:`,
    // MCP keys debug.*, the generated init of exports, the editor's Play)
    struct DebugSettings {
        enum class Mode { Never, Development, AllBuilds };
        enum class AllowLua { Development, AllBuilds, Never };

        Mode overlay = Mode::Development;
        std::string toggleKey = "backquote";
        std::string statsKey = "f3";
        DebugOverlaySettings::Gesture gesture = DebugOverlaySettings::Gesture::ThreeFingerHold;
        DebugOverlaySettings::Popup popup = DebugOverlaySettings::Popup::TouchDevices;
        bool openOnError = false;
        DebugOverlaySettings::Stats stats = DebugOverlaySettings::Stats::Off;
        DebugOverlaySettings::Corner statsCorner = DebugOverlaySettings::Corner::TopLeft;
        AllowLua allowLua = AllowLua::Development;
        unsigned int logBufferSize = 500;
        LogStackTraces logStackTraces = LogStackTraces::Warnings;

        static constexpr unsigned int maxLogBufferSize = 100000;

        bool operator==(const DebugSettings& other) const;
        bool operator!=(const DebugSettings& other) const { return !(*this == other); }

        // what the engine starts with: the overlay enabled per the mode and the build type
        DebugOverlaySettings toOverlaySettings(bool developmentBuild) const;
    };

    // The names of the values in project.yaml, MCP and the Project Settings window (the
    // labels are their human forms). Parsing is case-insensitive and takes '-' or ' ' for '_'.
    namespace DebugSettingsNames {
        const std::vector<std::string>& modes();         // never, development, always
        const std::vector<std::string>& gestures();      // three_finger_hold, four_finger_tap, none
        const std::vector<std::string>& popups();        // never, touch_devices, always
        const std::vector<std::string>& stats();         // off, fps, full
        const std::vector<std::string>& corners();       // top_left, top_right, bottom_left, bottom_right
        const std::vector<std::string>& allowLua();      // development, always, never
        const std::vector<std::string>& stackTraces();   // none, errors, warnings, all

        std::string name(DebugSettings::Mode value);
        std::string name(DebugOverlaySettings::Gesture value);
        std::string name(DebugOverlaySettings::Popup value);
        std::string name(DebugOverlaySettings::Stats value);
        std::string name(DebugOverlaySettings::Corner value);
        std::string name(DebugSettings::AllowLua value);
        std::string name(LogStackTraces value);

        bool parse(const std::string& text, DebugSettings::Mode& out);
        bool parse(const std::string& text, DebugOverlaySettings::Gesture& out);
        bool parse(const std::string& text, DebugOverlaySettings::Popup& out);
        bool parse(const std::string& text, DebugOverlaySettings::Stats& out);
        bool parse(const std::string& text, DebugOverlaySettings::Corner& out);
        bool parse(const std::string& text, DebugSettings::AllowLua& out);
        bool parse(const std::string& text, LogStackTraces& out);

        // a key name the overlay takes: "" (none) or one InputControlNames::parseKey knows
        bool isKeyName(const std::string& text);
    }

}
