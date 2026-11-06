// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Tempo and downbeat detection (docs/specs/S39-music.md R16, AC7; R22's budget). Oracles are the
// synthetic click tracks' own parameters: 2 ms 3 kHz clicks on every beat, a 40 ms 60 Hz thump
// added on each downbeat (sound-layer P6 §3.9), 48 kHz mono.

#include <gtest/gtest.h>

#include "TempDir.h"
#include "support/EditorTestSupport.h"

#include "audio/AudioMusicGrid.h"
#include "audio/WavWriter.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

constexpr unsigned int RATE = 48000;
constexpr double PI_VALUE = 3.14159265358979323846;

std::vector<float> clickTrack(double bpm, double firstBeatSeconds, int beatsPerBar, double seconds) {
    std::vector<float> x((size_t)(seconds * RATE), 0.0f);
    const double period = 60.0 / bpm;
    const size_t click = (size_t)(0.002 * RATE);
    const size_t thump = (size_t)(0.040 * RATE);
    for (int k = 0;; k++) {
        const double t = firstBeatSeconds + k * period;
        const size_t start = (size_t)std::llround(t * RATE);
        if (start >= x.size()) break;
        for (size_t i = 0; i < click && start + i < x.size(); i++) x[start + i] += (float)(0.5 * std::sin(2.0 * PI_VALUE * 3000.0 * (double)i / RATE));
        if (k % beatsPerBar == 0) {
            for (size_t i = 0; i < thump && start + i < x.size(); i++) x[start + i] += (float)(0.5 * std::sin(2.0 * PI_VALUE * 60.0 * (double)i / RATE));
        }
    }
    return x;
}

AudioAnalysis::Signal signalOf(std::vector<float> mono) {
    AudioAnalysis::Signal signal;
    signal.channels = 1;
    signal.sampleRate = RATE;
    signal.frames = mono.size();
    signal.samples = std::move(mono);
    return signal;
}

// white noise from a fixed LCG: the same on every platform
std::vector<float> noise(double seconds) {
    std::vector<float> x((size_t)(seconds * RATE));
    uint32_t state = 12345u;
    for (float& v : x) {
        state = state * 1664525u + 1013904223u;
        v = 0.25f * ((float)(state >> 8) / (float)(1u << 24) * 2.0f - 1.0f);
    }
    return x;
}

void expectGrid(double bpm, double firstBeatSeconds, int beatsPerBar) {
    const AudioMusicGrid::Grid grid = AudioMusicGrid::detect(signalOf(clickTrack(bpm, firstBeatSeconds, beatsPerBar, 12.0)));
    SCOPED_TRACE(std::to_string(bpm) + " bpm, " + std::to_string(beatsPerBar) + " beats a bar, first beat " + std::to_string(firstBeatSeconds) + " s");
    ASSERT_TRUE(grid.hasTempo()) << grid.toJson().dump();
    EXPECT_NEAR(grid.bpm, bpm, 0.1);
    EXPECT_NEAR(grid.firstBeatMs, firstBeatSeconds * 1000.0, 5.0);
    EXPECT_EQ(grid.beatsPerBarGuess, beatsPerBar);
    EXPECT_GT(grid.downbeatConfidence, 0.5);
    EXPECT_TRUE(grid.confident);
    ASSERT_EQ(grid.candidates.size(), 2u);
    EXPECT_EQ(grid.candidates[0].bpm, grid.bpm);
    EXPECT_EQ(grid.candidates[1].bpm, grid.bpmAlt);
}

std::string readText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST(AudioMusicGrid, DetectsNinetyBpmInFourAndThree) {
    expectGrid(90.0, 0.25, 4);
    expectGrid(90.0, 0.30, 3);
}

TEST(AudioMusicGrid, DetectsOneTwentyBpmInFourAndThree) {
    expectGrid(120.0, 0.10, 4);
    // a downbeat on the file's first sample
    expectGrid(120.0, 0.0, 3);
}

TEST(AudioMusicGrid, DetectsOneFortyBpmInFourAndThree) {
    expectGrid(140.0, 0.37, 4);
    expectGrid(140.0, 0.20, 3);
}

TEST(AudioMusicGrid, PulseFreeNoiseHasNoTempo) {
    const AudioMusicGrid::Grid grid = AudioMusicGrid::detect(signalOf(noise(12.0)));
    EXPECT_FALSE(grid.hasTempo());
    EXPECT_LT(grid.pulseClarity, 0.1);
    ASSERT_EQ(grid.notes.size(), 1u);
    EXPECT_EQ(grid.notes[0]["id"], "no_pulse");
    EXPECT_TRUE(grid.toJson()["bpm"].is_null());
    const nlohmann::json item = AudioMusicGrid::item(grid, nullptr, 0.0, 0.0);
    EXPECT_TRUE(item["fix"].is_null());
    EXPECT_TRUE(item["candidates"].empty());
}

TEST(AudioMusicGrid, TwoSecondsIsTooShort) {
    const AudioMusicGrid::Grid grid = AudioMusicGrid::detect(signalOf(clickTrack(120.0, 0.0, 4, 2.0)));
    EXPECT_FALSE(grid.hasTempo());
    ASSERT_EQ(grid.notes.size(), 1u);
    EXPECT_EQ(grid.notes[0]["id"], "too_short");
    EXPECT_EQ(grid.notes[0]["text"], "too short for a beat grid (2.0 s; needs 4)");
    EXPECT_EQ(AudioMusicGrid::line(grid), "too short for a beat grid (2.0 s; needs 4)");
}

TEST(AudioMusicGrid, CacheRoundTripIsTheSameGrid) {
    jasperengine::test::TempDir dir;
    const fs::path file = dir.path() / "loop.wav";
    const fs::path cache = dir.path() / "cache";
    {
        std::ofstream out(file, std::ios::binary);
        out << WavWriter::pcm16(clickTrack(120.0, 0.25, 4, 8.0), 1, RATE);
    }
    AudioMusicGrid::Grid first, second;
    std::string error;
    ASSERT_TRUE(AudioMusicGrid::get(file, cache, first, error)) << error;
    ASSERT_EQ(first.sha1.size(), 40u);
    const fs::path cached = AudioMusicGrid::cacheFile(cache, first.sha1);
    EXPECT_EQ(cached.filename().string(), first.sha1 + "-grid-v1.json");
    ASSERT_TRUE(fs::exists(cached));
    const std::string bytes = readText(cached);
    ASSERT_TRUE(AudioMusicGrid::get(file, cache, second, error)) << error;
    EXPECT_EQ(second.toJson().dump(), first.toJson().dump());
    EXPECT_EQ(readText(cached), bytes);
    EXPECT_NEAR(first.bpm, 120.0, 0.1);

    // the second answer is the file's: an edited cache shows
    nlohmann::json edited = nlohmann::json::parse(bytes);
    edited["bpm"] = 99.0;
    {
        std::ofstream out(cached, std::ios::binary);
        out << edited.dump();
    }
    ASSERT_TRUE(AudioMusicGrid::get(file, cache, second, error)) << error;
    EXPECT_EQ(second.bpm, 99.0);
    // another grid version is not read: detected afresh
    edited["grid_version"] = 0;
    {
        std::ofstream out(cached, std::ios::binary);
        out << edited.dump();
    }
    ASSERT_TRUE(AudioMusicGrid::get(file, cache, second, error)) << error;
    EXPECT_EQ(second.toJson().dump(), first.toJson().dump());
}

TEST(AudioMusicGrid, ItemReportsLoopBeatsAgreementAndFix) {
    AudioMusicGrid::Grid grid;
    grid.sampleRate = RATE;
    grid.frames = 8 * RATE;
    grid.bpm = 120.0;
    grid.bpmAlt = 60.0;
    grid.firstBeatMs = 12.4;
    grid.beatsPerBarGuess = 4;
    grid.pulseClarity = 0.71;
    grid.downbeatConfidence = 0.62;
    grid.confident = true;
    grid.candidates = {{120.0, 12.4, 4, 0.71}, {60.0, 12.4, 4, 0.44}};

    AudioImportSettings settings;
    nlohmann::json item = AudioMusicGrid::item(grid, &settings, 0.0, 0.0);
    EXPECT_EQ(item["loop_beats"], 16.0);
    EXPECT_TRUE(item["current"].is_null());
    EXPECT_EQ(item["agrees"], false);
    EXPECT_EQ(item["fix"], nlohmann::json({{"music", {{"bpm", 120.0}, {"beats_per_bar", 4}, {"first_beat_ms", 12.4}}}}));
    EXPECT_TRUE(item["notes"].empty());

    // fix applied: agrees
    std::string error;
    ASSERT_TRUE(settings.apply(item["fix"], error)) << error;
    item = AudioMusicGrid::item(grid, &settings, 0.0, 0.0);
    EXPECT_EQ(item["current"], item["fix"]["music"]);
    EXPECT_EQ(item["agrees"], true);
    // beat 1 a bar later is the same grid; 6 ms off, another beats per bar or 0.2 bpm off is not
    settings.music.firstBeatMs = 2012.4;
    EXPECT_EQ(AudioMusicGrid::item(grid, &settings, 0.0, 0.0)["agrees"], true);
    settings.music.firstBeatMs = 18.4;
    EXPECT_EQ(AudioMusicGrid::item(grid, &settings, 0.0, 0.0)["agrees"], false);
    settings.music.firstBeatMs = 12.4;
    settings.music.beatsPerBar = 3;
    EXPECT_EQ(AudioMusicGrid::item(grid, &settings, 0.0, 0.0)["agrees"], false);
    settings.music.beatsPerBar = 4;
    settings.music.bpm = 120.2;
    EXPECT_EQ(AudioMusicGrid::item(grid, &settings, 0.0, 0.0)["agrees"], false);

    // a loop region of 7.93 beats drifts; 8.00 does not
    ASSERT_TRUE(settings.apply({{"loop_start_ms", 1000.0}, {"loop_end_ms", 1000.0 + 7.93 * 500.0}}, error)) << error;
    item = AudioMusicGrid::item(grid, &settings, 0.0, 0.0);
    EXPECT_EQ(item["loop_beats"], 7.93);
    ASSERT_EQ(item["notes"].size(), 1u);
    EXPECT_EQ(item["notes"][0]["id"], "beat_drift");
    EXPECT_EQ(item["notes"][0]["text"], "loop region is 7.93 beats at 120.00 bpm: stingers will drift; make the loop a whole number of beats");
    ASSERT_TRUE(settings.apply({{"loop_end_ms", 5000.0}}, error)) << error;
    item = AudioMusicGrid::item(grid, &settings, 0.0, 0.0);
    EXPECT_EQ(item["loop_beats"], 8.0);
    EXPECT_TRUE(item["notes"].empty());

    // a built-in clip has no .meta: no current, no agreement
    item = AudioMusicGrid::item(grid, nullptr, 0.0, 0.0);
    EXPECT_TRUE(item["agrees"].is_null());
    EXPECT_EQ(AudioMusicGrid::line(grid), "tempo 120.00 bpm (alt 60.00), 4/4 guess, first beat 12.4 ms, pulse 0.71, downbeat 0.62");
}

// R22: a 3-minute track <= 1.5 s (Release). Printed always, enforced by perfSlack() on a benchmark run.
TEST(AudioMusicGridBench, ThreeMinuteTrack) {
    const AudioAnalysis::Signal signal = signalOf(clickTrack(128.0, 0.25, 4, 180.0));
    const auto start = std::chrono::steady_clock::now();
    const AudioMusicGrid::Grid grid = AudioMusicGrid::detect(signal);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("[bench] AudioMusicGrid of a 3-minute track: %.3f s (budget 1.5 s)\n", seconds);
    EXPECT_NEAR(grid.bpm, 128.0, 0.1);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LE(seconds, 1.5 * slack);
    }
}
