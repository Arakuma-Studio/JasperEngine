// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "DebugSettings.h"

#include "Input.h"
#include "input/InputControlNames.h"
#include "reflect/Reflect.h"


using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the names are the tokens of the reflection table's enums (editor/reflect/DebugReflect.cpp)
    const reflect::EnumDesc& enumOf(const char* field){
        return *reflect::debugSettingsField(field)->enumDesc;
    }

    // the tokens by value (the enums' order)
    std::vector<std::string> tokensOf(const char* field){
        const reflect::EnumDesc& desc = enumOf(field);
        std::vector<std::string> names(desc.items.size());
        for (const reflect::EnumItem& item : desc.items){
            if (item.value >= 0 && static_cast<size_t>(item.value) < names.size())
                names[static_cast<size_t>(item.value)] = item.token;
        }
        return names;
    }

    template<typename T>
    std::string nameOf(const char* field, T value){
        const reflect::EnumItem* item = reflect::enumItem(enumOf(field), static_cast<int>(value));
        return item ? std::string(item->token) : std::string();
    }

    // case-insensitive, '-' or ' ' for '_' (the table's normalize)
    template<typename T>
    bool parseOf(const char* field, const std::string& text, T& out){
        const reflect::EnumItem* item = reflect::enumItemByToken(enumOf(field), text);
        if (!item)
            return false;
        out = static_cast<T>(item->value);
        return true;
    }

}

bool DebugSettings::operator==(const DebugSettings& other) const{
    return overlay == other.overlay && toggleKey == other.toggleKey && statsKey == other.statsKey && gesture == other.gesture &&
           popup == other.popup && openOnError == other.openOnError && stats == other.stats && statsCorner == other.statsCorner &&
           allowLua == other.allowLua && logBufferSize == other.logBufferSize && logStackTraces == other.logStackTraces;
}

void DebugSettings::sanitize(const std::function<void(const std::string& key, const std::string& value)>& rejected){
    const DebugSettings defaults;
    if (!DebugSettingsNames::isKeyName(toggleKey)){
        if (rejected) rejected("toggleKey", toggleKey);
        toggleKey = defaults.toggleKey;
    }
    if (!DebugSettingsNames::isKeyName(statsKey)){
        if (rejected) rejected("statsKey", statsKey);
        statsKey = defaults.statsKey;
    }
    if (logBufferSize > maxLogBufferSize)
        logBufferSize = maxLogBufferSize;
}

DebugOverlaySettings DebugSettings::toOverlaySettings(bool developmentBuild) const{
    DebugOverlaySettings settings;
    settings.enabled = overlay == Mode::AllBuilds || (overlay == Mode::Development && developmentBuild);
    settings.toggleKey = toggleKey;
    settings.statsKey = statsKey;
    settings.gesture = gesture;
    settings.popup = popup;
    settings.openOnError = openOnError;
    settings.stats = stats;
    settings.statsCorner = statsCorner;
    settings.allowLua = allowLua == AllowLua::AllBuilds || (allowLua == AllowLua::Development && developmentBuild);
    return settings;
}

const std::vector<std::string>& DebugSettingsNames::modes(){ static const std::vector<std::string> names = tokensOf("overlay"); return names; }
const std::vector<std::string>& DebugSettingsNames::gestures(){ static const std::vector<std::string> names = tokensOf("gesture"); return names; }
const std::vector<std::string>& DebugSettingsNames::popups(){ static const std::vector<std::string> names = tokensOf("popup"); return names; }
const std::vector<std::string>& DebugSettingsNames::stats(){ static const std::vector<std::string> names = tokensOf("stats"); return names; }
const std::vector<std::string>& DebugSettingsNames::corners(){ static const std::vector<std::string> names = tokensOf("statsCorner"); return names; }
const std::vector<std::string>& DebugSettingsNames::allowLua(){ static const std::vector<std::string> names = tokensOf("allowLua"); return names; }
const std::vector<std::string>& DebugSettingsNames::stackTraces(){ static const std::vector<std::string> names = tokensOf("logStackTraces"); return names; }

std::string DebugSettingsNames::name(DebugSettings::Mode value){ return nameOf("overlay", value); }
std::string DebugSettingsNames::name(DebugOverlaySettings::Gesture value){ return nameOf("gesture", value); }
std::string DebugSettingsNames::name(DebugOverlaySettings::Popup value){ return nameOf("popup", value); }
std::string DebugSettingsNames::name(DebugOverlaySettings::Stats value){ return nameOf("stats", value); }
std::string DebugSettingsNames::name(DebugOverlaySettings::Corner value){ return nameOf("statsCorner", value); }
std::string DebugSettingsNames::name(DebugSettings::AllowLua value){ return nameOf("allowLua", value); }
std::string DebugSettingsNames::name(LogStackTraces value){ return nameOf("logStackTraces", value); }

bool DebugSettingsNames::parse(const std::string& text, DebugSettings::Mode& out){ return parseOf("overlay", text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugOverlaySettings::Gesture& out){ return parseOf("gesture", text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugOverlaySettings::Popup& out){ return parseOf("popup", text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugOverlaySettings::Stats& out){ return parseOf("stats", text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugOverlaySettings::Corner& out){ return parseOf("statsCorner", text, out); }
bool DebugSettingsNames::parse(const std::string& text, DebugSettings::AllowLua& out){ return parseOf("allowLua", text, out); }
bool DebugSettingsNames::parse(const std::string& text, LogStackTraces& out){ return parseOf("logStackTraces", text, out); }

bool DebugSettingsNames::isKeyName(const std::string& text){
    return text.empty() || InputControlNames::parseKey(text) != D_KEY_UNKNOWN;
}

namespace {
    constexpr DebugSettingsNames::SettingKey kSettingKeys[] = {
        {"debug.overlay", "overlay", "debug overlay console development builds"},
        {"debug.overlay_toggle_key", "toggleKey", "console key backquote"},
        {"debug.overlay_stats_key", "statsKey", "stats fps key"},
        {"debug.overlay_gesture", "gesture", "touch fingers console"},
        {"debug.overlay_popup", "popup", "bubble warnings errors counts"},
        {"debug.overlay_open_on_error", "openOnError", "console errors"},
        {"debug.overlay_stats", "stats", "stats fps frame time graph"},
        {"debug.overlay_stats_corner", "statsCorner", "stats position corner"},
        {"debug.overlay_allow_lua", "allowLua", "lua console commands"},
        {"debug.log_buffer_size", "logBufferSize", "log lines buffer entries"},
        {"debug.log_stack_traces", "logStackTraces", "stack trace traceback lua log"},
    };
}

std::span<const DebugSettingsNames::SettingKey> DebugSettingsNames::settingKeys(){
    return std::span<const SettingKey>(kSettingKeys);
}

std::string DebugSettingsNames::settingKeyOf(const char* field){
    for (const SettingKey& entry : kSettingKeys){
        if (std::string(field) == entry.field)
            return entry.key;
    }
    return std::string();
}
