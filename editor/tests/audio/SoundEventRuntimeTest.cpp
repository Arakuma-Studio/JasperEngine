// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sound Events playing (docs/specs/S37-sound-events.md R6-R11; AC3, AC4, AC5), on the null audio
// driver with the voice lines an observer sees: the picks, ranges, cooldown on the sample
// boundary, the instance limit and its order against the source's cap and the global budget, the
// event mode of a source, clip-mode variation, missing clips and events.

#include <gtest/gtest.h>

#include "../ScriptDataTestSupport.h"

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "audio/AudioMixer.h"
#include "audio/SoundEventIO.h"
#include "audio/SoundEventLibrary.h"
#include "component/ButtonComponent.h"
#include "component/UIComponent.h"
#include "object/sound/Audio.h"
#include "object/sound/AudioSource.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/PhysicsSystem.h"

#include "json.hpp"
#include "soloud.h"

#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    void writeSoundEventTone(const fs::path& path, double seconds, double frequency = 1000.0, double amplitude = 0.5, int rate = 44100) {
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
            const double value = amplitude * std::sin(2.0 * M_PI * frequency * i / rate);
            u16((uint16_t)(int16_t)std::lround(value * 32767.0));
        }
    }

    class SoundEventRuntimeTest : public scriptdata::ScriptDataTest {
    protected:
        Scene* scene = nullptr;
        bool wasAsync = false;
        std::vector<AudioVoiceLine> lines;

        void SetUp() override {
            ScriptDataTest::SetUp();
            wasAsync = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            AudioSystem::close();
            AudioSystem::setBackend(AudioBackend::Null);
            AudioConfig config;
            config.seed = 42;
            AudioSystem::configure(config);
            AudioMixer::unload();
            SoundPool::clear();
            SoundEventLibrary::clear();
            AudioSystem::resetSoundEventRuntime();
            for (int i = 0; i < 4; i++) {
                writeSoundEventTone(root / "assets" / ("c" + std::to_string(i) + ".wav"), 0.4, 500.0 + 200.0 * i);
            }
            scene = new Scene();
            Engine::setScene(scene);
            AudioSystem::setVoiceObserver([this](const AudioVoiceLine& line) { lines.push_back(line); });
        }

        void TearDown() override {
            AudioSystem::setVoiceObserver(nullptr);
            Engine::removeScene(scene);
            delete scene;
            scene = nullptr;
            AudioSystem::close();
            AudioMixer::unload();
            AudioSystem::configure(AudioConfig());
            SoundPool::clear();
            SoundEventLibrary::clear();
            AudioSystem::resetSoundEventRuntime();
            Engine::setAsyncLoading(wasAsync);
            ScriptDataTest::TearDown();
        }

        void step(double dt = 0.02) {
            AudioMixer::update(dt, dt);
            scene->getSystem<AudioSystem>()->update(dt);
        }

        void run(double seconds, double dt = 0.02) {
            for (double t = 0.0; t < seconds - 1e-9; t += dt) step(dt);
        }

        SoundEventDef def(SoundEventMode mode, int count) {
            SoundEventDef event;
            event.mode = mode;
            for (int i = 0; i < count; i++) {
                SoundEventClip clip;
                clip.clip.path = "c" + std::to_string(i) + ".wav";
                event.clips.push_back(clip);
            }
            return event;
        }

        std::string install(const SoundEventDef& event, const std::string& name = "e.soundevent") {
            std::ofstream(root / "assets" / name, std::ios::binary) << SoundEventIO::write(event);
            SoundEventLibrary::reload(name);
            return name;
        }

        AudioSource source(const std::string& event = "") {
            AudioSource sound(scene);
            if (!event.empty()) sound.getComponent<AudioSourceComponent>().event = event;
            return sound;
        }

        std::vector<AudioVoiceLine> of(AudioVoiceLine::Kind kind, const std::string& reason = "") const {
            std::vector<AudioVoiceLine> out;
            for (const AudioVoiceLine& line : lines) {
                if (line.kind == kind && (reason.empty() || reason == line.reason)) out.push_back(line);
            }
            return out;
        }
        std::vector<AudioVoiceLine> starts() const { return of(AudioVoiceLine::Kind::Start); }
        size_t rejects(const std::string& reason) const { return of(AudioVoiceLine::Kind::Reject, reason).size(); }
    };

}

// AC3: over a source's one-shots
TEST_F(SoundEventRuntimeTest, ShuffleOverFourClipsEightPlays) {
    const std::string name = install(def(SoundEventMode::Shuffle, 4));
    AudioSource sound = source();
    for (int i = 0; i < 8; i++) {
        sound.playOneShot(name);
        step();
    }
    const auto started = starts();
    ASSERT_EQ(started.size(), 8u);
    std::map<int, int> counts;
    int last = -1;
    for (const AudioVoiceLine& line : started) {
        EXPECT_EQ(line.eventPath, name);
        EXPECT_EQ(line.owner, std::string("one_shot"));
        EXPECT_NE(line.variation, last);
        last = line.variation;
        counts[line.variation]++;
    }
    for (int clip = 0; clip < 4; clip++) EXPECT_EQ(counts[clip], 2) << clip;
}

// AC3: audio.seed gives the Python reference's picks; another seed others
TEST_F(SoundEventRuntimeTest, PicksRepeatUnderTheSeed) {
    const SoundEventDef event = def(SoundEventMode::Random, 4);
    const std::string name = install(event);
    AudioSource sound = source();
    for (int i = 0; i < 16; i++) {
        sound.playOneShot(name);
        step();
    }
    const auto started = starts();
    ASSERT_EQ(started.size(), 16u);
    EXPECT_EQ(AudioSystem::getSoundEventSeed(), 42u);
    const std::vector<SoundEventPick> expected = SoundEventLibrary::preview(event, name, 42, 16);
    for (size_t i = 0; i < 16; i++) EXPECT_EQ(started[i].variation, expected[i].clips[0]) << i;
}

// R7: the values of a voice
TEST_F(SoundEventRuntimeTest, RangesAndClipValuesReachTheVoice) {
    SoundEventDef event = def(SoundEventMode::Sequence, 1);
    event.pitchMin = -2;
    event.pitchMax = 2;
    event.volumeMin = -6;
    event.volumeMax = 0;
    event.clips[0].pitchSemitones = 12;
    event.clips[0].volumeDb = -6;
    const std::string name = install(event);
    AudioSource sound = source();
    sound.setVolume(0.5);
    sound.setSpeed(0.5f);
    for (int i = 0; i < 12; i++) {
        sound.playOneShot(name, 0.5f);
        step();
    }
    const auto started = starts();
    ASSERT_EQ(started.size(), 12u);
    for (const AudioVoiceLine& line : started) {
        // speed = source x 2^((p + clip) / 12): 0.5 x 2^[10/12, 14/12]
        EXPECT_GE(line.pitch, 0.5f * std::pow(2.0f, 10.0f / 12.0f) - 1e-3f);
        EXPECT_LE(line.pitch, 0.5f * std::pow(2.0f, 14.0f / 12.0f) + 1e-3f);
        // gain = source x 10^((v + clip) / 20) x scale: 0.5 x 0.5 x 10^([-12, -6] / 20)
        EXPECT_GE(line.gain, 0.25f * std::pow(10.0f, -12.0f / 20.0f) - 1e-3f);
        EXPECT_LE(line.gain, 0.25f * std::pow(10.0f, -6.0f / 20.0f) + 1e-3f);
    }
}

// R7: a delay in frames of the mix
TEST_F(SoundEventRuntimeTest, DelayIsInFrames) {
    SoundEventDef event = def(SoundEventMode::Sequence, 1);
    event.clips[0].delayMs = 100;
    const std::string name = install(event);
    AudioSource sound = source();
    sound.playOneShot(name);
    step();
    ASSERT_EQ(starts().size(), 1u);
    const unsigned int rate = AudioSystem::getSoloud().getBackendSamplerate();
    EXPECT_EQ(starts()[0].delayFrames, rate / 10);
}

// AC3: cooldown rejections on the sample boundary
TEST_F(SoundEventRuntimeTest, CooldownRejectsThenAcceptsAtTheBoundary) {
    SoundEventDef event = def(SoundEventMode::Sequence, 2);
    event.cooldownMs = 100;
    const std::string name = install(event);
    AudioSource sound = source();
    sound.playOneShot(name);
    step();
    ASSERT_EQ(starts().size(), 1u);
    const uint64_t first = starts()[0].sample;
    const unsigned int rate = AudioSystem::getSoloud().getBackendSamplerate();
    const uint64_t boundary = first + (uint64_t)std::llround(0.1 * rate);

    // up to the frame before the boundary every play is rejected; its pick is not consumed
    while (true) {
        AudioMixer::update(0.01, 0.01);
        // the cursor this frame's play compares
        const size_t before = lines.size();
        sound.playOneShot(name);
        scene->getSystem<AudioSystem>()->update(0.01);
        ASSERT_EQ(lines.size(), before + 1);
        if (lines.back().kind == AudioVoiceLine::Kind::Start) {
            EXPECT_GE(lines.back().sample, boundary);
            EXPECT_EQ(lines.back().variation, 1) << "the rejected plays consumed no pick";
            break;
        }
        EXPECT_STREQ(lines.back().reason, "cooldown");
        EXPECT_LT(lines.back().sample, boundary);
    }
    EXPECT_GE(rejects("cooldown"), 1u);
}

// AC3: the stolen instance stops before the new one starts
TEST_F(SoundEventRuntimeTest, MaxInstancesStealsTheOldestFirst) {
    SoundEventDef event = def(SoundEventMode::Sequence, 3);
    event.maxInstances = 2;
    event.steal = AudioStealPolicy::Oldest;
    const std::string name = install(event);
    AudioSource a = source();
    AudioSource b = source();
    a.playOneShot(name);
    step();
    b.playOneShot(name);
    step();
    a.playOneShot(name);
    step();
    ASSERT_EQ(starts().size(), 3u);
    const auto stolen = of(AudioVoiceLine::Kind::Stop, "stolen_instance");
    ASSERT_EQ(stolen.size(), 1u);
    EXPECT_EQ(stolen[0].handle, starts()[0].handle);
    EXPECT_EQ(stolen[0].byInstance, starts()[2].instance);

    size_t stopAt = 0, startAt = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].kind == AudioVoiceLine::Kind::Stop && std::string(lines[i].reason) == "stolen_instance") stopAt = i;
        if (lines[i].kind == AudioVoiceLine::Kind::Start && lines[i].instance == starts()[2].instance) startAt = i;
    }
    EXPECT_LT(stopAt, startAt);
}

TEST_F(SoundEventRuntimeTest, MaxInstancesNewestAndNone) {
    SoundEventDef event = def(SoundEventMode::Sequence, 3);
    event.maxInstances = 2;
    event.steal = AudioStealPolicy::Newest;
    const std::string newest = install(event, "newest.soundevent");
    event.steal = AudioStealPolicy::None;
    const std::string none = install(event, "none.soundevent");
    AudioSource sound = source();
    for (int i = 0; i < 3; i++) {
        sound.playOneShot(newest);
        step();
    }
    auto stolen = of(AudioVoiceLine::Kind::Stop, "stolen_instance");
    ASSERT_EQ(stolen.size(), 1u);
    EXPECT_EQ(stolen[0].handle, starts()[1].handle);

    lines.clear();
    for (int i = 0; i < 3; i++) {
        sound.playOneShot(none);
        step();
    }
    EXPECT_EQ(starts().size(), 2u);
    EXPECT_EQ(rejects("instance_limit"), 1u);
    EXPECT_TRUE(of(AudioVoiceLine::Kind::Stop, "stolen_instance").empty());
}

// R6.2: a more important instance is not a candidate
TEST_F(SoundEventRuntimeTest, MaxInstancesLeavesAMoreImportantOne) {
    SoundEventDef event = def(SoundEventMode::Sequence, 3);
    event.maxInstances = 1;
    const std::string name = install(event);
    AudioSource important = source();
    important.setPriority(10);
    AudioSource other = source();
    important.playOneShot(name);
    step();
    other.playOneShot(name);
    step();
    EXPECT_EQ(starts().size(), 1u);
    EXPECT_EQ(rejects("instance_limit"), 1u);
}

// AC4: cooldown -> instance limit -> the source's cap -> the global budget
TEST_F(SoundEventRuntimeTest, LimitsComposeInOrder) {
    AudioConfig config;
    config.maxVoices = 2;
    AudioSystem::configure(config);
    SoundEventDef event = def(SoundEventMode::Sequence, 4);
    event.maxInstances = 4;
    const std::string name = install(event);
    AudioSource sound = source();
    sound.setMaxVoices(3);
    for (int i = 0; i < 6; i++) {
        sound.playOneShot(name);
        step();
    }
    // the global budget of two is hit first: the oldest voice goes (same priority) to the budget
    EXPECT_EQ(starts().size(), 6u);
    EXPECT_EQ(of(AudioVoiceLine::Kind::Stop, "stolen_budget").size(), 4u);
    EXPECT_TRUE(of(AudioVoiceLine::Kind::Stop, "stolen_source").empty());
    EXPECT_LE(AudioSystem::getVoiceCount(), 2u);

    // with room in the budget the source's cap acts, per voice
    lines.clear();
    AudioSystem::configure(AudioConfig());
    sound.stop();
    step();
    lines.clear();
    AudioSource capped = source();
    capped.setMaxVoices(3);
    for (int i = 0; i < 5; i++) {
        capped.playOneShot(name);
        step();
    }
    EXPECT_EQ(of(AudioVoiceLine::Kind::Stop, "stolen_source").size(), 2u);
    EXPECT_TRUE(of(AudioVoiceLine::Kind::Stop, "stolen_instance").empty()) << "max_instances 4 is above the cap of 3";
    EXPECT_EQ(capped.getVoiceCount(), 3);

    // and the instance limit acts before the cap, stopping before the source would have to
    lines.clear();
    event.maxInstances = 2;
    const std::string tight = install(event, "tight.soundevent");
    AudioSource limited = source();
    limited.setMaxVoices(3);
    for (int i = 0; i < 5; i++) {
        limited.playOneShot(tight);
        step();
    }
    EXPECT_EQ(of(AudioVoiceLine::Kind::Stop, "stolen_instance").size(), 3u);
    EXPECT_TRUE(of(AudioVoiceLine::Kind::Stop, "stolen_source").empty());
}

// R10: the main voice of a layered event
TEST_F(SoundEventRuntimeTest, LayerIsOneInstanceOfMainVoices) {
    SoundEventDef event = def(SoundEventMode::Layer, 3);
    const std::string name = install(event);
    AudioSource sound = source(name);
    int started = 0, stopped = 0;
    sound.getComponent<AudioSourceComponent>().onStart.add("test", std::function<void()>([&]() { started++; }));
    sound.getComponent<AudioSourceComponent>().onStop.add("test", std::function<void()>([&]() { stopped++; }));
    sound.play();
    step();
    const auto voices = starts();
    ASSERT_EQ(voices.size(), 3u);
    EXPECT_EQ(voices[0].instance, voices[2].instance);
    EXPECT_EQ(voices[1].layer, 1);
    EXPECT_EQ(voices[0].owner, std::string("main"));
    EXPECT_EQ(sound.getVoiceCount(), 3);
    EXPECT_TRUE(sound.isPlaying());
    EXPECT_EQ(started, 1);

    sound.pause();
    step();
    EXPECT_TRUE(AudioSystem::getSoloud().getPause(voices[0].handle));
    EXPECT_TRUE(AudioSystem::getSoloud().getPause(voices[2].handle));
    sound.play();
    step();
    EXPECT_FALSE(AudioSystem::getSoloud().getPause(voices[2].handle));
    sound.seek(0.1);
    EXPECT_NEAR(AudioSystem::getSoloud().getStreamPosition(voices[2].handle), 0.1, 0.05);

    // the natural end: none of them valid
    run(0.7);
    EXPECT_FALSE(sound.isPlaying());
    EXPECT_EQ(sound.getVoiceCount(), 0);
    EXPECT_EQ(stopped, 1);
    EXPECT_EQ(lines.size(), 3u) << "no second start";
}

// R10: stop() stops every layer
TEST_F(SoundEventRuntimeTest, StopAppliesToAllLayers) {
    const std::string name = install(def(SoundEventMode::Layer, 2));
    AudioSource sound = source(name);
    sound.play();
    step();
    EXPECT_EQ(sound.getVoiceCount(), 2);
    sound.stop();
    step();
    EXPECT_EQ(sound.getVoiceCount(), 0);
    EXPECT_FALSE(sound.isPlaying());
}

// R7: an event's loop, and a source's looping on its main voice
TEST_F(SoundEventRuntimeTest, LoopOfEventAndOfSource) {
    SoundEventDef looped = def(SoundEventMode::Sequence, 1);
    looped.loop = true;
    const std::string name = install(looped, "looped.soundevent");
    AudioSource sound = source(name);
    sound.play();
    run(1.0);
    EXPECT_TRUE(sound.isPlaying()) << "past the clip's 0.4 s";

    const std::string plain = install(def(SoundEventMode::Sequence, 1), "plain.soundevent");
    AudioSource looping = source(plain);
    looping.setLooping(true);
    looping.play();
    run(1.0);
    EXPECT_TRUE(looping.isPlaying());

    AudioSource oneShot = source();
    oneShot.setLooping(true);
    oneShot.playOneShot(plain);
    run(1.0);
    EXPECT_EQ(oneShot.getVoiceCount(), 0) << "the source's looping is its main voice's";
}

// R9: the event is played, its clip ignored; no fallback when it is missing
TEST_F(SoundEventRuntimeTest, EventWinsOverClipWithNoFallback) {
    const std::string name = install(def(SoundEventMode::Sequence, 1));
    AudioSource sound = source();
    sound.loadSound("c3.wav");
    sound.getComponent<AudioSourceComponent>().event = name;
    sound.play();
    step();
    ASSERT_EQ(starts().size(), 1u);
    EXPECT_EQ(starts()[0].clip, "c0.wav");

    lines.clear();
    AudioSource missing = source();
    missing.loadSound("c3.wav");
    missing.getComponent<AudioSourceComponent>().event = "gone.soundevent";
    missing.play();
    step();
    EXPECT_TRUE(starts().empty());
    EXPECT_EQ(rejects("missing_event"), 1u);
    EXPECT_FALSE(missing.isPlaying());
}

// R6.5: a clip that does not load is skipped with the other picks unchanged
TEST_F(SoundEventRuntimeTest, MissingClipSkippedPicksUnchanged) {
    SoundEventDef event = def(SoundEventMode::Sequence, 3);
    event.clips[1].clip.path = "gone.wav";
    const std::string name = install(event);
    AudioSource sound = source();
    for (int i = 0; i < 6; i++) {
        sound.playOneShot(name);
        step();
    }
    const auto started = starts();
    ASSERT_EQ(started.size(), 4u);
    EXPECT_EQ(started[0].variation, 0);
    EXPECT_EQ(started[1].variation, 2);
    EXPECT_EQ(started[2].variation, 0);
    EXPECT_EQ(started[3].variation, 2);
    EXPECT_EQ(rejects("missing_clip"), 2u);
}

// R4: the event's group; absent, the source's
TEST_F(SoundEventRuntimeTest, OutputGroupResolvesAsASourcesDoes) {
    std::string error;
    ASSERT_TRUE(AudioMixer::loadFromText(R"({"format": "jasperengine.mixer", "version": 1, "groups": [
        {"id": "00000001", "name": "Master", "parent": null},
        {"id": "5fx00001", "name": "SFX", "parent": "00000001"},
        {"id": "5fx00002", "name": "UI", "parent": "00000001"}]})", "m.mixer", &error)) << error;
    SoundEventDef event = def(SoundEventMode::Sequence, 1);
    event.outputGroupId = "5fx00001";
    event.outputGroupName = "Old name";
    const std::string byId = install(event, "id.soundevent");
    event.outputGroupId = "ffffffff";
    event.outputGroupName = "UI";
    const std::string byName = install(event, "name.soundevent");
    event.outputGroupId.clear();
    event.outputGroupName = "Nothing";
    const std::string master = install(event, "master.soundevent");
    const std::string own = install(def(SoundEventMode::Sequence, 1), "own.soundevent");

    AudioSource sound = source();
    sound.setOutputGroup("UI");
    step();
    for (const std::string& name : {byId, byName, master, own}) {
        sound.playOneShot(name);
        step();
    }
    const auto started = starts();
    ASSERT_EQ(started.size(), 4u);
    EXPECT_EQ(started[0].group, "5fx00001");
    EXPECT_EQ(started[1].group, "5fx00002");
    EXPECT_EQ(started[2].group, "");
    EXPECT_EQ(started[3].group, "5fx00002");
}

// R7: the priority of an event unless it says 128
TEST_F(SoundEventRuntimeTest, PriorityOfEventElseSource) {
    SoundEventDef event = def(SoundEventMode::Sequence, 1);
    event.priority = 5;
    const std::string set = install(event, "set.soundevent");
    const std::string own = install(def(SoundEventMode::Sequence, 1), "own.soundevent");
    AudioSource sound = source();
    sound.setPriority(200);
    sound.playOneShot(set);
    sound.playOneShot(own);
    step();
    ASSERT_EQ(starts().size(), 2u);
    EXPECT_EQ(starts()[0].priority, 5);
    EXPECT_EQ(starts()[1].priority, 200);
}

// AC5: clip mode varies each start within range, repeats under the seed, and both 0 is as before
TEST_F(SoundEventRuntimeTest, ClipVariationDrawsInRangeAndRepeats) {
    auto playAll = [&](float pitch, float volume) {
        lines.clear();
        AudioSystem::resetSoundEventRuntime();
        AudioSource sound = source();
        sound.loadSound("c0.wav");
        AudioSourceComponent& audio = sound.getComponent<AudioSourceComponent>();
        audio.pitchRandomSemitones = pitch;
        audio.volumeRandomDb = volume;
        step();
        std::vector<std::pair<float, float>> values;
        for (int i = 0; i < 6; i++) {
            sound.playOneShot("c0.wav");
            step();
        }
        sound.play();
        step();
        for (const AudioVoiceLine& line : starts()) values.push_back({line.pitch, line.gain});
        sound.stop();
        step();
        return values;
    };
    const auto first = playAll(2.0f, 6.0f);
    ASSERT_EQ(first.size(), 7u);
    bool varied = false;
    for (const auto& [pitch, gain] : first) {
        EXPECT_GE(pitch, std::pow(2.0f, -2.0f / 12.0f) - 1e-3f);
        EXPECT_LE(pitch, std::pow(2.0f, 2.0f / 12.0f) + 1e-3f);
        EXPECT_GE(gain, std::pow(10.0f, -6.0f / 20.0f) - 1e-3f);
        EXPECT_LE(gain, 1.0f + 1e-3f) << "never louder";
        varied = varied || pitch != first[0].first;
    }
    EXPECT_TRUE(varied);

    for (const auto& [pitch, gain] : playAll(0.0f, 0.0f)) {
        EXPECT_FLOAT_EQ(pitch, 1.0f);
        EXPECT_FLOAT_EQ(gain, 1.0f);
    }
}

TEST_F(SoundEventRuntimeTest, ClipVariationRepeatsUnderTheSeed) {
    auto play = [&]() {
        lines.clear();
        AudioSystem::resetSoundEventRuntime();
        std::vector<float> pitches;
        AudioSource sound = source();
        sound.loadSound("c0.wav");
        sound.getComponent<AudioSourceComponent>().pitchRandomSemitones = 3.0f;
        for (int i = 0; i < 6; i++) {
            sound.playOneShot("c0.wav");
            step();
        }
        for (const AudioVoiceLine& line : starts()) pitches.push_back(line.pitch);
        return std::make_pair(sound.getEntity(), pitches);
    };
    const auto first = play();
    // the same entity id is the same key: a fresh scene gives the same id
    Engine::removeScene(scene);
    delete scene;
    AudioSystem::stopAll();
    scene = new Scene();
    Engine::setScene(scene);
    const auto second = play();
    ASSERT_EQ(first.first, second.first);
    EXPECT_EQ(first.second, second.second);
}

// R5: playing keeps its voices through a reload, the next play uses the new definition
TEST_F(SoundEventRuntimeTest, ReloadKeepsVoicesNextPlayUsesTheNew) {
    SoundEventDef event = def(SoundEventMode::Sequence, 1);
    const std::string name = install(event);
    AudioSource sound = source();
    sound.playOneShot(name);
    step();
    ASSERT_EQ(starts().size(), 1u);
    const unsigned int voice = starts()[0].handle;

    event.clips[0].clip.path = "c2.wav";
    install(event);
    EXPECT_TRUE(AudioSystem::getSoloud().isValidVoiceHandle(voice));
    sound.playOneShot(name);
    step();
    ASSERT_EQ(starts().size(), 2u);
    EXPECT_EQ(starts()[1].clip, "c2.wav");
    EXPECT_TRUE(AudioSystem::getSoloud().isValidVoiceHandle(voice));
}

// the clip of a source without an event is as it was
TEST_F(SoundEventRuntimeTest, PlainSourceUnchanged) {
    AudioSource sound = source();
    sound.loadSound("c1.wav");
    sound.play();
    step();
    ASSERT_EQ(starts().size(), 1u);
    EXPECT_EQ(starts()[0].clip, "c1.wav");
    EXPECT_EQ(starts()[0].event, "");
    EXPECT_FLOAT_EQ(starts()[0].pitch, 1.0f);
    EXPECT_FLOAT_EQ(starts()[0].gain, 1.0f);
}

// ---------------------------------------------------------------- triggers (R12, AC6)

namespace {

    class SoundEventTriggerTest : public SoundEventRuntimeTest {
    protected:
        AudioSource uiSource(uint32_t flags) {
            AudioSource sound = source();
            sound.loadSound("c0.wav");
            scene->addComponent<UIComponent>(sound.getEntity(), UIComponent());
            scene->addComponent<ButtonComponent>(sound.getEntity(), ButtonComponent());
            sound.setPlayOn(flags);
            return sound;
        }
        UIComponent& ui(AudioSource& sound) { return scene->getComponent<UIComponent>(sound.getEntity()); }
        ButtonComponent& button(AudioSource& sound) { return scene->getComponent<ButtonComponent>(sound.getEntity()); }
    };

}

TEST_F(SoundEventTriggerTest, UiClickPressHoverEachStartAVoice) {
    AudioSource sound = uiSource(AudioPlayOn_UIClick | AudioPlayOn_UIPress | AudioPlayOn_UIHover);
    step();
    EXPECT_TRUE(ui(sound).onClick.hasSubscribers());
    ui(sound).onClick.call(1.0f, 2.0f);
    step();
    ASSERT_EQ(starts().size(), 1u);
    EXPECT_STREQ(starts()[0].trigger, "ui_click");
    EXPECT_STREQ(starts()[0].cause, "trigger");
    EXPECT_STREQ(starts()[0].owner, "one_shot");
    button(sound).onPress.call();
    step();
    ASSERT_EQ(starts().size(), 2u);
    EXPECT_STREQ(starts()[1].trigger, "ui_press");
    ui(sound).onPointerEnter.call(1.0f, 2.0f);
    step();
    ASSERT_EQ(starts().size(), 3u);
    EXPECT_STREQ(starts()[2].trigger, "ui_hover");
}

TEST_F(SoundEventTriggerTest, ATriggerPlaysTheEventOfAnEventSource) {
    SoundEventDef event = def(SoundEventMode::Sequence, 2);
    const std::string name = install(event);
    AudioSource sound = uiSource(AudioPlayOn_UIClick);
    sound.getComponent<AudioSourceComponent>().event = name;
    step();
    ui(sound).onClick.call(0.0f, 0.0f);
    step();
    ASSERT_EQ(starts().size(), 1u);
    EXPECT_EQ(starts()[0].eventPath, name);
    EXPECT_STREQ(starts()[0].trigger, "ui_click");
}

TEST_F(SoundEventTriggerTest, NoFlagNoSubscription) {
    AudioSource plain = source();
    plain.loadSound("c0.wav");
    scene->addComponent<UIComponent>(plain.getEntity(), UIComponent());
    run(0.1);
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    EXPECT_FALSE(ui(plain).onClick.hasSubscribers());
    EXPECT_FALSE(physics->beginContact2D.hasSubscribers());
    EXPECT_FALSE(physics->hitContact2D.hasSubscribers());
    EXPECT_FALSE(physics->beginSensorContact2D.hasSubscribers());
    EXPECT_FALSE(physics->onContactAdded3D.hasSubscribers());
}

TEST_F(SoundEventTriggerTest, SubscribedOnlyForWhatASourceNeedsAndGoneAfter) {
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    AudioSource sound = source();
    sound.loadSound("c0.wav");
    sound.setPlayOn(AudioPlayOn_Collision3D);
    step();
    EXPECT_TRUE(physics->onContactAdded3D.hasSubscribers());
    EXPECT_FALSE(physics->beginContact2D.hasSubscribers());

    sound.setPlayOn(AudioPlayOn_Sensor2D);
    step();
    EXPECT_FALSE(physics->onContactAdded3D.hasSubscribers());
    EXPECT_TRUE(physics->beginSensorContact2D.hasSubscribers());

    sound.setEnabled(false);
    step();
    EXPECT_FALSE(physics->beginSensorContact2D.hasSubscribers()) << "a disabled source needs nothing";
    sound.setEnabled(true);
    step();
    EXPECT_TRUE(physics->beginSensorContact2D.hasSubscribers());

    scene->getSystem<AudioSystem>()->stopSceneSounds();
    EXPECT_FALSE(physics->beginSensorContact2D.hasSubscribers()) << "Stop and teardown remove them";
}

TEST_F(SoundEventTriggerTest, UiFlagRemovedAndUsersSubscribersKept) {
    AudioSource sound = uiSource(AudioPlayOn_UIClick);
    int mine = 0;
    ui(sound).onClick.add("user", std::function<void(float, float)>([&](float, float) { mine++; }));
    step();
    sound.setPlayOn(0);
    step();
    ui(sound).onClick.call(0.0f, 0.0f);
    step();
    EXPECT_EQ(mine, 1);
    EXPECT_TRUE(starts().empty());
}

TEST_F(SoundEventTriggerTest, DroppedWhileTheScenesAudioIsPaused) {
    AudioSource sound = uiSource(AudioPlayOn_UIClick);
    step();
    scene->getSystem<AudioSystem>()->setPaused(true);
    ui(sound).onClick.call(0.0f, 0.0f);
    step();
    scene->getSystem<AudioSystem>()->setPaused(false);
    step();
    EXPECT_TRUE(starts().empty());
}

TEST_F(SoundEventTriggerTest, FlagsSetThroughTheLuaObject) {
    AudioSource sound = source();
    sound.setPlayOn(AudioPlayOn_All | 1024u);
    EXPECT_EQ(sound.getPlayOn(), (uint32_t)AudioPlayOn_All);
    sound.setMinImpactSpeed(-1.0f);
    EXPECT_FLOAT_EQ(sound.getMinImpactSpeed(), 0.0f);
    sound.setPitchRandomSemitones(30.0f);
    EXPECT_FLOAT_EQ(sound.getPitchRandomSemitones(), 24.0f);
    sound.setVolumeRandomDb(std::nanf(""));
    EXPECT_FLOAT_EQ(sound.getVolumeRandomDb(), 0.0f);
}

// ---------------------------------------------------------------- Audio.play and SoundHandle (R13, AC7)

TEST_F(SoundEventRuntimeTest, AudioPlayStartsAClipAndAHandleGoesInvalidAfterTheEnd) {
    SoundHandle handle = Audio::play("c0.wav");
    ASSERT_TRUE(handle.isValid());
    EXPECT_TRUE(handle.isPlaying());
    EXPECT_EQ(handle.getVoiceCount(), 1);
    ASSERT_EQ(starts().size(), 1u);
    EXPECT_STREQ(starts()[0].owner, "free");
    EXPECT_STREQ(starts()[0].cause, "cpp");
    run(0.7);
    EXPECT_FALSE(handle.isValid());
    EXPECT_FLOAT_EQ(handle.getVolume(), 0.0f);
    EXPECT_EQ(handle.getVoiceCount(), 0);
    // a setter of an invalid handle does nothing
    handle.setVolume(0.5f);
    handle.stop();
}

TEST_F(SoundEventRuntimeTest, AudioPlayUnknownNameIsInvalidWithOneWarning) {
    SoundHandle clip = Audio::play("gone.wav");
    SoundHandle again = Audio::play("gone.wav");
    SoundHandle event = Audio::play("gone.soundevent");
    EXPECT_FALSE(clip.isValid());
    EXPECT_FALSE(again.isValid());
    EXPECT_FALSE(event.isValid());
    EXPECT_EQ(rejects("missing_clip"), 2u);
    EXPECT_EQ(rejects("missing_event"), 1u);
}

TEST_F(SoundEventRuntimeTest, AudioPlayOptionsReachTheVoice) {
    std::string error;
    ASSERT_TRUE(AudioMixer::loadFromText(R"({"format": "jasperengine.mixer", "version": 1, "groups": [
        {"id": "00000001", "name": "Master", "parent": null},
        {"id": "5fx00001", "name": "SFX", "parent": "00000001"}]})", "m.mixer", &error)) << error;
    PlayOptions options;
    options.volume = 0.5f;
    options.pitch = 2.0f;
    options.group = "SFX";
    SoundHandle handle = Audio::play("c1.wav", options);
    ASSERT_TRUE(handle.isValid());
    ASSERT_EQ(starts().size(), 1u);
    EXPECT_FLOAT_EQ(starts()[0].gain, 0.5f);
    EXPECT_FLOAT_EQ(starts()[0].pitch, 2.0f);
    EXPECT_EQ(starts()[0].group, "5fx00001");
    EXPECT_FLOAT_EQ(handle.getVolume(), 0.5f);
    EXPECT_FLOAT_EQ(handle.getPitch(), 2.0f);
}

TEST_F(SoundEventRuntimeTest, AudioPlayGroupOverridesTheEvents) {
    std::string error;
    ASSERT_TRUE(AudioMixer::loadFromText(R"({"format": "jasperengine.mixer", "version": 1, "groups": [
        {"id": "00000001", "name": "Master", "parent": null},
        {"id": "5fx00001", "name": "SFX", "parent": "00000001"},
        {"id": "5fx00002", "name": "UI", "parent": "00000001"}]})", "m.mixer", &error)) << error;
    SoundEventDef event = def(SoundEventMode::Sequence, 1);
    event.outputGroupName = "SFX";
    const std::string name = install(event);
    Audio::play(name);
    PlayOptions options;
    options.group = "UI";
    Audio::play(name, options);
    ASSERT_EQ(starts().size(), 2u);
    EXPECT_EQ(starts()[0].group, "5fx00001");
    EXPECT_EQ(starts()[1].group, "5fx00002");
}

TEST_F(SoundEventRuntimeTest, AudioPlayOfALayeredEventHasItsVoices) {
    const std::string name = install(def(SoundEventMode::Layer, 3));
    SoundHandle handle = Audio::play(name);
    ASSERT_TRUE(handle.isValid());
    EXPECT_EQ(handle.getVoiceCount(), 3);
    handle.stop();
    EXPECT_FALSE(handle.isValid());
    EXPECT_EQ(AudioSystem::getVoiceCount(), 0u);
}

TEST_F(SoundEventRuntimeTest, AudioPlayRejectedByCooldownIsInvalid) {
    SoundEventDef event = def(SoundEventMode::Sequence, 1);
    event.cooldownMs = 1000;
    const std::string name = install(event);
    EXPECT_TRUE(Audio::play(name).isValid());
    EXPECT_FALSE(Audio::play(name).isValid());
    EXPECT_EQ(rejects("cooldown"), 1u);
}

TEST_F(SoundEventRuntimeTest, HandlePauseResume) {
    SoundHandle handle = Audio::play("c0.wav");
    step();
    const unsigned int voice = starts()[0].handle;
    handle.pause();
    step();
    EXPECT_FALSE(handle.isPlaying());
    EXPECT_TRUE(handle.isValid());
    EXPECT_TRUE(AudioSystem::getSoloud().getPause(voice));
    const double held = handle.getTime();
    run(0.1);
    EXPECT_DOUBLE_EQ(handle.getTime(), held);
    handle.resume();
    step();
    EXPECT_TRUE(handle.isPlaying());
    EXPECT_FALSE(AudioSystem::getSoloud().getPause(voice));
}

TEST_F(SoundEventRuntimeTest, HandleFadeOutStopsOrPausesAndPutsTheVolumeBack) {
    writeSoundEventTone(root / "assets" / "long.wav", 3.0);
    SoundHandle stopped = Audio::play("long.wav");
    step();
    stopped.fadeOut(0.1f);
    run(0.2);
    EXPECT_FALSE(stopped.isValid());

    SoundHandle paused = Audio::play("long.wav");
    step();
    paused.fadeOut(0.1f, true);
    run(0.05);
    EXPECT_LT(paused.getVolume(), 1.0f);
    run(0.15);
    EXPECT_TRUE(paused.isValid());
    EXPECT_FALSE(paused.isPlaying());
    EXPECT_FLOAT_EQ(paused.getVolume(), 1.0f);
    paused.resume();
    step();
    EXPECT_TRUE(paused.isPlaying());

    paused.fadeTo(0.25f, 0.1f);
    run(0.2);
    EXPECT_FLOAT_EQ(paused.getVolume(), 0.25f);
    EXPECT_TRUE(paused.isPlaying());
}

TEST_F(SoundEventRuntimeTest, HandleFollowsItsTargetStopsWhenDestroyedAndSettingPositionUnfollows) {
    const Entity cameraEntity = scene->createEntity();
    scene->addComponent<Transform>(cameraEntity, Transform());
    CameraComponent camera;
    camera.autoResize = false;
    scene->addComponent<CameraComponent>(cameraEntity, camera);
    scene->setCamera(cameraEntity);
    scene->getComponent<CameraComponent>(cameraEntity).worldTarget = Vector3(0, 0, -1);
    scene->getComponent<CameraComponent>(cameraEntity).steadyWorldUp = Vector3(0, 1, 0);

    writeSoundEventTone(root / "assets" / "long.wav", 3.0);
    AudioSource carrier(scene, true);
    carrier.getComponent<Transform>().worldPosition = Vector3(1, 2, 3);
    PlayOptions options;
    options.following(carrier);
    SoundHandle follows = Audio::play("long.wav", options);
    ASSERT_TRUE(follows.isValid());
    EXPECT_EQ(follows.getPosition(), Vector3(1, 2, 3));
    step();
    carrier.getComponent<Transform>().worldPosition = Vector3(4, 5, 6);
    step();
    EXPECT_EQ(follows.getPosition(), Vector3(4, 5, 6));

    // set: no longer following
    follows.setPosition(Vector3(7, 7, 7));
    carrier.getComponent<Transform>().worldPosition = Vector3(9, 9, 9);
    step();
    EXPECT_EQ(follows.getPosition(), Vector3(7, 7, 7));

    AudioSource other(scene, true);
    PlayOptions second;
    second.following(other);
    SoundHandle stopsWithIt = Audio::play("long.wav", second);
    ASSERT_TRUE(stopsWithIt.isValid());
    scene->destroyEntity(other.getEntity());
    step();
    EXPECT_FALSE(stopsWithIt.isValid());
    EXPECT_TRUE(follows.isValid());
}

TEST_F(SoundEventRuntimeTest, DelayedPlayStartsLaterHandleValidMeanwhile) {
    PlayOptions options;
    options.delay = 0.1f;
    SoundHandle handle = Audio::play("c0.wav", options);
    EXPECT_TRUE(handle.isValid());
    EXPECT_EQ(handle.getVoiceCount(), 0);
    step(0.05);
    EXPECT_TRUE(starts().empty());
    run(0.1, 0.05);
    ASSERT_EQ(starts().size(), 1u);
    EXPECT_EQ(handle.getVoiceCount(), 1);
}

TEST_F(SoundEventRuntimeTest, NonFiniteOptionsAreRefused) {
    PlayOptions options;
    options.volume = std::nanf("");
    EXPECT_FALSE(Audio::play("c0.wav", options).isValid());
    EXPECT_TRUE(starts().empty());
    SoundHandle handle = Audio::play("c0.wav");
    handle.setVolume(std::nanf(""));
    EXPECT_FLOAT_EQ(handle.getVolume(), 1.0f);
}

TEST_F(SoundEventRuntimeTest, StopAllCountsFreeAndOneShotsNotMainVoices) {
    std::string error;
    ASSERT_TRUE(AudioMixer::loadFromText(R"({"format": "jasperengine.mixer", "version": 1, "groups": [
        {"id": "00000001", "name": "Master", "parent": null},
        {"id": "5fx00001", "name": "SFX", "parent": "00000001"},
        {"id": "5fx00011", "name": "Steps", "parent": "5fx00001"},
        {"id": "5fx00002", "name": "UI", "parent": "00000001"}]})", "m.mixer", &error)) << error;
    AudioSource music = source();
    music.loadSound("c0.wav");
    music.setOutputGroup("SFX");
    music.play();
    step();
    PlayOptions steps;
    steps.group = "Steps";
    SoundHandle step1 = Audio::play("c1.wav", steps);
    PlayOptions sfx;
    sfx.group = "SFX";
    SoundHandle sfx1 = Audio::play("c1.wav", sfx);
    PlayOptions uiGroup;
    uiGroup.group = "UI";
    SoundHandle ui1 = Audio::play("c1.wav", uiGroup);
    music.playOneShot("c2.wav");
    step();
    EXPECT_EQ(AudioSystem::getVoiceCount(), 5u);

    EXPECT_EQ(Audio::stopAll("Nothing"), 0);
    EXPECT_EQ(Audio::stopAll("SFX"), 3) << "free on SFX and below it, and the one-shot on SFX";
    EXPECT_FALSE(step1.isValid());
    EXPECT_FALSE(sfx1.isValid());
    EXPECT_TRUE(ui1.isValid());
    EXPECT_TRUE(music.isPlaying()) << "main voices are not its";
    EXPECT_EQ(Audio::stopAll(), 1);
    EXPECT_TRUE(music.isPlaying());
}

TEST_F(SoundEventRuntimeTest, CallsInsideAPhysicsCallbackDefer) {
    SoundHandle handle;
    SoundHandle before = Audio::play("c0.wav");
    {
        AudioSystem::PhysicsCallbackScope scope;
        EXPECT_TRUE(AudioSystem::isDeferringCalls());
        handle = Audio::play("c1.wav");
        EXPECT_TRUE(handle.isValid()) << "valid at once";
        EXPECT_EQ(handle.getVoiceCount(), 0);
        EXPECT_EQ(starts().size(), 1u) << "nothing started in the callback";
        before.stop();
        EXPECT_TRUE(before.isValid()) << "a stop waits too";
        EXPECT_EQ(Audio::stopAll("x"), 0);
    }
    EXPECT_FALSE(AudioSystem::isDeferringCalls());
    step();
    EXPECT_EQ(starts().size(), 2u);
    EXPECT_EQ(handle.getVoiceCount(), 1);
    EXPECT_FALSE(before.isValid());
}

TEST_F(SoundEventRuntimeTest, ADeferredPlayThatStartsNothingGoesInvalid) {
    SoundHandle handle;
    {
        AudioSystem::PhysicsCallbackScope scope;
        handle = Audio::play("gone.wav");
        EXPECT_TRUE(handle.isValid());
    }
    step();
    EXPECT_FALSE(handle.isValid());
}

TEST_F(SoundEventRuntimeTest, SceneTeardownEndsItsFreeVoices) {
    SoundHandle handle = Audio::play("c0.wav");
    ASSERT_TRUE(handle.isValid());
    scene->getSystem<AudioSystem>()->stopSceneSounds();
    EXPECT_FALSE(handle.isValid());
    EXPECT_EQ(AudioSystem::getVoiceCount(), 0u);
}
