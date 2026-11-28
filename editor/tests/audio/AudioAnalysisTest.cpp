// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "audio/AudioAnalysis.h"
#include "audio/SoundTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace AudioAnalysisTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

constexpr double kPi = 3.14159265358979323846;

using Json = AudioAnalysis::Json;

AudioAnalysis::Signal twoChannels(size_t frames) {
    AudioAnalysis::Signal signal;
    signal.channels = 2;
    signal.sampleRate = 48000;
    signal.frames = frames;
    signal.samples.assign(frames * 2, 0.0f);
    return signal;
}

// deterministic white noise in [-amplitude, amplitude]
struct Noise {
    uint32_t state;
    float next(float amplitude) {
        state = state * 1664525u + 1013904223u;
        return amplitude * ((float)(state >> 8) / 8388608.0f - 1.0f);
    }
};

bool hasNote(const Json& notes, const std::string& id) {
    for (const Json& note : notes) {
        if (note.value("id", std::string()) == id) return true;
    }
    return false;
}

} // namespace

TEST(AudioAnalysis, SideMidWidth) {
    // identical channels: no side at all, clamped
    AudioAnalysis::Signal same = twoChannels(48000);
    for (size_t i = 0; i < same.frames; i++) {
        same.samples[i] = same.samples[same.frames + i] = 0.3f * (float)std::sin((double)i * 0.05);
    }
    EXPECT_EQ(AudioAnalysis::stereo(same)["side_mid_db"], AudioAnalysis::SIDE_MID_MIN_DB);

    // independent noise: as much side as mid
    AudioAnalysis::Signal independent = twoChannels(96000);
    Noise left{1}, right{987654321};
    for (size_t i = 0; i < independent.frames; i++) {
        independent.samples[i] = left.next(0.5f);
        independent.samples[independent.frames + i] = right.next(0.5f);
    }
    EXPECT_NEAR(AudioAnalysis::stereo(independent)["side_mid_db"].get<double>(), 0.0, 0.5);

    // a side mix 24 dB under the mid (orthogonal tones over whole cycles)
    AudioAnalysis::Signal mixed = twoChannels(48000);
    const double side = std::pow(10.0, -24.0 / 20.0);
    for (size_t i = 0; i < mixed.frames; i++) {
        const double t = (double)i / 48000.0;
        const double m = 0.4 * std::sin(2.0 * kPi * 440.0 * t), s = 0.4 * side * std::sin(2.0 * kPi * 1000.0 * t);
        mixed.samples[i] = (float)(m + s);
        mixed.samples[mixed.frames + i] = (float)(m - s);
    }
    EXPECT_NEAR(AudioAnalysis::stereo(mixed)["side_mid_db"].get<double>(), -24.0, 0.2);

    // opposite channels: a silent mid under a side
    AudioAnalysis::Signal opposite = twoChannels(4800);
    for (size_t i = 0; i < opposite.frames; i++) {
        opposite.samples[i] = 0.25f * (float)std::sin((double)i * 0.1);
        opposite.samples[opposite.frames + i] = -opposite.samples[i];
    }
    EXPECT_EQ(AudioAnalysis::stereo(opposite)["side_mid_db"], AudioAnalysis::SIDE_MID_MAX_DB);

    // a mono file and a silent one have no width
    AudioAnalysis::Signal mono;
    mono.channels = 1;
    mono.sampleRate = 48000;
    mono.frames = 4800;
    mono.samples.assign(4800, 0.25f);
    EXPECT_TRUE(AudioAnalysis::stereo(mono)["side_mid_db"].is_null());
    EXPECT_TRUE(AudioAnalysis::stereo(twoChannels(4800))["side_mid_db"].is_null());

    // the note: at -18 dB and below, not at -17
    Json item = AudioAnalysis::measure(mixed, AudioMeterMode::File);
    EXPECT_TRUE(hasNote(AudioAnalysis::notes(item, "", "", SoundTypes::HEAD_SILENCE_MAX_MS), "near_mono"));
    item["stereo"]["side_mid_db"] = -18.0;
    const Json at = AudioAnalysis::notes(item, "", "", SoundTypes::HEAD_SILENCE_MAX_MS);
    EXPECT_TRUE(hasNote(at, "near_mono"));
    for (const Json& note : at) {
        if (note.value("id", std::string()) == "near_mono") {
            EXPECT_EQ(note.value("text", std::string()), "near-mono stereo: Force To Mono plays it the same, pans it correctly as a positional "
                                                         "sound and, for a WAV, halves its bytes");
        }
    }
    item["stereo"]["side_mid_db"] = -17.0;
    EXPECT_FALSE(hasNote(AudioAnalysis::notes(item, "", "", SoundTypes::HEAD_SILENCE_MAX_MS), "near_mono"));

    // the analysis cache moved to version 2: its file names say so
    EXPECT_EQ(AudioAnalysis::VERSION, 2);
    const std::string name = AudioAnalysis::cacheFile("cache", std::string(40, 'a')).filename().string();
    EXPECT_NE(name.find("-v2"), std::string::npos) << name;
}

TEST(AudioAnalysis, ForcedMonoPlaysTheMeanOfTheChannels) {
    AudioAnalysis::Signal file = twoChannels(4800);
    for (size_t i = 0; i < file.frames; i++) {
        file.samples[i] = 0.5f;
        file.samples[file.frames + i] = -0.25f;
    }
    ClipImportSettings settings;
    settings.forceMono = true;
    const AudioAnalysis::Region region = AudioAnalysis::Region::of(settings, (uint32_t)file.frames);
    EXPECT_FALSE(region.isWholeFile());
    const AudioAnalysis::Signal played = AudioAnalysis::played(file, region);
    ASSERT_EQ(played.channels, 1u);
    ASSERT_EQ(played.frames, file.frames);
    for (size_t i = 0; i < played.frames; i++) { ASSERT_FLOAT_EQ(played.samples[i], 0.125f) << i; }
}

}  // namespace AudioAnalysisTest_cpp
