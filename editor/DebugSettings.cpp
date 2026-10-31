// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "DebugSettings.h"

#include "Input.h"
#include "input/InputControlNames.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    template<typename T>
    std::string nameOf(const std::vector<std::string>& names, T value){
        const size_t index = static_cast<size_t>(value);
        return index < names.size() ? names[index] : std::string();
    }

    std::string normalized(const std::string& text){
        std::string out;
        for (char c : text){
            if (c == '-' || c == ' ')
                out += '_';
            else
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    template<typename T>
    bool parseOf(const std::vector<std::string>& names, const std::string& text, T& out){
        const std::string wanted = normalized(text);
        for (size_t i = 0; i < names.size(); i++){
            if (names[i] == wanted){
                out = static_cast<T>(i);
                return true;
            }
        }
        return false;
    }

}

bool DebugSettings::operator==(const DebugSettings& other) const{
    return overlay == other.overlay && toggleKey == other.toggleKey && statsKey == other.statsKey && gesture == other.gesture &&
           popup == other.popup && openOnError == other.openOnError && stats == other.stats && statsCorner == other.statsCorner &&
           allowLua == other.allowLua && logBufferSize == other.logBufferSize && logStackTraces == other.logStackTraces;
}

DebugOverlaySettings DebugSettings::toOverlaySettings(bool developmentBuild) const{
    DebugOverlaySettings settings;
    settings.enabled = overlay == Mode::Always || (overlay == Mode::Development && developmentBuild);
    settings.toggleKey = toggleKey;
    settings.statsKey = statsKey;
    settings.gesture = gesture;
    settings.popup = popup;
    settings.openOnError = openOnError;
    settings.stats = stats;
    settings.statsCorner = statsCorner;
    settings.allowLua = allowLua == AllowLua::Always || (allowLua == AllowLua::Development && developmentBuild);
    return settings;
}

// the orders match the enums
const std::vector<std::string>& DebugSettingsNames::modes(){
    static const std::vector<std::string> names = {"never", "development", "always"};
    return names;
}

const std::vector<std::string>& DebugSettingsNames::gestures(){
    static const std::vector<std::string> names = {"three_finger_hold", "four_finger_tap", "none"};
    return names;
}

const std::vector<std::string>& DebugSettingsNames::popups(){
    static const std::vector<std::string> names = {"never", "touch_devices", "always"};
    return names;
}

const std::vector<std::string>& DebugSettingsNames::stats(){
    static const std::vector<std::string> names = {"off", "fps", "full"};
    return names;
}

const std::vector<std::string>& DebugSettingsNames::corners(){
    static const std::vector<std::string> names = {"top_left", "top_right", "bottom_left", "bottom_right"};
    return names;
}

const std::vector<std::string>& DebugSettingsNames::allowLua(){
    static const std::vector<std::string> names = {"development", "always", "never"};
    return names;
}

const std::vector<std::string>& DebugSettingsNames::stackTraces(){
    static const std::vector<std::string> names = {"none", "errors", "warnings", "all"};
    return names;
}

std::string DebugSettingsNames::name(DebugSettings::Mode value){ return nameOf(modes(), value); }
std::string DebugSettingsNames::name(DebugOverlaySettings::Gesture value){ return nameOf(gestures(), value); }
std::string DebugSettingsNames::name(DebugOverlaySettings::Popup value){ return nameOf(popups(), value); }
std::string DebugSettingsNames::name(DebugOverlaySettings::Stats value){ return nameOf(stats(), value); }
std::string DebugSettingsNames::name(DebugOverlaySettings::Corner value){ return nameOf(corners(), value); }
std::string DebugSettingsNames::name(DebugSettings::AllowLua value){ return nameOf(allowLua(), value); }
std::string DebugSettingsNames::name(LogStackTraces value){ return nameOf(stackTraces(), value); }

bool DebugSettingsNames::parse(const std::string& text, DebugSettings::Mode& out){ return parseOf(modes(), text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugOverlaySettings::Gesture& out){ return parseOf(gestures(), text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugOverlaySettings::Popup& out){ return parseOf(popups(), text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugOverlaySettings::Stats& out){ return parseOf(stats(), text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugOverlaySettings::Corner& out){ return parseOf(corners(), text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugSettings::AllowLua& out){ return parseOf(allowLua(), text, out); }
bool DebugSettingsNames::parse(const std::string& text, LogStackTraces& out){ return parseOf(stackTraces(), text, out); }

bool DebugSettingsNames::isKeyName(const std::string& text){
    return text.empty() || InputControlNames::parseKey(text) != D_KEY_UNKNOWN;
}
