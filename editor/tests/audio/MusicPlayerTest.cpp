// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The music system (docs/specs/S39-music.md R1, R4, R6, R8-R14) on the deterministic driver, frames
// as Engine::runFrame makes them (the scenes, then MusicPlayer::update, then the mix): what refuses a
// track, its voice and its survival of a scene's stop, quantized switches and stingers on their
// boundary, the crossfade's curves, the beat and section callbacks in the frame whose mix holds them.

#include <gtest/gtest.h>

#include "../ScriptDataTestSupport.h"
#include "support/EditorTestSupport.h"

#include "Scene.h"
#include "audio/AudioImport.h"
#include "audio/AudioMixer.h"
#include "audio/MusicPlayer.h"
#include "audio/SoundEventLibrary.h"
#include "object/sound/Audio.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include "soloud.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    constexpr int RATE = 48000;
    constexpr double DT = 0.02;
    constexpr int FRAME = 960;

    void writeTone(const fs::path& path, double seconds, int rate, int channels = 1) {
        fs::create_directories(path.parent_path());
        const uint32_t frames = (uint32_t)std::lround(seconds * rate);
        std::ofstream out(path, std::ios::binary);
        auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        out.write("RIFF", 4);
        u32(36 + frames * 2 * channels);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16((uint16_t)channels);
        u32((uint32_t)rate);
        u32((uint32_t)(rate * 2 * channels));
        u16((uint16_t)(2 * channels));
        u16(16);
        out.write("data", 4);
        u32(frames * 2 * channels);
        for (uint32_t i = 0; i < frames; i++) {
            for (int c = 0; c < channels; c++) u16((uint16_t)(int16_t)std::lround(0.3 * std::sin(2.0 * M_PI * 440.0 * i / rate) * 32767.0));
        }
    }

    class MusicPlayerTest : public scriptdata::ScriptDataTest {
    protected:
        Scene* scene = nullptr;
        bool wasAsync = false;
        std::vector<AudioVoiceLine> lines;

        void SetUp() override {
            ScriptDataTest::SetUp();
            wasAsync = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            AudioSystem::close();
            AudioSystem::setBackend(AudioBackend::Deterministic);
            AudioSystem::configure(AudioConfig());
            AudioMixer::unload();
            SoundPool::clear();
            SoundEventLibrary::clear();
            AudioImportTable::clear();
            AudioSystem::resetSoundEventRuntime();
            MusicPlayer::reset();
            writeTone(root / "assets" / "a.wav", 8.0, RATE);
            writeTone(root / "assets" / "b.wav", 8.0, RATE);
            writeTone(root / "assets" / "c.wav", 1.0, RATE);
            writeTone(root / "assets" / "slow.wav", 2.0, 44100);
            writeTone(root / "assets" / "hit.wav", 0.3, RATE);
            ClipImportSettings tempo;
            tempo.bpm = 120.0f;
            AudioImportTable::set("a.wav", tempo);
            AudioImportTable::set("b.wav", tempo);
            scene = new Scene();
            Engine::setScene(scene);
            AudioSystem::setVoiceObserver([this](const AudioVoiceLine& line) { lines.push_back(line); });
        }

        void TearDown() override {
            MusicPlayer::reset();
            AudioSystem::setVoiceObserver(nullptr);
            Engine::removeScene(scene);
            delete scene;
            scene = nullptr;
            AudioSystem::setBackend(AudioBackend::Auto);
            AudioSystem::close();
            AudioMixer::unload();
            AudioImportTable::clear();
            SoundPool::clear();
            SoundEventLibrary::clear();
            AudioSystem::resetSoundEventRuntime();
            Engine::setAsyncLoading(wasAsync);
            ScriptDataTest::TearDown();
        }

        // one engine frame's audio, in Engine::runFrame's order
        void frame(double dt = DT) {
            AudioMixer::update(dt, dt);
            scene->getSystem<AudioSystem>()->update(dt);
            MusicPlayer::update(dt);
            AudioSystem::pumpDeterministic(dt);
        }

        void run(int frames) {
            for (int i = 0; i < frames; i++) frame();
        }

        std::vector<AudioVoiceLine> starts(const char* owner) const {
            std::vector<AudioVoiceLine> out;
            for (const AudioVoiceLine& line : lines) {
                if (line.kind == AudioVoiceLine::Kind::Start && std::string(line.owner) == owner) out.push_back(line);
            }
            return out;
        }
    };

}

TEST_F(MusicPlayerTest, RefusesALayerEventAndSectionsOfAnotherRate) {
    ASSERT_TRUE(SoundEventLibrary::load("layer.soundevent", R"({"format": "jasperengine.soundevent", "version": 1, "mode": "layer",
        "clips": [{"clip": {"path": "a.wav"}}, {"clip": {"path": "b.wav"}}]})"));
    EXPECT_FALSE(Audio::playMusic("layer.soundevent"));
    ASSERT_TRUE(SoundEventLibrary::load("mixed.soundevent", R"({"format": "jasperengine.soundevent", "version": 1, "mode": "sequence",
        "clips": [{"clip": {"path": "a.wav"}}, {"clip": {"path": "slow.wav"}}]})"));
    EXPECT_FALSE(Audio::playMusic("mixed.soundevent"));
    EXPECT_TRUE(starts("music").empty());
    EXPECT_EQ(Audio::getMusic(), "");
}

// R4, R6: one protected voice of priority 0, owner and cause music; a scene's stop does not touch it
TEST_F(MusicPlayerTest, AClipTrackIsAProtectedVoiceThatOutlivesTheScenesSounds) {
    ASSERT_TRUE(Audio::playMusic("a.wav"));
    run(5);
    const std::vector<AudioVoiceLine> music = starts("music");
    ASSERT_EQ(music.size(), 1u);
    EXPECT_STREQ(music[0].cause, "music");
    EXPECT_EQ(music[0].priority, 0);
    EXPECT_EQ(Audio::getMusic(), "a.wav");
    EXPECT_TRUE(Audio::playMusic("a.wav")) << "the playing track again does nothing";
    EXPECT_EQ(starts("music").size(), 1u);

    scene->getSystem<AudioSystem>()->stopSceneSounds();
    run(2);
    EXPECT_EQ(Audio::getMusic(), "a.wav");
    EXPECT_NEAR(Audio::getMusicTime(), 7.0 * FRAME / RATE, 1e-6);

    MusicPlayer::reset();
    bool stoppedForScene = false;
    for (const AudioVoiceLine& line : lines) {
        if (line.kind == AudioVoiceLine::Kind::Stop && line.handle == music[0].handle) stoppedForScene = std::string(line.reason) == "scene";
    }
    EXPECT_TRUE(stoppedForScene);
    EXPECT_EQ(Audio::getMusic(), "");
}

// R1, R2: a playlist in order, each section at the frame after the last; a clip track ends once advanced
TEST_F(MusicPlayerTest, APlaylistHandsOverAndAnAdvancedClipTrackEnds) {
    ASSERT_TRUE(SoundEventLibrary::load("list.soundevent", R"({"format": "jasperengine.soundevent", "version": 1, "mode": "sequence", "type": "music",
        "clips": [{"clip": {"path": "c.wav"}}, {"clip": {"path": "hit.wav"}}]})"));
    std::vector<std::pair<int, std::string>> sections;
    Audio::onMusicSection.add("test", [&](int section, const std::string& clip) { sections.emplace_back(section, clip); });
    ASSERT_TRUE(Audio::playMusic("list.soundevent"));
    run(80);
    ASSERT_EQ(sections.size(), 2u);
    EXPECT_EQ(sections[0], std::make_pair(1, std::string("c.wav")));
    EXPECT_EQ(sections[1], std::make_pair(2, std::string("hit.wav")));
    EXPECT_EQ(Audio::getMusic(), "") << "a playlist without loop ends after its last section";

    ASSERT_TRUE(Audio::playMusic("c.wav"));
    run(120);
    EXPECT_EQ(Audio::getMusic(), "c.wav") << "a clip loops until advanced";
    EXPECT_TRUE(Audio::advanceMusic());
    run(60);
    EXPECT_EQ(Audio::getMusic(), "");
    EXPECT_FALSE(Audio::advanceMusic());
}

// R9, R10: sync bar starts the incoming voice delayed to the bar; R12: a beat stinger to the beat
TEST_F(MusicPlayerTest, SwitchesAndStingersWaitForTheirBoundary) {
    ASSERT_TRUE(Audio::playMusic("a.wav"));
    run(25);   // 24000 frames: beat 1 of bar 1 (24000 frames a beat at 120 bpm)
    MusicOptions options;
    options.sync = "bar";
    ASSERT_TRUE(Audio::playMusic("b.wav", options));
    const std::vector<AudioVoiceLine> music = starts("music");
    ASSERT_EQ(music.size(), 2u);
    EXPECT_EQ(music[1].delayFrames, 96000u - 24000u);

    frame(0.01);   // 480 more
    const SoundHandle stinger = Audio::playStinger("hit.wav", "beat");
    EXPECT_TRUE(stinger.isValid());
    const std::vector<AudioVoiceLine> stingers = starts("music");
    ASSERT_EQ(stingers.size(), 3u);
    // the grid is the incoming track's, which has not started: its first beat is at its start
    EXPECT_EQ(stingers[2].delayFrames, 96000u - 24480u);
    EXPECT_EQ(stingers[2].clip, "hit.wav");
}

// R10: an immediate crossfade follows cos and sin over unscaled time; a third switch keeps two voices
TEST_F(MusicPlayerTest, CrossfadeFollowsEqualPowerCurvesAndNeverKeepsAThirdVoice) {
    ASSERT_TRUE(Audio::playMusic("a.wav"));
    run(5);
    MusicOptions options;
    options.crossfade = 2.0f;
    ASSERT_TRUE(Audio::playMusic("b.wav", options));
    const std::vector<AudioVoiceLine> music = starts("music");
    ASSERT_EQ(music.size(), 2u);
    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    for (int i = 1; i <= 50; i++) {
        frame();
        const double t = i * DT / 2.0;
        EXPECT_NEAR(soloud.getVolume(music[0].handle), std::cos(M_PI / 2.0 * t), 1e-4) << i;
        EXPECT_NEAR(soloud.getVolume(music[1].handle), std::sin(M_PI / 2.0 * t), 1e-4) << i;
    }
    // halfway: the incoming track becomes the outgoing one from where it is
    ASSERT_TRUE(Audio::playMusic("c.wav", options));
    frame();
    EXPECT_NEAR(soloud.getVolume(music[1].handle), std::sin(M_PI / 4.0) * std::cos(M_PI / 2.0 * DT / 2.0), 1e-4);
    EXPECT_FALSE(AudioSystem::isVoicePlaying(music[0].handle)) << "the oldest stops at once";
    EXPECT_TRUE(AudioSystem::isVoicePlaying(music[1].handle));
    EXPECT_EQ(Audio::getMusic(), "c.wav");
    run(110);
    EXPECT_FALSE(AudioSystem::isVoicePlaying(music[1].handle)) << "stopped at the crossfade's end";
}

// R13, R14: 8 s at 120 bpm, 4/4: 16 beats in order, bars 1-4, each in the frame whose mix holds it
TEST_F(MusicPlayerTest, BeatsAreCalledBackInTheFrameThatMixesThem) {
    struct Beat {
        int bar, beat;
        uint64_t frameStart;
    };
    std::vector<Beat> beats;
    Audio::onMusicBeat.add("test", [&](int bar, int beat) { beats.push_back({bar, beat, AudioSystem::getMixedFrames()}); });
    ASSERT_TRUE(Audio::playMusic("a.wav"));
    const uint64_t start = AudioSystem::getMixedFrames();
    run(400);
    ASSERT_EQ(beats.size(), 16u);
    for (int k = 0; k < 16; k++) {
        EXPECT_EQ(beats[k].bar, k / 4 + 1);
        EXPECT_EQ(beats[k].beat, k % 4 + 1);
        const uint64_t sample = start + (uint64_t)k * 24000;
        EXPECT_LE(beats[k].frameStart, sample) << k;
        EXPECT_LT(sample, beats[k].frameStart + FRAME) << k;
    }
}

TEST_F(MusicPlayerTest, PositionAgreesWithTheGrid) {
    EXPECT_FALSE(Audio::getMusicPosition().has_value());
    ASSERT_TRUE(Audio::playMusic("a.wav"));
    run(75);   // 72000 frames: beat 4 of bar 1
    std::optional<MusicPosition> position = Audio::getMusicPosition();
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->section, 1);
    EXPECT_EQ(position->bar, 1);
    EXPECT_EQ(position->beat, 4);
    EXPECT_NEAR(position->beatFraction, 0.0, 1e-9);
    EXPECT_EQ(position->bpm, 120.0);
    EXPECT_EQ(position->beatsPerBar, 4);
    run(12);   // 11520 more: 0.48 into the beat
    position = Audio::getMusicPosition();
    ASSERT_TRUE(position.has_value());
    EXPECT_NEAR(position->beatFraction, 0.48, 1e-6);
    ASSERT_TRUE(Audio::playMusic("c.wav"));
    EXPECT_FALSE(Audio::getMusicPosition().has_value()) << "no tempo: no position";
}

TEST_F(MusicPlayerTest, RefusedArgumentsDoNothing) {
    MusicOptions options;
    options.sync = "downbeat";
    EXPECT_FALSE(Audio::playMusic("a.wav", options));
    options = MusicOptions();
    options.crossfade = -1.0f;
    EXPECT_FALSE(Audio::playMusic("a.wav", options));
    options.crossfade = NAN;
    EXPECT_FALSE(Audio::playMusic("a.wav", options));
    EXPECT_FALSE(Audio::playStinger("hit.wav", "section_end").isValid());
    EXPECT_TRUE(starts("music").empty());
}

// R22: MusicPlayer::update <= 20 us a frame, with a crossfade running and a beat callback subscribed
TEST_F(MusicPlayerTest, UpdateCostsAtMostTwentyMicrosecondsAFrame) {
    int beats = 0;
    Audio::onMusicBeat.add("bench", [&](int, int) { beats++; });
    ASSERT_TRUE(Audio::playMusic("a.wav"));
    run(10);
    MusicOptions options;
    options.crossfade = 4.0f;
    ASSERT_TRUE(Audio::playMusic("b.wav", options));
    double total = 0.0;
    const int frames = 180;
    for (int i = 0; i < frames; i++) {
        AudioMixer::update(DT, DT);
        scene->getSystem<AudioSystem>()->update(DT);
        const auto start = std::chrono::steady_clock::now();
        MusicPlayer::update(DT);
        total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        AudioSystem::pumpDeterministic(DT);
    }
    EXPECT_GT(beats, 0);
    const double perFrame = total / frames * 1e6;
    std::printf("[bench] MusicPlayer::update %.2f us a frame (crossfading, beat callbacks)\n", perFrame);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LE(perFrame, 20.0 * slack);
    }
}
