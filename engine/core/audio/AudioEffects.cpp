// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioEffects.h"
#include "AudioFilters.h"
#include "TruePeak.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

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
            {"release", "Release", "s", 0.1f, 0.001f, 2.0f, C::Linear, LimiterFilter::RELEASE},
            {"lookahead", "Lookahead", "ms", 0.0f, 0.0f, 5.0f, C::Linear, LimiterFilter::LOOKAHEAD},
            {"truePeak", "True Peak", "", 0.0f, 0.0f, 1.0f, C::Switch, LimiterFilter::TRUE_PEAK}}});
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

    // the detector aims this far under the threshold, for the rounding of a gain that moves inside the
    // true-peak filter's taps; the guarantee (S38 R5) allows 0.1 dB over the threshold
    constexpr double LIMITER_MARGIN_DB = 0.05;
    // the moving average leaves out the span of the filter's taps, so that all the samples an
    // interpolated point depends on leave the delay under the gain its detection asked for
    constexpr unsigned int LIMITER_TRUE_PEAK_SPAN = 2 * TruePeak::DELAY;
    constexpr double LIMITER_MAX_LOOKAHEAD_MS = 5.0;
    constexpr double LIMITER_FADE_SECONDS = 0.005;
    constexpr double LIMITER_READOUT_SECONDS = 0.1;

    class LimiterFilterInstance : public SoLoud::FilterInstance {
        LimiterFilter* parent;
        float gain = 1.0f;

        // The rest is the delay's path; unused and unallocated with lookahead 0 and truePeak 0.
        unsigned int rate = 0;
        unsigned int channelCount = 0;
        // frames of delay the buffers hold at most, and now
        unsigned int capacity = 0;
        unsigned int delayFrames = 0;
        // the moving average's length
        unsigned int averageFrames = 1;
        float runningLookahead = 0.0f;
        bool runningTruePeak = false;
        // the delay's path is on: limiting, or fading out to the direct signal
        bool running = false;
        // a frame went through: from now on a start crossfades (a new instance starts limited)
        bool started = false;
        // the crossfade between the direct signal and the limited one, 0 (direct) to fadeFrames + 1
        unsigned int fadePosition = 0;
        unsigned int fadeFrames = 0;

        // channel c at c * capacity
        std::vector<float> delay;
        unsigned int delayPosition = 0;
        // the last frames that came in, which a start plays through the limiter again, so that the
        // delayed signal continues the direct one instead of starting from silence
        std::vector<float> recent;
        unsigned int recentPosition = 0;
        unsigned int recentCount = 0;
        TruePeak detector{1};

        // the gain needed, after the release (one pole back toward 1)
        double envelope = 1.0;
        // its running minimum over the delay: a monotonic queue in a ring
        std::vector<double> minValue;
        std::vector<uint64_t> minFrame;
        unsigned int minHead = 0;
        unsigned int minCount = 0;
        // the moving average of those minima
        std::vector<double> averageRing;
        unsigned int averagePosition = 0;
        double averageSum = 1.0;
        uint64_t frame = 0;

        // the reduction of the last blocks, for the readout
        static constexpr unsigned int READOUT_BLOCKS = 32;
        float readoutDb[READOUT_BLOCKS] = {};
        unsigned int readoutFrames[READOUT_BLOCKS] = {};
        unsigned int readoutNext = 0;

        void allocate(unsigned int newRate, unsigned int channels) {
            rate = newRate;
            channelCount = channels;
            capacity = (unsigned int)std::ceil(LIMITER_MAX_LOOKAHEAD_MS * 0.001 * rate) + 1;
            fadeFrames = (unsigned int)std::lround(LIMITER_FADE_SECONDS * rate);
            delay.assign((size_t)capacity * channels, 0.0f);
            recent.assign((size_t)capacity * channels, 0.0f);
            detector = TruePeak(channels);
            minValue.assign((size_t)capacity + 1, 1.0);
            minFrame.assign((size_t)capacity + 1, 0);
            averageRing.assign(capacity, 1.0);
            recentPosition = 0;
            recentCount = 0;
            running = false;
        }

        // the delay's path starts over for these parameters
        void restart(float lookahead, bool truePeak) {
            runningLookahead = lookahead;
            runningTruePeak = truePeak;
            delayFrames = LimiterFilter::lookaheadFrames(lookahead, (float)rate);
            const unsigned int span = truePeak ? LIMITER_TRUE_PEAK_SPAN : 0;
            averageFrames = std::max(delayFrames > span ? delayFrames - span : 1u, 1u);
            std::fill(delay.begin(), delay.end(), 0.0f);
            detector.reset();
            delayPosition = 0;
            envelope = 1.0;
            minHead = 0;
            minCount = 0;
            std::fill(averageRing.begin(), averageRing.end(), 1.0);
            averagePosition = 0;
            averageSum = (double)averageFrames;
            frame = 0;
            running = true;
        }

        // the detector, the release, the running minimum and the moving average of one frame of
        // input: the gain of the frame leaving the delay
        double gainFor(const float* input, unsigned int channels, bool truePeak, double target, double releaseStep) {
            double level = 0.0;
            for (unsigned int c = 0; c < channels; c++) {
                level = std::max(level, truePeak ? (double)detector.processSample(c, input[c]) : (double)std::fabs(input[c]));
            }
            const double needed = level > target ? target / level : 1.0;
            envelope = std::min(needed, envelope + (1.0 - envelope) * releaseStep);

            // frames [frame - delayFrames, frame]: the one that left the window goes first, so the
            // queue never holds more than the ring has
            const unsigned int ring = capacity + 1;
            while (minCount > 0 && frame - minFrame[minHead] > delayFrames) {
                minHead = (minHead + 1) % ring;
                minCount--;
            }
            while (minCount > 0 && minValue[(minHead + minCount - 1) % ring] >= envelope) {
                minCount--;
            }
            const unsigned int slot = (minHead + minCount) % ring;
            minValue[slot] = envelope;
            minFrame[slot] = frame;
            minCount++;
            const double minimum = minValue[minHead];

            averageSum += minimum - averageRing[averagePosition];
            averageRing[averagePosition] = minimum;
            averagePosition++;
            if (averagePosition == averageFrames) {
                // the sum again from its terms, so that no rounding drifts in over hours
                averagePosition = 0;
                averageSum = 0.0;
                for (unsigned int i = 0; i < averageFrames; i++) {
                    averageSum += averageRing[i];
                }
            }
            frame++;
            return std::min(1.0, averageSum / (double)averageFrames);
        }

        void remember(const float* input, unsigned int channels) {
            for (unsigned int c = 0; c < channels; c++) {
                recent[(size_t)c * capacity + recentPosition] = input[c];
            }
            recentPosition = (recentPosition + 1) % capacity;
            recentCount = std::min(recentCount + 1, capacity);
        }

        // frames [from, to) of the buffer pass as they are; a start replays the last of them
        void keepDirect(const float* buffer, unsigned int from, unsigned int to, unsigned int bufferSize, unsigned int channels) {
            float input[8];
            for (unsigned int i = to - from > capacity ? to - capacity : from; i < to; i++) {
                for (unsigned int c = 0; c < channels; c++) {
                    const float x = buffer[i + c * bufferSize];
                    input[c] = std::isfinite(x) ? x : 0.0f;
                }
                remember(input, channels);
            }
        }

        // (out of the audio loop, and of its caller's inlining: called from three places)
        void publishReduction(double smallestGain, unsigned int samples) {
            const float db = smallestGain < 1.0 ? (float)(-20.0 * std::log10(std::max(smallestGain, 1e-6))) : 0.0f;
            readoutDb[readoutNext] = db;
            readoutFrames[readoutNext] = samples;
            readoutNext = (readoutNext + 1) % READOUT_BLOCKS;
            // back from the newest block until 100 ms are covered
            const float window = (float)(LIMITER_READOUT_SECONDS * (rate > 0 ? rate : (unsigned int)parent->sampleRate));
            float largest = 0.0f;
            float covered = 0.0f;
            for (unsigned int i = 1; i <= READOUT_BLOCKS && covered < window; i++) {
                const unsigned int at = (readoutNext + READOUT_BLOCKS - i) % READOUT_BLOCKS;
                largest = std::max(largest, readoutDb[at]);
                covered += (float)readoutFrames[at];
            }
            parent->gainReductionDb.store(largest, std::memory_order_relaxed);
        }

        void filterWithoutDelay(float* buffer, unsigned int samples, unsigned int bufferSize, unsigned int channels, float samplerate) {
            const float threshold = std::pow(10.0f, std::min(0.0f, mParam[LimiterFilter::THRESHOLD]) / 20.0f);
            const float release = std::max(0.001f, mParam[LimiterFilter::RELEASE]);
            // the share of the way back to unity gain per sample
            const float recover = 1.0f - std::exp(-1.0f / (release * samplerate));
            const float wet = mParam[LimiterFilter::WET];
            float smallest = 1.0f;
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
                smallest = std::min(smallest, gain);
                for (unsigned int c = 0; c < channels; c++) {
                    float& sample = buffer[i + c * bufferSize];
                    sample += (sample * gain - sample) * wet;
                }
            }
            publishReduction(smallest, samples);
        }

    public:
        explicit LimiterFilterInstance(LimiterFilter* parent): parent(parent) {
            initParams(LimiterFilter::ATTRIBUTE_COUNT);
            mParam[LimiterFilter::THRESHOLD] = parent->threshold;
            mParam[LimiterFilter::RELEASE] = parent->release;
            mParam[LimiterFilter::LOOKAHEAD] = parent->lookahead;
            mParam[LimiterFilter::TRUE_PEAK] = parent->truePeak ? 1.0f : 0.0f;
            mParam[LimiterFilter::BYPASS] = parent->bypass ? 1.0f : 0.0f;
            // made on the main thread, which is where a limiter with a delay should allocate
            if (parent->lookahead > 0.0f || parent->truePeak) {
                allocate((unsigned int)std::lround(parent->sampleRate), 2);
            }
        }

        void filter(float* buffer, unsigned int samples, unsigned int bufferSize, unsigned int channels, float samplerate, SoLoud::time time) override {
            updateParams(time);
            const float lookahead = std::clamp(mParam[LimiterFilter::LOOKAHEAD], 0.0f, (float)LIMITER_MAX_LOOKAHEAD_MS);
            const bool truePeak = mParam[LimiterFilter::TRUE_PEAK] >= 0.5f;
            const bool withDelay = lookahead > 0.0f || truePeak;
            if (!withDelay && !running) {
                filterWithoutDelay(buffer, samples, bufferSize, channels, samplerate);
                return;
            }
            if (samples == 0) return;

            const unsigned int frameRate = (unsigned int)std::lround(samplerate);
            if (frameRate != rate || channels > channelCount) {
                allocate(frameRate, channels);
            }
            const bool limiting = withDelay && mParam[LimiterFilter::BYPASS] < 0.5f;
            if (!limiting && !running) {
                // bypassed: the signal as it is, kept for a start
                keepDirect(buffer, 0, samples, bufferSize, std::min(channels, 8u));
                started = true;
                publishReduction(1.0, samples);
                return;
            }

            const double target = std::pow(10.0, (std::min(0.0, (double)mParam[LimiterFilter::THRESHOLD]) - LIMITER_MARGIN_DB) / 20.0);
            const double releaseStep = 1.0 - std::exp(-1.0 / (std::max(0.001, (double)mParam[LimiterFilter::RELEASE]) * rate));
            const double wet = mParam[LimiterFilter::WET];

            float input[8];
            const unsigned int active = std::min(channels, 8u);
            if (limiting && (!running || lookahead != runningLookahead || truePeak != runningTruePeak)) {
                restart(lookahead, truePeak);
                if (!started) {
                    // nothing was heard before it: it starts limited
                    fadePosition = fadeFrames + 1;
                } else {
                    // the last frames heard go through the limiter again, so that the delayed signal
                    // starts where the direct one was
                    const unsigned int replay = std::min(recentCount, delayFrames);
                    for (unsigned int k = 0; k < replay; k++) {
                        const unsigned int at = (recentPosition + capacity - replay + k) % capacity;
                        for (unsigned int c = 0; c < active; c++) {
                            input[c] = recent[(size_t)c * capacity + at];
                            delay[(size_t)c * capacity + delayPosition] = input[c];
                        }
                        gainFor(input, active, truePeak, target, releaseStep);
                        delayPosition = (delayPosition + 1) % std::max(delayFrames, 1u);
                    }
                    fadePosition = 0;
                }
            }
            started = true;

            double smallest = 1.0;
            for (unsigned int i = 0; i < samples; i++) {
                if (limiting && fadePosition <= fadeFrames) {
                    fadePosition++;
                } else if (!limiting) {
                    if (fadePosition == 0) {
                        // the fade out ended: the rest of the block is direct
                        running = false;
                        keepDirect(buffer, i, samples, bufferSize, active);
                        break;
                    }
                    fadePosition--;
                }

                for (unsigned int c = 0; c < active; c++) {
                    // nothing downstream of here keeps a state a NaN could poison: zeroed, it is gone
                    const float x = buffer[i + c * bufferSize];
                    input[c] = std::isfinite(x) ? x : 0.0f;
                }
                remember(input, active);
                const double limit = gainFor(input, active, runningTruePeak, target, releaseStep);
                const double applied = 1.0 + (limit - 1.0) * wet;
                smallest = std::min(smallest, applied);
                const double share = fadePosition > fadeFrames ? 1.0 : (double)fadePosition / (double)(fadeFrames + 1);
                for (unsigned int c = 0; c < active; c++) {
                    float delayed = input[c];
                    if (delayFrames > 0) {
                        float& stored = delay[(size_t)c * capacity + delayPosition];
                        delayed = stored;
                        stored = input[c];
                    }
                    buffer[i + c * bufferSize] = (float)(share * delayed * applied + (1.0 - share) * input[c]);
                }
                if (delayFrames > 0) {
                    delayPosition = (delayPosition + 1) % delayFrames;
                }
            }
            publishReduction(smallest, samples);
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
            if (MeterFilter::captureHook) MeterFilter::captureHook(*parent, buffer, samples, bufferSize, channels);
            if (LoudnessTapRing* tap = parent->tap.load(std::memory_order_acquire)) {
                tap->push(buffer, samples, bufferSize, channels);
            }
        }
    };

}

float LimiterFilter::getGainReductionDb() const {
    return gainReductionDb.load(std::memory_order_relaxed);
}

unsigned int LimiterFilter::lookaheadFrames(float lookaheadMs, float sampleRate) {
    return (unsigned int)std::lround((double)std::max(lookaheadMs, 0.0f) * 0.001 * (double)sampleRate);
}

int LimiterFilter::getParamCount() {
    return ATTRIBUTE_COUNT;
}

const char* LimiterFilter::getParamName(unsigned int index) {
    switch (index) {
        case WET: return "Wet";
        case THRESHOLD: return "Threshold";
        case RELEASE: return "Release";
        case LOOKAHEAD: return "Lookahead";
        case TRUE_PEAK: return "True Peak";
        case BYPASS: return "Bypass";
    }
    return nullptr;
}

unsigned int LimiterFilter::getParamType(unsigned int index) {
    return (index == TRUE_PEAK || index == BYPASS) ? BOOL_PARAM : FLOAT_PARAM;
}

float LimiterFilter::getParamMax(unsigned int index) {
    switch (index) {
        case THRESHOLD: return 0.0f;
        case RELEASE: return 2.0f;
        case LOOKAHEAD: return 5.0f;
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

// ---------------------------------------------------------------- LoudnessTapRing

LoudnessTapRing::LoudnessTapRing(size_t frames): data(frames * CHANNELS, 0.0f), capacity(frames) {
}

void LoudnessTapRing::push(const float* planar, unsigned int samples, unsigned int bufferSize, unsigned int channels) {
    const size_t written = head.load(std::memory_order_relaxed);
    if (capacity - (written - tail.load(std::memory_order_acquire)) < samples) return;
    for (unsigned int c = 0; c < CHANNELS; c++) {
        // a mono bus feeds both channels: a stereo pair of the same signal, as it is heard
        const float* source = planar + (size_t)std::min(c, channels - 1) * bufferSize;
        size_t at = (written % capacity) * CHANNELS + c;
        const size_t end = capacity * CHANNELS;
        for (unsigned int i = 0; i < samples; i++) {
            data[at] = source[i];
            at += CHANNELS;
            if (at >= end) at -= end;
        }
    }
    head.store(written + samples, std::memory_order_release);
}

size_t LoudnessTapRing::pop(float* interleaved, size_t maxFrames) {
    const size_t read = tail.load(std::memory_order_relaxed);
    const size_t frames = std::min(maxFrames, head.load(std::memory_order_acquire) - read);
    size_t at = (read % capacity) * CHANNELS;
    const size_t end = capacity * CHANNELS;
    for (size_t i = 0; i < frames * CHANNELS; i++) {
        interleaved[i] = data[at];
        if (++at >= end) at = 0;
    }
    tail.store(read + frames, std::memory_order_release);
    return frames;
}

void LoudnessTapRing::clear() {
    tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
}

// ---------------------------------------------------------------- MeterFilter

MeterFilter::CaptureHook MeterFilter::captureHook = nullptr;

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
