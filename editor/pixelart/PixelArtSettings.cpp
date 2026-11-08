// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelArtSettings.h"

#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor::pixelart;

std::vector<std::string> PixelArtSettings::defaultCheckScreens(){
    return {"1280x720", "1366x768", "1920x1080", "2560x1440", "3840x2160", "2400x1080", "1280x800"};
}

std::vector<std::string> PixelArtSettings::defaultBackdrops(){
    return {"#1a1c2c", "#f4f4f4"};
}

std::vector<PixelArtScreen> PixelArtSettings::screens() const{
    std::vector<PixelArtScreen> out;
    for (const std::string& text : checkScreens){
        PixelArtScreen screen;
        if (parsePixelArtScreen(text, screen))
            out.push_back(screen);
    }
    return out;
}

bool jasperengine::editor::pixelart::parsePixelArtScreen(const std::string& text, PixelArtScreen& out){
    const size_t x = text.find('x');
    if (x == std::string::npos || x == 0 || x + 1 >= text.size())
        return false;
    auto number = [](const std::string& digits, int& value){
        if (digits.empty() || digits.size() > 5)
            return false;
        value = 0;
        for (char c : digits){
            if (!std::isdigit((unsigned char)c))
                return false;
            value = value * 10 + (c - '0');
        }
        return value >= 1 && value <= 16384;
    };
    PixelArtScreen screen;
    if (!number(text.substr(0, x), screen.width) || !number(text.substr(x + 1), screen.height))
        return false;
    out = screen;
    return true;
}

bool jasperengine::editor::pixelart::isPixelArtBackdrop(const std::string& text){
    if (text.size() != 7 || text[0] != '#')
        return false;
    for (size_t i = 1; i < text.size(); i++)
        if (!std::isxdigit((unsigned char)text[i]))
            return false;
    return true;
}

const char* jasperengine::editor::pixelart::pixelUpscaleModeName(PixelUpscaleMode mode){
    switch (mode){
        case PixelUpscaleMode::INTEGER_EXPAND: return "integer_expand";
        case PixelUpscaleMode::STRETCH_FILL: return "stretch_fill";
        case PixelUpscaleMode::INTEGER: default: return "integer";
    }
}

bool jasperengine::editor::pixelart::pixelUpscaleModeFromName(const std::string& name, PixelUpscaleMode& mode){
    for (PixelUpscaleMode m : {PixelUpscaleMode::INTEGER, PixelUpscaleMode::INTEGER_EXPAND, PixelUpscaleMode::STRETCH_FILL}){
        if (name == pixelUpscaleModeName(m)){
            mode = m;
            return true;
        }
    }
    return false;
}

YAML::Node jasperengine::editor::pixelart::encodePixelArtSettings(const PixelArtSettings& settings){
    const PixelArtSettings defaults;
    YAML::Node node(YAML::NodeType::Map);
    if (settings.enabled != defaults.enabled)
        node["enabled"] = settings.enabled;
    if (settings.checkOnImport != defaults.checkOnImport)
        node["checkOnImport"] = settings.checkOnImport;
    if (settings.checkOnPlay != defaults.checkOnPlay)
        node["checkOnPlay"] = settings.checkOnPlay;
    if (settings.checkScreens != defaults.checkScreens){
        YAML::Node list(YAML::NodeType::Sequence);
        for (const std::string& screen : settings.checkScreens)
            list.push_back(screen);
        node["checkScreens"] = list;
    }
    if (settings.backdrops != defaults.backdrops){
        YAML::Node list(YAML::NodeType::Sequence);
        for (const std::string& backdrop : settings.backdrops)
            list.push_back(backdrop);
        node["backdrops"] = list;
    }
    if (settings.upscaleMode != defaults.upscaleMode)
        node["upscaleMode"] = pixelUpscaleModeName(settings.upscaleMode);
    return node;
}

void jasperengine::editor::pixelart::decodePixelArtSettings(const YAML::Node& node, PixelArtSettings& settings){
    settings = PixelArtSettings();
    if (!node || !node.IsMap())
        return;
    auto boolean = [&](const char* key, bool& out){
        try { if (node[key]) out = node[key].as<bool>(); } catch (...) {}
    };
    auto strings = [&](const char* key, std::vector<std::string>& out, bool (*valid)(const std::string&)){
        const YAML::Node list = node[key];
        if (!list || !list.IsSequence())
            return;
        std::vector<std::string> values;
        for (const YAML::Node& item : list){
            std::string text;
            try { text = item.as<std::string>(); } catch (...) { continue; }
            if (valid(text))
                values.push_back(text);
        }
        out = values;
    };
    boolean("enabled", settings.enabled);
    boolean("checkOnImport", settings.checkOnImport);
    boolean("checkOnPlay", settings.checkOnPlay);
    strings("checkScreens", settings.checkScreens, [](const std::string& text){ PixelArtScreen s; return parsePixelArtScreen(text, s); });
    strings("backdrops", settings.backdrops, isPixelArtBackdrop);
    if (node["upscaleMode"]){
        std::string name;
        try { name = node["upscaleMode"].as<std::string>(); } catch (...) {}
        PixelUpscaleMode mode;
        if (pixelUpscaleModeFromName(name, mode))
            settings.upscaleMode = mode;
    }
}
