// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A clip's lifetime in the SoundPool (docs/specs/S97-audio-gaps.md REQ-8 to REQ-14, AC8-AC14):
// switching keeps the old clip cached, the unused clips stay within their budget, unload, the
// load state and the load event, and fades that wait for their clip. On SoLoud's null backend,
// mixed by AudioMixer::update.

#include <gtest/gtest.h>

#include "ScriptDataTestSupport.h"

#include "Scene.h"
#include "audio/AudioMixer.h"
#include "component/AudioSourceComponent.h"
#include "io/AssetIOService.h"
#include "object/sound/Audio.h"
#include "object/sound/AudioSource.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include "soloud.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

// (a named namespace: a unity build merges the test sources)
namespace sound_pool_lifetime_test {

    namespace fs = std::filesystem;

    constexpr uint64_t MiB = 1024 * 1024;

    // a 16-bit mono 44.1 kHz WAV of `frames` frames of a sine: decoded it holds frames x 4 bytes
    void writeClip(const fs::path& path, uint32_t frames) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        out.write("RIFF", 4);
        u32(36 + frames * 2);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(1);
        u32(44100);
        u32(44100 * 2);
        u16(2);
        u16(16);
        out.write("data", 4);
        u32(frames * 2);
        for (uint32_t i = 0; i < frames; i++) {
            const double value = 0.5 * std::sin(2.0 * M_PI * 1000.0 * i / 44100.0);
            u16((uint16_t)(int16_t)std::lround(value * 32767.0));
        }
    }

    class SoundPoolLifetime : public scriptdata::ScriptDataTest {
    protected:
        Scene* scene = nullptr;
        bool wasAsync = false;

        void SetUp() override {
            ScriptDataTest::SetUp();
            // clips load in the frame they are asked for, unless a test turns async loading on
            wasAsync = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            AudioSystem::close();
            AudioSystem::setBackend(AudioBackend::Null);
            AudioSystem::configure(AudioConfig());
            AudioMixer::unload();
            SoundPool::clear();
            SoundPool::setUnusedBudget(SoundPool::defaultUnusedBudget);
            if (!AssetIOService::instance().isRunning()) AssetIOService::instance().start();
            scene = new Scene();
            Engine::setScene(scene);
        }

        void TearDown() override {
            SoundPool::holdLoads(false);
            Engine::removeScene(scene);
            delete scene;
            scene = nullptr;
            AudioSystem::close();
            AudioMixer::unload();
            SoundPool::clear();
            SoundPool::setUnusedBudget(SoundPool::defaultUnusedBudget);
            Engine::setAsyncLoading(wasAsync);
            ScriptDataTest::TearDown();
        }

        void step(double dt = 0.02) {
            AudioMixer::update(dt, dt);
            scene->getSystem<AudioSystem>()->update(dt);
            AudioSystem::pumpDeterministic(dt);
        }

        void run(double seconds) {
            for (double t = 0.0; t < seconds - 1e-9; t += 0.02) step();
        }

        // frames until `done` (a load on a worker thread): the state, not a time
        bool until(const std::function<bool()>& done) {
            for (int i = 0; i < 5000 && !done(); i++) {
                step();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return done();
        }

        AudioSource source(const std::string& file) {
            AudioSource sound(scene);
            sound.loadSound(file);
            step();
            return sound;
        }

        void clip(const std::string& name, uint32_t frames) { writeClip(root / "assets" / name, frames); }

        bool cached(const std::string& file) {
            for (const SoundPoolEntry& entry : SoundPool::getEntries()) {
                if (entry.file == file) return true;
            }
            return false;
        }

        void removeSource(AudioSource& sound) { scene->removeComponent<AudioSourceComponent>(sound.getEntity()); }
    };

}

using namespace sound_pool_lifetime_test;

// AC8 (REQ-8, REQ-9): A -> B -> A on one source decodes A once
TEST_F(SoundPoolLifetime, SwitchingBackDoesNotDecodeAgain) {
    clip("a.wav", 4410);
    clip("b.wav", 4410);
    const uint64_t before = SoundPool::getDecodeCount();
    AudioSource sound = source("a.wav");
    sound.loadSound("b.wav");
    step();
    sound.loadSound("a.wav");
    step();
    EXPECT_EQ(SoundPool::getDecodeCount() - before, 2u) << "a once, b once";
    EXPECT_EQ(sound.getLoadState(), AudioLoadState::Loaded);
    EXPECT_TRUE(cached("b.wav")) << "b stays, unused";

    // destroySound frees the clip at once
    sound.destroySound();
    EXPECT_FALSE(cached("a.wav"));
    EXPECT_EQ(sound.getLoadState(), AudioLoadState::Unloaded);
}

// AC9 (REQ-8, AU7): a clip another source holds stays, even with no cache at all
TEST_F(SoundPoolLifetime, ASharedClipStaysWhileAnotherSourceHoldsIt) {
    SoundPool::setUnusedBudget(0);
    clip("a.wav", 4410);
    clip("b.wav", 4410);
    AudioSource first = source("a.wav");
    AudioSource second = source("a.wav");
    second.play();
    step();
    first.loadSound("b.wav");
    step();
    ASSERT_TRUE(cached("a.wav"));
    const uint64_t before = SoundPool::getDecodeCount();
    second.stop();
    step();
    second.play();
    step();
    EXPECT_TRUE(second.isPlaying());
    EXPECT_EQ(SoundPool::getDecodeCount(), before) << "its next play decodes nothing";
}

// AC10 (REQ-10, REQ-37): the unused clips stay within the budget, the least recently released
// first out; a held clip and a streamed one stay; at budget 0 a release drops the clip
TEST_F(SoundPoolLifetime, UnusedClipsStayWithinTheBudget) {
    SoundPool::setUnusedBudget(1 * MiB);
    // 102400 frames: 400 KiB decoded
    for (int i = 1; i <= 5; i++) clip("c" + std::to_string(i) + ".wav", 102400);
    clip("s.wav", 4410);
    std::vector<AudioSource> sources;
    for (int i = 1; i <= 5; i++) sources.push_back(source("c" + std::to_string(i) + ".wav"));
    AudioSource streamed(scene);
    streamed.setLoadType(AudioLoadType::Streaming);
    streamed.loadSound("s.wav");
    step();
    ASSERT_EQ(SoundPool::getUnusedBytes(), 0u);

    // released in another order than they loaded: c2, c4, c1, c3; c5 stays held
    for (int index : {1, 3, 0, 2}) removeSource(sources[(size_t)index]);
    removeSource(streamed);
    EXPECT_LE(SoundPool::getUnusedBytes(), 1 * MiB);
    EXPECT_FALSE(cached("c2.wav")) << "released first";
    EXPECT_FALSE(cached("c4.wav"));
    EXPECT_TRUE(cached("c1.wav"));
    EXPECT_TRUE(cached("c3.wav"));
    EXPECT_TRUE(cached("c5.wav")) << "held";
    EXPECT_TRUE(cached("s.wav")) << "a stream counts 0 bytes";
    for (const SoundPoolEntry& entry : SoundPool::getEntries()) {
        if (entry.file == "c5.wav") {
            EXPECT_EQ(entry.holders, 1);
            EXPECT_FALSE(entry.unused);
        }
        if (entry.file == "c1.wav") {
            EXPECT_TRUE(entry.unused);
        }
    }

    // no cache: every unused clip goes, and each release drops its clip
    SoundPool::setUnusedBudget(0);
    EXPECT_FALSE(cached("c1.wav"));
    EXPECT_FALSE(cached("c3.wav"));
    EXPECT_FALSE(cached("s.wav"));
    EXPECT_TRUE(cached("c5.wav"));
    AudioSource again = source("c1.wav");
    ASSERT_TRUE(cached("c1.wav"));
    removeSource(again);
    EXPECT_FALSE(cached("c1.wav"));
}

// AC11 (REQ-11, REQ-8): unload removes a preload's pin and drops the clip; a held clip drops at
// its release instead of staying cached
TEST_F(SoundPoolLifetime, UnloadDropsPinsAndUnusedEntries) {
    clip("a.wav", 4410);
    clip("b.wav", 4410);
    ASSERT_TRUE(AudioSystem::preload("a.wav"));
    EXPECT_EQ(AudioSystem::getLoadState("a.wav"), AudioLoadState::Loaded);
    EXPECT_EQ(AudioSystem::unload("a.wav"), 1);
    EXPECT_EQ(AudioSystem::getLoadState("a.wav"), AudioLoadState::Unloaded);
    EXPECT_EQ(AudioSystem::unload("a.wav"), 0) << "nothing left";

    AudioSource sound = source("b.wav");
    EXPECT_EQ(AudioSystem::unload("b.wav"), 0) << "a source holds it";
    EXPECT_TRUE(cached("b.wav"));
    removeSource(sound);
    EXPECT_FALSE(cached("b.wav")) << "unloaded: not kept in the cache at its release";
    EXPECT_EQ(AudioSystem::getLoadState("missing.wav"), AudioLoadState::Unloaded);
}

// AC12 (REQ-8, D5): free voices' clips are bounded once they end
TEST_F(SoundPoolLifetime, FreeVoiceClipsAreBounded) {
    SoundPool::setUnusedBudget(1 * MiB);
    // 25600 frames: 100 KiB decoded each, 5 MiB in all
    for (int i = 0; i < 50; i++) clip("f" + std::to_string(i) + ".wav", 25600);
    for (int i = 0; i < 50; i++) EXPECT_TRUE(Audio::play("f" + std::to_string(i) + ".wav").isValid());
    step();
    ASSERT_TRUE(until([]() { return AudioSystem::getVoiceCount() == 0; }));
    step();
    EXPECT_LE(SoundPool::getUnusedBytes(), 1 * MiB);
    EXPECT_LE(SoundPool::getEntries().size(), 11u);
}

// AC13 (REQ-12, REQ-13): Loading right after loadSound, onLoad once in a later update, then
// Loaded with a length; a missing file: once false, Failed
TEST_F(SoundPoolLifetime, LoadStateAndOnLoadUnderAsyncLoading) {
    clip("a.wav", 44100);
    Engine::setAsyncLoading(true);
    AudioSource sound(scene);
    std::vector<bool> loads;
    sound.getComponent<AudioSourceComponent>().onLoad.add("test", std::function<void(bool)>([&loads](bool ok) { loads.push_back(ok); }));
    sound.loadSound("a.wav");
    EXPECT_EQ(sound.getLoadState(), AudioLoadState::Loading);
    EXPECT_TRUE(loads.empty()) << "never inside loadSound";
    EXPECT_EQ(sound.getLength(), 0.0);
    ASSERT_TRUE(until([&loads]() { return !loads.empty(); }));
    run(0.1);
    ASSERT_EQ(loads.size(), 1u);
    EXPECT_TRUE(loads[0]);
    EXPECT_EQ(sound.getLoadState(), AudioLoadState::Loaded);
    EXPECT_GT(sound.getLength(), 0.0);
    EXPECT_EQ(AudioSystem::getLoadState("a.wav"), AudioLoadState::Loaded);

    // a clip already loaded: still a later update
    sound.loadSound("a.wav");
    EXPECT_EQ(loads.size(), 1u);
    step();
    ASSERT_EQ(loads.size(), 2u);
    EXPECT_TRUE(loads[1]);

    AudioSource missing(scene);
    std::vector<bool> failures;
    missing.getComponent<AudioSourceComponent>().onLoad.add("test", std::function<void(bool)>([&failures](bool ok) { failures.push_back(ok); }));
    missing.loadSound("missing.wav");
    ASSERT_TRUE(until([&failures]() { return !failures.empty(); }));
    run(0.1);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_FALSE(failures[0]);
    EXPECT_EQ(missing.getLoadState(), AudioLoadState::Failed);
}

// AC14 (REQ-14): a fadeIn of a clip still loading starts from silence with its voice
TEST_F(SoundPoolLifetime, FadeInStartsWithTheVoice) {
    clip("a.wav", 44100);
    Engine::setAsyncLoading(true);
    SoundPool::holdLoads(true);
    AudioSource sound(scene);
    sound.loadSound("a.wav");
    sound.fadeIn(1.0);
    run(0.2);   // 10 frames of a slow load
    AudioSourceComponent& audio = sound.getComponent<AudioSourceComponent>();
    ASSERT_EQ(audio.handle, 0u);
    EXPECT_EQ(audio.fade.elapsed, 0.0) << "the fade waits for the voice";
    SoundPool::holdLoads(false);
    ASSERT_TRUE(until([&sound]() { return sound.getComponent<AudioSourceComponent>().handle != 0; }));
    const AudioSourceComponent& started = sound.getComponent<AudioSourceComponent>();
    // the frame the voice started: at most the fade's gain after 20 ms (one frame)
    EXPECT_LE(started.volume, 0.02 + 1e-6);
    EXPECT_LE(AudioSystem::getSoloud().getVolume(started.handle), 0.02f + 1e-6f);
}
