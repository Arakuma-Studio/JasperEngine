// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "GameResolutions.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;

const std::vector<editor::GameResolution>& editor::builtInGameResolutions() {
    static const std::vector<GameResolution> resolutions = {
        {"1280x720", 1280, 720, 0.0f, "HD"},
        {"1920x1080", 1920, 1080, 0.0f, "Full HD"},
        {"2560x1440", 2560, 1440, 0.0f, "QHD"},
        {"2400x1080", 2400, 1080, 400.0f, "20:9 phone"},
        {"2732x2048", 2732, 2048, 264.0f, "4:3 tablet"},
        {"720x1280", 720, 1280, 0.0f, "HD portrait"},
        {"1080x1920", 1080, 1920, 0.0f, "Full HD portrait"},
        {"1440x2560", 1440, 2560, 0.0f, "QHD portrait"},
        {"1080x2400", 1080, 2400, 400.0f, "20:9 phone portrait"},
        {"2048x2732", 2048, 2732, 264.0f, "4:3 tablet portrait"},
    };
    return resolutions;
}

std::string editor::gameResolutionLabel(const GameResolution& resolution) {
    const std::string size = std::to_string(resolution.width) + "x" + std::to_string(resolution.height);
    std::string label = (resolution.name.empty() || resolution.name == size) ? size : resolution.name + " " + size;
    std::string details = resolution.note;
    if (resolution.dpi > 0.0f) {
        char dpi[32];
        std::snprintf(dpi, sizeof(dpi), "%g dpi", resolution.dpi);
        details += (details.empty() ? "" : ", ") + std::string(dpi);
    }
    if (!details.empty()) label += " (" + details + ")";
    return label;
}

bool editor::isValidGameResolution(int width, int height, float dpi) {
    return width >= 16 && width <= 16384 && height >= 16 && height <= 16384 && std::isfinite(dpi) && dpi >= 0.0f && dpi <= 2000.0f;
}

bool editor::findGameResolution(const std::string& text, const std::vector<GameResolution>& custom, GameResolution& out,
                                std::string& error) {
    for (const GameResolution& resolution : builtInGameResolutions()) {
        if (resolution.name == text) {
            out = resolution;
            return true;
        }
    }
    for (const GameResolution& resolution : custom) {
        if (resolution.name == text) {
            out = resolution;
            return true;
        }
    }

    // "WxH" or "WxH@dpi"
    int width = 0, height = 0;
    float dpi = 0.0f;
    char rest = 0;
    const bool sized = std::sscanf(text.c_str(), "%dx%d%c", &width, &height, &rest) == 2 ||
                       std::sscanf(text.c_str(), "%dx%d@%f%c", &width, &height, &dpi, &rest) == 3;
    if (!sized || !isValidGameResolution(width, height, dpi)) {
        std::string names;
        for (const GameResolution& resolution : builtInGameResolutions()) names += " " + resolution.name;
        for (const GameResolution& resolution : custom) names += " \"" + resolution.name + "\"";
        error = "a resolution is \"WxH\" (16..16384), \"WxH@dpi\", or a preset:" + names;
        return false;
    }
    out = GameResolution();
    out.width = width;
    out.height = height;
    out.dpi = dpi;
    out.name = std::to_string(width) + "x" + std::to_string(height);
    // a built-in size keeps its dpi and hint unless one is given
    for (const GameResolution& resolution : builtInGameResolutions()) {
        if (resolution.width == width && resolution.height == height) {
            out.note = resolution.note;
            if (dpi == 0.0f) out.dpi = resolution.dpi;
        }
    }
    if (dpi > 0.0f) out.dpi = dpi;
    return true;
}
