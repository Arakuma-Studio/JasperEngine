// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The deterministic audio driver (docs/specs/S35-agent-audio.md R1-R9; AC1, AC2): what a frame mixes,
// where a voice lands in it, the mixer through the one init path, the holds, synchronous loads and
// the tap's order. Every frame here is Engine::runFrame's order: the mixer, the scene, the pump.

#include <gtest/gtest.h>

#include "../ScriptDataTestSupport.h"

#include "PlatformConfig.h"
#include "Scene.h"
#include "audio/AudioMixer.h"
#include "audio/AudioTap.h"
#include "object/sound/AudioSource.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include "soloud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    void writeMonoWav(const fs::path& path, int rate, uint32_t samples, const std::function<double(uint32_t)>& value) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        out.write("RIFF", 4);
        u32(36 + samples * 2);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(1);
        u32((uint32_t)rate);
        u32((uint32_t)rate * 2);
        u16(2);
        u16(16);
        out.write("data", 4);
        u32(samples * 2);
        for (uint32_t i = 0; i < samples; i++) u16((uint16_t)(int16_t)std::lround(value(i) * 32767.0));
    }

    const char* const lowPassMixer = R"({
  "format": "jasperengine.mixer", "version": 1, "updateMode": "scaled",
  "groups": [
    {"id": "00000001", "name": "Master", "parent": null, "volume": 0},
    {"id": "9b51c0de", "name": "Music", "parent": "00000001", "volume": 0, "effects": [
       {"id": "c4f00d01", "type": "lowpass", "params": {"cutoff": 200, "resonance": 1}}]}
  ]
})";

    class DeterministicDriverTest : public scriptdata::ScriptDataTest {
    protected:
        Scene* scene = nullptr;
        bool wasAsync = false;
        std::vector<float> output;
        int tap = 0;

        void SetUp() override {
            ScriptDataTest::SetUp();
            wasAsync = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            AudioSystem::close();
            AudioSystem::setBackend(AudioBackend::Deterministic);
            AudioSystem::configure(AudioConfig());
            AudioMixer::unload();
            SoundPool::clear();
            scene = new Scene();
            Engine::setScene(scene);
            tap = AudioTap::subscribe("test", [this](const float* samples, uint32_t frames, uint64_t) {
                output.insert(output.end(), samples, samples + (size_t)frames * 2);
            });
        }

        void TearDown() override {
            AudioTap::unsubscribe(tap);
            Engine::removeScene(scene);
            delete scene;
            scene = nullptr;
            AudioSystem::setBackend(AudioBackend::Auto);
            platformConfig().silentAudio = false;
            AudioSystem::close();
            AudioMixer::unload();
            SoundPool::clear();
            Engine::setAsyncLoading(wasAsync);
            ScriptDataTest::TearDown();
        }

        void frame(double dt = 1.0 / 60.0) {
            AudioMixer::update(dt, dt);
            scene->getSystem<AudioSystem>()->update(dt);
            AudioSystem::pumpDeterministic(dt);
        }

        AudioSource source(const std::string& file, const std::string& group = "") {
            AudioSource sound(scene);
            sound.loadSound(file);
            if (!group.empty()) sound.setOutputGroup(group);
            return sound;
        }

        void writeClick(const std::string& file, int rate) {
            writeMonoWav(root / "assets" / file, rate, (uint32_t)rate / 10, [](uint32_t i) { return i == 0 ? 0.9 : 0.0; });
        }

        void writeSine(const std::string& file, double frequency, double seconds = 1.0) {
            writeMonoWav(root / "assets" / file, 48000, (uint32_t)(48000 * seconds),
                         [frequency](uint32_t i) { return 0.5 * std::sin(2.0 * M_PI * frequency * i / 48000.0); });
        }

        int firstNonZero() const {
            for (size_t i = 0; i < output.size(); i++) {
                if (output[i] != 0.0f) return (int)(i / 2);
            }
            return -1;
        }
    };

}

TEST_F(DeterministicDriverTest, OpensAt48kStereoThroughTheOneInitPath) {
    ASSERT_TRUE(AudioSystem::init());
    EXPECT_EQ(AudioSystem::getOutputMode(), AudioOutputMode::Deterministic);
    EXPECT_EQ(AudioSystem::getSoloud().getBackendSamplerate(), 48000u);
    EXPECT_EQ(AudioSystem::getSoloud().getBackendChannels(), 2u);
    EXPECT_EQ(AudioSystem::getSoloud().getBackendId(), (unsigned int)SoLoud::Soloud::NULLDRIVER);
    EXPECT_FALSE(AudioSystem::isOutputAvailable());
}

TEST_F(DeterministicDriverTest, SixtyFpsMixes800FramesAFrameInChunksOfAtMost512) {
    ASSERT_TRUE(AudioSystem::init());
    std::vector<uint32_t> chunks;
    std::vector<uint64_t> firsts;
    const int counting = AudioTap::subscribe("chunks", [&](const float*, uint32_t frames, uint64_t first) {
        chunks.push_back(frames);
        firsts.push_back(first);
    });
    frame();
    EXPECT_EQ(chunks, (std::vector<uint32_t>{512, 288}));
    EXPECT_EQ(firsts, (std::vector<uint64_t>{0, 512}));
    frame();
    AudioTap::unsubscribe(counting);
    EXPECT_EQ(AudioSystem::getMixedFrames(), 1600u);
    EXPECT_EQ(output.size(), 1600u * 2);
}

TEST_F(DeterministicDriverTest, ZeroDeltaMixesNothing) {
    ASSERT_TRUE(AudioSystem::init());
    frame(0.0);
    EXPECT_EQ(AudioSystem::getMixedFrames(), 0u);
    EXPECT_TRUE(output.empty());
}

TEST_F(DeterministicDriverTest, TheNullFallbackPumpDoesNotMixIt) {
    ASSERT_TRUE(AudioSystem::init());
    AudioSystem::mixManually(1.0);
    AudioMixer::update(1.0, 1.0);
    EXPECT_EQ(AudioSystem::getMixedFrames(), 0u) << "one pump, after the scenes";
}

TEST_F(DeterministicDriverTest, StartIsInItsFrame) {
    writeClick("click.wav", 48000);
    AudioSource click = source("click.wav");
    for (int k = 0; k < 5; k++) frame();
    const size_t before = output.size() / 2;
    EXPECT_EQ(firstNonZero(), -1);
    click.play();
    frame();
    ASSERT_EQ(output.size() / 2, before + 800);
    const int at = firstNonZero();
    ASSERT_GE(at, (int)before) << "nothing before the frame that started it";
    EXPECT_EQ((unsigned int)at - before, AudioSystem::getDeterministicLatency()) << "in that frame, at the constant latency";
}

TEST_F(DeterministicDriverTest, LatencyIsTheSameForEveryClipRateAndFrame) {
    for (int rate : {48000, 44100}) {
        const std::string file = "click" + std::to_string(rate) + ".wav";
        writeClick(file, rate);
        AudioSource click = source(file);
        for (int k = 0; k < 7; k++) {
            frame();
            output.clear();
            click.stop();
            frame();
            output.clear();
            click.play();
            frame(1.0 / 60.0 + k * 0.001);
            EXPECT_EQ((unsigned int)firstNonZero(), AudioSystem::getDeterministicLatency()) << "clip at " << rate << " Hz, frame " << k;
        }
    }
}

// R8 said one constant: it is measured here to be one sample for the voice and one for each bus
TEST_F(DeterministicDriverTest, LatencyIsOneSampleForEachBusOnTheWay) {
    const char* const nested = R"({
  "format": "jasperengine.mixer", "version": 1, "updateMode": "scaled",
  "groups": [
    {"id": "00000001", "name": "Master", "parent": null, "volume": 0},
    {"id": "9b51c0de", "name": "Music", "parent": "00000001", "volume": 0},
    {"id": "9b51c0df", "name": "Score", "parent": "9b51c0de", "volume": 0}
  ]
})";
    std::string error;
    ASSERT_TRUE(AudioMixer::loadFromText(nested, "test.mixer", &error)) << error;
    writeClick("click.wav", 48000);
    const std::vector<std::pair<const char*, unsigned int>> groups = {{"Master", 2}, {"Music", 3}, {"Score", 4}};
    for (const auto& [group, measured] : groups) {
        AudioSource click = source("click.wav", group);
        frame();
        output.clear();
        click.play();
        frame();
        EXPECT_EQ((unsigned int)firstNonZero(), measured) << "through " << group;
        EXPECT_EQ(AudioSystem::getDeterministicLatency(AudioMixer::findGroupId(group)), measured) << "through " << group;
        click.stop();
        frame();
        output.clear();
    }
}

// S62 pinned a movie's bytes when the Recorder mixed a game frame in one call: chunks of 512 differ from
// that only while SoLoud ramps a voice in over its first mix call (soloud.cpp, pani), 1,600 frames there
TEST_F(DeterministicDriverTest, ChunksDifferFromOneCallOnlyInTheFirstRamp) {
    writeMonoWav(root / "assets" / "tone.wav", 44100, 44100, [](uint32_t i) { return 0.5 * std::sin(2.0 * M_PI * 1000.0 * i / 44100.0); });
    AudioSource tone = source("tone.wav");
    tone.setLooping(true);

    tone.play();
    std::vector<float> oneCall;
    for (int k = 0; k < 30; k++) {
        AudioMixer::update(1.0 / 30.0, 1.0 / 30.0);
        scene->getSystem<AudioSystem>()->update(1.0 / 30.0);
        std::vector<float> block(1600 * 2);
        AudioSystem::getSoloud().mix(block.data(), 1600);
        oneCall.insert(oneCall.end(), block.begin(), block.end());
    }
    tone.stop();
    frame(0.0);
    AudioSystem::close();
    output.clear();

    tone.play();
    for (int k = 0; k < 30; k++) frame(1.0 / 30.0);
    ASSERT_EQ(output.size(), oneCall.size());

    size_t firstDifferent = oneCall.size(), lastDifferent = 0;
    float largest = 0.0f;
    for (size_t i = 0; i < oneCall.size(); i++) {
        const float difference = std::fabs(output[i] - oneCall[i]);
        if (difference > 0.0f) {
            firstDifferent = std::min(firstDifferent, i);
            lastDifferent = i;
            largest = std::max(largest, difference);
        }
    }
    std::printf("[chunks] first different frame %zu, last %zu, largest difference %g\n", firstDifferent / 2, lastDifferent / 2, largest);
    EXPECT_LT(lastDifferent / 2, 1600u) << "the first call's ramp only";
}

TEST_F(DeterministicDriverTest, MixerIsBuilt) {
    std::string error;
    ASSERT_TRUE(AudioMixer::loadFromText(lowPassMixer, "test.mixer", &error)) << error;
    writeSine("tone.wav", 2000.0, 2.0);
    AudioSource tone = source("tone.wav", "Music");
    tone.setLooping(true);
    tone.play();
    for (int k = 0; k < 20; k++) frame();
    const float filtered = AudioMixer::getMeterDb("Music");
    ASSERT_TRUE(AudioMixer::setGroupBypassEffects("Music", true));
    for (int k = 0; k < 20; k++) frame();
    const float bypassed = AudioMixer::getMeterDb("Music");
    EXPECT_GT(bypassed, -20.0f) << "the tone reaches the group";
    EXPECT_GT(bypassed - filtered, 12.0f) << "its low-pass is in the mix: filtered " << filtered << ", bypassed " << bypassed;
}

TEST_F(DeterministicDriverTest, ClockedVoicesStartAtTheFramesFirstSample) {
    writeClick("click.wav", 48000);
    AudioSource click = source("click.wav");
    click.setClockedSound(true);
    frame();
    output.clear();
    click.play();
    frame();
    EXPECT_EQ((unsigned int)firstNonZero(), AudioSystem::getDeterministicLatency());
}

TEST_F(DeterministicDriverTest, LoadsAreSynchronousWhileAsyncLoadingIsOn) {
    Engine::setAsyncLoading(true);
    writeClick("click.wav", 48000);
    const SoundLoadResult result = SoundPool::loadClip("click.wav", AudioLoadType::DecompressOnLoad);
    EXPECT_EQ(result.state, ResourceLoadState::Finished) << "no frame waits for it";
    EXPECT_TRUE(result.data);
    Engine::setAsyncLoading(false);
}

TEST_F(DeterministicDriverTest, AStartThatWaitedForItsClipSaysSo) {
    writeClick("click.wav", 48000);
    AudioSource sound = source("click.wav");
    std::vector<AudioVoiceLine> lines;
    AudioSystem::setVoiceObserver([&lines](const AudioVoiceLine& line) { lines.push_back(line); });
    // what a one-shot holds when an earlier frame found its clip still loading
    AudioPendingOneShot waited;
    waited.filename = "click.wav";
    waited.waited = 0.01;
    sound.getComponent<AudioSourceComponent>().pendingOneShots.push_back(waited);
    frame();
    AudioSystem::setVoiceObserver(nullptr);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].loadDelay, 480u) << "the frames it waited, in the line that makes the session not deterministic";
}

TEST_F(DeterministicDriverTest, SilentAudioOpensItWhenNothingIsSelected) {
    AudioSystem::close();
    AudioSystem::setBackend(AudioBackend::Auto);
    platformConfig().silentAudio = true;
    EXPECT_EQ(AudioSystem::getOutputMode(), AudioOutputMode::Deterministic) << "before it opens too";
    ASSERT_TRUE(AudioSystem::init());
    EXPECT_EQ(AudioSystem::getOutputMode(), AudioOutputMode::Deterministic);
}

TEST_F(DeterministicDriverTest, HoldsReopenWhatWasSelected) {
    AudioSystem::close();
    AudioSystem::setBackend(AudioBackend::Null);
    ASSERT_TRUE(AudioSystem::init());
    EXPECT_EQ(AudioSystem::getOutputMode(), AudioOutputMode::NoOutput);

    ASSERT_TRUE(AudioSystem::beginDeterministic());
    EXPECT_EQ(AudioSystem::getOutputMode(), AudioOutputMode::Deterministic);
    EXPECT_EQ(AudioSystem::getBackend(), AudioBackend::Deterministic);
    ASSERT_TRUE(AudioSystem::beginDeterministic()) << "a movie and a capture in one Play";
    AudioSystem::endDeterministic();
    EXPECT_EQ(AudioSystem::getOutputMode(), AudioOutputMode::Deterministic) << "the other still holds it";

    AudioSystem::endDeterministic();
    EXPECT_EQ(AudioSystem::getBackend(), AudioBackend::Null);
    EXPECT_FALSE(AudioSystem::isInited()) << "the next sound opens it";
    ASSERT_TRUE(AudioSystem::init());
    EXPECT_EQ(AudioSystem::getOutputMode(), AudioOutputMode::NoOutput);
}

TEST_F(DeterministicDriverTest, AHoldOfAnOpenDriverDoesNotReopenIt) {
    ASSERT_TRUE(AudioSystem::init());
    frame();
    ASSERT_TRUE(AudioSystem::beginDeterministic());
    EXPECT_EQ(AudioSystem::getMixedFrames(), 800u) << "voices keep playing: nothing re-initialised";
    AudioSystem::endDeterministic();
    EXPECT_TRUE(AudioSystem::isInited());
    EXPECT_EQ(AudioSystem::getMixedFrames(), 800u);
}

TEST_F(DeterministicDriverTest, TapClientsRunInSubscriptionOrder) {
    ASSERT_TRUE(AudioSystem::init());
    std::string order;
    const int a = AudioTap::subscribe("a", [&](const float*, uint32_t, uint64_t) { order += 'a'; });
    const int b = AudioTap::subscribe("b", [&](const float*, uint32_t, uint64_t) { order += 'b'; });
    frame(256.0 / 48000.0);
    EXPECT_EQ(order, "ab");
    AudioTap::unsubscribe(a);
    frame(256.0 / 48000.0);
    EXPECT_EQ(order, "abb");
    AudioTap::unsubscribe(b);
}
