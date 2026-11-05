// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sound Events' budgets (docs/specs/S37-sound-events.md R22; AC12), on the null audio driver:
// startEvent <= 15 us a voice above startVoice, no file access after the first load, and 64 trigger
// plays in a frame <= 1 ms. Wall-clock budgets scale by perfSlack() on a loaded machine.

#include <gtest/gtest.h>

#include "../ScriptDataTestSupport.h"
#include "support/EditorTestSupport.h"

#include "Scene.h"
#include "audio/AudioMixer.h"
#include "audio/SoundEventIO.h"
#include "audio/SoundEventLibrary.h"
#include "component/UIComponent.h"
#include "object/sound/Audio.h"
#include "object/sound/AudioSource.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/PhysicsSystem.h"

#include <chrono>
#include <cstdio>
#include <fstream>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    class SoundEventBench : public scriptdata::ScriptDataTest {
    protected:
        Scene* scene = nullptr;
        bool wasAsync = false;

        void SetUp() override {
            ScriptDataTest::SetUp();
            wasAsync = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            AudioSystem::close();
            AudioSystem::setBackend(AudioBackend::Null);
            AudioSystem::configure(AudioConfig());
            AudioMixer::unload();
            SoundPool::clear();
            SoundEventLibrary::clear();
            AudioSystem::resetSoundEventRuntime();
            // 0.05 s of silence, 16-bit mono
            const uint32_t samples = 2205;
            std::ofstream out(root / "assets" / "c.wav", std::ios::binary);
            auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
            out.write("RIFF", 4); u32(36 + samples * 2); out.write("WAVEfmt ", 8); u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
            out.write("data", 4); u32(samples * 2);
            for (uint32_t i = 0; i < samples; i++) u16(0);
            out.close();
            SoundEventDef def;
            def.mode = SoundEventMode::Shuffle;
            def.pitchMax = 1.0f;
            SoundEventClip clip;
            clip.clip.path = "c.wav";
            def.clips.push_back(clip);
            def.clips.push_back(clip);
            std::ofstream(root / "assets" / "e.soundevent", std::ios::binary) << SoundEventIO::write(def);
            scene = new Scene();
            Engine::setScene(scene);
        }

        void TearDown() override {
            Engine::removeScene(scene);
            delete scene;
            AudioSystem::close();
            AudioMixer::unload();
            SoundPool::clear();
            SoundEventLibrary::clear();
            AudioSystem::resetSoundEventRuntime();
            Engine::setAsyncLoading(wasAsync);
            ScriptDataTest::TearDown();
        }

        // seconds a play over `plays` plays of `name`, the voices stopped between batches so the
        // voice table stays small
        double timePlays(const char* name, int plays) {
            using Clock = std::chrono::steady_clock;
            double total = 0.0;
            for (int i = 0; i < plays; i++) {
                const auto start = Clock::now();
                SoundHandle handle = Audio::play(name);
                total += std::chrono::duration<double>(Clock::now() - start).count();
                EXPECT_TRUE(handle.isValid());
                handle.stop();
            }
            return total / plays;
        }
    };

}

TEST_F(SoundEventBench, StartEventAboveStartVoice) {
    // the first of each loads its file
    Audio::play("c.wav").stop();
    Audio::play("e.soundevent").stop();
    // no file access after the first load: the event plays with its file gone
    fs::remove(root / "assets" / "e.soundevent");
    const double clip = timePlays("c.wav", 2000);
    const double event = timePlays("e.soundevent", 2000);
    const double above = (event - clip) * 1e6;
    std::printf("[bench] startEvent above startVoice: %.2f us a voice (clip %.2f us, event %.2f us)\n", above, clip * 1e6, event * 1e6);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LE(above, 15.0 * slack);
    }
}

TEST_F(SoundEventBench, SixtyFourTriggersInAFrame) {
    std::vector<AudioSource> sources;
    sources.reserve(64);
    for (int i = 0; i < 64; i++) {
        sources.emplace_back(scene);
        AudioSource& sound = sources.back();
        sound.loadSound("c.wav");
        scene->addComponent<UIComponent>(sound.getEntity(), UIComponent());
        sound.setPlayOn(AudioPlayOn_UIClick);
    }
    AudioMixer::update(0.016, 0.016);
    scene->getSystem<AudioSystem>()->update(0.016);
    double best = 1e9;
    for (int round = 0; round < 20; round++) {
        for (AudioSource& sound : sources) scene->getComponent<UIComponent>(sound.getEntity()).onClick.call(0.0f, 0.0f);
        const auto start = std::chrono::steady_clock::now();
        scene->getSystem<AudioSystem>()->update(0.0);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        AudioSystem::stopAll();
    }
    std::printf("[bench] 64 trigger plays in a frame: %.3f ms\n", best);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LE(best, 1.0 * slack);
    }
}

TEST_F(SoundEventBench, NoSubscriptionWithoutAFlagCostsAnUpdate) {
    AudioSource sound(scene);
    sound.loadSound("c.wav");
    scene->getSystem<AudioSystem>()->update(0.016);
    EXPECT_FALSE(scene->getSystem<PhysicsSystem>()->onContactAdded3D.hasSubscribers());
}
