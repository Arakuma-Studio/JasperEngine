// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef AudioEffects_h
#define AudioEffects_h

#include "Export.h"

#include <string>
#include <vector>

namespace jasperengine {

    // The effects a mixer group can hold (audio-mixer spec 7.2). Unknown: a type this version does
    // not know, kept in the file and inactive.
    enum class AudioEffectType {
        Unknown,
        LowPass,
        HighPass,
        BandPass,
        Echo,
        Reverb,
        Equalizer,
        Distortion,
        Flanger,
        Lofi,
        BassBoost,
        Duck,
        Limiter
    };

    // How a parameter moves in a transition (spec A7): dB and plain values linearly, frequencies
    // along log2(Hz). A Switch moves linearly too and counts as on from 0.5; the editor draws a checkbox.
    enum class AudioParamCurve { Linear, Decibel, Frequency, Switch };

    struct AudioEffectParamInfo {
        const char* name;        // the file / Lua / MCP name ("cutoff")
        const char* label;       // the editor's ("Cutoff")
        const char* unit;        // "Hz", "s", "dB", "" ...
        float defaultValue;
        float minValue;
        float maxValue;
        AudioParamCurve curve;
        int attribute;           // the SoLoud filter attribute it writes
    };

    struct AudioEffectInfo {
        AudioEffectType type;
        const char* name;        // "lowpass"
        const char* label;       // "Low Pass"
        const char* description; // one line for the editor and the guide
        bool costly;             // the editor shows "cost: high" (reverb, equalizer)
        std::vector<AudioEffectParamInfo> params;   // without wet (every effect has it, attribute 0)
    };

    class JASPERENGINE_API AudioEffects {
    public:
        // every known effect, in the editor's add-menu order
        static const std::vector<AudioEffectInfo>& all();
        static const AudioEffectInfo* find(AudioEffectType type);
        // by file name ("lowpass"), case-insensitive; also accepts the label ("Low Pass")
        static const AudioEffectInfo* find(const std::string& name);
        static AudioEffectType typeFromName(const std::string& name);
        static std::string typeName(AudioEffectType type);
        // the parameter of an effect by name ("cutoff"; "wet" is the common one), null when unknown
        static const AudioEffectParamInfo* findParam(AudioEffectType type, const std::string& name);
        static const AudioEffectParamInfo& wetParam();
        // the comma separated parameter names of an effect, for messages ("cutoff, resonance, wet")
        static std::string paramNames(AudioEffectType type);

        // Low-pass at or above this share of the sample rate and high-pass at or below 10 Hz run
        // bypassed (spec A5): Unity's "off" values (22000 Hz, 10 Hz) behave as off
        static constexpr float lowPassBypassRatio = 0.45f;
        static constexpr float highPassBypassHz = 10.0f;
        static constexpr int maxEffectsPerGroup = 8;
    };

}

#endif /* AudioEffects_h */
