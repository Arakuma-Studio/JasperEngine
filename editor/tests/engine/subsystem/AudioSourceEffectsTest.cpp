// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// An Audio Source's own effects (docs/specs/S97-audio-gaps.md REQ-1 to REQ-4, REQ-36; AC1 to AC6):
// the voices of one source go through its effects and no other source's do, a structure change
// waits for the next start while values reach the live voices at once, timed changes follow the
// mixer's curves, and a source with no effect on plays the pool's sample itself. On SoLoud's null
// backend, mixed by the test, so the meters are exact.

#include <gtest/gtest.h>

#include "ScriptDataTestSupport.h"
#include "support/EditorTestSupport.h"

#include "Scene.h"
#include "audio/AudioMixer.h"
#include "audio/SourceEffects.h"
#include "component/AudioSourceComponent.h"
#include "object/sound/AudioSource.h"
#include "object/sound/AudioSourceEffect.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include "soloud.h"
#include "soloud_biquadresonantfilter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    namespace fs = std::filesystem;

    // a 16-bit mono PCM WAV of a sine
    void writeSourceEffectTone(const fs::path& path, double seconds, double frequency, int rate) {
        fs::create_directories(path.parent_path());
        const uint32_t samples = (uint32_t)(seconds * rate);
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
        for (uint32_t i = 0; i < samples; i++) {
            const double value = 0.5 * std::sin(2.0 * M_PI * frequency * i / rate);
            u16((uint16_t)(int16_t)std::lround(value * 32767.0));
        }
    }

    // Master with groups A and B, so two sources are metered apart
    const char* sourceEffectsMixer = R"({
  "format": "jasperengine.mixer", "version": 1,
  "groups": [
    {"id": "00000001", "name": "Master", "parent": null},
    {"id": "0000000a", "name": "A", "parent": "00000001"},
    {"id": "0000000b", "name": "B", "parent": "00000001"}
  ]
})";

    class SourceEffectsFixture : public scriptdata::ScriptDataTest {
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
            // 5 kHz at 48 kHz: far above a 500 Hz low-pass
            writeSourceEffectTone(root / "assets" / "tone5k.wav", 1.0, 5000.0, 48000);
            std::string error;
            ASSERT_TRUE(AudioMixer::loadFromText(sourceEffectsMixer, "test.mixer", &error)) << error;
            scene = new Scene();
            Engine::setScene(scene);
        }

        void TearDown() override {
            Engine::removeScene(scene);
            delete scene;
            scene = nullptr;
            AudioSystem::close();
            AudioMixer::unload();
            SoundPool::clear();
            Engine::setAsyncLoading(wasAsync);
            ScriptDataTest::TearDown();
        }

        // one frame as the null backend runs it: the mix, then the scene's audio update
        void step(double dt, double scaled = -1.0) {
            AudioMixer::update(scaled < 0.0 ? dt : scaled, dt);
            scene->getSystem<AudioSystem>()->update(scaled < 0.0 ? dt : scaled);
        }

        void run(double seconds, double dt = 0.02) {
            for (double t = 0.0; t < seconds - 1e-9; t += dt) step(dt);
        }

        AudioSource source(const std::string& group, bool looping = true) {
            AudioSource sound(scene);
            sound.loadSound("tone5k.wav");
            sound.setOutputGroup(group);
            sound.setLooping(looping);
            return sound;
        }

        SoLoud::Soloud& soloud() { return AudioSystem::getSoloud(); }

        unsigned int filterBuilds() {
            unsigned int limits = 0, builds = 0;
            AudioMixer::getDebugCounters(limits, builds);
            return builds;
        }
    };

    class AudioSourceEffects : public SourceEffectsFixture {};
    class AudioSourceEffectsBench : public SourceEffectsFixture {};

}

TEST_F(AudioSourceEffects, LowPassFiltersEveryVoiceOfTheSource) {
    AudioSource filtered = source("A");
    AudioSource twin = source("B");
    AudioSourceEffect lowpass = filtered.addEffect("lowpass");
    ASSERT_TRUE(lowpass.isValid());
    ASSERT_TRUE(lowpass.setParameter("cutoff", 500.0f));
    EXPECT_EQ(filtered.getComponent<AudioSourceComponent>().effects.size(), 1u);

    filtered.play();
    twin.play();
    run(0.3);
    const float open = AudioMixer::getMeterDb("B");
    EXPECT_GT(open, -20.0f) << "the unfiltered twin is heard";
    EXPECT_LT(AudioMixer::getMeterDb("A"), open - 20.0f) << "the main voice goes through the low-pass";

    filtered.stop();
    twin.stop();
    run(0.3);
    EXPECT_LT(AudioMixer::getMeterDb("A"), -79.0f);
    filtered.playOneShot("tone5k.wav");
    twin.playOneShot("tone5k.wav");
    run(0.3);
    const float openShot = AudioMixer::getMeterDb("B");
    EXPECT_GT(openShot, -20.0f);
    EXPECT_LT(AudioMixer::getMeterDb("A"), openShot - 20.0f) << "a one-shot of the source goes through it too";
}

TEST_F(AudioSourceEffects, TwoSourcesOfOneClipFilterApart) {
    AudioSource twin = source("B");
    twin.play();
    run(0.3);
    const float lone = AudioMixer::getMeterDb("B");
    twin.stop();
    run(0.3);

    AudioSource filtered = source("A");
    filtered.addEffect("lowpass").setParameter("cutoff", 500.0f);
    filtered.play();
    twin.play();
    run(0.3);
    EXPECT_NEAR(AudioMixer::getMeterDb("B"), lone, 0.1f) << "the other source of the file is not filtered";
    EXPECT_LT(AudioMixer::getMeterDb("A"), lone - 20.0f);

    // the pool's sample is shared and carries no filter: the filtered voice plays a wrapper of it
    const AudioSourceComponent& audio = twin.getComponent<AudioSourceComponent>();
    ASSERT_TRUE(audio.sample);
    EXPECT_EQ(audio.sample.get(), filtered.getComponent<AudioSourceComponent>().sample.get()) << "one pool entry";
    for (unsigned int slot = 0; slot < FILTERS_PER_STREAM; slot++) EXPECT_EQ(audio.sample->mFilter[slot], nullptr) << "slot " << slot;
    EXPECT_EQ(soloud().countAudioSource(*audio.sample), 1) << "only the twin's voice plays the sample itself";
}

TEST_F(AudioSourceEffects, StructureChangesTakeTheNextStart) {
    AudioSource sound = source("A");
    AudioSourceEffect lowpass = sound.addEffect("lowpass");
    lowpass.setParameter("cutoff", 15000.0f);
    sound.play();
    run(0.3);
    const float before = AudioMixer::getMeterDb("A");
    EXPECT_GT(before, -20.0f) << "a 15 kHz low-pass leaves a 5 kHz tone";

    // a second effect while it plays: the voice keeps the chain it started with
    AudioSourceEffect highpass = sound.addEffect("highpass");
    ASSERT_TRUE(highpass.setParameter("cutoff", 20000.0f));
    run(0.3);
    EXPECT_NEAR(AudioMixer::getMeterDb("A"), before, 0.5f) << "the added high-pass waits for the next start";

    // a value change reaches the live voice in the mix after the frame's update
    ASSERT_TRUE(lowpass.isValid());
    lowpass.setParameter("cutoff", 500.0f);
    scene->getSystem<AudioSystem>()->update(0.0);
    AudioSystem::mixManually(0.15);
    EXPECT_LT(AudioMixer::getMeterDb("A"), before - 20.0f) << "the cutoff is heard at once";

    // the next start takes the new structure: the high-pass at 20 kHz cuts the tone
    lowpass.setParameter("cutoff", 22000.0f);
    sound.play();
    run(0.3);
    EXPECT_LT(AudioMixer::getMeterDb("A"), before - 12.0f) << "play() again starts with both effects";
}

TEST_F(AudioSourceEffects, TimedCutoffMovesAlongLogFrequency) {
    AudioSource sound = source("A");
    AudioSourceEffect lowpass = sound.addEffect("lowpass");
    lowpass.setParameter("cutoff", 20000.0f);
    sound.play();
    run(0.1);

    ASSERT_TRUE(lowpass.setParameter("cutoff", 500.0f, 1.0f));
    for (int i = 0; i < 25; i++) step(0.02);
    float cutoff = 0.0f;
    ASSERT_TRUE(lowpass.getParameter("cutoff", cutoff));
    EXPECT_NEAR(cutoff, 3162.3f, 3162.3f * 0.01f) << "half way along log2(Hz)";
    EXPECT_NEAR(sound.getComponent<AudioSourceComponent>().effects[0].params.at("cutoff"), cutoff, 1e-3f) << "the component holds it";
    for (int i = 0; i < 30; i++) step(0.02);
    ASSERT_TRUE(lowpass.getParameter("cutoff", cutoff));
    EXPECT_FLOAT_EQ(cutoff, 500.0f);

    // scaled time stands still at time scale 0; unscaled time goes on
    lowpass.setParameter("cutoff", 20000.0f);
    lowpass.setParameter("cutoff", 500.0f, 1.0f);
    for (int i = 0; i < 25; i++) step(0.02, 0.0);
    ASSERT_TRUE(lowpass.getParameter("cutoff", cutoff));
    EXPECT_FLOAT_EQ(cutoff, 20000.0f);
    lowpass.setParameter("cutoff", 500.0f, 1.0f, true);
    for (int i = 0; i < 25; i++) step(0.02, 0.0);
    ASSERT_TRUE(lowpass.getParameter("cutoff", cutoff));
    EXPECT_NEAR(cutoff, 3162.3f, 3162.3f * 0.01f) << "{unscaled = true} moves at time scale 0";
}

TEST_F(AudioSourceEffects, EmptyOrBypassedPlaysTheSampleDirectly) {
    const unsigned int builds = filterBuilds();
    AudioSource plain = source("A");
    AudioSource off = source("A");
    off.addEffect("lowpass").setParameter("cutoff", 22000.0f);
    AudioSource bypassed = source("A");
    bypassed.addEffect("lowpass").setParameter("cutoff", 500.0f);
    bypassed.getEffect(1).setBypass(true);

    plain.play();
    off.play();
    bypassed.play();
    off.playOneShot("tone5k.wav");
    run(0.1);
    const AudioSourceComponent& audio = plain.getComponent<AudioSourceComponent>();
    ASSERT_TRUE(audio.sample);
    EXPECT_EQ(soloud().countAudioSource(*audio.sample), 4) << "every voice plays the pool's sample: no wrapper";
    EXPECT_EQ(filterBuilds(), builds) << "no filter made";
    EXPECT_FALSE(off.getComponent<AudioSourceComponent>().effectRuntime);
    EXPECT_FALSE(bypassed.getComponent<AudioSourceComponent>().effectRuntime);
}

// REQ-36 (revised, spec Deviations): 32 voices of one clip, each with a 500 Hz low-pass, mixed one
// 1024-frame block at a time on the null driver. The source's wrapper costs at most 15 % over SoLoud
// filtering each voice natively (the same BiquadResonantFilter in the sound's own filter slot, which
// the engine cannot use on a shared sample: D2), a source without effects plays the sample itself
// (no wrapper, AC5), and a block stays under an absolute ceiling. Medians of interleaved rounds;
// enforced through perfSlack() (docs/testing.md), printed always.
TEST_F(AudioSourceEffectsBench, LowPassCostOn32Voices) {
    constexpr int voices = 32;
    constexpr unsigned int frames = 1024;
    // the ceiling of one block: about 3x the box's measured median (docs/specs/S97-audio-gaps.md
    // Deviations, REQ-36)
    constexpr double ceilingMicros = 600.0;

    std::vector<AudioSource> sources;
    for (int i = 0; i < voices; i++) sources.push_back(source("A"));
    SoLoud::BiquadResonantFilter native;
    native.setParams(SoLoud::BiquadResonantFilter::LOWPASS, 500.0f, 1.0f);
    std::vector<float> buffer(frames * 2);

    enum class Mode { Plain, Native, Wrapper };
    auto start = [&](Mode mode) {
        for (AudioSource& sound : sources) sound.stop();
        run(0.04);
        SoLoud::AudioSource& sample = *sources[0].getComponent<AudioSourceComponent>().sample;
        sample.setFilter(0, mode == Mode::Native ? &native : nullptr);
        for (AudioSource& sound : sources) {
            while (sound.getEffectCount() > 0) sound.removeEffect(1);
            if (mode == Mode::Wrapper) sound.addEffect("lowpass").setParameter("cutoff", 500.0f);
            sound.play();
        }
        run(0.04);
    };
    // the median of 31 blocks, microseconds
    auto blocks = [&]() {
        std::vector<double> times;
        for (int i = 0; i < 31; i++) {
            const auto begin = std::chrono::steady_clock::now();
            soloud().mix(buffer.data(), frames);
            times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    };
    std::vector<double> plain, nativeTimes, wrapper;
    for (int round = 0; round < 5; round++) {
        start(Mode::Plain);
        plain.push_back(blocks());
        start(Mode::Native);
        nativeTimes.push_back(blocks());
        EXPECT_EQ(soloud().countAudioSource(*sources[0].getComponent<AudioSourceComponent>().sample), voices) << "native: the sample itself";
        start(Mode::Wrapper);
        wrapper.push_back(blocks());
        EXPECT_EQ(soloud().countAudioSource(*sources[0].getComponent<AudioSourceComponent>().sample), 0) << "wrapped: no voice plays the sample itself";
    }
    start(Mode::Plain);
    auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    const double plainMicros = median(plain);
    const double nativeMicros = median(nativeTimes);
    const double wrapperMicros = median(wrapper);
    const double ratio = wrapperMicros / nativeMicros;
    std::printf("[bench] 32 voices, one 1024-frame block on the null driver: %.1f us without effects, %.1f us with SoLoud's own "
                "low-pass on each voice, %.1f us through the sources' effects (x%.3f of native; ceiling %.0f us)\n",
                plainMicros, nativeMicros, wrapperMicros, ratio, ceilingMicros);
    const double slack = editor::test::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(ratio, 1.15 * slack) << "REQ-36: the wrapper over SoLoud's native per-voice filter";
        EXPECT_LE(wrapperMicros, ceilingMicros * slack) << "REQ-36: the block's ceiling";
    }
}
