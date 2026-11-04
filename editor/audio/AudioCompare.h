// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"

#include <string>

namespace jasperengine::editor {

    // Differences in words: what a listener
    // would hear differ between two analysed sounds, from their analysis items alone (no decode):
    // loudness, brightness, attack, tail, low end, length and bandwidth, in that order, each only
    // past its hearing-sized limit (SoundTypes.h's `-- differences`), at most
    // SoundTypes::DIFFERENCES_LISTED. analyze_audio's differences_vs_1 and the Sound Browser's
    // Compare with selected both call it.
    class AudioCompare {
    public:
        using Json = nlohmann::json;

        // {keys: ["loudness", ...], text: "2 is 3.0 dB quieter, darker (2.1 -> 1.6 kHz) and has a
        // shorter tail (180 -> 95 ms)"}: item b against item a (analyze_audio items, or the cached
        // analysis of a file); `name` names b in the text. With nothing past its limit, keys is
        // empty and the text "no measurable difference big enough to hear".
        static Json differences(const Json& a, const Json& b, const std::string& name);

        static constexpr const char* NONE = "no measurable difference big enough to hear";
    };

}
