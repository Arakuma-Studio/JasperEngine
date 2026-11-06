// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// AudioMeter (S38 R1) against what the standards state: the signals of EBU Tech 3341 (loudness) and
// EBU Tech 3342 (loudness range) with their expected values, the BS.1770-4 K-weighting table and
// signals whose levels follow from their formula. tools/mcp/tests/test_audio_meter.py checks the
// same meter against pyloudnorm.

#include <gtest/gtest.h>

#include "audio/AudioMeter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr double kMeterPi = 3.14159265358979323846;
    constexpr unsigned int kMeterRate = 48000;
    // EBU Tech 3341's tolerance for its loudness values, and Tech 3342's for its LRA values
    constexpr double kLufsTolerance = 0.1;
    constexpr double kLraTolerance = 1.0;

    struct Segment {
        double seconds;
        double dbfs;
    };

    // a 1 kHz sine that changes level between segments, in one phase throughout
    std::vector<float> toneSegments(const std::vector<Segment>& segments, unsigned int rate = kMeterRate, double frequency = 1000.0) {
        std::vector<float> samples;
        size_t i = 0;
        for (const Segment& segment : segments) {
            const double amplitude = std::pow(10.0, segment.dbfs / 20.0);
            const size_t end = i + (size_t)std::llround(segment.seconds * rate);
            for (; i < end; i++) {
                samples.push_back((float)(amplitude * std::sin(2.0 * kMeterPi * frequency * (double)i / rate)));
            }
        }
        return samples;
    }

    std::vector<float> toStereo(const std::vector<float>& mono) {
        std::vector<float> interleaved(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); i++) {
            interleaved[i * 2] = mono[i];
            interleaved[i * 2 + 1] = mono[i];
        }
        return interleaved;
    }

    LoudnessResult measureStereo(const std::vector<Segment>& segments, unsigned int rate = kMeterRate) {
        const std::vector<float> interleaved = toStereo(toneSegments(segments, rate));
        return AudioMeter::measure(interleaved.data(), interleaved.size() / 2, rate, 2, false);
    }

    std::vector<float> meterNoise(size_t samples, double amplitude, uint32_t seed) {
        std::mt19937 generator(seed);
        std::uniform_real_distribution<float> distribution((float)-amplitude, (float)amplitude);
        std::vector<float> values(samples);
        for (float& value : values) {
            value = distribution(generator);
        }
        return values;
    }

    // a stereo signal whose level moves, the sort of thing the two modes must agree on
    std::vector<float> movingSignal(unsigned int rate, double seconds) {
        const size_t frames = (size_t)(seconds * rate);
        const std::vector<float> white = meterNoise(frames * 2, 0.5, 7);
        std::vector<float> samples(frames * 2);
        for (size_t i = 0; i < frames; i++) {
            const double t = (double)i / rate;
            const double swell = 0.2 + 0.8 * std::fabs(std::sin(2.0 * kMeterPi * 0.3 * t));
            const double tone = 0.3 * std::sin(2.0 * kMeterPi * 440.0 * t);
            samples[i * 2] = (float)(swell * (tone + white[i * 2]));
            samples[i * 2 + 1] = (float)(swell * (tone - white[i * 2 + 1]));
        }
        return samples;
    }

}

// -- EBU Tech 3341 ----------------------------------------------------------------------------------

TEST(AudioMeter, Ebu3341Case1Stereo23) {
    const LoudnessResult r = measureStereo({{20.0, -23.0}});
    ASSERT_TRUE(r.integrated && r.momentaryMax && r.shortTermMax);
    EXPECT_NEAR(*r.integrated, -23.0, kLufsTolerance);
    EXPECT_NEAR(*r.momentaryMax, -23.0, kLufsTolerance);
    EXPECT_NEAR(*r.shortTermMax, -23.0, kLufsTolerance);
}

TEST(AudioMeter, Ebu3341Case2Stereo33) {
    const LoudnessResult r = measureStereo({{20.0, -33.0}});
    ASSERT_TRUE(r.integrated && r.momentaryMax && r.shortTermMax);
    EXPECT_NEAR(*r.integrated, -33.0, kLufsTolerance);
    EXPECT_NEAR(*r.momentaryMax, -33.0, kLufsTolerance);
    EXPECT_NEAR(*r.shortTermMax, -33.0, kLufsTolerance);
}

// the relative gate: -36 is under -23's, so it does not count
TEST(AudioMeter, Ebu3341Case3RelativeGate) {
    const LoudnessResult r = measureStereo({{10.0, -36.0}, {60.0, -23.0}, {10.0, -36.0}});
    ASSERT_TRUE(r.integrated);
    EXPECT_NEAR(*r.integrated, -23.0, kLufsTolerance);
}

// the absolute gate too: -72 would pull the relative gate down
TEST(AudioMeter, Ebu3341Case4AbsoluteGate) {
    const LoudnessResult r = measureStereo({{10.0, -72.0}, {10.0, -36.0}, {60.0, -23.0}, {10.0, -36.0}, {10.0, -72.0}});
    ASSERT_TRUE(r.integrated);
    EXPECT_NEAR(*r.integrated, -23.0, kLufsTolerance);
}

// both levels pass the gate: their mean counts
TEST(AudioMeter, Ebu3341Case5LevelsAverage) {
    const LoudnessResult r = measureStereo({{20.0, -26.0}, {20.1, -20.0}, {20.0, -26.0}});
    ASSERT_TRUE(r.integrated);
    EXPECT_NEAR(*r.integrated, -23.0, kLufsTolerance);
}

// 5.0: L, R -28 dBFS, C -24, Ls, Rs -30
TEST(AudioMeter, Ebu3341Case6FiveChannels) {
    const std::vector<float> front = toneSegments({{20.0, -28.0}});
    const std::vector<float> centre = toneSegments({{20.0, -24.0}});
    const std::vector<float> surround = toneSegments({{20.0, -30.0}});
    const float* planar[5] = {front.data(), front.data(), centre.data(), surround.data(), surround.data()};
    const LoudnessResult r = AudioMeter::measurePlanar(planar, front.size(), kMeterRate, 5, false);
    ASSERT_TRUE(r.integrated);
    EXPECT_NEAR(*r.integrated, -23.0, kLufsTolerance);
}

// 3 s of a pattern whose mean is -23: every full 3 s window reads -23 whatever it starts on
TEST(AudioMeter, Ebu3341Case9ShortTermHasNoRipple) {
    std::vector<Segment> segments;
    for (int i = 0; i < 5; i++) {
        segments.push_back({1.34, -20.0});
        segments.push_back({1.66, -30.0});
    }
    const std::vector<float> interleaved = toStereo(toneSegments(segments));
    AudioMeter meter(kMeterRate, 2, AudioMeterOptions{AudioMeterMode::Stream, false});
    meter.process(interleaved.data(), interleaved.size() / 2);
    const std::vector<float> curve = meter.shortTermCurve();
    ASSERT_GE(curve.size(), 130u);
    for (size_t i = 0; i < curve.size(); i++) {
        if (i < 29) {
            EXPECT_TRUE(std::isnan(curve[i])) << "point " << i << " has no 3 s of signal";
        } else {
            EXPECT_NEAR(curve[i], -23.0, kLufsTolerance) << "point " << i;
        }
    }
}

// the same over 400 ms for the momentary loudness
TEST(AudioMeter, Ebu3341Case12MomentaryHasNoRipple) {
    std::vector<Segment> segments;
    for (int i = 0; i < 20; i++) {
        segments.push_back({0.18, -20.0});
        segments.push_back({0.22, -30.0});
    }
    const std::vector<float> interleaved = toStereo(toneSegments(segments));
    AudioMeter meter(kMeterRate, 2, AudioMeterOptions{AudioMeterMode::Stream, false});
    meter.process(interleaved.data(), interleaved.size() / 2);
    const std::vector<float> curve = meter.momentaryCurve();
    ASSERT_GE(curve.size(), 70u);
    for (size_t i = 3; i < curve.size(); i++) {
        EXPECT_NEAR(curve[i], -23.0, kLufsTolerance) << "point " << i;
    }
    const std::optional<double> maximum = meter.result().momentaryMax;
    ASSERT_TRUE(maximum);
    EXPECT_NEAR(*maximum, -23.0, kLufsTolerance);
}

// -- EBU Tech 3342 ----------------------------------------------------------------------------------

TEST(AudioMeter, Ebu3342Case1) {
    const LoudnessResult r = measureStereo({{20.0, -20.0}, {20.0, -30.0}});
    ASSERT_TRUE(r.lra);
    EXPECT_NEAR(*r.lra, 10.0, kLraTolerance);
}

TEST(AudioMeter, Ebu3342Case2) {
    const LoudnessResult r = measureStereo({{20.0, -20.0}, {20.0, -15.0}});
    ASSERT_TRUE(r.lra);
    EXPECT_NEAR(*r.lra, 5.0, kLraTolerance);
}

TEST(AudioMeter, Ebu3342Case3) {
    const LoudnessResult r = measureStereo({{20.0, -40.0}, {20.0, -20.0}});
    ASSERT_TRUE(r.lra);
    EXPECT_NEAR(*r.lra, 20.0, kLraTolerance);
}

TEST(AudioMeter, Ebu3342Case4) {
    const LoudnessResult r = measureStereo({{20.0, -50.0}, {20.0, -35.0}, {20.0, -20.0}, {20.0, -35.0}, {20.0, -50.0}});
    ASSERT_TRUE(r.lra);
    EXPECT_NEAR(*r.lra, 15.0, kLraTolerance);
}

TEST(AudioMeter, RangeIsZeroForASteadySignalAndEmptyBelowThreeSeconds) {
    const LoudnessResult steady = measureStereo({{10.0, -23.0}});
    ASSERT_TRUE(steady.lra);
    EXPECT_NEAR(*steady.lra, 0.0, 0.01);

    const LoudnessResult shorter = measureStereo({{2.9, -23.0}});
    EXPECT_FALSE(shorter.lra);
    EXPECT_FALSE(shorter.shortTermMax);
}

// -- K-weighting ------------------------------------------------------------------------------------

TEST(AudioMeter, KWeightingMatchesTheBs1770TableAt48k) {
    double shelf[5];
    double highPass[5];
    AudioMeter::kWeighting(48000, shelf, highPass);

    const double expectedShelf[5] = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
    const double expectedHighPass[5] = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    for (int i = 0; i < 5; i++) {
        EXPECT_NEAR(shelf[i], expectedShelf[i], 1e-6) << "shelf " << i;
        EXPECT_NEAR(highPass[i], expectedHighPass[i], 1e-6) << "high-pass " << i;
    }
}

// At 1 kHz the K-weighting adds 0.691 dB, which BS.1770's -0.691 takes off again: a -23 dBFS stereo
// tone reads -23 LUFS at any rate. 8 kHz has room for 1 kHz too.
TEST(AudioMeter, RatesAgreeOnA1kHzTone) {
    for (unsigned int rate : {8000u, 22050u, 32000u, 44100u, 48000u, 88200u, 96000u, 192000u}) {
        const LoudnessResult r = measureStereo({{5.0, -23.0}}, rate);
        ASSERT_TRUE(r.integrated) << rate;
        EXPECT_NEAR(*r.integrated, -23.0, 0.2) << rate;
    }
}

TEST(AudioMeter, KWeightingShapesTheSpectrum) {
    const auto lufs = [](double frequency) {
        const std::vector<float> interleaved = toStereo(toneSegments({{2.0, -23.0}}, kMeterRate, frequency));
        const LoudnessResult r = AudioMeter::measure(interleaved.data(), interleaved.size() / 2, kMeterRate, 2, false);
        return r.integrated.value_or(std::numeric_limits<double>::quiet_NaN());
    };
    // the high-pass takes low end out, the shelf lifts the top by about 4 dB
    EXPECT_LT(lufs(30.0), -23.0 - 2.0);
    EXPECT_GT(lufs(8000.0), -23.0 + 3.0);
}

// -- channels ---------------------------------------------------------------------------------------

TEST(AudioMeter, ChannelWeights) {
    EXPECT_EQ(AudioMeter::channelWeight(1, 0), 1.0);
    EXPECT_EQ(AudioMeter::channelWeight(2, 0), 1.0);
    EXPECT_EQ(AudioMeter::channelWeight(2, 1), 1.0);
    const double layout50[5] = {1.0, 1.0, 1.0, 1.41, 1.41};
    for (unsigned int c = 0; c < 5; c++) {
        EXPECT_EQ(AudioMeter::channelWeight(5, c), layout50[c]) << c;
    }
    const double layout51[6] = {1.0, 1.0, 1.0, 0.0, 1.41, 1.41};
    for (unsigned int c = 0; c < 6; c++) {
        EXPECT_EQ(AudioMeter::channelWeight(6, c), layout51[c]) << c;
    }
    const double layout71[8] = {1.0, 1.0, 1.0, 0.0, 1.0, 1.0, 1.41, 1.41};
    for (unsigned int c = 0; c < 8; c++) {
        EXPECT_EQ(AudioMeter::channelWeight(8, c), layout71[c]) << c;
    }
}

TEST(AudioMeter, TheLfeDoesNotCountAndSurroundsWeighMore) {
    const std::vector<float> tone = toneSegments({{2.0, -23.0}});
    const std::vector<float> none(tone.size(), 0.0f);

    const auto oneChannel = [&](unsigned int active) {
        const float* planar[6] = {none.data(), none.data(), none.data(), none.data(), none.data(), none.data()};
        planar[active] = tone.data();
        return AudioMeter::measurePlanar(planar, tone.size(), kMeterRate, 6, false);
    };

    // an LFE alone has no block over the gate
    EXPECT_FALSE(oneChannel(3).integrated);
    // 10 log10(1.41) over a front channel
    const std::optional<double> front = oneChannel(0).integrated;
    const std::optional<double> surround = oneChannel(4).integrated;
    ASSERT_TRUE(front && surround);
    EXPECT_NEAR(*surround - *front, 10.0 * std::log10(1.41), 0.01);
}

TEST(AudioMeter, PlanarEqualsInterleaved) {
    const std::vector<float> interleaved = movingSignal(kMeterRate, 5.0);
    const size_t frames = interleaved.size() / 2;
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    for (size_t i = 0; i < frames; i++) {
        left[i] = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
    }
    const float* planar[2] = {left.data(), right.data()};
    const LoudnessResult a = AudioMeter::measure(interleaved.data(), frames, kMeterRate, 2);
    const LoudnessResult b = AudioMeter::measurePlanar(planar, frames, kMeterRate, 2);
    EXPECT_EQ(*a.integrated, *b.integrated);
    EXPECT_EQ(a.truePeakDb, b.truePeakDb);
    EXPECT_EQ(a.bandsDb, b.bandsDb);
}

TEST(AudioMeter, RefusesWhatItCannotMeasure) {
    const std::pair<unsigned int, unsigned int> refused[] = {{kMeterRate, 0}, {kMeterRate, 9}, {7999, 2}, {192001, 2}};
    for (const auto& [rate, channels] : refused) {
        AudioMeter meter(rate, channels);
        EXPECT_FALSE(meter.isValid()) << rate << " " << channels;
        const float frame[2] = {1.0f, 1.0f};
        meter.process(frame, 1);
        meter.finish();
        EXPECT_EQ(meter.result().frames, 0u);
        EXPECT_FALSE(meter.result().momentaryMax);
    }
    const std::pair<unsigned int, unsigned int> accepted[] = {{8000, 1}, {192000, 8}};
    for (const auto& [rate, channels] : accepted) {
        AudioMeter meter(rate, channels);
        EXPECT_TRUE(meter.isValid()) << rate << " " << channels;
        EXPECT_EQ(meter.getSampleRate(), rate);
        EXPECT_EQ(meter.getChannels(), channels);
    }
}

// -- gates, empty and short signals -----------------------------------------------------------------

TEST(AudioMeter, DigitalSilenceHasNoLoudness) {
    const std::vector<float> silence(kMeterRate * 2 * 4, 0.0f);
    const LoudnessResult r = AudioMeter::measure(silence.data(), silence.size() / 2, kMeterRate, 2);
    EXPECT_FALSE(r.integrated);
    EXPECT_FALSE(r.relativeGate);
    EXPECT_EQ(r.gatedBlocks, 0u);
    EXPECT_EQ(r.samplePeakDb, AudioMeter::SILENCE_DB);
    EXPECT_EQ(r.truePeakDb, AudioMeter::SILENCE_DB);
    EXPECT_EQ(r.rmsDb, AudioMeter::SILENCE_DB);
    ASSERT_TRUE(r.momentaryMax);
    EXPECT_EQ(*r.momentaryMax, AudioMeter::SILENCE_DB);
}

TEST(AudioMeter, ThreeSecondsAreThePointOfTheShortTerm) {
    EXPECT_TRUE(measureStereo({{3.0, -23.0}}).shortTermMax);
    EXPECT_FALSE(measureStereo({{2.99, -23.0}}).shortTermMax);
}

TEST(AudioMeter, ASignalUnder400msHasMomentaryButNoIntegrated) {
    const LoudnessResult r = measureStereo({{0.3, -23.0}});
    EXPECT_FALSE(r.integrated);
    // the file mode's padding: a window holds all 300 ms of the 400
    ASSERT_TRUE(r.momentaryMax);
    EXPECT_NEAR(*r.momentaryMax, -23.0 + 10.0 * std::log10(0.3 / 0.4), 0.2);
    EXPECT_EQ(r.frames, (uint64_t)std::llround(0.3 * kMeterRate));
}

TEST(AudioMeter, NothingProcessed) {
    AudioMeter meter(kMeterRate, 2);
    const LoudnessResult r = meter.result();
    EXPECT_EQ(r.frames, 0u);
    EXPECT_EQ(r.channels, 2u);
    EXPECT_EQ(r.sampleRate, kMeterRate);
    EXPECT_FALSE(r.integrated);
    EXPECT_FALSE(r.momentaryMax);
    EXPECT_TRUE(meter.momentaryCurve().empty());
}

// BS.1770's gates count: blocks are 400 ms every 100 ms inside the signal
TEST(AudioMeter, GatedBlocksCountTheBlocksThatPass) {
    const LoudnessResult r = measureStereo({{10.0, -23.0}});
    EXPECT_EQ(r.gatedBlocks, 97u);
    ASSERT_TRUE(r.relativeGate);
    EXPECT_NEAR(*r.relativeGate, -33.0, kLufsTolerance);

    const LoudnessResult gated = measureStereo({{10.0, -23.0}, {10.0, -50.0}});
    EXPECT_LT(gated.gatedBlocks, 110u);
    EXPECT_GE(gated.gatedBlocks, 97u);
}

// -- peaks and levels -------------------------------------------------------------------------------

TEST(AudioMeter, TruePeakIsThreeDecibelsOverSamplePeakAtQuarterRate45Degrees) {
    const size_t frames = kMeterRate;
    const double amplitude = std::pow(10.0, -3.0 / 20.0);
    std::vector<float> samples(frames);
    for (size_t i = 0; i < frames; i++) {
        samples[i] = (float)(amplitude * std::sin(2.0 * kMeterPi * 0.25 * (double)i + kMeterPi / 4.0));
    }
    const LoudnessResult r = AudioMeter::measure(samples.data(), frames, kMeterRate, 1, false);
    EXPECT_NEAR(r.samplePeakDb, -6.01, 0.01);
    EXPECT_NEAR(r.truePeakDb - r.samplePeakDb, 3.01, 0.2);
}

TEST(AudioMeter, TruePeakIsAtLeastSamplePeak) {
    const float impulse[] = {0.0f, 0.75f, 0.0f};
    const LoudnessResult r = AudioMeter::measure(impulse, 3, kMeterRate, 1);
    EXPECT_NEAR(r.samplePeakDb, 20.0 * std::log10(0.75), 1e-9);
    EXPECT_GE(r.truePeakDb, r.samplePeakDb);
}

// two equal last samples peak between them, which the filter shows DELAY steps later: only the
// trailing samples finish() adds reach it
TEST(AudioMeter, FileModeFlushesTheTruePeakFilter) {
    std::vector<float> samples(24, 0.0f);
    samples.push_back(0.5f);
    samples.push_back(0.5f);
    const LoudnessResult file = AudioMeter::measure(samples.data(), samples.size(), kMeterRate, 1);
    AudioMeter stream(kMeterRate, 1);
    stream.process(samples.data(), samples.size());
    stream.finish();
    const LoudnessResult live = stream.result();
    EXPECT_NEAR(live.truePeakDb, live.samplePeakDb, 1e-6);
    EXPECT_GT(file.truePeakDb, file.samplePeakDb + 1.0);
}

TEST(AudioMeter, ClippedSamplesCountOverAllChannels) {
    const float limit = (float)AudioMeter::CLIP_LEVEL;
    const float below = std::nextafter(limit, 0.0f);
    const float samples[] = {limit, -limit, 1.5f, below, 0.0f, -1.0f};
    const LoudnessResult r = AudioMeter::measure(samples, 3, kMeterRate, 2);
    EXPECT_EQ(r.clipped, 4u);
}

TEST(AudioMeter, RmsIsAes17) {
    const std::vector<float> full = toneSegments({{1.0, 0.0}});
    const LoudnessResult fullScale = AudioMeter::measure(full.data(), full.size(), kMeterRate, 1, false);
    EXPECT_NEAR(fullScale.rmsDb, 0.0, 0.01);
    EXPECT_NEAR(fullScale.meanSquare, 0.5, 0.001);

    const std::vector<float> quiet = toneSegments({{1.0, -20.0}});
    EXPECT_NEAR(AudioMeter::measure(quiet.data(), quiet.size(), kMeterRate, 1, false).rmsDb, -20.0, 0.01);
}

TEST(AudioMeter, DcOffsetIsTheLargestChannelMean) {
    std::vector<float> interleaved(kMeterRate * 2);
    for (size_t i = 0; i < kMeterRate; i++) {
        interleaved[i * 2] = 0.1f;
        interleaved[i * 2 + 1] = -0.25f;
    }
    const LoudnessResult r = AudioMeter::measure(interleaved.data(), kMeterRate, kMeterRate, 2, false);
    EXPECT_NEAR(r.dcOffset, 0.25, 1e-6);
}

TEST(AudioMeter, NonFiniteSamplesAreCountedAndMeteredAsSilence) {
    std::vector<float> samples = toneSegments({{2.0, -23.0}});
    std::vector<float> zeroed = samples;
    const size_t where[3] = {100, 101, 50000};
    const float bad[3] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()};
    for (int i = 0; i < 3; i++) {
        samples[where[i]] = bad[i];
        zeroed[where[i]] = 0.0f;
    }
    const LoudnessResult r = AudioMeter::measure(samples.data(), samples.size(), kMeterRate, 1);
    const LoudnessResult expected = AudioMeter::measure(zeroed.data(), zeroed.size(), kMeterRate, 1);
    EXPECT_EQ(r.nonFinite, 3u);
    EXPECT_EQ(expected.nonFinite, 0u);
    ASSERT_TRUE(r.integrated);
    EXPECT_EQ(r.integrated, expected.integrated);
    EXPECT_EQ(r.momentaryMax, expected.momentaryMax);
    EXPECT_EQ(r.truePeakDb, expected.truePeakDb);
    EXPECT_EQ(r.samplePeakDb, expected.samplePeakDb);
    EXPECT_EQ(r.rmsDb, expected.rmsDb);
    EXPECT_EQ(r.bandsDb, expected.bandsDb);
}

// -- bands ------------------------------------------------------------------------------------------

TEST(AudioMeter, BandCentresAndLabels) {
    EXPECT_DOUBLE_EQ(AudioMeter::bandCentre(0), 31.25);
    EXPECT_DOUBLE_EQ(AudioMeter::bandCentre(5), 1000.0);
    EXPECT_DOUBLE_EQ(AudioMeter::bandCentre(9), 16000.0);
    EXPECT_STREQ(AudioMeter::bandLabel(0), "31.5");
    EXPECT_STREQ(AudioMeter::bandLabel(5), "1k");
    EXPECT_STREQ(AudioMeter::bandLabel(9), "16k");
    EXPECT_STREQ(AudioMeter::bandLabel(10), "");
}

TEST(AudioMeter, A40HzSineLandsInThe31And63HzBandsAndTheSubBand) {
    const std::vector<float> tone = toneSegments({{5.0, -6.0}}, kMeterRate, 40.0);
    const LoudnessResult r = AudioMeter::measure(tone.data(), tone.size(), kMeterRate, 1);
    const double low = std::max(r.bandsDb[0], r.bandsDb[1]);
    EXPECT_NEAR(low, -6.0, 3.0);
    for (int b = 2; b < AudioMeter::BANDS; b++) {
        EXPECT_LT(r.bandsDb[b], low - 20.0) << AudioMeter::bandLabel(b);
    }
    EXPECT_GT(r.subShare, 0.5);
    EXPECT_LE(r.subShare, 1.0);
}

// The gain of a band at its centre, from R1's definition: 4th-order Butterworth edges at fc/sqrt(2)
// and fc*sqrt(2), through the bilinear transform; no low-pass from 0.45 of the rate. Both edges are
// half an octave away, so even mid-band a tone loses about 0.53 dB.
static double bandCentreGainDb(double centre, double rate) {
    const auto warped = [rate](double hz) { return std::tan(kMeterPi * hz / rate); };
    double attenuation = 1.0 + std::pow(warped(centre / std::sqrt(2.0)) / warped(centre), 8.0);
    if (centre * std::sqrt(2.0) < 0.45 * rate) {
        attenuation *= 1.0 + std::pow(warped(centre) / warped(centre * std::sqrt(2.0)), 8.0);
    }
    return -10.0 * std::log10(attenuation);
}

TEST(AudioMeter, EachBandReadsAToneAtItsCentre) {
    for (int b = 0; b < AudioMeter::BANDS; b++) {
        const double centre = AudioMeter::bandCentre(b);
        const std::vector<float> tone = toneSegments({{2.0, -12.0}}, kMeterRate, centre);
        const LoudnessResult r = AudioMeter::measure(tone.data(), tone.size(), kMeterRate, 1);
        // (a window's main lobe is 9 Hz wide: at 31.5 Hz it reads a little of the band's slopes too)
        EXPECT_NEAR(r.bandsDb[(size_t)b], -12.0 + bandCentreGainDb(centre, kMeterRate), 0.2) << AudioMeter::bandLabel(b);
        if (b > 0) {
            EXPECT_LT(r.bandsDb[(size_t)b - 1], -12.0 - 6.0) << AudioMeter::bandLabel(b) << " in the band below";
        }
        if (b < AudioMeter::BANDS - 1) {
            EXPECT_LT(r.bandsDb[(size_t)b + 1], -12.0 - 6.0) << AudioMeter::bandLabel(b) << " in the band above";
        }
    }
}

TEST(AudioMeter, HighBandsAboveNyquistAreSilent) {
    const std::vector<float> tone = toneSegments({{1.0, -12.0}}, 8000, 1000.0);
    const LoudnessResult r = AudioMeter::measure(tone.data(), tone.size(), 8000, 1);
    EXPECT_EQ(r.bandsDb[8], AudioMeter::SILENCE_DB);
    EXPECT_EQ(r.bandsDb[9], AudioMeter::SILENCE_DB);
    EXPECT_NEAR(r.bandsDb[5], -12.0 + bandCentreGainDb(1000.0, 8000.0), 0.1);
}

TEST(AudioMeter, ANonSubToneHasAlmostNoSubShare) {
    const std::vector<float> tone = toneSegments({{2.0, -6.0}});
    EXPECT_LT(AudioMeter::measure(tone.data(), tone.size(), kMeterRate, 1).subShare, 0.001);
}

// a run through the filters and a sample at a time are the same filters
TEST(AudioMeter, TheRunsOfTheFiltersEqualTheirSamples) {
    const std::vector<float> samples = movingSignal(kMeterRate, 0.5);
    std::vector<double> run(samples.size() / 2);
    for (size_t i = 0; i < run.size(); i++) run[i] = samples[i * 2];

    AudioKWeighting a;
    AudioKWeighting b;
    a.init(kMeterRate);
    b.init(kMeterRate);
    std::vector<double> filtered = run;
    a.processRun(filtered.data(), filtered.size());
    for (size_t i = 0; i < run.size(); i++) {
        ASSERT_EQ(filtered[i], b.process(run[i])) << i;
    }

    AudioKWeighting c;
    AudioKWeighting d;
    AudioKWeighting e;
    c.init(kMeterRate);
    d.init(kMeterRate);
    e.init(kMeterRate);
    std::vector<double> paired = run;
    std::vector<double> silent(run.size(), 0.0);
    c.processRuns(d, paired.data(), silent.data(), paired.size());
    for (size_t i = 0; i < run.size(); i++) {
        ASSERT_EQ(paired[i], e.process(run[i])) << i;
        ASSERT_EQ(silent[i], 0.0) << i;
    }

}

TEST(AudioMeter, BandsOffLeaveTheirFieldsAtTheFloor) {
    const std::vector<float> tone = toneSegments({{1.0, -6.0}});
    const LoudnessResult r = AudioMeter::measure(tone.data(), tone.size(), kMeterRate, 1, false);
    for (double db : r.bandsDb) {
        EXPECT_EQ(db, AudioMeter::SILENCE_DB);
    }
    EXPECT_EQ(r.subShare, 0.0);
    EXPECT_EQ(r.subMeanSquare, 0.0);
}

// -- stream and file --------------------------------------------------------------------------------

TEST(AudioMeter, StreamEqualsFileMode) {
    for (unsigned int rate : {44100u, 48000u}) {
        const std::vector<float> samples = movingSignal(rate, 12.0);
        const size_t frames = samples.size() / 2;
        const LoudnessResult file = AudioMeter::measure(samples.data(), frames, rate, 2);

        AudioMeter stream(rate, 2);
        size_t position = 0;
        size_t block = 1;
        while (position < frames) {
            const size_t count = std::min(block, frames - position);
            stream.process(samples.data() + position * 2, count);
            position += count;
            block = block % 4096 + 257;
        }
        stream.finish();
        const LoudnessResult r = stream.result();

        ASSERT_TRUE(file.integrated && file.momentaryMax && file.shortTermMax && file.lra);
        ASSERT_TRUE(r.integrated && r.momentaryMax && r.shortTermMax && r.lra);
        EXPECT_NEAR(*r.integrated, *file.integrated, 0.01) << rate;
        EXPECT_NEAR(*r.momentaryMax, *file.momentaryMax, 0.01) << rate;
        EXPECT_NEAR(*r.shortTermMax, *file.shortTermMax, 0.01) << rate;
        EXPECT_NEAR(*r.lra, *file.lra, 0.01) << rate;
        EXPECT_EQ(r.frames, file.frames);
        EXPECT_NEAR(r.samplePeakDb, file.samplePeakDb, 1e-9);
        EXPECT_NEAR(r.rmsDb, file.rmsDb, 1e-9);
    }
}

TEST(AudioMeter, BlockSizesDoNotChangeTheResult) {
    const std::vector<float> samples = movingSignal(kMeterRate, 4.0);
    const size_t frames = samples.size() / 2;
    AudioMeter whole(kMeterRate, 2);
    whole.process(samples.data(), frames);
    AudioMeter single(kMeterRate, 2);
    for (size_t i = 0; i < frames; i++) {
        single.process(samples.data() + i * 2, 1);
    }
    const LoudnessResult a = whole.result();
    const LoudnessResult b = single.result();
    // the same frames summed in other runs: the last bits of a double, nothing more
    ASSERT_TRUE(a.integrated && b.integrated && a.momentaryMax && b.momentaryMax && a.shortTermMax && b.shortTermMax);
    EXPECT_NEAR(*a.integrated, *b.integrated, 1e-9);
    EXPECT_NEAR(*a.momentaryMax, *b.momentaryMax, 1e-9);
    EXPECT_NEAR(*a.shortTermMax, *b.shortTermMax, 1e-9);
    EXPECT_EQ(a.truePeakDb, b.truePeakDb);
    for (size_t band = 0; band < a.bandsDb.size(); band++) {
        EXPECT_NEAR(a.bandsDb[band], b.bandsDb[band], 1e-9) << band;
    }
}

// result() is a read: a stream goes on as if nobody had looked
TEST(AudioMeter, ResultCanBeReadAtAnyTime) {
    const std::vector<float> samples = movingSignal(kMeterRate, 5.0);
    const size_t frames = samples.size() / 2;
    AudioMeter watched(kMeterRate, 2);
    AudioMeter unwatched(kMeterRate, 2);
    for (size_t position = 0; position < frames; position += 4800) {
        const size_t count = std::min<size_t>(4800, frames - position);
        watched.process(samples.data() + position * 2, count);
        unwatched.process(samples.data() + position * 2, count);
        watched.result();
    }
    EXPECT_EQ(watched.result().integrated, unwatched.result().integrated);

    // the short-term maximum appears at 3 s and stays
    AudioMeter growing(kMeterRate, 2);
    growing.process(samples.data(), 2 * kMeterRate);
    EXPECT_FALSE(growing.result().shortTermMax);
    EXPECT_TRUE(growing.result().momentaryMax);
    growing.process(samples.data() + 2 * kMeterRate * 2, 2 * kMeterRate);
    EXPECT_TRUE(growing.result().shortTermMax);
}

// A stream's windows stop at the last whole 10 ms of it; a file's padding completes them. So what
// follows the last sub-block boundary counts only in a file.
TEST(AudioMeter, FileModePadsTheEndAndStreamModeDoesNot) {
    std::vector<float> samples(2 * kMeterRate / 5, 0.0f);
    const std::vector<float> burst = toneSegments({{0.005, -6.0}});
    samples.insert(samples.end(), burst.begin(), burst.end());

    const LoudnessResult file = AudioMeter::measure(samples.data(), samples.size(), kMeterRate, 1, false);

    AudioMeter stream(kMeterRate, 1, AudioMeterOptions{AudioMeterMode::Stream, false});
    stream.process(samples.data(), samples.size());
    stream.finish();
    const LoudnessResult live = stream.result();

    ASSERT_TRUE(file.momentaryMax && live.momentaryMax);
    EXPECT_GT(*file.momentaryMax, -40.0);
    EXPECT_EQ(*live.momentaryMax, AudioMeter::SILENCE_DB);
    EXPECT_EQ(file.frames, live.frames);
}

TEST(AudioMeter, ProcessAfterFinishDoesNothing) {
    const std::vector<float> samples = toneSegments({{1.0, -23.0}});
    AudioMeter meter(kMeterRate, 1, AudioMeterOptions{AudioMeterMode::File, false});
    meter.process(samples.data(), samples.size());
    meter.finish();
    EXPECT_TRUE(meter.isFinished());
    const uint64_t frames = meter.result().frames;
    meter.process(samples.data(), samples.size());
    meter.finish();
    EXPECT_EQ(meter.result().frames, frames);
    EXPECT_EQ(frames, samples.size());
}

// the padding is not signal: it changes no level
TEST(AudioMeter, PaddingCountsInNoLevel) {
    const std::vector<float> samples = toneSegments({{1.0, -20.0}});
    const LoudnessResult file = AudioMeter::measure(samples.data(), samples.size(), kMeterRate, 1);
    AudioMeter stream(kMeterRate, 1);
    stream.process(samples.data(), samples.size());
    const LoudnessResult live = stream.result();
    EXPECT_EQ(file.frames, samples.size());
    EXPECT_DOUBLE_EQ(file.rmsDb, live.rmsDb);
    EXPECT_DOUBLE_EQ(file.samplePeakDb, live.samplePeakDb);
}

// The bands come from the spectrum, the sample-by-sample bank has them from its filters: the same
// magnitudes, the same levels. Bands 70 dB under the loudest one only hold each method's leakage.
TEST(AudioMeter, TheBandsAgreeWithTheFilterBank) {
    for (unsigned int rate : {44100u, 48000u, 96000u}) {
        const std::vector<float> samples = movingSignal(rate, 6.0);
        const size_t frames = samples.size() / 2;
        const LoudnessResult r = AudioMeter::measure(samples.data(), frames, rate, 2);

        AudioOctaveBank left;
        AudioOctaveBank right;
        left.init(rate);
        right.init(rate);
        double energy[AudioOctaveBank::BANDS + 1] = {};
        for (size_t i = 0; i < frames; i++) {
            double y[AudioOctaveBank::BANDS + 1];
            left.process(samples[i * 2], y);
            for (int b = 0; b <= AudioOctaveBank::BANDS; b++) energy[b] += y[b] * y[b];
            right.process(samples[i * 2 + 1], y);
            for (int b = 0; b <= AudioOctaveBank::BANDS; b++) energy[b] += y[b] * y[b];
        }
        const auto db = [frames](double e) { return 10.0 * std::log10(e / (double)(frames * 2)) + 3.0103; };
        double loudest = -200.0;
        for (int b = 0; b < AudioOctaveBank::BANDS; b++) loudest = std::max(loudest, db(energy[b]));
        for (int b = 0; b < AudioOctaveBank::BANDS; b++) {
            if (db(energy[b]) < loudest - 70.0) continue;
            EXPECT_NEAR(r.bandsDb[(size_t)b], db(energy[b]), 0.3) << AudioMeter::bandLabel(b) << " at " << rate;
        }
        EXPECT_NEAR(r.subShare, energy[AudioOctaveBank::BANDS] / (double)(frames * 2) / r.meanSquare, 0.002) << rate;
    }
}

// The windows hold all of a signal's energy, wherever it ends in a window: a tone at a band's centre
// reads the band's gain over any length.
TEST(AudioMeter, TheSpectrumKeepsAllTheEnergy) {
    const double expected = -12.0 + bandCentreGainDb(1000.0, kMeterRate);
    for (size_t frames : {size_t(4800), size_t(8192), size_t(16384), size_t(24576), size_t(40000), size_t(48000)}) {
        std::vector<float> tone = toneSegments({{(double)frames / kMeterRate, -12.0}});
        const LoudnessResult r = AudioMeter::measure(tone.data(), tone.size(), kMeterRate, 1);
        EXPECT_NEAR(r.bandsDb[5], expected, 0.05) << frames;
    }
}

// a stream's bands read before the end are the mean of the windows so far
TEST(AudioMeter, AStreamsBandsCanBeReadBeforeTheEnd) {
    const std::vector<float> tone = toneSegments({{1.0, -12.0}});
    AudioMeter stream(kMeterRate, 1);
    stream.process(tone.data(), 4800);
    EXPECT_EQ(stream.result().bandsDb[5], AudioMeter::SILENCE_DB) << "before the first window";
    stream.process(tone.data() + 4800, tone.size() - 4800);
    const double expected = -12.0 + bandCentreGainDb(1000.0, kMeterRate);
    EXPECT_NEAR(stream.result().bandsDb[5], expected, 0.5) << "the first windows hold silence";
    stream.finish();
    EXPECT_NEAR(stream.result().bandsDb[5], expected, 0.05);
}

// rates whose 10 ms are not a whole number of frames: the sub-block lengths vary, the grid stays
TEST(AudioMeter, RatesWithoutAWholeSubBlock) {
    for (unsigned int rate : {22050u, 11025u}) {
        const LoudnessResult r = measureStereo({{10.0, -23.0}}, rate);
        ASSERT_TRUE(r.integrated);
        EXPECT_NEAR(*r.integrated, -23.0, kLufsTolerance + 0.1) << rate;
        EXPECT_EQ(r.gatedBlocks, 97u) << rate;
    }
}

TEST(AudioMeter, CurvesHaveAPointEvery100ms) {
    const std::vector<float> samples = toneSegments({{4.05, -23.0}});
    AudioMeter meter(kMeterRate, 1, AudioMeterOptions{AudioMeterMode::Stream, false});
    meter.process(samples.data(), samples.size());
    const std::vector<float> momentary = meter.momentaryCurve();
    const std::vector<float> shortTerm = meter.shortTermCurve();
    EXPECT_EQ(momentary.size(), 40u);
    EXPECT_EQ(shortTerm.size(), 40u);
    EXPECT_TRUE(std::isnan(shortTerm[28]));
    EXPECT_FALSE(std::isnan(shortTerm[29]));
}

// a long stream's quiet window is not lost to the running sums behind the windows
TEST(AudioMeter, AQuietWindowAfterALoudStretchKeepsItsLevel) {
    const std::vector<float> interleaved = toStereo(toneSegments({{60.0, 0.0}, {5.0, -60.0}}));
    AudioMeter meter(kMeterRate, 2, AudioMeterOptions{AudioMeterMode::Stream, false});
    meter.process(interleaved.data(), interleaved.size() / 2);
    const std::vector<float> curve = meter.momentaryCurve();
    ASSERT_GE(curve.size(), 650u);
    EXPECT_NEAR(curve[649], -60.0, kLufsTolerance);
}

// a filter in digital silence, settled every 10 ms as the meter does, reaches exact zero and not a
// denormal or a limit cycle
TEST(AudioMeter, FiltersSettleOnZeroInSilence) {
    AudioKWeighting filter;
    filter.init(kMeterRate);
    filter.process(1.0);
    for (unsigned int block = 0; block < 6000; block++) {
        for (unsigned int i = 0; i < kMeterRate / 100; i++) {
            filter.process(0.0);
        }
        filter.settle();
    }
    EXPECT_EQ(filter.shelf.z1, 0.0);
    EXPECT_EQ(filter.shelf.z2, 0.0);
    EXPECT_EQ(filter.highPass.z1, 0.0);
    EXPECT_EQ(filter.highPass.z2, 0.0);
}

// the meter does settle them: a signal's level is unchanged by it, however quiet the tail
TEST(AudioMeter, SettlingChangesNoLevelOverTheFloor) {
    const LoudnessResult r = measureStereo({{5.0, -110.0}});
    ASSERT_TRUE(r.momentaryMax);
    EXPECT_NEAR(*r.momentaryMax, -110.0, kLufsTolerance);
}

// and digital silence reads as silence after any amount of signal
TEST(AudioMeter, SilenceAfterSignalIsSilence) {
    std::vector<float> samples = toneSegments({{2.0, 0.0}});
    samples.resize(samples.size() + kMeterRate * 2, 0.0f);
    AudioMeter meter(kMeterRate, 1, AudioMeterOptions{AudioMeterMode::Stream, false});
    meter.process(samples.data(), samples.size());
    const std::vector<float> curve = meter.momentaryCurve();
    EXPECT_EQ(curve.back(), (float)AudioMeter::SILENCE_DB);
}
