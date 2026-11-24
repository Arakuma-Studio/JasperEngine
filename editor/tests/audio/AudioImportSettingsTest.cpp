// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "audio/AudioAnalysis.h"
#include "audio/AudioImportSettings.h"
#include "audio/AudioProvenance.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

constexpr double importSettingsPi = 3.14159265358979323846;

AudioImportSettings parsed(const std::string& text) {
    AudioImportSettings settings;
    std::string error;
    EXPECT_TRUE(AudioImportSettings::parse(YAML::Load(text), settings, error)) << error;
    return settings;
}

std::string refused(const std::string& text) {
    AudioImportSettings settings;
    std::string error;
    EXPECT_FALSE(AudioImportSettings::parse(YAML::Load(text), settings, error)) << text;
    return error;
}

}

TEST(AudioImportSettings, RoundTripInKeyOrderKeepingUnknownKeys) {
    const AudioImportSettings settings = parsed(
        "later_key: {a: 1}\n"
        "loop_end_ms: 2000\n"
        "loop_start_ms: 1250.5\n"
        "fade_out_ms: 20\n"
        "fade_in_ms: 2\n"
        "trim_tail_ms: 0\n"
        "trim_head_ms: auto\n"
        "gain_db: -3\n"
        "type: footstep\n"
        "version: 1\n"
        "music: {first_beat_ms: 12.4, bpm: 120}\n");
    // music is a known key since S39: after loop_end_ms, its keys in their order, the unknown ones after it
    EXPECT_EQ(YAML::Dump(settings.toYaml()),
              "version: 1\n"
              "type: footstep\n"
              "gain_db: -3\n"
              "trim_head_ms: auto\n"
              "fade_in_ms: 2\n"
              "fade_out_ms: 20\n"
              "loop_start_ms: 1250.5\n"
              "loop_end_ms: 2000\n"
              "music:\n"
              "  bpm: 120\n"
              "  beats_per_bar: 4\n"
              "  first_beat_ms: 12.4\n"
              "later_key: {a: 1}");
    EXPECT_TRUE(parsed(YAML::Dump(settings.toYaml())) == settings);
}

TEST(AudioImportSettings, DefaultValuesAreNotWritten) {
    AudioImportSettings settings = parsed("version: 1\ngain_db: 0\nfade_in_ms: 0\nloop_start_ms: -1\n");
    EXPECT_TRUE(settings.isDefault());
    EXPECT_TRUE(settings.toYaml().IsNull());
    EXPECT_TRUE(settings.toJson().empty());
}

TEST(AudioImportSettings, ValidationNamesTheKey) {
    EXPECT_NE(refused("gain_db: -61\n").find("gain_db"), std::string::npos);
    EXPECT_NE(refused("gain_db: 25\n").find("gain_db"), std::string::npos);
    EXPECT_NE(refused("gain_db: loud\n").find("gain_db"), std::string::npos);
    EXPECT_NE(refused("trim_head_ms: -1\n").find("trim_head_ms"), std::string::npos);
    EXPECT_NE(refused("trim_tail_ms: soon\n").find("trim_tail_ms"), std::string::npos);
    EXPECT_NE(refused("fade_in_ms: 1001\n").find("fade_in_ms"), std::string::npos);
    EXPECT_NE(refused("fade_out_ms: -2\n").find("fade_out_ms"), std::string::npos);
    EXPECT_NE(refused("loop_start_ms: -2\n").find("loop_start_ms"), std::string::npos);
    EXPECT_NE(refused("loop_start_ms: 100\nloop_end_ms: 100\n").find("loop_end_ms"), std::string::npos);
    EXPECT_NE(refused("type: not_a_type\n").find("not_a_type"), std::string::npos);
    EXPECT_NE(refused("[1, 2]\n").find("mapping"), std::string::npos);
}

TEST(AudioImportSettings, ApplySetsResetsAndRefusesUnknownKeys) {
    AudioImportSettings settings;
    std::string error;
    ASSERT_TRUE(settings.apply({{"gain_db", -6}, {"trim_head_ms", "auto"}, {"type", "footstep"}}, error)) << error;
    EXPECT_EQ(settings.gainDb, -6.0);
    EXPECT_TRUE(settings.trimHead.automatic);
    ASSERT_TRUE(settings.apply({{"gain_db", nullptr}, {"trim_head_ms", nullptr}}, error)) << error;
    EXPECT_EQ(settings.gainDb, 0.0);
    EXPECT_FALSE(settings.trimHead.automatic);
    EXPECT_EQ(settings.type, "footstep");

    const AudioImportSettings before = settings;
    EXPECT_FALSE(settings.apply({{"gain", 1}}, error));
    EXPECT_NE(error.find("gain_db"), std::string::npos);
    EXPECT_FALSE(settings.apply({{"gain_db", 99}, {"fade_in_ms", 1}}, error));
    EXPECT_TRUE(settings == before);
}

TEST(AudioImportSettings, ResolveRoundsToFramesAndChecksTheFile) {
    AudioImportSettings settings = parsed("gain_db: -3\ntrim_head_ms: auto\ntrim_tail_ms: 10\nfade_in_ms: 2\nloop_end_ms: 500\n");
    ClipImportSettings clip;
    std::string error;
    nlohmann::json resolved;
    ASSERT_TRUE(settings.resolve(48000, 48000, 25.0, 0.0, clip, error, &resolved)) << error;
    EXPECT_EQ(clip.gainDb, -3.0f);
    EXPECT_EQ(clip.trimHeadFrames, 1200u);
    EXPECT_EQ(clip.trimTailFrames, 480u);
    EXPECT_EQ(clip.fadeInFrames, 96u);
    EXPECT_EQ(clip.loopStartFrame, -1);
    EXPECT_EQ(clip.loopEndFrame, 24000);
    EXPECT_EQ(resolved["trim_head_frames"], 1200);

    // a loop end past the trimmed end, a loop start before the trimmed start, trims that eat the file
    EXPECT_FALSE(parsed("trim_tail_ms: 600\nloop_end_ms: 500\n").resolve(48000, 48000, 0, 0, clip, error));
    EXPECT_NE(error.find("loop region"), std::string::npos);
    EXPECT_FALSE(parsed("trim_head_ms: 100\nloop_start_ms: 50\n").resolve(48000, 48000, 0, 0, clip, error));
    EXPECT_FALSE(parsed("trim_head_ms: 600\ntrim_tail_ms: 600\n").resolve(48000, 48000, 0, 0, clip, error));
    EXPECT_NE(error.find("nothing"), std::string::npos);
}

TEST(AudioImportSettings, MusicKeyIsValidatedAndResolvedToFrames) {
    AudioImportSettings settings = parsed("music: {bpm: 128.5, beats_per_bar: 3, first_beat_ms: 250}\n");
    EXPECT_EQ(settings.music.bpm, 128.5);
    EXPECT_EQ(settings.music.beatsPerBar, 3);
    EXPECT_EQ(settings.music.firstBeatMs, 250.0);
    EXPECT_FALSE(settings.isDefault());
    EXPECT_EQ(settings.toJson()["music"], nlohmann::json({{"bpm", 128.5}, {"beats_per_bar", 3}, {"first_beat_ms", 250.0}}));
    // beats_per_bar and first_beat_ms default
    EXPECT_EQ(parsed("music: {bpm: 90}\n").music.beatsPerBar, 4);

    // the tempo never changes the samples: the clip still plays as the file
    ClipImportSettings clip;
    std::string error;
    nlohmann::json resolved;
    ASSERT_TRUE(settings.resolve(48000 * 10, 44100, 0, 0, clip, error, &resolved)) << error;
    EXPECT_EQ(clip.bpm, 128.5f);
    EXPECT_EQ(clip.beatsPerBar, 3);
    EXPECT_EQ(clip.firstBeatFrame, 11025);
    EXPECT_TRUE(clip.playsAsFile());
    EXPECT_EQ(resolved["first_beat_frame"], 11025);
    // round(ms x rate / 1000)
    ASSERT_TRUE(parsed("music: {bpm: 120, first_beat_ms: 12.41}\n").resolve(48000, 48000, 0, 0, clip, error)) << error;
    EXPECT_EQ(clip.firstBeatFrame, 596);
    // without a tempo nothing is set
    ASSERT_TRUE(parsed("gain_db: -1\n").resolve(48000, 48000, 0, 0, clip, error)) << error;
    EXPECT_EQ(clip.bpm, 0.0f);
    EXPECT_EQ(clip.firstBeatFrame, 0);

    EXPECT_NE(refused("music: {bpm: 19}\n").find("music.bpm"), std::string::npos);
    EXPECT_NE(refused("music: {bpm: 401}\n").find("music.bpm"), std::string::npos);
    EXPECT_NE(refused("music: {bpm: 0}\n").find("music.bpm"), std::string::npos);
    EXPECT_NE(refused("music: {beats_per_bar: 4}\n").find("needs bpm"), std::string::npos);
    EXPECT_NE(refused("music: {bpm: 120, beats_per_bar: 17}\n").find("beats_per_bar"), std::string::npos);
    EXPECT_NE(refused("music: {bpm: 120, beats_per_bar: 3.5}\n").find("beats_per_bar"), std::string::npos);
    EXPECT_NE(refused("music: {bpm: 120, first_beat_ms: -1}\n").find("first_beat_ms"), std::string::npos);
    EXPECT_NE(refused("music: {bpm: 120, swing: 0.6}\n").find("music.swing"), std::string::npos);
    EXPECT_NE(refused("music: 120\n").find("mapping"), std::string::npos);
}

TEST(AudioImportSettings, ApplySetsAndRemovesTheMusicBlock) {
    AudioImportSettings settings;
    std::string error;
    ASSERT_TRUE(settings.apply({{"music", {{"bpm", 140}, {"beats_per_bar", 3}, {"first_beat_ms", 12.4}}}}, error)) << error;
    EXPECT_EQ(settings.music.bpm, 140.0);
    EXPECT_EQ(settings.music.beatsPerBar, 3);
    EXPECT_EQ(YAML::Dump(settings.toYaml()), "version: 1\nmusic:\n  bpm: 140\n  beats_per_bar: 3\n  first_beat_ms: 12.4");
    EXPECT_TRUE(parsed(YAML::Dump(settings.toYaml())) == settings);

    const AudioImportSettings before = settings;
    EXPECT_FALSE(settings.apply({{"music", {{"bpm", 500}}}}, error));
    EXPECT_NE(error.find("music.bpm"), std::string::npos);
    EXPECT_FALSE(settings.apply({{"music", {{"tempo", 120}}}}, error));
    EXPECT_NE(error.find("bpm, beats_per_bar, first_beat_ms"), std::string::npos);
    EXPECT_FALSE(settings.apply({{"music", 120}}, error));
    EXPECT_TRUE(settings == before);

    ASSERT_TRUE(settings.apply({{"music", nullptr}}, error)) << error;
    EXPECT_TRUE(settings.isDefault());
    EXPECT_TRUE(settings.toYaml().IsNull());
}

TEST(AudioImportSettings, ForceToMonoKey) {
    // absent when off; after loop_end_ms (before music) otherwise; a later version's key still round-trips
    EXPECT_TRUE(parsed("force_to_mono: off\n").isDefault());
    const AudioImportSettings settings = parsed("later_key: 7\nmusic: {bpm: 90}\nforce_to_mono: auto\nloop_end_ms: 2000\nversion: 1\n");
    EXPECT_EQ(settings.forceToMono, AudioImportSettings::ForceToMono::Auto);
    EXPECT_EQ(YAML::Dump(settings.toYaml()),
              "version: 1\n"
              "loop_end_ms: 2000\n"
              "force_to_mono: auto\n"
              "music:\n"
              "  bpm: 90\n"
              "  beats_per_bar: 4\n"
              "  first_beat_ms: 0\n"
              "later_key: 7");
    EXPECT_TRUE(parsed(YAML::Dump(settings.toYaml())) == settings);
    EXPECT_EQ(parsed("force_to_mono: on\n").forceToMono, AudioImportSettings::ForceToMono::On);
    EXPECT_EQ(parsed(YAML::Dump(parsed("force_to_mono: on\n").toYaml())).forceToMono, AudioImportSettings::ForceToMono::On);
    EXPECT_EQ(refused("force_to_mono: yes please\n"), "force_to_mono must be off, on or auto");
    EXPECT_EQ(refused("force_to_mono: [on]\n"), "force_to_mono must be off, on or auto");

    AudioImportSettings applied;
    std::string error;
    ASSERT_TRUE(applied.apply({{"force_to_mono", "on"}}, error)) << error;
    EXPECT_EQ(applied.toJson()["force_to_mono"], "on");
    EXPECT_FALSE(applied.sameSound(AudioImportSettings()));
    EXPECT_FALSE(applied.apply({{"force_to_mono", "stereo"}}, error));
    EXPECT_NE(error.find("force_to_mono must be off, on or auto"), std::string::npos);
    ASSERT_TRUE(applied.apply({{"force_to_mono", nullptr}}, error)) << error;
    EXPECT_TRUE(applied.isDefault());
}

namespace {

// two channels of 48000 frames: a mid tone with a side tone (orthogonal over whole cycles) `sideDb` below it
AudioAnalysis::Signal stereoSignal(double sideDb, bool invertRight = false, float dc = 0.0f) {
    AudioAnalysis::Signal signal;
    signal.channels = 2;
    signal.sampleRate = 48000;
    signal.frames = 48000;
    signal.samples.resize(signal.frames * 2);
    const double side = std::pow(10.0, sideDb / 20.0);
    for (size_t i = 0; i < signal.frames; i++) {
        const double t = (double)i / 48000.0;
        const double m = 0.4 * std::sin(2.0 * importSettingsPi * 440.0 * t);
        const double s = 0.4 * side * std::sin(2.0 * importSettingsPi * 1000.0 * t);
        signal.samples[i] = dc + (float)(m + s);
        signal.samples[signal.frames + i] = invertRight ? dc - (float)(m + s) : (float)(m - s) + dc;
    }
    return signal;
}

AudioImportSettings::Channels channelsOf(const AudioAnalysis::Signal& signal) {
    return AudioImportSettings::Channels::fromAnalysis({{"format", {{"channels", signal.channels}}}, {"stereo", AudioAnalysis::stereo(signal)}});
}

} // namespace

TEST(AudioImportSettings, AutoResolvesFromWidth) {
    AudioImportSettings settings;
    settings.forceToMono = AudioImportSettings::ForceToMono::Auto;
    ClipImportSettings clip;
    nlohmann::json resolved = nlohmann::json::object();

    const AudioImportSettings::Channels nearMono = channelsOf(stereoSignal(-24.0));
    ASSERT_TRUE(nearMono.sideMidDb.has_value());
    EXPECT_NEAR(*nearMono.sideMidDb, -24.0, 0.2);
    settings.resolveChannels(nearMono, clip, &resolved);
    EXPECT_TRUE(clip.forceMono);
    EXPECT_EQ(resolved["force_mono"], true);
    EXPECT_EQ(resolved["channels"], 1);
    EXPECT_NE(nearMono.autoHint().find("Mono (width -24.0 dB)"), std::string::npos) << nearMono.autoHint();

    const AudioImportSettings::Channels wide = channelsOf(stereoSignal(-9.0));
    settings.resolveChannels(wide, clip, &resolved);
    EXPECT_FALSE(clip.forceMono);
    EXPECT_EQ(resolved["channels"], 2);
    EXPECT_NE(wide.autoHint().find("Stereo (width -9.0 dB: a wide image)"), std::string::npos) << wide.autoHint();

    // phase-inverted channels: their side is all there is
    settings.resolveChannels(channelsOf(stereoSignal(-90.0, true)), clip);
    EXPECT_FALSE(clip.forceMono);
    // quiet inverted content riding a shared offset: a narrow width (the offset is all mid), but the
    // correlation says the content cancels in mono
    AudioAnalysis::Signal offset;
    offset.channels = 2;
    offset.sampleRate = 48000;
    offset.frames = 48000;
    offset.samples.resize(offset.frames * 2);
    for (size_t i = 0; i < offset.frames; i++) {
        const float x = 0.02f * (float)std::sin(2.0 * importSettingsPi * 440.0 * (double)i / 48000.0);
        offset.samples[i] = 0.5f + x;
        offset.samples[offset.frames + i] = 0.5f - x;
    }
    const AudioImportSettings::Channels cancels = channelsOf(offset);
    ASSERT_TRUE(cancels.sideMidDb.has_value() && cancels.correlation.has_value());
    EXPECT_LE(*cancels.sideMidDb, -18.0);
    EXPECT_LT(*cancels.correlation, -0.3);
    settings.resolveChannels(cancels, clip);
    EXPECT_FALSE(clip.forceMono);
    EXPECT_NE(cancels.autoHint().find("cancels in mono"), std::string::npos) << cancels.autoHint();

    // On folds any stereo file; nothing folds a mono one
    settings.forceToMono = AudioImportSettings::ForceToMono::On;
    settings.resolveChannels(wide, clip);
    EXPECT_TRUE(clip.forceMono);
    AudioImportSettings::Channels mono;
    mono.channels = 1;
    settings.resolveChannels(mono, clip, &resolved);
    EXPECT_FALSE(clip.forceMono);
    EXPECT_EQ(resolved["channels"], 1);
    settings.forceToMono = AudioImportSettings::ForceToMono::Off;
    settings.resolveChannels(nearMono, clip);
    EXPECT_FALSE(clip.forceMono);
}

TEST(AudioProvenance, RoundTripAndApply) {
    AudioProvenance provenance;
    std::string error;
    ASSERT_TRUE(AudioProvenance::parse(YAML::Load("version: 1\nsource: kenney\nlicence: CC0-1.0\nfuture: 3\n"
                                                  "builtin: {set: essentials, id: ui/tick, sha256: abc}\n"), provenance, error));
    EXPECT_EQ(provenance.licence(), "CC0-1.0");
    EXPECT_EQ(YAML::Dump(provenance.toYaml()), "version: 1\nlicence: CC0-1.0\nsource: kenney\nbuiltin:\n  id: ui/tick\n  set: essentials\n  sha256: abc\nfuture: 3");

    ASSERT_TRUE(provenance.apply({{"author", "Someone"}, {"source", nullptr}}, error)) << error;
    EXPECT_EQ(provenance.text("author"), "Someone");
    EXPECT_EQ(provenance.text("source"), "");
    EXPECT_FALSE(provenance.apply({{"licensee", "x"}}, error));
    EXPECT_NE(error.find("licence"), std::string::npos);
    EXPECT_FALSE(provenance.apply({{"builtin", "x"}}, error));
    EXPECT_NE(error.find("object"), std::string::npos);
}
