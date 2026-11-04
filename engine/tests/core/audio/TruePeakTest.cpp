// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The BS.1770-4 Annex 2 true-peak detector on its own (S38 R2): signals whose true peak is known
// from their formula, and the streaming contract the limiter relies on.

#include <gtest/gtest.h>

#include "audio/TruePeak.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr double kTruePeakPi = 3.14159265358979323846;

    // a sine of this share of the sample rate, phase in degrees, amplitude in dBFS
    std::vector<float> rateSine(double shareOfRate, double phaseDegrees, double dbfs, size_t frames) {
        const double amplitude = std::pow(10.0, dbfs / 20.0);
        std::vector<float> samples(frames);
        for (size_t i = 0; i < frames; i++) {
            samples[i] = (float)(amplitude * std::sin(2.0 * kTruePeakPi * shareOfRate * (double)i + phaseDegrees * kTruePeakPi / 180.0));
        }
        return samples;
    }

    double dbOf(double amplitude) {
        return 20.0 * std::log10(amplitude);
    }

    // The largest value once the filter is full: a signal that starts abruptly really does overshoot
    // between its first samples, and that onset is not what these tests are about.
    float settledPeak(const std::vector<float>& samples) {
        TruePeak detector(1);
        detector.process(samples.data(), TruePeak::TAPS);
        return detector.process(samples.data() + TruePeak::TAPS, samples.size() - TruePeak::TAPS);
    }

}

// EBU Tech 3341's true-peak signals: sines that reach a known level between their samples.
// Tech 3341 allows +0.2/-0.4 dB.
TEST(TruePeak, ReadsTheLevelBetweenTheSamples) {
    struct Case {
        double share, phase, dbfs;
    };
    const Case cases[] = {{0.25, 0.0, -6.0}, {0.25, 45.0, -6.0}, {1.0 / 6.0, 60.0, -6.0}, {0.125, 67.5, -6.0}, {0.25, 45.0, 3.0}};
    for (const Case& c : cases) {
        const std::vector<float> samples = rateSine(c.share, c.phase, c.dbfs, 4800);
        const double value = dbOf(settledPeak(samples));
        EXPECT_LE(value, c.dbfs + 0.2) << "share " << c.share << " phase " << c.phase;
        EXPECT_GE(value, c.dbfs - 0.4) << "share " << c.share << " phase " << c.phase;
    }
}

TEST(TruePeak, QuarterRateAt45DegreesIsThreeDecibelsOverItsSamples) {
    const std::vector<float> samples = rateSine(0.25, 45.0, -6.0, 4800);
    double samplePeak = 0.0;
    for (float x : samples) {
        samplePeak = std::max(samplePeak, (double)std::fabs(x));
    }
    const double truePeak = settledPeak(samples);
    EXPECT_NEAR(dbOf(truePeak) - dbOf(samplePeak), 3.01, 0.2);
}

// the filter's phases give 0.972 of an isolated sample: the sample itself has to count
TEST(TruePeak, CountsTheOriginalSamples) {
    TruePeak detector(1);
    const float impulse[] = {1.0f};
    EXPECT_EQ(detector.process(impulse, 1), 1.0f);

    TruePeak negative(1);
    const float down[] = {-0.5f};
    EXPECT_EQ(negative.process(down, 1), 0.5f);
}

// an impulse's response peaks on the points either side of it: those between the inputs DELAY and
// DELAY - 1 steps back hold it at both ends of that span
TEST(TruePeak, InterpolatedPointsComeDelayStepsLater) {
    TruePeak detector(1);
    EXPECT_EQ(detector.processSample(0, 1.0f), 1.0f);
    for (unsigned int step = 1; step < TruePeak::TAPS; step++) {
        const float value = detector.processSample(0, 0.0f);
        if (step == TruePeak::DELAY - 1 || step == TruePeak::DELAY) {
            EXPECT_FLOAT_EQ(value, 0.97216796875f) << "step " << step;
        } else {
            EXPECT_LT(value, 0.8f) << "step " << step;
        }
    }
}

TEST(TruePeak, ConstantInputPassesAtUnityGain) {
    // the Annex 2 filter's largest phase sums to 1.0016
    const std::vector<float> samples(256, 0.5f);
    EXPECT_NEAR(settledPeak(samples), 0.5f * 1.0015869f, 1e-5f);
}

TEST(TruePeak, BlockSizesDoNotChangeTheResult) {
    const std::vector<float> samples = rateSine(0.1973, 11.0, -3.0, 9600);

    TruePeak whole(1);
    const float expected = whole.process(samples.data(), samples.size());

    TruePeak pieces(1);
    float largest = 0.0f;
    size_t position = 0;
    size_t block = 1;
    while (position < samples.size()) {
        const size_t count = std::min(block, samples.size() - position);
        largest = std::max(largest, pieces.process(samples.data() + position, count));
        position += count;
        block = block % 97 + 13;
    }
    EXPECT_EQ(largest, expected);
}

// the step into a signal overshoots between its samples, and that counts: it is a true peak
TEST(TruePeak, AnAbruptStartOvershoots) {
    const std::vector<float> samples(256, 0.5f);
    TruePeak detector(1);
    EXPECT_GT(detector.process(samples.data(), samples.size()), 0.55f);
}

TEST(TruePeak, ChannelsHaveTheirOwnFilters) {
    // left: the inter-sample signal, right: silence; sharing a history would change the left's value
    const std::vector<float> left = rateSine(0.25, 45.0, -6.0, 4800);
    std::vector<float> interleaved(left.size() * 2);
    for (size_t i = 0; i < left.size(); i++) {
        interleaved[i * 2] = left[i];
        interleaved[i * 2 + 1] = 0.0f;
    }
    TruePeak stereo(2);
    TruePeak mono(1);
    EXPECT_EQ(stereo.getChannels(), 2u);
    EXPECT_EQ(stereo.process(interleaved.data(), left.size()), mono.process(left.data(), left.size()));
}

TEST(TruePeak, ResetForgetsTheHistory) {
    TruePeak detector(1);
    const float loud[] = {1.0f};
    detector.process(loud, 1);
    detector.reset();
    float largest = 0.0f;
    for (unsigned int step = 0; step < TruePeak::TAPS; step++) {
        largest = std::max(largest, detector.processSample(0, 0.0f));
    }
    EXPECT_EQ(largest, 0.0f);
}

TEST(TruePeak, NonFiniteSamplesReadAsSilence) {
    TruePeak detector(1);
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity()};
    EXPECT_EQ(detector.process(bad, 3), 0.0f);
    float largest = 0.0f;
    for (unsigned int step = 0; step < TruePeak::TAPS; step++) {
        largest = std::max(largest, detector.processSample(0, 0.0f));
    }
    EXPECT_EQ(largest, 0.0f);
}

TEST(TruePeak, EmptyInput) {
    TruePeak detector(2);
    EXPECT_EQ(detector.process(nullptr, 100), 0.0f);
    const float none[] = {1.0f, 1.0f};
    EXPECT_EQ(detector.process(none, 0), 0.0f);
    // zero channels would make the interleaved stride 0
    EXPECT_EQ(TruePeak(0).getChannels(), 1u);
}
