// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioEffects.h"
#include "AudioFilters.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace jasperengine;

namespace {

    std::string lower(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            if (c == ' ' || c == '_' || c == '-') continue;
            out += (char)std::tolower((unsigned char)c);
        }
        return out;
    }

    // SoLoud attribute numbers (the filters' FILTERATTRIBUTE / FILTERPARAMS enums)
    constexpr int BIQUAD_FREQUENCY = 2, BIQUAD_RESONANCE = 3;
    constexpr int ECHO_DELAY = 1, ECHO_DECAY = 2, ECHO_FILTER = 3;
    constexpr int REVERB_FREEZE = 1, REVERB_ROOMSIZE = 2, REVERB_DAMP = 3, REVERB_WIDTH = 4;
    constexpr int SHAPER_AMOUNT = 1;
    constexpr int FLANGER_DELAY = 1, FLANGER_FREQ = 2;
    constexpr int LOFI_SAMPLERATE = 1, LOFI_BITDEPTH = 2;
    constexpr int BASSBOOST_BOOST = 1;
    constexpr int DUCK_ONRAMP = 1, DUCK_OFFRAMP = 2, DUCK_LEVEL = 3;

    using C = AudioParamCurve;

    std::vector<AudioEffectInfo> makeTable() {
        std::vector<AudioEffectInfo> table;
        table.push_back({AudioEffectType::LowPass, "lowpass", "Low Pass", "Cuts the frequencies above the cutoff (22000 Hz = off)", false, {
            {"cutoff", "Cutoff", "Hz", 5000.0f, 10.0f, 22000.0f, C::Frequency, BIQUAD_FREQUENCY},
            {"resonance", "Resonance", "", 1.0f, 0.1f, 20.0f, C::Linear, BIQUAD_RESONANCE}}});
        table.push_back({AudioEffectType::HighPass, "highpass", "High Pass", "Cuts the frequencies below the cutoff (10 Hz = off)", false, {
            {"cutoff", "Cutoff", "Hz", 500.0f, 10.0f, 22000.0f, C::Frequency, BIQUAD_FREQUENCY},
            {"resonance", "Resonance", "", 1.0f, 0.1f, 20.0f, C::Linear, BIQUAD_RESONANCE}}});
        table.push_back({AudioEffectType::BandPass, "bandpass", "Band Pass", "Keeps the frequencies around the cutoff", false, {
            {"cutoff", "Cutoff", "Hz", 1000.0f, 10.0f, 22000.0f, C::Frequency, BIQUAD_FREQUENCY},
            {"resonance", "Resonance", "", 1.0f, 0.1f, 20.0f, C::Linear, BIQUAD_RESONANCE}}});
        table.push_back({AudioEffectType::Echo, "echo", "Echo", "Repeats the sound after a delay", false, {
            {"delay", "Delay", "s", 0.3f, 0.01f, 2.0f, C::Linear, ECHO_DELAY},
            {"decay", "Decay", "", 0.5f, 0.0f, 1.0f, C::Linear, ECHO_DECAY},
            {"filter", "Filter", "", 0.0f, 0.0f, 0.99f, C::Linear, ECHO_FILTER}}});
        table.push_back({AudioEffectType::Reverb, "reverb", "Reverb", "Room reverberation (Freeverb)", true, {
            {"roomSize", "Room Size", "", 0.5f, 0.0f, 1.0f, C::Linear, REVERB_ROOMSIZE},
            {"damp", "Damp", "", 0.5f, 0.0f, 1.0f, C::Linear, REVERB_DAMP},
            {"width", "Width", "", 1.0f, 0.0f, 1.0f, C::Linear, REVERB_WIDTH},
            {"freeze", "Freeze", "", 0.0f, 0.0f, 1.0f, C::Linear, REVERB_FREEZE}}});
        table.push_back({AudioEffectType::Equalizer, "equalizer", "Equalizer", "Eight band gains, low to high (1 = unchanged)", true, {
            {"band1", "Band 1", "", 1.0f, 0.0f, 4.0f, C::Linear, 1},
            {"band2", "Band 2", "", 1.0f, 0.0f, 4.0f, C::Linear, 2},
            {"band3", "Band 3", "", 1.0f, 0.0f, 4.0f, C::Linear, 3},
            {"band4", "Band 4", "", 1.0f, 0.0f, 4.0f, C::Linear, 4},
            {"band5", "Band 5", "", 1.0f, 0.0f, 4.0f, C::Linear, 5},
            {"band6", "Band 6", "", 1.0f, 0.0f, 4.0f, C::Linear, 6},
            {"band7", "Band 7", "", 1.0f, 0.0f, 4.0f, C::Linear, 7},
            {"band8", "Band 8", "", 1.0f, 0.0f, 4.0f, C::Linear, 8}}});
        table.push_back({AudioEffectType::Distortion, "distortion", "Distortion", "Wave shaping", false, {
            {"amount", "Amount", "", 0.5f, -1.0f, 1.0f, C::Linear, SHAPER_AMOUNT}}});
        table.push_back({AudioEffectType::Flanger, "flanger", "Flanger", "A short delay swept by an oscillator", false, {
            {"delay", "Delay", "s", 0.005f, 0.001f, 0.1f, C::Linear, FLANGER_DELAY},
            {"frequency", "Frequency", "Hz", 10.0f, 0.01f, 100.0f, C::Frequency, FLANGER_FREQ}}});
        table.push_back({AudioEffectType::Lofi, "lofi", "Lo-Fi", "Lower sample rate and bit depth", false, {
            {"sampleRate", "Sample Rate", "Hz", 8000.0f, 100.0f, 22000.0f, C::Frequency, LOFI_SAMPLERATE},
            {"bitDepth", "Bit Depth", "", 8.0f, 0.5f, 16.0f, C::Linear, LOFI_BITDEPTH}}});
        table.push_back({AudioEffectType::BassBoost, "bassboost", "Bass Boost", "Boosts the low frequencies", false, {
            {"boost", "Boost", "", 4.0f, 0.0f, 10.0f, C::Linear, BASSBOOST_BOOST}}});
        table.push_back({AudioEffectType::Duck, "duck", "Duck", "Lowers this group while the side-chain group plays", false, {
            {"level", "Level", "", 0.5f, 0.0f, 1.0f, C::Linear, DUCK_LEVEL},
            {"attack", "Attack", "s", 0.1f, 0.0f, 5.0f, C::Linear, DUCK_ONRAMP},
            {"release", "Release", "s", 0.5f, 0.0f, 5.0f, C::Linear, DUCK_OFFRAMP}}});
        table.push_back({AudioEffectType::Limiter, "limiter", "Limiter", "Keeps the peaks under the threshold", false, {
            {"threshold", "Threshold", "dB", -1.0f, -30.0f, 0.0f, C::Linear, LimiterFilter::THRESHOLD},
            {"release", "Release", "s", 0.1f, 0.001f, 2.0f, C::Linear, LimiterFilter::RELEASE}}});
        return table;
    }

    const AudioEffectParamInfo wetInfo = {"wet", "Wet", "", 1.0f, 0.0f, 1.0f, C::Linear, 0};

}

const std::vector<AudioEffectInfo>& AudioEffects::all() {
    static const std::vector<AudioEffectInfo> table = makeTable();
    return table;
}

const AudioEffectInfo* AudioEffects::find(AudioEffectType type) {
    for (const AudioEffectInfo& info : all()) {
        if (info.type == type) return &info;
    }
    return nullptr;
}

const AudioEffectInfo* AudioEffects::find(const std::string& name) {
    const std::string key = lower(name);
    if (key.empty()) return nullptr;
    for (const AudioEffectInfo& info : all()) {
        if (key == info.name || key == lower(info.label)) return &info;
    }
    return nullptr;
}

AudioEffectType AudioEffects::typeFromName(const std::string& name) {
    const AudioEffectInfo* info = find(name);
    return info ? info->type : AudioEffectType::Unknown;
}

std::string AudioEffects::typeName(AudioEffectType type) {
    const AudioEffectInfo* info = find(type);
    return info ? info->name : "unknown";
}

const AudioEffectParamInfo* AudioEffects::findParam(AudioEffectType type, const std::string& name) {
    if (name == "wet") return &wetInfo;
    const AudioEffectInfo* info = find(type);
    if (!info) return nullptr;
    for (const AudioEffectParamInfo& param : info->params) {
        if (name == param.name) return &param;
    }
    // case-insensitive second chance ("roomsize", "room_size")
    const std::string key = lower(name);
    for (const AudioEffectParamInfo& param : info->params) {
        if (key == lower(param.name)) return &param;
    }
    return nullptr;
}

const AudioEffectParamInfo& AudioEffects::wetParam() {
    return wetInfo;
}

std::string AudioEffects::paramNames(AudioEffectType type) {
    std::string out;
    if (const AudioEffectInfo* info = find(type)) {
        for (const AudioEffectParamInfo& param : info->params) {
            out += std::string(param.name) + ", ";
        }
    }
    if (type == AudioEffectType::Duck) out += "sidechainGroup, ";
    return out + "wet";
}

// ---------------------------------------------------------------- LimiterFilter

namespace {

    class LimiterFilterInstance : public SoLoud::FilterInstance {
        float gain = 1.0f;
    public:
        explicit LimiterFilterInstance(LimiterFilter* parent) {
            initParams(3);
            mParam[LimiterFilter::THRESHOLD] = parent->threshold;
            mParam[LimiterFilter::RELEASE] = parent->release;
        }

        void filter(float* buffer, unsigned int samples, unsigned int bufferSize, unsigned int channels, float samplerate, SoLoud::time time) override {
            updateParams(time);
            const float threshold = std::pow(10.0f, std::min(0.0f, mParam[LimiterFilter::THRESHOLD]) / 20.0f);
            const float release = std::max(0.001f, mParam[LimiterFilter::RELEASE]);
            // the share of the way back to unity gain per sample
            const float recover = 1.0f - std::exp(-1.0f / (release * samplerate));
            const float wet = mParam[LimiterFilter::WET];
            for (unsigned int i = 0; i < samples; i++) {
                float peak = 0.0f;
                for (unsigned int c = 0; c < channels; c++) {
                    peak = std::max(peak, std::fabs(buffer[i + c * bufferSize]));
                }
                const float target = peak > threshold ? threshold / peak : 1.0f;
                if (target < gain) {
                    gain = target;
                } else {
                    gain += (target - gain) * recover;
                }
                for (unsigned int c = 0; c < channels; c++) {
                    float& sample = buffer[i + c * bufferSize];
                    sample += (sample * gain - sample) * wet;
                }
            }
        }
    };

    class MeterFilterInstance : public SoLoud::FilterInstance {
        MeterFilter* parent;
    public:
        explicit MeterFilterInstance(MeterFilter* parent): parent(parent) {
            initParams(1);
        }

        void filter(float* buffer, unsigned int samples, unsigned int bufferSize, unsigned int channels, float, SoLoud::time) override {
            float peak = 0.0f;
            for (unsigned int c = 0; c < channels; c++) {
                const float* data = buffer + c * bufferSize;
                for (unsigned int i = 0; i < samples; i++) {
                    peak = std::max(peak, std::fabs(data[i]));
                }
            }
            const unsigned int slot = parent->next.fetch_add(1, std::memory_order_relaxed) % MeterFilter::blockCount;
            parent->blocks[slot].store(peak, std::memory_order_relaxed);
        }
    };

}

int LimiterFilter::getParamCount() {
    return 3;
}

const char* LimiterFilter::getParamName(unsigned int index) {
    switch (index) {
        case WET: return "Wet";
        case THRESHOLD: return "Threshold";
        case RELEASE: return "Release";
    }
    return nullptr;
}

unsigned int LimiterFilter::getParamType(unsigned int) {
    return FLOAT_PARAM;
}

float LimiterFilter::getParamMax(unsigned int index) {
    switch (index) {
        case THRESHOLD: return 0.0f;
        case RELEASE: return 2.0f;
    }
    return 1.0f;
}

float LimiterFilter::getParamMin(unsigned int index) {
    switch (index) {
        case THRESHOLD: return -30.0f;
        case RELEASE: return 0.001f;
    }
    return 0.0f;
}

SoLoud::FilterInstance* LimiterFilter::createInstance() {
    return new LimiterFilterInstance(this);
}

// ---------------------------------------------------------------- MeterFilter

MeterFilter::MeterFilter() {
    reset();
}

float MeterFilter::getPeak() const {
    float peak = 0.0f;
    for (int i = 0; i < blockCount; i++) {
        peak = std::max(peak, blocks[i].load(std::memory_order_relaxed));
    }
    return peak;
}

void MeterFilter::reset() {
    for (int i = 0; i < blockCount; i++) {
        blocks[i].store(0.0f, std::memory_order_relaxed);
    }
}

SoLoud::FilterInstance* MeterFilter::createInstance() {
    return new MeterFilterInstance(this);
}
