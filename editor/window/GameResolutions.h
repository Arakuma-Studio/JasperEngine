// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace jasperengine::editor {

    // A fixed resolution of the Game view's resolution menu (Unity's Game view sizes): the game
    // renders at width x height pixels into an offscreen image fitted to the panel, and
    // Screen.dpi reports dpi (0: the host's) while it plays. Built-in ones and the project's
    // custom ones ("Add custom...", kept in the editor workspace).
    struct GameResolution {
        std::string name;    // "1920x1080", "2400x1080", or a custom name
        int width = 0;
        int height = 0;
        float dpi = 0.0f;
        std::string note;    // the menu's hint ("20:9 phone")

        bool operator==(const GameResolution& other) const {
            return name == other.name && width == other.width && height == other.height && dpi == other.dpi;
        }
        bool operator!=(const GameResolution& other) const { return !(*this == other); }
    };

    // the built-in sizes, landscape then portrait
    const std::vector<GameResolution>& builtInGameResolutions();

    // "1920x1080 (20:9 phone, 400 dpi)"
    std::string gameResolutionLabel(const GameResolution& resolution);

    // A resolution by name (built-in, then custom), or "WxH" / "WxH@dpi" (any size from 16 to
    // 16384); false with `error` otherwise
    bool findGameResolution(const std::string& text, const std::vector<GameResolution>& custom, GameResolution& out,
                            std::string& error);

    // a size the Game view can render (16..16384 a side, dpi 0..2000)
    bool isValidGameResolution(int width, int height, float dpi);

}
