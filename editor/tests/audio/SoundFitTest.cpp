// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "audio/SoundDesign.h"
#include "audio/SoundFit.h"
#include "audio/SoundSynth.h"
#include "audio/SoundTypes.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

const double PI = 3.14159265358979323846;

AudioAnalysis::Signal tone(double hz, double seconds, double amplitude, double decay = 0.0) {
    AudioAnalysis::Signal signal;
    signal.channels = 1;
    signal.sampleRate = 48000;
    signal.frames = (size_t)(seconds * 48000);
    signal.samples.resize(signal.frames);
    for (size_t i = 0; i < signal.frames; i++) {
        const double t = (double)i / 48000.0;
        signal.samples[i] = (float)(amplitude * std::sin(2.0 * PI * hz * t) * std::exp(-decay * t));
    }
    return signal;
}

}

TEST(SoundFit, ThirdOctaveBandHoldsAToneAtItsCentre) {
    const SoundFit::Measure m = SoundFit::measure(tone(1000.0, 0.5, 0.5));
    const auto levels = SoundFit::bandLevels(m, 48000);
    EXPECT_DOUBLE_EQ(SoundFit::bandCentre(13), 1000.0);
    EXPECT_NEAR(SoundFit::bandCentre(0), 50.1, 0.1);
    EXPECT_NEAR(SoundFit::bandCentre(25), 15849.0, 1.0);
    EXPECT_GT(levels[13], -0.5);
    EXPECT_LT(levels[3], -30.0);
}

TEST(SoundFit, MeasuresDoNotDependOnTheLevel) {
    const SoundFit::Measure a = SoundFit::measure(tone(700.0, 0.4, 0.8, 8.0));
    const SoundFit::Measure b = SoundFit::measure(tone(700.0, 0.4, 0.05, 8.0));
    const SoundFit::Quality q = SoundFit::compare(a, b);
    EXPECT_LT(q.distance, 0.05);
    EXPECT_EQ(q.grade, "close");
}

TEST(SoundFit, DistanceTermsAreOneAtTheirStep) {
    // on 1.25 x the reference's 0.5 s: 100 frames alike, then 25 where the reference reads the floor (-60 dB)
    const SoundFit::Measure shortTone = SoundFit::measure(tone(1000.0, 0.5, 0.5));
    const SoundFit::Measure longTone = SoundFit::measure(tone(1000.0, 1.0, 0.5));
    const SoundFit::Quality q = SoundFit::compare(shortTone, longTone);
    EXPECT_NEAR(q.lengthRatio, 2.0, 0.01);
    EXPECT_NEAR(q.envelopeDb, std::sqrt(25.0 * 3600.0 / 125.0), 0.5);
    // the abrupt ends' leakage, a smaller share of the longer tone
    EXPECT_LT(q.bandDb, 3.0);
    EXPECT_NEAR(q.distance, q.bandDb / 3.0 + q.envelopeDb / 6.0 + 4.0, 0.1);
    EXPECT_EQ(q.grade, "rough");
    EXPECT_EQ(SoundFit::gradeOf(1.0), "close");
    EXPECT_EQ(SoundFit::gradeOf(1.5), "similar");
    EXPECT_EQ(SoundFit::gradeOf(2.5), "rough");
}

TEST(SoundFit, SilenceIsNotMeasured) {
    AudioAnalysis::Signal silent = tone(100.0, 0.1, 0.0);
    EXPECT_TRUE(SoundFit::measure(silent).silent);
}

TEST(SoundFit, TheFitIsDeterministicAndBounded) {
    const SoundSynth::Preset& thump = *SoundSynth::find("thump");
    std::vector<float> samples;
    unsigned int rate = 0;
    std::string error;
    ASSERT_TRUE(SoundSynth::render(thump, 3, nlohmann::json{{"freq_hz", 100}, {"decay_ms", 500}, {"length_ms", 900}}, samples, rate, error)) << error;
    AudioAnalysis::Signal reference;
    reference.channels = 1;
    reference.sampleRate = rate;
    reference.frames = samples.size();
    reference.samples = samples;
    const SoundFit::Measure measured = SoundFit::measure(reference);
    SoundFit::Result one, two;
    ASSERT_TRUE(SoundFit::fit(measured, thump, "", 3, one, error)) << error;
    ASSERT_TRUE(SoundFit::fit(measured, thump, "", 3, two, error)) << error;
    EXPECT_EQ(one.params.dump(), two.params.dump());
    EXPECT_DOUBLE_EQ(one.quality.distance, two.quality.distance);
    EXPECT_LE(one.renders, SoundTypes::FIT_RENDERS);
    EXPECT_LE(one.quality.distance, one.startDistance);
}

TEST(SoundFit, CancelEndsTheSearch) {
    const SoundFit::Measure measured = SoundFit::measure(tone(800.0, 0.3, 0.5, 10.0));
    std::atomic<bool> cancel{true};
    SoundFit::Result result;
    std::string error;
    EXPECT_FALSE(SoundFit::fit(measured, *SoundSynth::find("click"), "", 1, result, error, nullptr, &cancel));
    EXPECT_EQ(error, "cancelled");
}

TEST(SoundDesign, TheTableIsTheSpecs) {
    EXPECT_EQ(SoundDesign::families("ui_click"), (std::vector<std::string>{"tink", "snap", "mouse", "glass", "soft"}));
    EXPECT_EQ(SoundDesign::families("swoosh"), (std::vector<std::string>{"fast", "big", "thin"}));
    EXPECT_EQ(SoundDesign::families("impact"), (std::vector<std::string>{"wood", "metal", "stone", "soft", "heavy"}));
    EXPECT_EQ(SoundDesign::families("pop"), (std::vector<std::string>{"bubble", "cork", "soft"}));
    EXPECT_TRUE(SoundDesign::families("click").empty());
}
