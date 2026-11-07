// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Layered placeholder presets in character families (spec S36 M8): ui_click, swoosh, impact and
    // pop, each a list of layers summed, then a chain. A family fixes the timbre; the seed varies only
    // how hard it is struck, its timing and its noise, so variations sound like one palette.
    // Idea from earshot (MIT), reimplemented from sound-layer's phase 7 spec. Plain functions, safe on
    // any thread; the output is a pure function of the preset, family, seed and params.
    class SoundDesign {
    public:
        using Json = nlohmann::json;

        static constexpr unsigned int SAMPLE_RATE = 48000;

        static bool isLayered(const std::string& preset);
        // Its families; the first is the default. Empty for any other preset.
        static std::vector<std::string> families(const std::string& preset);
        // The raw sum and chain, mono, before the render's fades. An unknown family fails with the list.
        static bool render(const std::string& preset, const std::string& family, uint32_t seed, const Json& params,
                           std::vector<float>& samples, std::string& error);
    };

}
