// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "audio/AudioProvenance.h"
#include "audio/SoundSlicer.h"
#include "audio/SoundTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

const double PI = 3.14159265358979323846;
const unsigned int RATE = 48000;

// decaying bursts at these times, seconds
AudioAnalysis::Signal takes(const std::vector<double>& times, double seconds, double amplitude = 0.7) {
    AudioAnalysis::Signal signal;
    signal.channels = 1;
    signal.sampleRate = RATE;
    signal.frames = (size_t)(seconds * RATE);
    signal.samples.assign(signal.frames, 0.0f);
    for (double at : times) {
        const size_t start = (size_t)(at * RATE);
        for (size_t i = start; i < signal.frames; i++) {
            const double t = (double)(i - start) / RATE;
            signal.samples[i] += (float)(amplitude * std::sin(2.0 * PI * 900.0 * t) * std::exp(-30.0 * t));
        }
    }
    return signal;
}

}

TEST(SoundSlicer, CutsOneSlicePerTake) {
    const std::vector<double> times = {0.2, 0.7, 1.2, 1.7};
    std::vector<SoundSlicer::Slice> slices;
    std::string error;
    ASSERT_TRUE(SoundSlicer::detect(takes(times, 2.2), SoundSlicer::Options(), SoundTypes::TRANSIENT, slices, error)) << error;
    ASSERT_EQ(slices.size(), times.size());
    for (size_t i = 0; i < slices.size(); i++) {
        const double start = (double)slices[i].startFrame / RATE;
        EXPECT_NEAR(start, times[i], 0.012) << i;
        EXPECT_LT(start, times[i] + 1e-3) << "the pre_ms lead";
        EXPECT_GT(slices[i].endFrame, slices[i].startFrame);
        if (i + 1 < slices.size()) {
            EXPECT_LE(slices[i].endFrame, slices[i + 1].startFrame) << "a take never bleeds into the next";
        }
    }
}

TEST(SoundSlicer, GateDropsAQuietHit) {
    AudioAnalysis::Signal signal = takes({0.2, 1.2}, 1.8);
    const AudioAnalysis::Signal quiet = takes({0.7}, 1.8, 0.002);
    for (size_t i = 0; i < signal.frames; i++) signal.samples[i] += quiet.samples[i];
    std::vector<SoundSlicer::Slice> slices;
    std::string error;
    ASSERT_TRUE(SoundSlicer::detect(signal, SoundSlicer::Options(), SoundTypes::TRANSIENT, slices, error)) << error;
    EXPECT_EQ(slices.size(), 2u);
}

TEST(SoundSlicer, MinGapMergesCloseOnsets) {
    SoundSlicer::Options options;
    options.minGapMs = 400.0;
    std::vector<SoundSlicer::Slice> slices;
    std::string error;
    ASSERT_TRUE(SoundSlicer::detect(takes({0.2, 0.4, 1.2}, 1.8), options, SoundTypes::TRANSIENT, slices, error)) << error;
    EXPECT_EQ(slices.size(), 2u);
}

TEST(SoundSlicer, MaxLengthCutsAndTheCutFades) {
    SoundSlicer::Options options;
    options.maxMs = 25.0 + 5.0;
    std::vector<SoundSlicer::Slice> slices;
    std::string error;
    ASSERT_TRUE(SoundSlicer::detect(takes({0.2, 0.8}, 1.4), options, SoundTypes::TRANSIENT, slices, error)) << error;
    ASSERT_EQ(slices.size(), 2u);
    EXPECT_LE(slices[0].endFrame - slices[0].startFrame, (uint32_t)(0.03 * RATE) + 1);
    // the burst is loud where max_ms cut it
    EXPECT_GT(slices[0].fadeOutMs, 0.0);
    EXPECT_GT(slices[0].tailCutDb, -20.0);
}

TEST(SoundSlicer, RefusalsSayWhatToChange) {
    std::vector<SoundSlicer::Slice> slices;
    std::string error;
    SoundSlicer::Options options;
    options.gateDb = 0.0;
    EXPECT_FALSE(SoundSlicer::detect(takes({0.2}, 0.6), options, SoundTypes::TRANSIENT, slices, error));
    EXPECT_NE(error.find("gate_db"), std::string::npos);

    AudioAnalysis::Signal silent = takes({}, 0.5);
    EXPECT_FALSE(SoundSlicer::detect(silent, SoundSlicer::Options(), SoundTypes::TRANSIENT, slices, error));
    EXPECT_NE(error.find("silent"), std::string::npos);

    std::vector<double> many;
    for (int i = 0; i < 70; i++) many.push_back(0.1 + i * 0.15);
    EXPECT_FALSE(SoundSlicer::detect(takes(many, 11.0), SoundSlicer::Options(), SoundTypes::TRANSIENT, slices, error));
    EXPECT_NE(error.find("raise gate_db or min_gap_ms"), std::string::npos) << error;
}

TEST(SoundSlicer, PlanWritesPickedSlicesWithTheirProvenance) {
    const fs::path directory = fs::temp_directory_path() / "s36-slicer-test";
    fs::remove_all(directory);
    fs::create_directories(directory);
    const fs::path source = directory / "steps.wav";
    const AudioAnalysis::Signal signal = takes({0.2, 0.7, 1.2}, 1.8);
    std::vector<SoundSlicer::Slice> slices;
    std::string error;
    ASSERT_TRUE(SoundSlicer::detect(signal, SoundSlicer::Options(), SoundTypes::TRANSIENT, slices, error)) << error;

    std::vector<SoundSlicer::File> files;
    ASSERT_TRUE(SoundSlicer::plan(source, "assets/steps.wav", "abc", signal, slices, {1, 3}, directory / "ev.soundevent", files, error)) << error;
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].wav.filename().string(), "ev_01.wav");
    EXPECT_EQ(files[1].wav.filename().string(), "ev_02.wav");
    EXPECT_NE(files[1].metaText.find("sliced_from"), std::string::npos);
    EXPECT_NE(files[1].metaText.find(std::to_string(slices[2].startFrame)), std::string::npos);
    EXPECT_EQ(files[0].wavBytes.size(), 44u + (size_t)(slices[0].endFrame - slices[0].startFrame) * 2);

    EXPECT_FALSE(SoundSlicer::plan(source, "assets/steps.wav", "abc", signal, slices, {9}, directory / "ev.soundevent", files, error));
    EXPECT_NE(error.find("pick 9"), std::string::npos);
    { std::ofstream(directory / "ev_02.wav") << "x"; }
    EXPECT_FALSE(SoundSlicer::plan(source, "assets/steps.wav", "abc", signal, slices, {}, directory / "ev.soundevent", files, error));
    EXPECT_NE(error.find("ev_02.wav exists"), std::string::npos);
    EXPECT_TRUE(files.empty()) << "nothing is made before a refusal";
    fs::remove_all(directory);
}
