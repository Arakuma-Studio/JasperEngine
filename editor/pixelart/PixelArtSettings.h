// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Pixel Art (docs/specs/S42-pixel-art-mode.md R1, R18, §6.2): Pixel Art Mode and
// what it decides for new things, when the validator runs, the screens it lays the scenes out on,
// the Upscale Mode new Pixel Perfect Cameras get and the backdrops S46-S48 read. project.yaml's
// `pixelArt:` holds only what differs from these defaults (no key at all when nothing does).

#pragma once

#include "pixelart/PixelPerfectLayout.h"

#include "yaml-cpp/yaml.h"

#include <string>
#include <vector>

namespace jasperengine::editor::pixelart {

    struct PixelArtScreen {
        int width = 0, height = 0;
        bool operator==(const PixelArtScreen&) const = default;
    };

    struct PixelArtSettings {
        bool enabled = false;
        bool checkOnImport = true;
        bool checkOnPlay = true;
        std::vector<std::string> checkScreens = defaultCheckScreens();
        std::vector<std::string> backdrops = defaultBackdrops();
        PixelUpscaleMode upscaleMode = PixelUpscaleMode::INTEGER;

        bool operator==(const PixelArtSettings&) const = default;

        // 1280x720, 1366x768, 1920x1080, 2560x1440, 3840x2160, 2400x1080, 1280x800
        static std::vector<std::string> defaultCheckScreens();
        // the dark and the light backdrop: #1a1c2c, #f4f4f4
        static std::vector<std::string> defaultBackdrops();

        // the screens of checkScreens that parse, in order
        std::vector<PixelArtScreen> screens() const;
    };

    // "1366x768" (1...16384 each side); false otherwise
    bool parsePixelArtScreen(const std::string& text, PixelArtScreen& out);
    // "#1a1c2c" (six hex digits, any case)
    bool isPixelArtBackdrop(const std::string& text);
    // "integer", "integer_expand", "stretch_fill"
    const char* pixelUpscaleModeName(PixelUpscaleMode mode);
    bool pixelUpscaleModeFromName(const std::string& name, PixelUpscaleMode& mode);

    // project.yaml's `pixelArt:` map (empty when every value is the default) and back; values
    // that do not parse keep their defaults
    YAML::Node encodePixelArtSettings(const PixelArtSettings& settings);
    void decodePixelArtSettings(const YAML::Node& node, PixelArtSettings& settings);

}
