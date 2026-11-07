// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The audio mixer (spec docs/superpowers/specs/2026-10-31-audio-mixer-design.md 14): the .mixer
// format (AudioMixerIOTest), the dB / solo / snapshot math (AudioMixerMathTest), the runtime on
// SoLoud's null backend, mixed by AudioMixer::update so every result is exact and needs no audio
// device (AudioMixerRuntimeTest), the Audio Source voices, events, fades, pause and budgets
// (AudioSourceTest), the Lua surface (AudioLuaTest) and the exported init (GeneratorAudioTest).

#include <gtest/gtest.h>
#include "component/Transform.h"
#include "camera/CameraType.h"

#include "ScriptDataTestSupport.h"

#include "Generator.h"
#include "PlatformConfig.h"
#include "Project.h"
#include "Scene.h"
#include "Stream.h"
#include "audio/AudioMeter.h"
#include "audio/AudioMixer.h"
#include "audio/AudioMixerDocument.h"
#include "audio/AudioMixerIO.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AudioSnapshotComponent.h"
#include "component/CameraComponent.h"
#include "io/UserSettings.h"
#include "object/sound/AudioSource.h"
#include "player/PlayerConfig.h"
#include "pool/SoundPool.h"
#include "reflect/Reflect.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/AudioSystem.h"

#include "LuaBridge.h"
#include "json.hpp"
#include "soloud_bus.h"
#include "yaml-cpp/yaml.h"
#include "soloud.h"
#include "audio/AudioTap.h"
#include "soloud_wav.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    // the Audio Source through its S08 codec (what Stream writes and reads); decoded over `old`
    // (an undo, a prefab instance's whole-component override)
    YAML::Node encodeAudioSource(const AudioSourceComponent& value){
        return reflect::encode(*reflect::find(ComponentType::AudioSourceComponent), &value, reflect::EncodeContext{});
    }
    AudioSourceComponent decodeAudioSource(const YAML::Node& node, const AudioSourceComponent* old){
        AudioSourceComponent out = old ? *old : AudioSourceComponent();
        reflect::DecodeReport report;
        reflect::decode(*reflect::find(ComponentType::AudioSourceComponent), YAML::Load(YAML::Dump(node)), &out, reflect::DecodeContext{}, report);
        return out;
    }
}

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
        bool wasAsync = false;

        void SetUp() override {
            ScriptDataTest::SetUp();
            // clips load in the frame they are asked for (no worker thread to wait on)
            wasAsync = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            AudioSystem::close();
            AudioSystem::setBackend(AudioBackend::Null);
            AudioSystem::configure(AudioConfig());
            AudioSystem::setListenerPaused(false);
            AudioMixer::setMasterPaused(false);
            AudioMixer::unload();
            SoundPool::clear();
            writeTone(root / "assets" / "tone.wav", 0.5);
            scene = new Scene();
            // the game's main scene (its camera is the 3D listener)
            Engine::setScene(scene);
        }

        void TearDown() override {
            Engine::removeScene(scene);
            delete scene;
            scene = nullptr;
            platformConfig().silentAudio = false;
            AudioSystem::setGlobalVolume(1.0f);
            AudioSystem::close();
            AudioMixer::unload();
            AudioSystem::setListenerPaused(false);
            AudioMixer::setMasterPaused(false);
            SoundPool::clear();
            Engine::setAsyncLoading(wasAsync);
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

        // one frame: the mixer mixes `dt` of audio, the scene's system updates (on the deterministic
        // driver, as Engine::runFrame, the mix is after the scene)
        void step(double dt, double scaled = -1.0) {
            AudioMixer::update(scaled < 0.0 ? dt : scaled, dt);
            scene->getSystem<AudioSystem>()->update(scaled < 0.0 ? dt : scaled);
            AudioSystem::pumpDeterministic(dt);
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

        // a source with a Transform: 3D where its scene has a camera
        AudioSource source3d(const std::string& file = "tone.wav") {
            AudioSource sound(scene, true);
            sound.loadSound(file);
            return sound;
        }

        // ---- 3D (the camera and transforms as RenderSystem leaves them for the frame)

        // the scene's camera at `position`, looking at `target` with +Y up; an orthographic or UI
        // view spans [left, right] x [bottom, top] from it
        Entity camera(Scene* in, CameraType type, const Vector3& position, const Vector3& target,
                      float left = 0, float right = 0, float bottom = 0, float top = 0) {
            const Entity entity = in->createEntity();
            in->addComponent<Transform>(entity, Transform());
            CameraComponent component;
            component.type = type;
            component.autoResize = false;
            in->addComponent<CameraComponent>(entity, component);
            in->setCamera(entity);
            moveCamera(in, position, target);
            CameraComponent& placed = in->getComponent<CameraComponent>(entity);
            placed.leftClip = left;
            placed.rightClip = right;
            placed.bottomClip = bottom;
            placed.topClip = top;
            return entity;
        }

        void moveCamera(Scene* in, const Vector3& position, const Vector3& target) {
            const Entity entity = in->getCamera();
            in->getComponent<Transform>(entity).worldPosition = position;
            CameraComponent& component = in->getComponent<CameraComponent>(entity);
            component.worldTarget = target;
            component.steadyWorldUp = Vector3(0, 1, 0);
        }

        void place(AudioSource& sound, const Vector3& position) {
            sound.getComponent<Transform>().worldPosition = position;
        }

        // SoLoud's state of a voice (its 3D data, channel volumes)
        int voiceIndex(unsigned int handle) { return soloud().getVoiceFromHandle_internal(handle); }
        float doppler(unsigned int handle) { return soloud().m3dData[voiceIndex(handle)].mDopplerValue; }
        Vector3 voicePosition(unsigned int handle) {
            const float* p = soloud().m3dData[voiceIndex(handle)].m3dPosition;
            return Vector3(p[0], p[1], p[2]);
        }
        Vector3 listenerPosition() { return Vector3(soloud().m3dPosition[0], soloud().m3dPosition[1], soloud().m3dPosition[2]); }
        float channelVolume(unsigned int handle, int channel) { return soloud().mVoice[voiceIndex(handle)]->mChannelVolume[channel]; }
    };

    // a 16-bit mono WAV of a constant level (onsets are the first loud frame)
    void writeLevel(const fs::path& path, double seconds, double level = 0.5, int rate = 44100) {
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
        for (uint32_t i = 0; i < samples; i++) u16((uint16_t)(int16_t)std::lround(level * 32767.0));
    }

    // the first frame of a channel of interleaved stereo louder than `threshold`, -1 when none
    int onsetOf(const std::vector<float>& stereo, int channel, float threshold = 0.05f) {
        for (size_t i = channel; i < stereo.size(); i += 2) {
            if (std::fabs(stereo[i]) > threshold) return (int)(i / 2);
        }
        return -1;
    }

    class AudioMixerMathRuntimeTest : public AudioFixture {};
    class AudioMixerRuntimeTest : public AudioFixture {};
    class AudioSourceTest : public AudioFixture {};
    class AudioLuaTest : public AudioFixture {};

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

TEST_F(AudioMixerMathRuntimeTest, SoloFollowsUnityOnAThreeLevelTree) {
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

TEST_F(AudioMixerMathRuntimeTest, SetFloatOverridesSnapshotsUntilCleared) {
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

TEST_F(AudioMixerMathRuntimeTest, TimedChangesRunOnScaledTimeUnlessAskedOrTheMixerSaysUnscaled) {
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

TEST_F(AudioMixerRuntimeTest, NoMixerIsAnImplicitMasterAtZeroDecibels) {
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

TEST_F(AudioMixerRuntimeTest, MetersVolumesMuteAndLowPass) {
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
    def.exposed.erase(std::remove_if(def.exposed.begin(), def.exposed.end(),
                                     [](const AudioMixerExposedDef& e) { return e.name != "MusicVolume"; }), def.exposed.end());
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
    def.exposed.push_back({"cut", "0000beef/cutoff", false});
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

TEST_F(AudioMixerRuntimeTest, RememberedParametersComeBackWhenTheMixerLoads) {
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

TEST_F(AudioMixerRuntimeTest, ARemovedGroupsSourcesGoOnInMaster) {
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

TEST_F(AudioMixerRuntimeTest, AppPauseHoldsTheMasterBusAndKeepsAGamePausedSourcePaused) {
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

    // what the device would get (the null backend: mixed here)
    auto peakOut = [this]() {
        std::vector<float> buffer(2048 * 2);
        float peak = 0.0f;
        for (int i = 0; i < 4; i++) {
            soloud().mix(buffer.data(), 2048);
            for (float v : buffer) peak = std::max(peak, std::fabs(v));
        }
        return peak;
    };
    EXPECT_GT(peakOut(), 0.1f);
    AudioSystem::pauseAll();
    run(0.2);
    EXPECT_FLOAT_EQ(peakOut(), 0.0f) << "nothing routed is heard";
    AudioSystem::resumeAll();
    run(0.1);
    EXPECT_GT(peakOut(), 0.1f);
    EXPECT_TRUE(soloud().getPause(ha)) << "the game paused it: it stays paused";
    EXPECT_TRUE(a.isPaused());
}

// ================================================================== AudioSourceTest

TEST_F(AudioSourceTest, OneClipPlaysFromManySourcesAtOnceAndPlayRestarts) {
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

TEST_F(AudioSourceTest, OneShotsOverlapUpToMaxVoicesTheOldestStolen) {
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

TEST_F(AudioSourceTest, TheGlobalBudgetStealsTheLeastImportantVoice) {
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

TEST_F(AudioSourceTest, IsPlayingFollowsTheVoiceForLoopsAndPitch) {
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

TEST_F(AudioSourceTest, EventsFireOnStartPauseStopAndTheNaturalEnd) {
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

TEST_F(AudioSourceTest, FadesMoveTheVolumeAndFadeOutStopsOrPauses) {
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

TEST_F(AudioSourceTest, ListenerPauseSparesIgnoreListenerPauseSources) {
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

TEST_F(AudioSourceTest, AutoStreamsBigFilesAndDecodesSmallOnes) {
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

TEST_F(AudioSourceTest, PlayClipAtPointBelongsToItsScene) {
    loadMusicMixer();
    AudioSource::playClipAtPoint(scene, "tone.wav", Vector3(0, 0, 0), 1.0f, "SFX");
    run(0.1);
    EXPECT_EQ(AudioSystem::getVoiceCount(), 1u);
    EXPECT_GT(AudioMixer::getMeterDb("SFX"), -20.0f);
    scene->getSystem<AudioSystem>()->stopSceneSounds();
    EXPECT_EQ(AudioSystem::getVoiceCount(), 0u);
}

TEST_F(AudioSourceTest, MovedVoicesFollowTheirSourceToAnotherScene) {
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

// ================================================================== review regressions

TEST_F(AudioMixerRuntimeTest, ALiveEditKeepsTheVoiceBuffersWhenTheVoiceLimitStays) {
    loadMusicMixer();
    AudioSource music = source("tone.wav", "Music");
    music.setLooping(true);
    music.play();
    run(0.1);
    unsigned int limitChanges = 0, builds = 0;
    AudioMixer::getDebugCounters(limitChanges, builds);
    float** buffers = soloud().mResampleData;
    // a fader drag: the same groups, another volume, every frame
    for (int i = 0; i < 10; i++) {
        AudioMixerDef def = AudioMixer::getDef();
        def.findGroup("Music")->volumeDb = -1.0f * i;
        AudioMixer::apply(def);
        step(0.01);
    }
    unsigned int limitChangesAfter = 0;
    AudioMixer::getDebugCounters(limitChangesAfter, builds);
    EXPECT_EQ(limitChangesAfter, limitChanges) << "SoLoud's voice limit set again (its buffers reallocated: a dropout)";
    EXPECT_EQ(soloud().mResampleData, buffers);
    // another group: one more bus, the limit follows once
    AudioMixerDef def = AudioMixer::getDef();
    AudioMixerGroupDef ui;
    ui.id = "0000aaaa";
    ui.name = "UI";
    ui.parentId = def.master()->id;
    def.groups.push_back(ui);
    AudioMixer::apply(def);
    AudioMixer::getDebugCounters(limitChangesAfter, builds);
    EXPECT_EQ(limitChangesAfter, limitChanges + 1);
    EXPECT_EQ(soloud().getMaxActiveVoiceCount(), 32u + 4u);
}

TEST_F(AudioMixerRuntimeTest, TheVoiceLimitStaysWithinSoLoudsMaximum) {
    AudioConfig config;
    config.maxRealVoices = 255;
    AudioSystem::configure(config);
    loadMusicMixer();
    AudioSource music = source("tone.wav", "Music");
    music.play();
    run(0.1);
    EXPECT_EQ(soloud().getMaxActiveVoiceCount(), AudioMixer::maxActiveVoices) << "255 real voices + 3 buses, clamped";
    EXPECT_TRUE(music.isPlaying());
    EXPECT_GT(AudioMixer::getMeterDb("Music"), -20.0f);
}

TEST_F(AudioMixerRuntimeTest, AnotherMixerWithAnotherMasterIdKeepsMastersVoices) {
    loadMusicMixer();
    AudioSource sfx = source("tone.wav");
    sfx.setLooping(true);
    sfx.play();
    run(0.1);
    const unsigned int handle = sfx.getComponent<AudioSourceComponent>().handle;
    // a voice the engine does not register (the HTML UI video's, the Inspector preview's)
    SoLoud::Wav loose;
    writeTone(root / "assets" / "loose.wav", 1.0);
    ASSERT_EQ(loose.load((root / "assets" / "loose.wav").string().c_str()), SoLoud::SO_NO_ERROR);
    loose.setLooping(true);
    const unsigned int looseHandle = AudioMixer::play("", loose, false, Vector3(0, 0, 0), 1.0f, false, 0.0);
    ASSERT_NE(looseHandle, 0u);
    soloud().setPause(looseHandle, false);

    const unsigned int busBefore = AudioMixer::getBusHandle("");
    ASSERT_TRUE(soloud().isValidVoiceHandle(handle));
    loadMixer(R"({"format": "jasperengine.mixer", "groups": [{"id": "abcdef01", "name": "Master"},
        {"id": "abcdef02", "name": "Dialogue", "parent": "abcdef01"}]})");
    EXPECT_EQ(AudioMixer::getBusHandle(""), busBefore) << "the same Master bus";
    run(0.1);
    EXPECT_TRUE(soloud().isValidVoiceHandle(handle)) << "the source on Master plays on";
    EXPECT_TRUE(soloud().isValidVoiceHandle(looseHandle)) << "so does a voice the engine does not track";
    EXPECT_GT(AudioMixer::getMeterDb("Master"), -20.0f);
    soloud().stop(looseHandle);
}

TEST_F(AudioMixerRuntimeTest, SaveParametersKeepsOnlyWhatScriptsSet) {
    nlohmann::json json = nlohmann::json::parse(exampleMixer);
    json["exposed"][1]["remember"] = true;   // lowPassCutoff
    json["snapshots"].push_back({{"name", "Gameplay"}, {"values", {{"c4f00d01/cutoff", 525}}}});
    loadMixer(json.dump());
    ASSERT_TRUE(AudioMixer::setFloat("MusicVolume", -20.0f));
    AudioMixer::saveRemembered();
    EXPECT_TRUE(UserSettings::hasKey("audio.mixer.MusicVolume"));
    EXPECT_FALSE(UserSettings::hasKey("audio.mixer.lowPassCutoff")) << "not set by a script: not pinned";
    // the next launch: the cutoff still follows the snapshots
    AudioMixer::unload();
    loadMixer(json.dump());
    EXPECT_FALSE(AudioMixer::isOverridden("lowPassCutoff"));
    ASSERT_TRUE(AudioMixer::transitionTo("Gameplay", 0.0f));
    float cutoff = 0.0f;
    ASSERT_TRUE(AudioMixer::getFloat("lowPassCutoff", cutoff));
    EXPECT_FLOAT_EQ(cutoff, 525.0f);
    float volume = 0.0f;
    ASSERT_TRUE(AudioMixer::getFloat("MusicVolume", volume));
    EXPECT_FLOAT_EQ(volume, -20.0f);
}

TEST_F(AudioMixerRuntimeTest, ATimedEchoDelayRebuildsTheEchoOnceAtTheEnd) {
    loadMixer(R"({"format": "jasperengine.mixer", "groups": [{"id": "00000001", "name": "Master",
        "effects": [{"id": "0000ec40", "type": "echo", "params": {"delay": 0.2}}]}]})");
    AudioSource sound = source();
    sound.setLooping(true);
    sound.play();
    run(0.05);
    unsigned int limits = 0, before = 0, after = 0;
    AudioMixer::getDebugCounters(limits, before);
    ASSERT_TRUE(AudioMixer::setTarget("0000ec40/delay", 1.0f, 0.5f));
    run(0.4);
    AudioMixer::getDebugCounters(limits, after);
    EXPECT_EQ(after, before) << "no rebuild while the delay moves";
    run(0.3);
    AudioMixer::getDebugCounters(limits, after);
    EXPECT_EQ(after, before + 1) << "one, where it ends";
    float delay = 0.0f;
    ASSERT_TRUE(AudioMixer::getTarget("0000ec40/delay", delay));
    EXPECT_FLOAT_EQ(delay, 1.0f);
}

TEST_F(AudioSourceTest, PlayOverAFadeOutCutsTheFade) {
    AudioSource sound = source();
    sound.setLooping(true);
    sound.setVolume(0.7);
    sound.play();
    run(0.05);
    sound.fadeOut(0.2);
    run(0.1);
    EXPECT_LT(sound.getVolume(), 0.7);
    sound.play();
    run(0.3);
    EXPECT_TRUE(sound.isPlaying()) << "the old fade does not stop the new start";
    EXPECT_FALSE(sound.isFading());
    EXPECT_NEAR(sound.getVolume(), 0.7, 1e-6) << "at the authored level";

    sound.fadeOut(0.2, true);
    run(0.1);
    sound.play();
    run(0.3);
    EXPECT_TRUE(sound.isPlaying()) << "nor pauses it";
    EXPECT_NEAR(sound.getVolume(), 0.7, 1e-6);
}

TEST_F(AudioSourceTest, AClockedVoiceStartsHeldWhileTheListenerIsPaused) {
    AudioSystem::setListenerPaused(true);
    AudioSource sound = source();
    sound.setClockedSound(true);
    sound.play();
    step(0.02);
    const unsigned int handle = sound.getComponent<AudioSourceComponent>().handle;
    ASSERT_NE(handle, 0u);
    EXPECT_TRUE(soloud().getPause(handle));
    AudioSystem::setListenerPaused(false);
    step(0.02);
    EXPECT_FALSE(soloud().getPause(handle));
}

TEST_F(AudioSourceTest, AStreamWhoseFileWentAwayEndsItsNewVoices) {
    AudioSource music = source("tone.wav");
    music.setLoadType(AudioLoadType::Streaming);
    music.play();
    run(0.05);
    ASSERT_TRUE(music.isPlaying());
    fs::remove(root / "assets" / "tone.wav");
    // the sample is parsed and cached: a new voice cannot open the file
    music.playOneShot("tone.wav");
    music.play();
    run(0.1);
    EXPECT_FALSE(music.isPlaying()) << "over at once, not silent for ever";
    EXPECT_EQ(music.getVoiceCount(), 0);
    EXPECT_TRUE(music.isStopped());
}

TEST(AudioSourceSerializationTest, AbsentMixerKeysAreTheirDefaultsOverAnExistingComponent) {
    AudioSourceComponent edited;
    edited.filename = "music.ogg";
    edited.outputGroup = "9b51c0de";
    edited.outputGroupName = "Music";
    edited.loadType = AudioLoadType::Streaming;
    edited.priority = 10;
    edited.maxVoices = 3;
    edited.ignoreListenerPause = true;
    // an undo (or a prefab instance's whole-component override) back to a source with the defaults
    AudioSourceComponent plain;
    plain.filename = "music.ogg";
    const YAML::Node node = encodeAudioSource(plain);
    EXPECT_FALSE(node["outputGroup"]) << "defaults are not written";
    EXPECT_FALSE(node["priority"]);
    const AudioSourceComponent restored = decodeAudioSource(node, &edited);
    EXPECT_EQ(restored.outputGroup, "");
    EXPECT_EQ(restored.outputGroupName, "") << "back to Master";
    EXPECT_EQ(restored.loadType, AudioLoadType::Auto);
    EXPECT_EQ(restored.priority, 128);
    EXPECT_EQ(restored.maxVoices, 8);
    EXPECT_FALSE(restored.ignoreListenerPause);
    // and the other way
    const AudioSourceComponent again = decodeAudioSource(encodeAudioSource(edited), &plain);
    EXPECT_EQ(again.outputGroupName, "Music");
    EXPECT_EQ(again.priority, 10);
    EXPECT_EQ(again.loadType, AudioLoadType::Streaming);
}

// ================================================================== AudioLuaTest

TEST_F(AudioLuaTest, LuaDrivesTheMixerAndSources) {
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
                        TagsAndLayers(), 4, ThreadingProjectSettings(), HtmlUiRenderSettings(), PbrTier::REDUCED,
                        ShaderCreationProjectSettings(), false, {}, PlayerSettings(), settings);
    const std::string mainCpp = scriptdata::readFile(internal / "generated" / "main.cpp");
    const size_t load = mainCpp.find("AudioMixer::load(\"audio/Main.mixer\")");
    ASSERT_NE(load, std::string::npos);
    EXPECT_LT(load, mainCpp.find("SceneManager::loadScene"));
    fs::remove_all(project);
}

// ================================================================== BF14: audio and engine bug fixes
// (found comparing the unmerged snd/sound-layer branch with this engine)

namespace jasperengine {
    // the voice parameters a needUpdate applies, without the frame's 3D pass after them
    struct AudioSystemTestAccess {
        static void applyVoiceParameters(AudioSystem& system, AudioSourceComponent& audio, unsigned int handle, bool use3D) {
            system.applyVoiceParameters(audio, handle, true, use3D);
        }
    };
}

namespace {
    constexpr double kFrame60 = 1.0 / 60.0;
}

// ---- S-6: a voice through a group bus starts in the next mix ------------------------------------

TEST_F(AudioMixerRuntimeTest, AVoiceThroughGroupBusesIsHeardFromTheNextMix) {
    loadMusicMixer();
    writeLevel(root / "assets" / "level.wav", 1.0);
    AudioSource music = source("level.wav", "Music");
    // the onset of a voice played after a first mix of `first` frames (each bus read a 512-frame
    // block of it and kept the rest)
    auto onsetAfter = [&](unsigned int first) {
        AudioSystem::close();
        EXPECT_TRUE(AudioSystem::init());
        EXPECT_FALSE(soloud().getAutoStop(AudioMixer::getBusHandle("Master"))) << "a bus never ends by itself";
        EXPECT_FALSE(soloud().getAutoStop(AudioMixer::getBusHandle("Music")));
        std::vector<float> out(2048 * 2);
        soloud().mix(out.data(), first);
        music.play();
        scene->getSystem<AudioSystem>()->update(0.0);
        soloud().mix(out.data(), 735);
        out.resize(735 * 2);
        return onsetOf(out, 0);
    };
    const int atABlock = onsetAfter(512);
    ASSERT_GE(atABlock, 0);
    EXPECT_LE(atABlock, 3) << "SoLoud's linear resampler: a frame a level (Master > Music > voice)";
    EXPECT_EQ(onsetAfter(300), atABlock) << "no bus mixes a block it read ahead";
    EXPECT_EQ(onsetAfter(735), atABlock);
}

// ---- S-7: the null pump keeps in step with the frames -------------------------------------------

TEST_F(AudioMixerRuntimeTest, TheNullPumpMixesExactlyTheTimeOfItsFrames) {
    AudioSource loop = source();
    loop.setLooping(true);
    loop.play();
    step(1.0 / 144.0);
    ASSERT_TRUE(loop.isPlaying());
    const double start = soloud().mStreamTime;
    // 144 fps at 44.1 kHz: 306.25 frames each, which a per-frame rounding mixes as 306
    for (int i = 0; i < 1440; i++) step(1.0 / 144.0);
    EXPECT_NEAR(soloud().mStreamTime - start, 10.0, 1e-4) << "10 s of frames are 10 s of audio";
}

// ---- X-1: a Recorder capture mixes each frame once ---------------------------------------------

TEST_F(AudioSourceTest, ARecorderCaptureMixesEachFrameOnce) {
    ASSERT_TRUE(AudioSystem::beginDeterministic());
    AudioSource loop = source();
    loop.setLooping(true);
    loop.play();
    // the Recorder's frame: the game steps 1/30 s, the driver's pump mixes 1/30 s of audio
    const double start = soloud().mStreamTime;
    for (int i = 0; i < 30; i++) step(1.0 / 30.0);
    EXPECT_NEAR(soloud().mStreamTime - start, 1.0, 1e-3) << "one second of frames, one second mixed";
    AudioSystem::endDeterministic();
}

TEST_F(AudioSourceTest, ARecorderCaptureHearsTheMixer) {
    loadMusicMixer();
    ASSERT_TRUE(AudioSystem::beginDeterministic());
    EXPECT_NE(AudioMixer::getBusHandle("Music"), AudioMixer::getBusHandle("Master")) << "the groups' buses are there";
    writeLevel(root / "assets" / "level.wav", 1.0);
    AudioSource music = source("level.wav", "Music");
    AudioMixer::setGroupMute("Music", true);
    music.setLooping(true);
    music.play();
    uint64_t mixed = 0;
    float peak = 0.0f;
    const int tap = AudioTap::subscribe("test", [&](const float* out, uint32_t frames, uint64_t first) {
        // (SoLoud ramps a volume change over the mix call that takes it, and each bus below it holds
        // a sample of that: the group's mute, set before the first frame, is over after it)
        mixed = first + frames;
        if (first >= 1600) for (uint32_t i = 0; i < frames * 2; i++) peak = std::max(peak, std::fabs(out[i]));
    });
    for (int i = 0; i < 10; i++) step(1.0 / 30.0);
    AudioTap::unsubscribe(tap);
    EXPECT_EQ(mixed, 16000u);
    EXPECT_TRUE(music.isPlaying());
    EXPECT_FLOAT_EQ(peak, 0.0f) << "a muted group is muted in the capture too";
    AudioSystem::endDeterministic();
}

// ---- B5: --headless and --silent-audio mix on the null driver ------------------------------------

TEST_F(AudioSourceTest, AHeadlessRunPlaysItsSoundsAsANormalRunDoes) {
    AudioSystem::close();
    AudioSystem::setBackend(AudioBackend::Auto);
    platformConfig().silentAudio = true;
    writeTone(root / "assets" / "short.wav", 0.1);
    AudioSource sound = source("short.wav");
    std::vector<std::string> events;
    AudioSourceComponent& audio = sound.getComponent<AudioSourceComponent>();
    audio.onStart.add("test", std::function<void()>([&events]() { events.push_back("start"); }));
    audio.onStop.add("test", std::function<void()>([&events]() { events.push_back("stop"); }));
    sound.play();
    step(0.02);
    EXPECT_TRUE(sound.isPlaying()) << "the game sees it play";
    EXPECT_EQ(events, (std::vector<std::string>{"start"}));
    run(0.2);
    EXPECT_EQ(events, (std::vector<std::string>{"start", "stop"})) << "and end after its length of game time";
    EXPECT_FALSE(AudioSystem::isOutputAvailable()) << "no device";
    EXPECT_EQ(AudioSystem::getBackendName(), "null driver");
}

// ---- B9: clocked voices start at the game time they were played --------------------------------

// A deliberate change: this drove AudioSystem::beginOfflineCapture, which S35 deleted. No null pump
// lets a test place a mix between two frames now, so it plays at the frames' clock times itself.
TEST_F(AudioSourceTest, ClockedVoicesStartAsFarApartAsTheFramesThatPlayedThem) {
    ASSERT_TRUE(AudioSystem::beginDeterministic());
    writeLevel(root / "assets" / "level.wav", 1.0, 0.25);
    SoLoud::Wav wav;
    ASSERT_EQ(wav.load((root / "assets" / "level.wav").string().c_str()), SoLoud::SO_NO_ERROR);
    ASSERT_NE(AudioMixer::play("", wav, false, Vector3(0, 0, 0), 1.0f, true, kFrame60), 0u);
    ASSERT_NE(AudioMixer::play("", wav, false, Vector3(0, 0, 0), 1.0f, true, 2.0 * kFrame60), 0u);
    std::vector<float> out(4096 * 2);
    soloud().mix(out.data(), 4096);
    // the first is one level, both are twice it: the second starts where the level doubles
    const int a = onsetOf(out, 0, 0.01f);
    ASSERT_GE(a, 0);
    const float one = std::fabs(out[(a + 100) * 2]);
    ASSERT_GT(one, 0.05f);
    const int b = onsetOf(out, 0, one * 1.5f);
    ASSERT_GE(b, 0);
    EXPECT_NEAR(b - a, 800, 1) << "a frame of 1/60 s apart (enableClocked)";
    AudioSystem::endDeterministic();
}

// ---- B11: a game's global volume ends with its Play ---------------------------------------------

TEST_F(AudioSourceTest, PlayStartAndStopRestoreTheGlobalVolume) {
    AudioSource sound = source();
    sound.play();
    step(0.02);
    editor::Project project;
    for (bool starting : {false, true}) {
        // AudioListener.volume = 0 in a game
        AudioSystem::setGlobalVolume(0.0f);
        project.resetPlayAudio(starting);
        EXPECT_FLOAT_EQ(AudioSystem::getGlobalVolume(), 1.0f) << (starting ? "Play start" : "Play stop");
        EXPECT_FLOAT_EQ(soloud().getGlobalVolume(), 1.0f);
    }
}

// ---- B13: an exported game keeps a saved Paused state --------------------------------------------

TEST(AudioSourceSerializationTest, AnExportedSourceKeepsItsSavedState) {
    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::AudioSourceComponent);
    reflect::CodegenContext ctx;
    ctx.variable = "audio";
    AudioSourceComponent audio;
    audio.state = AudioSourceState::Paused;
    std::string code = reflect::cppAssignments(desc, &audio, ctx);
    EXPECT_NE(code.find("audio.state = AudioSourceState::Paused;"), std::string::npos) << "as the editor's Play loads it:\n" << code;
    EXPECT_EQ(code.find("startTrigger"), std::string::npos);

    // Playing starts through startTrigger, from Stopped
    audio.state = AudioSourceState::Playing;
    code = reflect::cppAssignments(desc, &audio, ctx);
    EXPECT_NE(code.find("audio.state = AudioSourceState::Stopped;"), std::string::npos) << code;
    EXPECT_NE(code.find("audio.startTrigger = true;"), std::string::npos) << code;
}

// ---- B6: the platform's pause is not the game's pauseAll ----------------------------------------

TEST_F(AudioMixerRuntimeTest, ReturningFromTheBackgroundKeepsTheGamesPauseAll) {
    AudioSource sound = source();
    sound.setLooping(true);
    sound.play();
    run(0.1);
    auto peakOut = [this]() {
        std::vector<float> buffer(2048 * 2);
        float peak = 0.0f;
        for (int i = 0; i < 4; i++) {
            soloud().mix(buffer.data(), 2048);
            for (float v : buffer) peak = std::max(peak, std::fabs(v));
        }
        return peak;
    };
    EXPECT_GT(peakOut(), 0.1f);
    // a pause menu
    AudioSystem::pauseAll();
    run(0.05);
    EXPECT_FLOAT_EQ(peakOut(), 0.0f);
    // the app goes to the background and comes back
    Engine::systemPause();
    Engine::systemResume();
    run(0.05);
    EXPECT_FLOAT_EQ(peakOut(), 0.0f) << "the game's pause holds";
    EXPECT_TRUE(AudioMixer::isMasterPaused());
    AudioSystem::resumeAll();
    run(0.05);
    EXPECT_GT(peakOut(), 0.1f);

    // and the other way: the platform's pause holds what the game resumes
    Engine::systemPause();
    AudioSystem::resumeAll();
    run(0.05);
    EXPECT_FLOAT_EQ(peakOut(), 0.0f) << "in the background";
    Engine::systemResume();
    run(0.05);
    EXPECT_GT(peakOut(), 0.1f);
}

// ---- setPan on a 3D voice ----------------------------------------------------------------------

TEST_F(AudioSourceTest, AppliedParametersKeepA3DVoicesPan) {
    camera(scene, CameraType::CAMERA_PERSPECTIVE, Vector3(0, 0, 0), Vector3(0, 0, -1));
    AudioSource sound = source3d();
    place(sound, Vector3(10, 0, -1));
    sound.setLooping(true);
    sound.play();
    step(kFrame60);
    AudioSourceComponent& audio = sound.getComponent<AudioSourceComponent>();
    ASSERT_NE(audio.handle, 0u);
    const float left = channelVolume(audio.handle, 0);
    const float right = channelVolume(audio.handle, 1);
    ASSERT_GT(right, left + 0.5f) << "hard right";
    // a needUpdate (a pitch, a loop change) applies the parameters before the frame's 3D pass
    AudioSystemTestAccess::applyVoiceParameters(*scene->getSystem<AudioSystem>(), audio, audio.handle, true);
    EXPECT_FLOAT_EQ(channelVolume(audio.handle, 0), left) << "not the centre of a 2D pan";
    EXPECT_FLOAT_EQ(channelVolume(audio.handle, 1), right);
}

// ---- decision 9: a 2D or UI camera hears at the centre of its view --------------------------------

TEST_F(AudioSourceTest, A2DCamerasListenerStandsAtTheCentreOfItsView) {
    for (CameraType type : {CameraType::CAMERA_ORTHO, CameraType::CAMERA_UI}) {
        SCOPED_TRACE((int)type);
        Scene* view = new Scene();
        Engine::setScene(view);
        Scene* old = scene;
        scene = view;
        camera(view, type, Vector3(0, 0, 1), Vector3(0, 0, 0), 0, 800, 0, 600);
        AudioSource sound = source3d();
        place(sound, Vector3(400, 300, 0));
        sound.setLooping(true);
        sound.play();
        step(kFrame60);
        const unsigned int handle = sound.getComponent<AudioSourceComponent>().handle;
        ASSERT_NE(handle, 0u);
        const Vector3 listener = listenerPosition();
        EXPECT_FLOAT_EQ(listener.x, 400.0f);
        EXPECT_FLOAT_EQ(listener.y, 300.0f);
        EXPECT_FLOAT_EQ(listener.z, 1.0f);
        EXPECT_NEAR(channelVolume(handle, 0), channelVolume(handle, 1), 1e-5f) << "a sound at the centre of the screen is centred";
        sound.stop();
        step(kFrame60);
        scene = old;
        Engine::setScene(scene);
        Engine::removeScene(view);
        delete view;
    }
}

// ---- B4b, B4c, pixel units: Doppler ---------------------------------------------------------------

TEST_F(AudioSourceTest, DopplerFollowsSpeedsInUnitsPerSecondAtAnyFrameRate) {
    const float c = soloud().get3dSoundSpeed();
    camera(scene, CameraType::CAMERA_PERSPECTIVE, Vector3(0, 0, 0), Vector3(0, 0, -1));
    for (double fps : {30.0, 60.0, 144.0}) {
        SCOPED_TRACE(fps);
        const double dt = 1.0 / fps;
        // a source coming at the listener at a tenth of the speed of sound
        AudioSource sound = source3d();
        place(sound, Vector3(0, 0, -100));
        sound.setLooping(true);
        sound.play();
        step(dt);
        const unsigned int handle = sound.getComponent<AudioSourceComponent>().handle;
        ASSERT_NE(handle, 0u);
        Vector3 at(0, 0, -100);
        for (int i = 0; i < 5; i++) {
            at.z += (float)(0.1 * c * dt);
            place(sound, at);
            step(dt);
        }
        EXPECT_NEAR(doppler(handle), 1.0f / 0.9f, 1e-3f) << "c / (c - v)";

        // B4c: it stops; its pitch comes back
        step(dt);
        step(dt);
        EXPECT_NEAR(doppler(handle), 1.0f, 1e-6f) << "a still source has no Doppler";

        // the listener coming at the still source at a tenth of the speed of sound
        Vector3 eye(0, 0, 0);
        for (int i = 0; i < 5; i++) {
            eye.z -= (float)(0.1 * c * dt);
            moveCamera(scene, eye, eye + Vector3(0, 0, -1));
            step(dt);
        }
        EXPECT_NEAR(doppler(handle), 1.1f, 1e-3f) << "(c + v) / c";
        step(dt);
        step(dt);
        EXPECT_NEAR(doppler(handle), 1.0f, 1e-6f) << "the listener stopped";
        moveCamera(scene, Vector3(0, 0, 0), Vector3(0, 0, -1));
        sound.stop();
        step(dt);
        step(dt);
    }
}

TEST_F(AudioSourceTest, A2DSceneMovingInPixelsHasNoDoppler) {
    for (CameraType type : {CameraType::CAMERA_ORTHO, CameraType::CAMERA_UI}) {
        Scene* view = new Scene();
        Engine::setScene(view);
        Scene* old = scene;
        scene = view;
        camera(view, type, Vector3(0, 0, 1), Vector3(0, 0, 0), 0, 800, 0, 600);
        for (double fps : {30.0, 60.0}) {
            SCOPED_TRACE(fps);
            // a sprite crossing the screen at 600 px/s
            AudioSource sound = source3d();
            Vector3 at(100, 300, 0);
            place(sound, at);
            sound.setLooping(true);
            sound.play();
            step(1.0 / fps);
            const unsigned int handle = sound.getComponent<AudioSourceComponent>().handle;
            ASSERT_NE(handle, 0u);
            for (int i = 0; i < 5; i++) {
                at.x += (float)(600.0 / fps);
                place(sound, at);
                step(1.0 / fps);
            }
            EXPECT_FLOAT_EQ(doppler(handle), 1.0f) << "no pitch from a speed in pixels";
            sound.stop();
            step(1.0 / fps);
        }
        scene = old;
        Engine::setScene(scene);
        Engine::removeScene(view);
        delete view;
    }
}

// ---- B4d: a layer scene hears through the main scene's listener ----------------------------------

TEST_F(AudioSourceTest, ALayerScenesSoundsAreHeardByTheMainCamera) {
    // the main scene: a perspective camera at the origin looking down -Z
    camera(scene, CameraType::CAMERA_PERSPECTIVE, Vector3(0, 0, 0), Vector3(0, 0, -1));
    // a layer with its own camera at (100, 0, 0) looking down -X, and a sound 1 m in front of it
    // and 2 m to its right
    Scene* layer = new Scene();
    Engine::addSceneLayer(layer);
    camera(layer, CameraType::CAMERA_PERSPECTIVE, Vector3(100, 0, 0), Vector3(99, 0, 0));
    Scene* main = scene;
    scene = layer;
    AudioSource sound = source3d();
    place(sound, Vector3(99, 0, -2));
    sound.setLooping(true);
    sound.play();
    scene = main;
    for (int i = 0; i < 3; i++) {
        AudioMixer::update(kFrame60, kFrame60);
        main->getSystem<AudioSystem>()->update(kFrame60);
        layer->getSystem<AudioSystem>()->update(kFrame60);
    }
    const unsigned int handle = sound.getComponent<AudioSourceComponent>().handle;
    ASSERT_NE(handle, 0u);
    const Vector3 listener = listenerPosition();
    EXPECT_FLOAT_EQ(listener.x, 0.0f) << "the listener stays at the main camera";
    EXPECT_FLOAT_EQ(listener.y, 0.0f);
    EXPECT_FLOAT_EQ(listener.z, 0.0f);
    const Vector3 heard = voicePosition(handle);
    EXPECT_NEAR(heard.x, 2.0f, 1e-4f) << "2 m to the main camera's right";
    EXPECT_NEAR(heard.y, 0.0f, 1e-4f);
    EXPECT_NEAR(heard.z, -1.0f, 1e-4f) << "1 m in front of it";
    sound.stop();
    layer->getSystem<AudioSystem>()->update(kFrame60);
    Engine::removeScene(layer);
    delete layer;
}

// ---- prebuilt players: the project's audio, as the generated init and Play have it -------------

TEST_F(AudioMixerRuntimeTest, APrebuiltPlayerConfiguresTheAudioAsTheGeneratedInitDoes) {
    fs::create_directories(root / "assets" / "audio");
    std::ofstream(root / "assets" / "audio" / "Main.mixer") << exampleMixer;
    player::PlayerConfig game;
    game.audioMixer = "audio/Main.mixer";
    game.audioMaxRealVoices = 16;
    game.audioMaxVoices = 100;
    game.audioSampleRate = 48000;
    player::PlayerConfig read;
    std::string error;
    ASSERT_TRUE(player::PlayerConfig::fromJson(game.toJson(), read, error)) << error;
    player::applyAudio(read);
    EXPECT_EQ(AudioSystem::getConfig().maxRealVoices, 16);
    EXPECT_EQ(AudioSystem::getConfig().maxVoices, 100);
    EXPECT_EQ(AudioSystem::getConfig().sampleRate, 48000);
    EXPECT_TRUE(AudioMixer::hasAsset());
    EXPECT_EQ(AudioMixer::getPath(), "audio/Main.mixer");
    EXPECT_EQ(AudioMixer::getGroupNames(), (std::vector<std::string>{"Master", "Music", "SFX"}));

    // a game.json of an older editor (no audio): the defaults, no mixer
    nlohmann::json old = game.toJson();
    old.erase("audio");
    ASSERT_TRUE(player::PlayerConfig::fromJson(old, read, error)) << error;
    EXPECT_EQ(read.audioMixer, "");
    EXPECT_EQ(read.audioMaxRealVoices, AudioConfig().maxRealVoices);
    EXPECT_EQ(read.audioMaxVoices, AudioConfig().maxVoices);
    EXPECT_EQ(read.audioSampleRate, AudioConfig().sampleRate);
}

// ================================================================== S38: the limiter's format, the output stage, new projects

TEST(AudioMixerIOTest, OnlyALimiterWithADelayOrATruePeakDetectorIsVersionTwo) {
    AudioMixerDef def = AudioMixerIO::createDefault();
    AudioMixerEffectDef limiter;
    limiter.type = AudioEffectType::Limiter;
    limiter.typeName = "limiter";
    limiter.id = "0000000a";
    limiter.params = {{"threshold", -3.0f}, {"release", 0.2f}};
    def.groups.front().effects.push_back(limiter);

    // an old file's bytes: no new key, version 1
    const std::string old = AudioMixerIO::write(def);
    EXPECT_NE(old.find("\"version\": 1"), std::string::npos) << old;
    EXPECT_EQ(old.find("lookahead"), std::string::npos) << old;
    AudioMixerDef readOld;
    std::string error;
    ASSERT_TRUE(AudioMixerIO::parse(old, readOld, &error)) << error;
    EXPECT_EQ(AudioMixerIO::write(readOld), old);

    def.groups.front().effects[0].params["truePeak"] = 1.0f;
    const std::string next = AudioMixerIO::write(def);
    EXPECT_NE(next.find("\"version\": 2"), std::string::npos) << next;
    AudioMixerDef readNext;
    ASSERT_TRUE(AudioMixerIO::parse(next, readNext, &error)) << error;
    EXPECT_EQ(readNext.groups.front().effects[0].params["truePeak"], 1.0f);
    EXPECT_EQ(AudioMixerIO::write(readNext), next);

    def.groups.front().effects[0].params.erase("truePeak");
    def.groups.front().effects[0].params["lookahead"] = 1.5f;
    EXPECT_NE(AudioMixerIO::write(def).find("\"version\": 2"), std::string::npos);
}

TEST(AudioMixerIOTest, AVersionNewerThanThisEngineReadsIsRefusedByItsVersion) {
    // An engine from before S38 reads up to 1 and refuses a version 2 file with this sentence, with
    // its own numbers: "version: 2 is newer than this engine reads (1)". The check is the same.
    AudioMixerDef def;
    std::string error;
    EXPECT_FALSE(AudioMixerIO::parse("{\"format\": \"jasperengine.mixer\", \"version\": 3, \"groups\": []}", def, &error));
    EXPECT_EQ(error, "version: 3 is newer than this engine reads (2)");
}

namespace {

    class AudioOutputStageTest : public AudioFixture {
    protected:
        // from the Master bus (the tap) to what leaves SoLoud, on a small constant signal
        double outputGainDb(AudioOutputStage stage) {
            AudioSystem::close();
            AudioConfig config;
            config.outputStage = stage;
            AudioSystem::configure(config);
            writeLevel(root / "assets" / "small.wav", 1.0, 0.01);
            AudioSource sound = source("small.wav");
            sound.setLooping(true);
            sound.play();
            run(0.1);
            AudioMixer::setLoudnessTap(true);
            std::vector<float> out(2048 * 2);
            soloud().mix(out.data(), 2048);
            std::vector<float> tap(2048 * 2);
            unsigned int channels = 0;
            unsigned int rate = 0;
            const size_t frames = AudioMixer::drainLoudnessTap(tap.data(), 2048, channels, rate);
            AudioMixer::setLoudnessTap(false);
            sound.stop();
            EXPECT_GT(frames, 512u);
            EXPECT_GT(tap[(frames - 1) * 2], 0.0f);
            return 20.0 * std::log10(out[(frames - 1) * 2] / tap[(frames - 1) * 2]);
        }
    };

}

TEST_F(AudioOutputStageTest, RoundoffIsTodaysGainAndLinearPassesTheSamples) {
    EXPECT_NEAR(outputGainDb(AudioOutputStage::Roundoff), -1.66, 0.01);
    EXPECT_NEAR(outputGainDb(AudioOutputStage::Linear), 0.0, 0.001);
}

TEST_F(AudioMixerRuntimeTest, ABypassedLimiterWithADelayKeepsItsSlotAndAnotherLeavesIt) {
    AudioMixerDef def = AudioMixerDocument::newProjectMixer();
    AudioMixer::apply(def);
    AudioSource sound = source("tone.wav");
    sound.setLooping(true);
    sound.play();
    run(0.1);
    const std::string id = def.groups.front().effects[0].id;
    SoLoud::Bus* master = AudioMixer::getBus("Master");
    ASSERT_NE(master, nullptr);
    const unsigned int rate = soloud().getBackendSamplerate();
    EXPECT_EQ(AudioMixer::getGroupLatencyFrames("Master"), (unsigned int)std::lround(0.0015 * rate));

    AudioMixer::setEffectBypass(id, true);
    EXPECT_NE(master->mFilter[0], nullptr) << "so that the switch crossfades";
    EXPECT_EQ(AudioMixer::getGroupLatencyFrames("Master"), 0u);
    AudioMixer::setEffectBypass(id, false);

    // no lookahead, no detector: as a limiter always was, an empty slot
    def.groups.front().effects[0].params.erase("lookahead");
    def.groups.front().effects[0].params.erase("truePeak");
    AudioMixer::apply(def);
    AudioMixer::setEffectBypass(id, true);
    EXPECT_EQ(AudioMixer::getBus("Master")->mFilter[0], nullptr);
    sound.stop();
}

// A hot signal through what a new project has: the output holds -1 dBTP, and what the Mixer window
// and agents read at the Master is what leaves the device (the linear stage).
TEST_F(AudioMixerRuntimeTest, ANewProjectHoldsItsCeilingAtTheOutput) {
    AudioSystem::close();
    AudioConfig config;
    config.outputStage = AudioOutputStage::Linear;
    AudioSystem::configure(config);
    AudioMixerDef def = AudioMixerDocument::newProjectMixer();
    AudioMixerGroupDef hot;
    hot.id = "00000002";
    hot.name = "Hot";
    hot.parentId = def.groups.front().id;
    hot.volumeDb = 12.0f;
    def.groups.push_back(hot);
    AudioMixer::apply(def);

    AudioSource sound = source("tone.wav", "Hot");
    sound.setLooping(true);
    sound.play();
    run(0.1);
    AudioMixer::setLoudnessTap(true);
    const unsigned int rate = soloud().getBackendSamplerate();
    AudioMeter atOutput(rate, 2);
    AudioMeter atMaster(rate, 2);
    std::vector<float> out(1024 * 2);
    std::vector<float> tap(1024 * 2);
    for (int block = 0; block < 300; block++) {
        soloud().mix(out.data(), 1024);
        atOutput.process(out.data(), 1024);
        unsigned int channels = 0;
        unsigned int tapRate = 0;
        const size_t frames = AudioMixer::drainLoudnessTap(tap.data(), 1024, channels, tapRate);
        atMaster.process(tap.data(), frames);
    }
    AudioMixer::setLoudnessTap(false);
    sound.stop();

    const LoudnessResult output = atOutput.result();
    const LoudnessResult master = atMaster.result();
    EXPECT_LE(output.truePeakDb, -0.9);
    EXPECT_GT(output.truePeakDb, -3.0) << "the limiter holds it at its ceiling, not under";
    ASSERT_TRUE(output.integrated && master.integrated);
    EXPECT_NEAR(*output.integrated, *master.integrated, 0.1);
}

TEST(GeneratorAudioTest, TheOutputStageAndTheIosSessionReachTheExportedInit) {
    AudioSettings settings;
    settings.outputStage = "linear";
    settings.iosSessionCategory = "ambient";
    EXPECT_NE(Generator::audioInitSource(settings).find(
                  "AudioSystem::configure({32, 256, 0, AudioOutputStage::Linear, IosAudioSession::Ambient});"), std::string::npos)
        << Generator::audioInitSource(settings);
    settings.outputStage = "roundoff";
    settings.iosSessionCategory = "solo_ambient";
    EXPECT_NE(Generator::audioInitSource(settings).find("AudioOutputStage::Roundoff, IosAudioSession::SoloAmbient"), std::string::npos);
    settings.iosSessionCategory = "playback";
    EXPECT_NE(Generator::audioInitSource(settings).find("IosAudioSession::Playback"), std::string::npos);
    // the settings that only measure change no code
    AudioSettings measuring;
    measuring.loudnessTargetLufs = -14.0f;
    EXPECT_EQ(Generator::audioInitSource(measuring), "");
}

TEST(PlayerConfigAudioTest, TheOutputStageAndTheIosSessionTravelInGameJson) {
    player::PlayerConfig game;
    game.audioOutputStage = "linear";
    game.audioIosSession = "ambient";
    player::PlayerConfig back;
    std::string error;
    ASSERT_TRUE(player::PlayerConfig::fromJson(game.toJson(), back, error)) << error;
    EXPECT_EQ(back.audioOutputStage, "linear");
    EXPECT_EQ(back.audioIosSession, "ambient");

    // a game.json of an older export has neither
    EXPECT_EQ(player::PlayerConfig().audioOutputStage, "roundoff");
    EXPECT_EQ(player::PlayerConfig().audioIosSession, "default");
    EXPECT_EQ(AudioSystem::outputStageFromName("roundoff"), AudioOutputStage::Roundoff);
    EXPECT_EQ(AudioSystem::outputStageFromName("linear"), AudioOutputStage::Linear);
    EXPECT_EQ(AudioSystem::iosSessionFromName("default"), IosAudioSession::Default);
    EXPECT_EQ(AudioSystem::iosSessionFromName("ambient"), IosAudioSession::Ambient);
    EXPECT_EQ(AudioSystem::iosSessionFromName("solo_ambient"), IosAudioSession::SoloAmbient);
    EXPECT_EQ(AudioSystem::iosSessionFromName("playback"), IosAudioSession::Playback);
}

TEST(NewProjectAudioTest, TheMixerIsTheSpecsAndATemplatesAudioNodeIsLeftAlone) {
    const AudioMixerDef mixer = AudioMixerDocument::newProjectMixer();
    const nlohmann::json json = nlohmann::json::parse(AudioMixerIO::write(mixer));
    EXPECT_EQ(json["format"], "jasperengine.mixer");
    EXPECT_EQ(json["version"], 2);
    EXPECT_EQ(json["updateMode"], "scaled");
    ASSERT_EQ(json["groups"].size(), 1u);
    const nlohmann::json& master = json["groups"][0];
    EXPECT_EQ(master["name"], "Master");
    EXPECT_TRUE(master["parent"].is_null());
    EXPECT_EQ(master["volume"], 0);
    ASSERT_EQ(master["effects"].size(), 1u);
    EXPECT_EQ(master["effects"][0]["type"], "limiter");
    EXPECT_EQ(master["effects"][0]["params"], nlohmann::json::parse(
        R"({"threshold": -1, "release": 0.1, "lookahead": 1.5, "truePeak": 1})"));

    const fs::path dir = fs::temp_directory_path() / ("jasper-new-audio-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "project.yaml") << "name: T\nassetsDir: assets\n";
    }
    std::string error;
    ASSERT_TRUE(AudioMixerDocument::writeNewProjectAudio(dir, &error)) << error;
    EXPECT_TRUE(fs::is_regular_file(dir / "assets" / "audio" / "Main.mixer"));
    YAML::Node written = YAML::LoadFile((dir / "project.yaml").string());
    EXPECT_EQ(written["audio"]["mixer"].as<std::string>(), "audio/Main.mixer");
    EXPECT_EQ(written["audio"]["outputStage"].as<std::string>(), "linear");
    EXPECT_EQ(written["audio"]["iosSessionCategory"].as<std::string>(), "ambient");
    EXPECT_EQ(written["name"].as<std::string>(), "T");

    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "project.yaml") << "name: T\nassetsDir: assets\naudio:\n  maxVoices: 12\n";
    }
    ASSERT_TRUE(AudioMixerDocument::writeNewProjectAudio(dir, &error)) << error;
    EXPECT_FALSE(fs::exists(dir / "assets" / "audio"));
    EXPECT_EQ(YAML::LoadFile((dir / "project.yaml").string())["audio"]["maxVoices"].as<int>(), 12);
    fs::remove_all(dir);
}

// S40 + S39: a timeline's Mixer Snapshot action and a music switch asking for other snapshots in one
// frame. The animation update runs first and the music's switch after it (MusicPlayer, in the audio
// update), so the music's snapshot holds; at the action's end its revert leaves it, since the mixer no
// longer shows the action's own snapshot (S40 R18, Deviations "Snapshots with music")
TEST_F(AudioMixerMathRuntimeTest, MusicSwitchInTheFrameOfASnapshotActionHolds) {
    loadMixer(R"({"format": "jasperengine.mixer", "groups": [{"id": "00000001", "name": "Master"}],
      "snapshots": [{"name": "Calm", "values": {}}, {"name": "Duck", "values": {}}, {"name": "Combat", "values": {}}],
      "startSnapshot": "Calm"})");
    ASSERT_EQ(AudioMixer::getCurrentSnapshot(), "Calm");

    auto timeline = [this](){
        const Entity snapshot = scene->createEntity();
        scene->addComponent<ActionComponent>(snapshot, {});
        AudioSnapshotComponent duck;
        duck.snapshot = "Duck";
        scene->addComponent<AudioSnapshotComponent>(snapshot, duck);
        const Entity animation = scene->createEntity();
        scene->addComponent<ActionComponent>(animation, {});
        AnimationComponent frames;
        ActionFrame frame;
        frame.startTime = 0.0f;
        frame.duration = 0.1f;
        frame.action = snapshot;
        frames.actions.push_back(frame);
        scene->addComponent<AnimationComponent>(animation, frames);
        scene->getComponent<ActionComponent>(animation).startTrigger = true;
        return snapshot;
    };
    ActionSystem* actions = scene->getSystem<ActionSystem>().get();

    // the control: alone, the action reverts to what was there at its end
    const Entity alone = timeline();
    actions->update(1.0 / 60.0);
    EXPECT_EQ(AudioMixer::getCurrentSnapshot(), "Duck") << "the action transitions at its start";
    for (int i = 0; i < 12; i++) actions->update(1.0 / 60.0);
    EXPECT_EQ(AudioMixer::getCurrentSnapshot(), "Calm") << "and reverts at its end, the mixer still showing its snapshot";
    EXPECT_FALSE(scene->getComponent<AudioSnapshotComponent>(alone).applied);
    scene->destroyEntity(alone);

    // with a music switch in the action's first frame
    const Entity withMusic = timeline();
    actions->update(1.0 / 60.0);
    ASSERT_EQ(AudioMixer::getCurrentSnapshot(), "Duck");
    // the music player's switch in the same frame, after the animation update (MusicPlayer.cpp's call)
    ASSERT_TRUE(AudioMixer::transitionTo("Combat", 0.0f, false, "music"));
    EXPECT_EQ(AudioMixer::getCurrentSnapshot(), "Combat") << "the last asked holds";
    for (int i = 0; i < 12; i++) actions->update(1.0 / 60.0);
    EXPECT_FALSE(scene->getComponent<AudioSnapshotComponent>(withMusic).applied) << "the action ended";
    EXPECT_EQ(AudioMixer::getCurrentSnapshot(), "Combat") << "its revert leaves the music's snapshot";
}
