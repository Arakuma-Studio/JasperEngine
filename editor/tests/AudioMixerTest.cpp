// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The audio mixer (spec docs/superpowers/specs/2026-10-31-audio-mixer-design.md 14): the .mixer
// format (AudioMixerIOTest), the dB / solo / snapshot math (AudioMixerMathTest), the runtime on
// SoLoud's null backend, mixed by AudioMixer::update so every result is exact and needs no audio
// device (AudioMixerRuntimeTest), the Audio Source voices, events, fades, pause and budgets
// (AudioSourceTest), the Lua surface (AudioLuaTest) and the exported init (GeneratorAudioTest).

#include <gtest/gtest.h>

#include "ScriptDataTestSupport.h"

#include "Generator.h"
#include "Scene.h"
#include "audio/AudioMixer.h"
#include "audio/AudioMixerIO.h"
#include "io/UserSettings.h"
#include "object/sound/AudioSource.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include "LuaBridge.h"
#include "json.hpp"
#include "soloud.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    namespace fs = std::filesystem;

    // the example of spec 7.4 (the game's mixer)
    const char* exampleMixer = R"({
  "format": "jasperengine.mixer", "version": 1, "updateMode": "scaled",
  "groups": [
    {"id": "00000001", "name": "Master", "parent": null, "volume": 0, "effects": [{"id": "e1a2b3c4", "type": "limiter"}]},
    {"id": "9b51c0de", "name": "Music", "parent": "00000001", "volume": 0, "mute": false, "solo": false,
     "bypassEffects": false, "effects": [
       {"id": "c4f00d01", "type": "lowpass", "params": {"cutoff": 22000, "resonance": 1}},
       {"id": "c4f00d02", "type": "highpass", "params": {"cutoff": 10, "resonance": 1}}]},
    {"id": "5fx00001", "name": "SFX", "parent": "00000001"}
  ],
  "exposed": [
    {"name": "MusicVolume", "target": "9b51c0de/volume", "remember": true},
    {"name": "lowPassCutoff", "target": "c4f00d01/cutoff"}
  ],
  "snapshots": [{"name": "Normal", "values": {"9b51c0de/volume": 0, "c4f00d01/cutoff": 22000}}],
  "startSnapshot": "Normal"
})";

    AudioMixerDef parseOk(const std::string& text) {
        AudioMixerDef def;
        std::string error;
        EXPECT_TRUE(AudioMixerIO::parse(text, def, &error)) << error;
        return def;
    }

    std::string parseError(const std::string& text) {
        AudioMixerDef def;
        std::string error;
        EXPECT_FALSE(AudioMixerIO::parse(text, def, &error));
        return error;
    }

    // a 16-bit mono PCM WAV of a sine
    void writeTone(const fs::path& path, double seconds, double frequency = 1000.0, double amplitude = 0.5, int rate = 44100) {
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

    // Master > Music (low-pass, exposed volume and cutoff) and SFX, on the null backend
    class AudioFixture : public scriptdata::ScriptDataTest {
    protected:
        Scene* scene = nullptr;

        void SetUp() override {
            ScriptDataTest::SetUp();
            AudioSystem::close();
            AudioSystem::setBackend(AudioBackend::Null);
            AudioSystem::configure(AudioConfig());
            AudioSystem::setListenerPaused(false);
            AudioMixer::setMasterPaused(false);
            AudioMixer::unload();
            SoundPool::clear();
            writeTone(root / "assets" / "tone.wav", 0.5);
            scene = new Scene();
        }

        void TearDown() override {
            delete scene;
            scene = nullptr;
            AudioSystem::close();
            AudioMixer::unload();
            AudioSystem::setListenerPaused(false);
            AudioMixer::setMasterPaused(false);
            SoundPool::clear();
            ScriptDataTest::TearDown();
        }

        void loadMixer(const std::string& text) {
            std::string error;
            ASSERT_TRUE(AudioMixer::loadFromText(text, "test.mixer", &error)) << error;
        }

        void loadMusicMixer(float cutoff = 22000.0f, bool remember = false) {
            nlohmann::json json = nlohmann::json::parse(exampleMixer);
            json["groups"][1]["effects"][0]["params"]["cutoff"] = cutoff;
            json["groups"][0].erase("effects");
            json["exposed"][0]["remember"] = remember;
            json.erase("snapshots");
            json.erase("startSnapshot");
            loadMixer(json.dump());
        }

        // one frame: the mixer mixes `dt` of audio, the scene's system updates
        void step(double dt, double scaled = -1.0) {
            AudioMixer::update(scaled < 0.0 ? dt : scaled, dt);
            scene->getSystem<AudioSystem>()->update(scaled < 0.0 ? dt : scaled);
        }

        void run(double seconds, double dt = 0.02) {
            for (double t = 0.0; t < seconds - 1e-9; t += dt) step(dt);
        }

        AudioSource source(const std::string& file = "tone.wav", const std::string& group = "") {
            AudioSource sound(scene);
            sound.loadSound(file);
            if (!group.empty()) sound.setOutputGroup(group);
            return sound;
        }

        SoLoud::Soloud& soloud() { return AudioSystem::getSoloud(); }
    };

}

// ================================================================== AudioMixerIOTest

TEST(AudioMixerIOTest, TheSpecExampleReadsAndWritesBackTheSame) {
    const AudioMixerDef def = parseOk(exampleMixer);
    ASSERT_EQ(def.groups.size(), 3u);
    EXPECT_EQ(def.master()->name, "Master");
    EXPECT_EQ(def.findGroupByName("Music")->effects.size(), 2u);
    EXPECT_EQ(def.findGroupByName("Music")->effects[0].type, AudioEffectType::LowPass);
    EXPECT_FLOAT_EQ(def.findGroupByName("Music")->effects[0].params.at("cutoff"), 22000.0f);
    ASSERT_EQ(def.exposed.size(), 2u);
    EXPECT_TRUE(def.exposed[0].remember);
    EXPECT_EQ(def.startSnapshot, "Normal");

    const std::string written = AudioMixerIO::write(def);
    const AudioMixerDef again = parseOk(written);
    EXPECT_EQ(AudioMixerIO::write(again), written) << "stable";

    // keys in the spec's order, 2-space indent
    EXPECT_EQ(written.rfind("{\n  \"format\": \"jasperengine.mixer\",\n  \"version\": 1,\n  \"updateMode\": \"scaled\",\n  \"groups\"", 0), 0u) << written;
    EXPECT_LT(written.find("\"groups\""), written.find("\"exposed\""));
    EXPECT_LT(written.find("\"exposed\""), written.find("\"snapshots\""));
    EXPECT_LT(written.find("\"snapshots\""), written.find("\"startSnapshot\""));
    EXPECT_LT(written.find("\"id\": \"9b51c0de\""), written.find("\"name\": \"Music\""));
}

TEST(AudioMixerIOTest, OmittedFieldsTakeTheirDefaults) {
    const AudioMixerDef def = parseOk(R"({"format": "jasperengine.mixer", "groups": [
        {"id": "aaaaaaaa", "name": "Master"},
        {"id": "bbbbbbbb", "name": "FX", "parent": "aaaaaaaa", "effects": [{"id": "cccccccc", "type": "echo"}]}]})");
    EXPECT_EQ(def.version, 1);
    EXPECT_FALSE(def.unscaled);
    const AudioMixerGroupDef* fx = def.findGroupByName("FX");
    ASSERT_NE(fx, nullptr);
    EXPECT_FLOAT_EQ(fx->volumeDb, 0.0f);
    EXPECT_FALSE(fx->mute);
    EXPECT_FALSE(fx->solo);
    EXPECT_FLOAT_EQ(fx->effects[0].wet, 1.0f);
    float delay = 0.0f;
    ASSERT_TRUE(AudioMixerIO::authoredValue(def, "cccccccc/delay", delay));
    EXPECT_FLOAT_EQ(delay, 0.3f) << "the table's default";
    // defaults are left out of the file
    const std::string written = AudioMixerIO::write(def);
    EXPECT_EQ(written.find("\"mute\""), std::string::npos);
    EXPECT_EQ(written.find("\"wet\""), std::string::npos);
    EXPECT_EQ(written.find("\"params\""), std::string::npos);
}

TEST(AudioMixerIOTest, InvalidFilesSayWhereAndWhy) {
    EXPECT_NE(parseError("{nope").find("not valid JSON"), std::string::npos);
    EXPECT_NE(parseError(R"({"format": "x", "groups": []})").find("format"), std::string::npos);
    EXPECT_NE(parseError(R"({"format": "jasperengine.mixer", "groups": [
        {"id": "aaaaaaaa", "name": "Master"}, {"id": "bbbbbbbb", "name": "Other"}]})").find("only Master has no parent"), std::string::npos);
    EXPECT_NE(parseError(R"({"format": "jasperengine.mixer", "groups": [
        {"id": "aaaaaaaa", "name": "Master"}, {"id": "bbbbbbbb", "name": "Master", "parent": "aaaaaaaa"}]})").find("groups[1].name"), std::string::npos);
    EXPECT_NE(parseError(R"({"format": "jasperengine.mixer", "groups": [{"id": "aaaaaaaa", "name": "Master"}],
        "exposed": [{"name": "X", "target": "zzzzzzzz/volume"}]})").find("exposed[0].target"), std::string::npos);
    EXPECT_NE(parseError(R"({"format": "jasperengine.mixer", "groups": [{"id": "aaaaaaaa", "name": "Master",
        "effects": [{"id": "e", "type": "lowpass", "params": {"q": 1}}]}]})").find("lowpass has no such parameter"), std::string::npos);
    EXPECT_NE(parseError(R"({"format": "jasperengine.mixer", "groups": [{"id": "aaaaaaaa", "name": "Master"}],
        "startSnapshot": "Nope"})").find("startSnapshot"), std::string::npos);
    EXPECT_NE(parseError(R"({"format": "jasperengine.mixer", "groups": [
        {"id": "aaaaaaaa", "name": "Master"}, {"id": "bbbbbbbb", "name": "A", "parent": "cccccccc"}]})").find("groups[1].parent"), std::string::npos);
}

TEST(AudioMixerIOTest, UnknownEffectsAreKeptAndInactive) {
    std::vector<std::string> warnings;
    AudioMixerDef def;
    std::string error;
    ASSERT_TRUE(AudioMixerIO::parse(R"({"format": "jasperengine.mixer", "groups": [{"id": "aaaaaaaa", "name": "Master",
        "effects": [{"id": "eeeeeeee", "type": "pitchshifter", "params": {"pitch": 1.5}}]}]})", def, &error, &warnings)) << error;
    ASSERT_EQ(def.groups[0].effects.size(), 1u);
    EXPECT_EQ(def.groups[0].effects[0].type, AudioEffectType::Unknown);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("pitchshifter"), std::string::npos);
    const std::string written = AudioMixerIO::write(def);
    EXPECT_NE(written.find("\"pitchshifter\""), std::string::npos);
    EXPECT_NE(written.find("\"pitch\": 1.5"), std::string::npos) << "round trip";
}

// ================================================================== AudioMixerMathTest

TEST(AudioMixerMathTest, DecibelsAndLinear) {
    EXPECT_FLOAT_EQ(AudioMath::dbToLinear(0.0f), 1.0f);
    EXPECT_NEAR(AudioMath::dbToLinear(-6.02f), 0.5f, 0.001f);
    EXPECT_FLOAT_EQ(AudioMath::dbToLinear(-80.0f), 0.0f);
    EXPECT_FLOAT_EQ(AudioMath::dbToLinear(-120.0f), 0.0f);
    EXPECT_NEAR(AudioMath::linearToDb(0.5f), -6.02f, 0.01f);
    EXPECT_FLOAT_EQ(AudioMath::linearToDb(0.0f), -80.0f);
    EXPECT_FLOAT_EQ(AudioMixer::linearToDb(1.0f), 0.0f);
}

TEST(AudioMixerMathTest, TransitionCurves) {
    // volumes linearly in dB, frequencies along log2: the LP sweep's midpoint is the geometric mean
    EXPECT_FLOAT_EQ(AudioMath::interpolate(AudioParamCurve::Decibel, -80.0f, 0.0f, 0.5f), -40.0f);
    EXPECT_NEAR(AudioMath::interpolate(AudioParamCurve::Frequency, 22000.0f, 525.0f, 0.5f), 3398.5f, 1.0f);
    EXPECT_FLOAT_EQ(AudioMath::interpolate(AudioParamCurve::Linear, 0.0f, 1.0f, 0.25f), 0.25f);
    // weighted blends, normalized
    EXPECT_FLOAT_EQ(AudioMath::blend(AudioParamCurve::Decibel, {0.0f, -20.0f}, {3.0f, 1.0f}), -5.0f);
    EXPECT_NEAR(AudioMath::blend(AudioParamCurve::Frequency, {100.0f, 10000.0f}, {1.0f, 1.0f}), 1000.0f, 0.5f);
    EXPECT_FLOAT_EQ(AudioMath::blend(AudioParamCurve::Linear, {2.0f, 4.0f}, {0.0f, 0.0f}), 2.0f) << "no weight: the first";
}

TEST_F(AudioFixture, SoloFollowsUnityOnAThreeLevelTree) {
    loadMixer(R"({"format": "jasperengine.mixer", "groups": [
        {"id": "00000001", "name": "Master"},
        {"id": "00000002", "name": "Music", "parent": "00000001"},
        {"id": "00000003", "name": "Drums", "parent": "00000002"},
        {"id": "00000004", "name": "SFX", "parent": "00000001"},
        {"id": "00000005", "name": "UI", "parent": "00000004"}]})");
    for (const char* group : {"Master", "Music", "Drums", "SFX", "UI"}) EXPECT_TRUE(AudioMixer::isGroupAudible(group)) << group;
    AudioMixer::setGroupSolo("Music", true);
    EXPECT_TRUE(AudioMixer::isGroupAudible("Master")) << "an ancestor of the soloed group";
    EXPECT_TRUE(AudioMixer::isGroupAudible("Music"));
    EXPECT_TRUE(AudioMixer::isGroupAudible("Drums")) << "below the soloed group";
    EXPECT_FALSE(AudioMixer::isGroupAudible("SFX"));
    EXPECT_FALSE(AudioMixer::isGroupAudible("UI"));
    AudioMixer::setGroupSolo("UI", true);
    EXPECT_TRUE(AudioMixer::isGroupAudible("SFX")) << "above a soloed group";
    AudioMixer::setGroupMute("Drums", true);
    EXPECT_FALSE(AudioMixer::isGroupAudible("Drums")) << "mute wins";
}

TEST_F(AudioFixture, SetFloatOverridesSnapshotsUntilCleared) {
    loadMixer(R"({"format": "jasperengine.mixer", "groups": [
        {"id": "00000001", "name": "Master"},
        {"id": "00000002", "name": "Music", "parent": "00000001", "effects": [{"id": "0000000a", "type": "lowpass", "params": {"cutoff": 22000}}]}],
      "exposed": [{"name": "MusicVolume", "target": "00000002/volume"}, {"name": "lowPassCutoff", "target": "0000000a/cutoff"}],
      "snapshots": [{"name": "Menu", "values": {"0000000a/cutoff": 22000, "00000002/volume": 0}},
                    {"name": "Gameplay", "values": {"0000000a/cutoff": 525, "00000002/volume": -10}}],
      "startSnapshot": "Menu"})");
    float value = 0.0f;
    ASSERT_TRUE(AudioMixer::getFloat("lowPassCutoff", value));
    EXPECT_FLOAT_EQ(value, 22000.0f);
    EXPECT_EQ(AudioMixer::getCurrentSnapshot(), "Menu");

    ASSERT_TRUE(AudioMixer::transitionTo("Gameplay", 1.0f));
    AudioMixer::update(0.5, 0.5);
    ASSERT_TRUE(AudioMixer::getFloat("lowPassCutoff", value));
    EXPECT_NEAR(value, 3398.5f, 2.0f) << "half way along log2";
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_NEAR(value, -5.0f, 0.01f) << "half way in dB";

    // a script value wins over the transition and stays
    ASSERT_TRUE(AudioMixer::setFloat("MusicVolume", -30.0f));
    AudioMixer::update(1.0, 1.0);
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_FLOAT_EQ(value, -30.0f);
    EXPECT_TRUE(AudioMixer::isOverridden("MusicVolume"));
    // cleared: the snapshot's value at once
    ASSERT_TRUE(AudioMixer::clearFloat("MusicVolume"));
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_FLOAT_EQ(value, -10.0f);
    ASSERT_TRUE(AudioMixer::getFloat("lowPassCutoff", value));
    EXPECT_FLOAT_EQ(value, 525.0f);

    // a weighted blend
    ASSERT_TRUE(AudioMixer::transitionToSnapshots({"Menu", "Gameplay"}, {1.0f, 1.0f}, 0.0f));
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_FLOAT_EQ(value, -5.0f);
    EXPECT_EQ(AudioMixer::getCurrentSnapshot(), "") << "a blend is no one snapshot";

    // unknown names: false
    EXPECT_FALSE(AudioMixer::setFloat("Nope", 1.0f));
    EXPECT_FALSE(AudioMixer::getFloat("Nope", value));
    EXPECT_FALSE(AudioMixer::transitionTo("Nope", 1.0f));
}

TEST_F(AudioFixture, TimedChangesRunOnScaledTimeUnlessAskedOrTheMixerSaysUnscaled) {
    loadMusicMixer();
    float value = 0.0f;
    ASSERT_TRUE(AudioMixer::setFloat("MusicVolume", -20.0f, 1.0f));
    AudioMixer::update(0.0, 0.5);   // time scale 0
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_FLOAT_EQ(value, 0.0f) << "scaled: frozen";
    ASSERT_TRUE(AudioMixer::setFloat("MusicVolume", -20.0f, 1.0f, true));
    AudioMixer::update(0.0, 0.5);
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_NEAR(value, -10.0f, 0.01f) << "unscaled: moves";

    AudioMixerDef def = AudioMixer::getDef();
    def.unscaled = true;
    AudioMixer::apply(def);
    ASSERT_TRUE(AudioMixer::setFloat("MusicVolume", -40.0f, 1.0f));
    AudioMixer::update(0.0, 1.0);
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_FLOAT_EQ(value, -40.0f) << "updateMode unscaled";
}

// ================================================================== AudioMixerRuntimeTest

TEST_F(AudioFixture, NoMixerIsAnImplicitMasterAtZeroDecibels) {
    EXPECT_FALSE(AudioMixer::hasAsset());
    ASSERT_EQ(AudioMixer::getGroupNames(), std::vector<std::string>{"Master"});
    AudioSource sound = source();
    sound.setLooping(true);
    sound.play();
    run(0.2);
    EXPECT_TRUE(sound.isPlaying());
    EXPECT_GT(AudioMixer::getMeterDb("Master"), -20.0f);
    EXPECT_FALSE(AudioSystem::isOutputAvailable()) << "the null backend";
}

TEST_F(AudioFixture, MetersVolumesMuteAndLowPass) {
    loadMusicMixer();
    AudioSource music = source("tone.wav", "Music");
    music.setLooping(true);
    music.play();
    run(0.2);
    const float open = AudioMixer::getMeterDb("Music");
    EXPECT_GT(open, -20.0f) << "a 1 kHz tone on Music within 0.2 s";
    EXPECT_LT(AudioMixer::getMeterDb("SFX"), -79.0f);
    EXPECT_NEAR(AudioMixer::getMeterDb("Master"), open, 1.0f);

    // the A5 bypass: the 22000 Hz low-pass leaves the tone as it is
    AudioMixerDef def = AudioMixer::getDef();
    def.findGroup("Music")->effects.clear();
    AudioMixer::apply(def);
    run(0.2);
    EXPECT_NEAR(AudioMixer::getMeterDb("Music"), open, 1.0f);

    // a low-pass at 200 Hz takes more than 12 dB off a 1 kHz tone
    def = AudioMixer::getDef();
    AudioMixerEffectDef lowpass;
    lowpass.id = "0000beef";
    lowpass.type = AudioEffectType::LowPass;
    lowpass.params["cutoff"] = 200.0f;
    def.findGroup("Music")->effects.push_back(lowpass);
    AudioMixer::apply(def);
    run(0.3);
    EXPECT_LT(AudioMixer::getMeterDb("Music"), open - 12.0f);
    // and an exposed cutoff set by a script opens it again
    def = AudioMixer::getDef();
    def.exposed = {{"cut", "0000beef/cutoff", false}};
    AudioMixer::apply(def);
    ASSERT_TRUE(AudioMixer::setFloat("cut", 22000.0f));
    run(0.2);
    EXPECT_NEAR(AudioMixer::getMeterDb("Music"), open, 1.0f);

    // mute
    AudioMixer::setGroupMute("Music", true);
    run(0.1);
    EXPECT_LT(AudioMixer::getMeterDb("Music"), -79.0f);
    EXPECT_TRUE(music.isPlaying()) << "a muted group's voices go on";
    AudioMixer::setGroupMute("Music", false);
    run(0.1);
    EXPECT_GT(AudioMixer::getMeterDb("Music"), -20.0f);

    // a timed fade to -80 dB over 0.5 s
    ASSERT_TRUE(AudioMixer::setFloat("MusicVolume", -80.0f, 0.5f));
    run(0.25);
    EXPECT_LT(AudioMixer::getMeterDb("Music"), open - 20.0f);
    EXPECT_GT(AudioMixer::getMeterDb("Music"), -79.0f) << "half way";
    run(0.3);
    float value = 0.0f;
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_FLOAT_EQ(value, -80.0f);
    run(0.1);
    EXPECT_LT(AudioMixer::getMeterDb("Music"), -79.0f);
}

TEST_F(AudioFixture, RememberedParametersComeBackWhenTheMixerLoads) {
    loadMusicMixer(22000.0f, true);
    ASSERT_TRUE(AudioMixer::setFloat("MusicVolume", -80.0f));
    EXPECT_FALSE(UserSettings::hasKey("audio.mixer.MusicVolume")) << "written at the end of the frame";
    AudioMixer::update(0.0, 0.0);
    ASSERT_TRUE(UserSettings::hasKey("audio.mixer.MusicVolume"));
    EXPECT_FLOAT_EQ(UserSettings::getFloatForKey("audio.mixer.MusicVolume"), -80.0f);
    EXPECT_FALSE(UserSettings::hasKey("audio.mixer.lowPassCutoff")) << "not remembered";

    // the next launch
    AudioMixer::unload();
    loadMusicMixer(22000.0f, true);
    float value = 0.0f;
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", value));
    EXPECT_FLOAT_EQ(value, -80.0f);
    EXPECT_TRUE(AudioMixer::isOverridden("MusicVolume"));

    // clearFloat forgets it
    ASSERT_TRUE(AudioMixer::clearFloat("MusicVolume"));
    EXPECT_FALSE(UserSettings::hasKey("audio.mixer.MusicVolume"));
}

TEST_F(AudioFixture, ARemovedGroupsSourcesGoOnInMaster) {
    loadMusicMixer();
    AudioSource music = source("tone.wav", "Music");
    music.setLooping(true);
    music.play();
    run(0.2);
    EXPECT_GT(AudioMixer::getMeterDb("Music"), -20.0f);
    const unsigned int handle = music.getComponent<AudioSourceComponent>().handle;

    AudioMixerDef def = AudioMixer::getDef();
    def.exposed.clear();
    def.groups.erase(std::remove_if(def.groups.begin(), def.groups.end(), [](const AudioMixerGroupDef& g) { return g.name == "Music"; }), def.groups.end());
    AudioMixer::apply(def);
    run(0.2);
    EXPECT_TRUE(soloud().isValidVoiceHandle(handle)) << "the running voice keeps playing";
    EXPECT_EQ(music.getComponent<AudioSourceComponent>().handle, handle);
    EXPECT_GT(AudioMixer::getMeterDb("Master"), -20.0f) << "through Master";
    EXPECT_EQ(music.getOutputGroup(), "Music") << "the source keeps its name (a group of that name comes back)";
}

TEST_F(AudioFixture, AppPauseHoldsTheMasterBusAndKeepsAGamePausedSourcePaused) {
    AudioSource a = source();
    AudioSource b = source();
    a.setLooping(true);
    b.setLooping(true);
    a.play();
    b.play();
    run(0.1);
    a.pause();
    run(0.05);
    const unsigned int ha = a.getComponent<AudioSourceComponent>().handle;
    const unsigned int hb = b.getComponent<AudioSourceComponent>().handle;
    ASSERT_TRUE(soloud().getPause(ha));
    ASSERT_FALSE(soloud().getPause(hb));

    AudioSystem::pauseAll();
    const double before = soloud().getStreamPosition(hb);
    run(0.2);
    EXPECT_DOUBLE_EQ(soloud().getStreamPosition(hb), before) << "nothing routed advances";
    AudioSystem::resumeAll();
    run(0.1);
    EXPECT_TRUE(soloud().getPause(ha)) << "the game paused it: it stays paused";
    EXPECT_GT(soloud().getStreamPosition(hb), before);
    EXPECT_TRUE(a.isPaused());
}

// ================================================================== AudioSourceTest

TEST_F(AudioFixture, OneClipPlaysFromManySourcesAtOnceAndPlayRestarts) {
    AudioSource a = source();
    AudioSource b = source();
    a.play();
    b.play();
    run(0.05);
    const unsigned int ha = a.getComponent<AudioSourceComponent>().handle;
    const unsigned int hb = b.getComponent<AudioSourceComponent>().handle;
    ASSERT_NE(ha, 0u);
    ASSERT_NE(hb, 0u);
    EXPECT_TRUE(soloud().isValidVoiceHandle(ha)) << "the second play of the clip does not stop the first";
    EXPECT_TRUE(soloud().isValidVoiceHandle(hb));

    a.play();
    run(0.02);
    const unsigned int restarted = a.getComponent<AudioSourceComponent>().handle;
    EXPECT_NE(restarted, ha);
    EXPECT_FALSE(soloud().isValidVoiceHandle(ha)) << "play() on a playing source starts it over";
    EXPECT_TRUE(soloud().isValidVoiceHandle(hb));
}

TEST_F(AudioFixture, OneShotsOverlapUpToMaxVoicesTheOldestStolen) {
    AudioSource sfx = source();
    for (int i = 0; i < 10; i++) {
        sfx.playOneShot("tone.wav", 0.5f);
        step(0.01);
    }
    const AudioSourceComponent& audio = sfx.getComponent<AudioSourceComponent>();
    EXPECT_EQ(sfx.getVoiceCount(), 8) << "maxVoices 8";
    ASSERT_EQ(audio.oneShots.size(), 8u);
    for (size_t i = 1; i < audio.oneShots.size(); i++) EXPECT_LT(audio.oneShots[i - 1].serial, audio.oneShots[i].serial);
    EXPECT_TRUE(sfx.isPlaying()) << "one-shots count";
    sfx.stop();
    step(0.01);
    EXPECT_EQ(sfx.getVoiceCount(), 0) << "stop() stops one-shots too";
}

TEST_F(AudioFixture, TheGlobalBudgetStealsTheLeastImportantVoice) {
    AudioConfig config;
    config.maxVoices = 2;
    AudioSystem::configure(config);
    AudioSource low = source();
    low.setPriority(200);
    low.setLooping(true);
    AudioSource mid = source();
    mid.setPriority(128);
    mid.setLooping(true);
    low.play();
    mid.play();
    run(0.05);
    ASSERT_EQ(AudioSystem::getVoiceCount(), 2u);

    // less important than both: does not start
    AudioSource least = source();
    least.setPriority(250);
    least.play();
    run(0.05);
    EXPECT_FALSE(least.isPlaying());
    EXPECT_TRUE(low.isPlaying());

    // more important: the least important one (200) stops
    AudioSource high = source();
    high.setPriority(0);
    high.setLooping(true);
    high.play();
    run(0.05);
    EXPECT_TRUE(high.isPlaying());
    EXPECT_TRUE(mid.isPlaying());
    EXPECT_FALSE(low.isPlaying());
    EXPECT_TRUE(low.isStopped());
}

TEST_F(AudioFixture, IsPlayingFollowsTheVoiceForLoopsAndPitch) {
    writeTone(root / "assets" / "short.wav", 0.1);
    AudioSource loop = source("short.wav");
    loop.setLooping(true);
    loop.setPitch(1.5f);
    EXPECT_FALSE(loop.isPlaying());
    loop.play();
    EXPECT_TRUE(loop.isPlaying()) << "right after play()";
    run(0.35);
    EXPECT_TRUE(loop.isPlaying()) << "after more than 3 lengths";
    EXPECT_LT(loop.getPlayingTime(), 0.1 + 1e-6) << "the position in the clip";
    EXPECT_FLOAT_EQ(loop.getSpeed(), 1.5f) << "pitch is speed";

    AudioSource once = source("short.wav");
    once.setPitch(2.0f);
    once.play();
    run(0.1);
    EXPECT_FALSE(once.isPlaying()) << "0.1 s at pitch 2 ends in 0.05 s";
    EXPECT_TRUE(once.isStopped());
}

TEST_F(AudioFixture, EventsFireOnStartPauseStopAndTheNaturalEnd) {
    writeTone(root / "assets" / "short.wav", 0.1);
    AudioSource sound = source("short.wav");
    std::vector<std::string> events;
    AudioSourceComponent& audio = sound.getComponent<AudioSourceComponent>();
    audio.onStart.add("test", std::function<void()>([&events]() { events.push_back("start"); }));
    audio.onPause.add("test", std::function<void()>([&events]() { events.push_back("pause"); }));
    audio.onStop.add("test", std::function<void()>([&events]() { events.push_back("stop"); }));

    sound.play();
    step(0.02);
    sound.pause();
    step(0.02);
    sound.play();
    step(0.02);
    sound.stop();
    step(0.02);
    EXPECT_EQ(events, (std::vector<std::string>{"start", "pause", "start", "stop"}));

    events.clear();
    sound.play();
    run(0.2);
    EXPECT_EQ(events, (std::vector<std::string>{"start", "stop"})) << "the natural end";
}

TEST_F(AudioFixture, FadesMoveTheVolumeAndFadeOutStopsOrPauses) {
    AudioSource sound = source();
    sound.setLooping(true);
    sound.setVolume(0.8);
    sound.fadeIn(0.2);
    step(0.0);
    EXPECT_TRUE(sound.isFading());
    run(0.1);
    EXPECT_NEAR(sound.getVolume(), 0.4, 0.05);
    run(0.12);
    EXPECT_FALSE(sound.isFading());
    EXPECT_NEAR(sound.getVolume(), 0.8, 1e-6);

    sound.fadeOut(0.2);
    run(0.3);
    EXPECT_TRUE(sound.isStopped());
    EXPECT_NEAR(sound.getVolume(), 0.8, 1e-6) << "the authored level for the next play()";

    sound.play();
    run(0.05);
    sound.fadeOut(0.1, true);
    run(0.2);
    EXPECT_TRUE(sound.isPaused());
    EXPECT_NEAR(sound.getVolume(), 0.8, 1e-6);

    // scaled time by default: frozen at time scale 0; unscaled moves on
    sound.play();
    run(0.05);
    sound.fadeTo(0.2, 0.2);
    for (int i = 0; i < 10; i++) step(0.05, 0.0);
    EXPECT_NEAR(sound.getVolume(), 0.8, 1e-6);
    sound.fadeTo(0.2, 0.2, true);
    for (int i = 0; i < 10; i++) step(0.05, 0.0);
    EXPECT_NEAR(sound.getVolume(), 0.2, 1e-6);
}

TEST_F(AudioFixture, ListenerPauseSparesIgnoreListenerPauseSources) {
    AudioSource game = source();
    AudioSource click = source();
    game.setLooping(true);
    click.setLooping(true);
    click.setIgnoreListenerPause(true);
    game.play();
    click.play();
    run(0.05);
    AudioSystem::setListenerPaused(true);
    run(0.05);
    EXPECT_TRUE(soloud().getPause(game.getComponent<AudioSourceComponent>().handle));
    EXPECT_FALSE(soloud().getPause(click.getComponent<AudioSourceComponent>().handle));
    EXPECT_TRUE(game.isPlaying()) << "Unity: still playing, the listener holds it";
    AudioSystem::setListenerPaused(false);
    run(0.05);
    EXPECT_FALSE(soloud().getPause(game.getComponent<AudioSourceComponent>().handle));
}

TEST_F(AudioFixture, AutoStreamsBigFilesAndDecodesSmallOnes) {
    writeTone(root / "assets" / "music.wav", 12.0);   // ~1 MiB of 16-bit mono
    ASSERT_GE(fs::file_size(root / "assets" / "music.wav"), SoundPool::streamingThreshold);
    EXPECT_EQ(SoundPool::resolveLoadType("music.wav", AudioLoadType::Auto), AudioLoadType::Streaming);
    EXPECT_EQ(SoundPool::resolveLoadType("tone.wav", AudioLoadType::Auto), AudioLoadType::DecompressOnLoad);
    EXPECT_EQ(SoundPool::resolveLoadType("tone.wav", AudioLoadType::Streaming), AudioLoadType::Streaming);

    AudioSource music = source("music.wav");
    music.play();
    AudioSource sfx = source("tone.wav");
    sfx.play();
    run(0.1);
    EXPECT_TRUE(music.isPlaying());
    EXPECT_EQ(music.getResolvedLoadType(), AudioLoadType::Streaming);
    bool foundMusic = false;
    bool foundSfx = false;
    for (const SoundPoolEntry& entry : SoundPool::getEntries()) {
        if (entry.file == "music.wav") {
            foundMusic = true;
            EXPECT_EQ(entry.loadType, AudioLoadType::Streaming);
            EXPECT_EQ(entry.memoryBytes, 0u) << "streamed: memory = the file, read as it plays";
        }
        if (entry.file == "tone.wav") {
            foundSfx = true;
            EXPECT_EQ(entry.loadType, AudioLoadType::DecompressOnLoad);
            EXPECT_EQ(entry.memoryBytes, (size_t)(0.5 * 44100) * sizeof(float));
        }
    }
    EXPECT_TRUE(foundMusic);
    EXPECT_TRUE(foundSfx);

    // a streamed clip plays as one-shots too, each voice reading on its own
    AudioSource shots = source("music.wav");
    shots.setLoadType(AudioLoadType::Streaming);
    shots.playOneShot("music.wav");
    shots.playOneShot("music.wav");
    run(0.1);
    EXPECT_EQ(shots.getVoiceCount(), 2);
}

TEST_F(AudioFixture, PlayClipAtPointBelongsToItsScene) {
    loadMusicMixer();
    AudioSource::playClipAtPoint(scene, "tone.wav", Vector3(0, 0, 0), 1.0f, "SFX");
    run(0.1);
    EXPECT_EQ(AudioSystem::getVoiceCount(), 1u);
    EXPECT_GT(AudioMixer::getMeterDb("SFX"), -20.0f);
    scene->getSystem<AudioSystem>()->stopSceneSounds();
    EXPECT_EQ(AudioSystem::getVoiceCount(), 0u);
}

TEST_F(AudioFixture, MovedVoicesFollowTheirSourceToAnotherScene) {
    AudioSource sound = source();
    sound.setLooping(true);
    sound.play();
    run(0.05);
    AudioSourceComponent& from = sound.getComponent<AudioSourceComponent>();
    const unsigned int handle = from.handle;

    Scene other;
    Entity entity = other.createEntity();
    other.addComponent<AudioSourceComponent>(entity, from);
    AudioSourceComponent& to = other.getComponent<AudioSourceComponent>(entity);
    AudioSystem::moveSourceVoices(scene, from, &other, to);
    EXPECT_EQ(to.handle, handle);
    EXPECT_EQ(from.handle, 0u);
    scene->getSystem<AudioSystem>()->stopSceneSounds();
    EXPECT_TRUE(soloud().isValidVoiceHandle(handle)) << "the old scene stops nothing of it";
    other.getSystem<AudioSystem>()->update(0.02);
    EXPECT_TRUE(soloud().isValidVoiceHandle(handle));
    other.getSystem<AudioSystem>()->stopSceneSounds();
    EXPECT_FALSE(soloud().isValidVoiceHandle(handle));
}

// ================================================================== AudioLuaTest

TEST_F(AudioFixture, LuaDrivesTheMixerAndSources) {
    loadMusicMixer();
    ASSERT_EQ(luaL_dostring(L,
        "assert(AudioMixer.setFloat('MusicVolume', -12) == true)\n"
        "assert(math.abs(AudioMixer.getFloat('MusicVolume') + 12) < 1e-4)\n"
        "assert(AudioMixer.getFloat('Nope') == nil)\n"
        "assert(AudioMixer.setFloat('lowPassCutoff', 525, 0.9, {unscaled = true}) == true)\n"
        "local music = AudioMixer.getGroup('Music')\n"
        "assert(music.name == 'Music')\n"
        "assert(math.abs(music.volume + 12) < 1e-4)\n"
        "assert(music:getEffectCount() == 2)\n"
        "local lp = music:getEffect(1)\n"
        "assert(lp.type == 'lowpass')\n"
        "assert(music:getEffect('highpass').type == 'highpass')\n"
        "assert(lp:setParameter('resonance', 2) == true)\n"
        "assert(lp:getParameter('resonance') == 2)\n"
        "assert(lp:setParameter('nope', 2) == false)\n"
        "assert(AudioMixer.getGroup('Nope') == nil)\n"
        "music.mute = true\n"
        "assert(music.mute == true)\n"
        "assert(#AudioMixer.getGroupNames() == 3)\n"
        "assert(AudioMixer.getParameterNames()[1] == 'MusicVolume')\n"
        "assert(math.abs(AudioMixer.dbToLinear(-6.0206) - 0.5) < 1e-3)\n"
        "AudioListener.pause = true\n"
        "assert(AudioListener.pause == true)\n"
        "AudioListener.pause = false\n"
        "assert(AudioLoadType.Streaming ~= AudioLoadType.Auto)\n"), LUA_OK) << lua_tostring(L, -1);
    EXPECT_TRUE(AudioMixer::getGroupMute("Music"));

    AudioSource sound = source("tone.wav");
    ASSERT_TRUE(luabridge::push(L, sound));
    lua_setglobal(L, "sound");
    ASSERT_EQ(luaL_dostring(L,
        "sound.outputGroup = 'SFX'\n"
        "assert(sound.outputGroup == 'SFX')\n"
        "sound.pitch = 1.25\n"
        "assert(math.abs(sound.speed - 1.25) < 1e-6)\n"
        "sound.priority = 10\n"
        "sound.maxVoices = 3\n"
        "sound.ignoreListenerPause = true\n"
        "sound.loadType = AudioLoadType.DecompressOnLoad\n"
        "sound:playOneShot('tone.wav', 0.5)\n"
        "sound:fadeTo(0.5, 1, {unscaled = true})\n"
        "assert(sound:isFading())\n"
        "AudioSource.playClipAtPoint(sound.scene, 'tone.wav', Vector3(0, 0, 0), 0.5, 'SFX')\n"), LUA_OK) << lua_tostring(L, -1);
    const AudioSourceComponent& audio = sound.getComponent<AudioSourceComponent>();
    EXPECT_EQ(audio.outputGroupName, "SFX");
    EXPECT_EQ(audio.outputGroup, AudioMixer::findGroupId("SFX"));
    EXPECT_EQ(audio.priority, 10);
    EXPECT_EQ(audio.maxVoices, 3);
    EXPECT_TRUE(audio.ignoreListenerPause);
    EXPECT_EQ(audio.loadType, AudioLoadType::DecompressOnLoad);
    run(0.05);
    EXPECT_EQ(sound.getVoiceCount(), 1);
    EXPECT_EQ(AudioSystem::getVoiceCount(), 2u);
    lua_pushnil(L);
    lua_setglobal(L, "sound");
}

// ================================================================== GeneratorAudioTest

TEST(GeneratorAudioTest, InitConfiguresTheVoicesAndLoadsTheMixer) {
    EXPECT_EQ(Generator::audioInitSource(AudioSettings()), "") << "the defaults: nothing";
    AudioSettings settings;
    settings.mixer = "audio/Main.mixer";
    settings.maxRealVoices = 16;
    settings.maxVoices = 100;
    settings.sampleRate = 48000;
    const std::string source = Generator::audioInitSource(settings);
    EXPECT_NE(source.find("AudioSystem::configure({16, 100, 48000});"), std::string::npos) << source;
    EXPECT_NE(source.find("AudioMixer::load(\"audio/Main.mixer\");"), std::string::npos) << source;
    EXPECT_LT(source.find("AudioSystem::configure"), source.find("AudioMixer::load"));

    // main.cpp has them before the first scene loads
    const fs::path project = fs::temp_directory_path() / ("jasper-generator-audio-" + std::to_string(::getpid()));
    fs::remove_all(project);
    fs::create_directories(project / "assets");
    const fs::path internal = project / ".jasperengine";
    Generator generator;
    const std::vector<SceneBuildInfo> scenes = {SceneBuildInfo{1, "Level", {1}, {1}, true}};
    generator.configure(scenes, "test", {}, {}, project, internal, project / "assets", project / "lua", {}, 17,
                        Scaling::FITWIDTH, TextureStrategy::NONE, 1280, 720, true, WindowSettings(), LoadingSettings(),
                        TagsAndLayers(), 4, "", settings);
    const std::string mainCpp = scriptdata::readFile(internal / "generated" / "main.cpp");
    const size_t load = mainCpp.find("AudioMixer::load(\"audio/Main.mixer\")");
    ASSERT_NE(load, std::string::npos);
    EXPECT_LT(load, mainCpp.find("SceneManager::loadScene"));
    fs::remove_all(project);
}
