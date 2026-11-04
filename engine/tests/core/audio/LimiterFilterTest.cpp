// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The limiter effect on its own, without a mixer (S38 R4, R5): at its defaults it is the code it was,
// bit for bit; with a lookahead and the true-peak detector it holds its ceiling, and its switches
// make no click.

#include <gtest/gtest.h>

#include "audio/AudioFilters.h"
#include "audio/TruePeak.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr double kLimiterPi = 3.14159265358979323846;

    double limiterDbOf(double amplitude) {
        return 20.0 * std::log10(amplitude);
    }

    double limiterLinear(double db) {
        return std::pow(10.0, db / 20.0);
    }

    // Stereo blocks through an instance, as a bus runs it: planar, in pieces of `block` frames.
    void runLimiter(SoLoud::FilterInstance& instance, std::vector<float>& interleaved, float rate, unsigned int block = 512) {
        const size_t frames = interleaved.size() / 2;
        std::vector<float> planar(block * 2);
        double time = 0.0;
        for (size_t position = 0; position < frames; position += block) {
            const unsigned int count = (unsigned int)std::min<size_t>(block, frames - position);
            for (unsigned int i = 0; i < count; i++) {
                planar[i] = interleaved[(position + i) * 2];
                planar[block + i] = interleaved[(position + i) * 2 + 1];
            }
            instance.filter(planar.data(), count, block, 2, rate, time);
            for (unsigned int i = 0; i < count; i++) {
                interleaved[(position + i) * 2] = planar[i];
                interleaved[(position + i) * 2 + 1] = planar[block + i];
            }
            time += (double)count / rate;
        }
    }

    std::unique_ptr<SoLoud::FilterInstance> newLimiter(LimiterFilter& filter, float threshold, float lookahead, bool truePeak, float rate) {
        filter.threshold = threshold;
        filter.lookahead = lookahead;
        filter.truePeak = truePeak;
        filter.sampleRate = rate;
        return std::unique_ptr<SoLoud::FilterInstance>(filter.createInstance());
    }

    std::vector<float> stereoOf(const std::vector<float>& mono) {
        std::vector<float> interleaved(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); i++) {
            interleaved[i * 2] = mono[i];
            interleaved[i * 2 + 1] = mono[i];
        }
        return interleaved;
    }

    // a sine of this share of the rate, in dB
    std::vector<float> limiterSine(double share, double phaseDegrees, double db, size_t frames) {
        const double amplitude = limiterLinear(db);
        std::vector<float> out(frames);
        for (size_t i = 0; i < frames; i++) {
            out[i] = (float)(amplitude * std::sin(2.0 * kLimiterPi * share * (double)i + phaseDegrees * kLimiterPi / 180.0));
        }
        return out;
    }

    std::vector<float> limiterNoisePlusTone(size_t frames, double amplitude, unsigned int seed) {
        std::mt19937 generator(seed);
        std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
        std::vector<float> interleaved(frames * 2);
        for (size_t i = 0; i < frames; i++) {
            const double tone = 0.5 * std::sin(2.0 * kLimiterPi * 440.0 * (double)i / 48000.0);
            interleaved[i * 2] = (float)(amplitude * (tone + 0.5 * noise(generator)));
            interleaved[i * 2 + 1] = (float)(amplitude * (tone + 0.5 * noise(generator)));
        }
        return interleaved;
    }

    double outputTruePeakDb(const std::vector<float>& interleaved) {
        TruePeak detector(2);
        const float peak = detector.process(interleaved.data(), interleaved.size() / 2);
        return peak > 0.0f ? limiterDbOf(peak) : -200.0;
    }

    // The limiter before S38, as it was in AudioEffects.cpp: what the defaults must stay.
    class BaseLimiter {
        float gain = 1.0f;
    public:
        void filter(float* buffer, unsigned int samples, unsigned int bufferSize, unsigned int channels, float samplerate,
                    float thresholdDb, float releaseSeconds, float wet) {
            const float threshold = std::pow(10.0f, std::min(0.0f, thresholdDb) / 20.0f);
            const float release = std::max(0.001f, releaseSeconds);
            const float recover = 1.0f - std::exp(-1.0f / (release * samplerate));
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

}

TEST(LimiterFilter, LimiterDefaultsBitIdentical) {
    const float rate = 48000.0f;
    std::vector<float> input = limiterNoisePlusTone(480000, 3.0, 11);

    std::vector<float> expected = input;
    {
        BaseLimiter base;
        const unsigned int block = 512;
        std::vector<float> planar(block * 2);
        const size_t frames = expected.size() / 2;
        for (size_t position = 0; position < frames; position += block) {
            const unsigned int count = (unsigned int)std::min<size_t>(block, frames - position);
            for (unsigned int i = 0; i < count; i++) {
                planar[i] = expected[(position + i) * 2];
                planar[block + i] = expected[(position + i) * 2 + 1];
            }
            base.filter(planar.data(), count, block, 2, rate, -1.0f, 0.1f, 1.0f);
            for (unsigned int i = 0; i < count; i++) {
                expected[(position + i) * 2] = planar[i];
                expected[(position + i) * 2 + 1] = planar[block + i];
            }
        }
    }

    LimiterFilter filter;
    std::unique_ptr<SoLoud::FilterInstance> instance(filter.createInstance());
    runLimiter(*instance, input, rate);
    ASSERT_EQ(input.size(), expected.size());
    EXPECT_EQ(std::memcmp(input.data(), expected.data(), input.size() * sizeof(float)), 0);
    EXPECT_GT(filter.getGainReductionDb(), 0.0f) << "the signal was over the threshold";
}

TEST(LimiterFilter, LimiterTruePeakGuarantee) {
    for (const float rate : {44100.0f, 48000.0f}) {
        for (const double over : {6.0, 12.0, 24.0}) {
            const double level = -1.0 + over;
            const size_t frames = (size_t)rate;

            std::vector<std::vector<float>> signals;
            // reaches its true peak only between its samples
            signals.push_back(stereoOf(limiterSine(0.25, 45.0, level, frames)));
            // the same with a gap, so that the limiter has released when it comes back
            std::vector<float> gated = limiterSine(0.1973, 11.0, level, frames);
            std::fill(gated.begin() + (long)(frames / 3), gated.begin() + (long)(frames * 2 / 3), 0.0f);
            signals.push_back(stereoOf(gated));
            // transients: single samples and pairs out of silence
            std::vector<float> transients(frames, 0.0f);
            const float amplitude = (float)limiterLinear(level);
            for (size_t i = 1000; i + 2 < frames; i += 4000) {
                transients[i] = amplitude;
                if ((i / 4000) % 2 == 1) transients[i + 1] = amplitude;
            }
            signals.push_back(stereoOf(transients));
            signals.push_back(limiterNoisePlusTone(frames, limiterLinear(level) * 1.5, 23));

            for (size_t s = 0; s < signals.size(); s++) {
                LimiterFilter filter;
                auto instance = newLimiter(filter, -1.0f, 1.5f, true, rate);
                runLimiter(*instance, signals[s], rate);
                EXPECT_LE(outputTruePeakDb(signals[s]), -0.9) << "rate " << rate << " over " << over << " signal " << s;
            }
        }
    }
}

// the same guarantee in small and large blocks: nothing depends on where a block ends
TEST(LimiterFilter, TheBlockSizeDoesNotChangeTheOutput) {
    std::vector<float> a = limiterNoisePlusTone(48000, 6.0, 5);
    std::vector<float> b = a;
    LimiterFilter first;
    LimiterFilter second;
    auto one = newLimiter(first, -1.0f, 1.5f, true, 48000.0f);
    auto other = newLimiter(second, -1.0f, 1.5f, true, 48000.0f);
    runLimiter(*one, a, 48000.0f, 64);
    runLimiter(*other, b, 48000.0f, 2048);
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)), 0);
}

TEST(LimiterFilter, TheDelayIsTheLookahead) {
    const std::pair<float, size_t> cases[] = {{48000.0f, 72}, {44100.0f, 66}};
    for (const auto& [rate, expected] : cases) {
        std::vector<float> interleaved(4096, 0.0f);
        interleaved[200] = 0.25f;
        interleaved[201] = 0.25f;
        LimiterFilter filter;
        auto instance = newLimiter(filter, -1.0f, 1.5f, true, rate);
        runLimiter(*instance, interleaved, rate);
        size_t at = 0;
        for (size_t i = 0; i < interleaved.size() / 2; i++) {
            if (interleaved[i * 2] != 0.0f) {
                at = i;
                break;
            }
        }
        EXPECT_EQ(at, 100 + expected) << rate;
        EXPECT_EQ(interleaved[(100 + expected) * 2], 0.25f) << "under the threshold it is not touched";
    }
    EXPECT_EQ(LimiterFilter::lookaheadFrames(1.5f, 48000.0f), 72u);
    EXPECT_EQ(LimiterFilter::lookaheadFrames(0.0f, 48000.0f), 0u);
}

// the gain has arrived when the peak leaves the delay: the sample peak holds without the true-peak
// detector too
TEST(LimiterFilter, ALookaheadAloneHoldsTheSamplePeak) {
    std::vector<float> interleaved = limiterNoisePlusTone(48000, 8.0, 3);
    LimiterFilter filter;
    auto instance = newLimiter(filter, -3.0f, 1.0f, false, 48000.0f);
    runLimiter(*instance, interleaved, 48000.0f);
    float peak = 0.0f;
    for (float x : interleaved) peak = std::max(peak, std::fabs(x));
    EXPECT_LE(limiterDbOf(peak), -3.0 + 0.01);
}

TEST(LimiterFilter, NonFiniteInputRecoversWithinABlock) {
    std::vector<float> interleaved = stereoOf(limiterSine(0.01, 0.0, -6.0, 9600));
    const std::vector<float> clean = interleaved;
    interleaved[2000 * 2] = std::numeric_limits<float>::quiet_NaN();
    interleaved[2000 * 2 + 1] = std::numeric_limits<float>::infinity();
    interleaved[2001 * 2] = -std::numeric_limits<float>::infinity();

    LimiterFilter filter;
    auto instance = newLimiter(filter, -1.0f, 1.5f, true, 48000.0f);
    runLimiter(*instance, interleaved, 48000.0f);
    for (float x : interleaved) ASSERT_TRUE(std::isfinite(x));
    // a block after it the signal is the one that went in, delayed
    for (size_t i = 3000; i < 9600; i++) {
        ASSERT_NEAR(interleaved[i * 2], clean[(i - 72) * 2], 1e-3f) << i;
    }
}

// Toggling bypass crossfades the delayed signal and the direct one. Where they are in phase (a whole
// number of periods apart), the fade adds nothing: no step over the signal's own.
TEST(LimiterFilter, TogglingBypassMakesNoStepLargerThanTheSignalsOwn) {
    // 24 frames a period, the 72 frames of delay are 3 periods
    const std::vector<float> mono = limiterSine(1.0 / 24.0, 0.0, -6.0, 48000);
    double own = 0.0;
    for (size_t i = 1; i < mono.size(); i++) own = std::max(own, (double)std::fabs(mono[i] - mono[i - 1]));

    const auto largestStep = [](const std::vector<float>& interleaved) {
        double step = 0.0;
        for (size_t i = 1; i < interleaved.size() / 2; i++) {
            step = std::max(step, (double)std::fabs(interleaved[i * 2] - interleaved[(i - 1) * 2]));
        }
        return step;
    };
    const auto toggled = [&](const std::vector<float>& source) {
        LimiterFilter filter;
        auto instance = newLimiter(filter, -1.0f, 1.5f, true, 48000.0f);
        std::vector<float> first(source.begin(), source.begin() + (long)(source.size() / 4));
        std::vector<float> second(source.begin() + (long)(source.size() / 4), source.begin() + (long)(source.size() / 2));
        std::vector<float> third(source.begin() + (long)(source.size() / 2), source.end());
        runLimiter(*instance, first, 48000.0f);
        instance->setFilterParameter(LimiterFilter::BYPASS, 1.0f);
        runLimiter(*instance, second, 48000.0f);
        instance->setFilterParameter(LimiterFilter::BYPASS, 0.0f);
        runLimiter(*instance, third, 48000.0f);
        std::vector<float> out = first;
        out.insert(out.end(), second.begin(), second.end());
        out.insert(out.end(), third.begin(), third.end());
        return out;
    };

    // the first 72 frames of delay leave silence, then the signal starts: only what follows counts
    const std::vector<float> inPhase = toggled(stereoOf(mono));
    const std::vector<float> settled(inPhase.begin() + 200, inPhase.end());
    EXPECT_LE(largestStep(settled), own * 1.0001);

    // Out of phase the two differ, and the fade's own slope comes on top: a signal's worth over 5 ms,
    // a fraction of a step, where a click would be the whole difference at once.
    const std::vector<float> other = limiterSine(0.01, 0.0, -6.0, 48000);
    double otherOwn = 0.0;
    for (size_t i = 1; i < other.size(); i++) otherOwn = std::max(otherOwn, (double)std::fabs(other[i] - other[i - 1]));
    const std::vector<float> outOfPhase = toggled(stereoOf(other));
    const std::vector<float> settledOther(outOfPhase.begin() + 200, outOfPhase.end());
    EXPECT_LE(largestStep(settledOther), otherOwn * 1.5);
}

TEST(LimiterFilter, BypassedItPassesTheSignalUnchanged) {
    std::vector<float> interleaved = limiterNoisePlusTone(4800, 3.0, 9);
    const std::vector<float> input = interleaved;
    LimiterFilter filter;
    filter.bypass = true;
    auto instance = newLimiter(filter, -1.0f, 1.5f, true, 48000.0f);
    runLimiter(*instance, interleaved, 48000.0f);
    EXPECT_EQ(std::memcmp(interleaved.data(), input.data(), input.size() * sizeof(float)), 0);
    EXPECT_EQ(filter.getGainReductionDb(), 0.0f);
}

TEST(LimiterFilter, GainReductionIsTheLargestOfTheLast100Milliseconds) {
    LimiterFilter filter;
    auto instance = newLimiter(filter, -1.0f, 1.5f, true, 48000.0f);
    // a steady tone 6 dB over a -1 dB ceiling: 6 dB and the margin
    std::vector<float> loud = stereoOf(limiterSine(0.01, 0.0, 5.0, 24000));
    runLimiter(*instance, loud, 48000.0f);
    EXPECT_NEAR(filter.getGainReductionDb(), 6.05, 0.2);

    // quiet: the release takes it back, and the readout forgets it after 100 ms
    std::vector<float> quiet(48000 * 2, 0.0f);
    runLimiter(*instance, quiet, 48000.0f);
    EXPECT_LT(filter.getGainReductionDb(), 0.1f);
}

TEST(LimiterFilter, ItsParametersCanChangeWhileItRuns) {
    std::vector<float> interleaved = limiterNoisePlusTone(9600, 3.0, 13);
    LimiterFilter filter;
    std::unique_ptr<SoLoud::FilterInstance> instance(filter.createInstance());
    std::vector<float> first(interleaved.begin(), interleaved.begin() + 9600);
    std::vector<float> second(interleaved.begin() + 9600, interleaved.end());
    runLimiter(*instance, first, 48000.0f);
    // from no delay to a delay, and the true-peak detector
    instance->setFilterParameter(LimiterFilter::LOOKAHEAD, 1.5f);
    instance->setFilterParameter(LimiterFilter::TRUE_PEAK, 1.0f);
    runLimiter(*instance, second, 48000.0f);
    // the ceiling holds from the end of the 5 ms fade
    std::vector<float> settled(second.begin() + 1000, second.end());
    TruePeak detector(2);
    detector.process(settled.data(), TruePeak::TAPS);
    const float peak = detector.process(settled.data() + TruePeak::TAPS * 2, settled.size() / 2 - TruePeak::TAPS);
    EXPECT_LE(limiterDbOf(peak), -0.9);
}

TEST(LoudnessTapRing, PassesFramesInOrderAndDropsWhatDoesNotFit) {
    LoudnessTapRing ring(8);
    const float planar[8] = {1, 2, 3, 4, 10, 20, 30, 40};
    ring.push(planar, 4, 4, 2);
    float out[16] = {};
    EXPECT_EQ(ring.pop(out, 2), 2u);
    EXPECT_EQ(out[0], 1.0f);
    EXPECT_EQ(out[1], 10.0f);
    EXPECT_EQ(out[2], 2.0f);
    EXPECT_EQ(out[3], 20.0f);

    // around the end of the ring
    ring.push(planar, 4, 4, 2);
    ring.push(planar, 2, 4, 2);
    // full: 8 frames, this block does not fit and goes
    ring.push(planar, 4, 4, 2);
    EXPECT_EQ(ring.pop(out, 16), 8u);
    EXPECT_EQ(out[0], 3.0f);
    EXPECT_EQ(out[1], 30.0f);
    EXPECT_EQ(out[4], 1.0f);
    EXPECT_EQ(out[5], 10.0f);
    EXPECT_EQ(out[14], 2.0f);
    EXPECT_EQ(out[15], 20.0f);
    EXPECT_EQ(ring.pop(out, 16), 0u);

    // a mono bus feeds both channels
    const float mono[2] = {5, 6};
    ring.push(mono, 2, 2, 1);
    EXPECT_EQ(ring.pop(out, 16), 2u);
    EXPECT_EQ(out[0], 5.0f);
    EXPECT_EQ(out[1], 5.0f);
    EXPECT_EQ(out[2], 6.0f);
    EXPECT_EQ(out[3], 6.0f);
}
