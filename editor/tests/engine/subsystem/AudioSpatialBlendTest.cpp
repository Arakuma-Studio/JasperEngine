// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Spatial Blend and the GameObject of every new Audio Source (docs/specs/S97-audio-gaps.md REQ-18
// to REQ-20, AC17 to AC19): 0 plays 2D, 1 through SoLoud's 3D, a value between them a 2D voice the
// engine attenuates and pans. On SoLoud's null backend, mixed by the test, so every value is exact
// and no audio device is needed.

#include <gtest/gtest.h>

#include "ScriptDataTestSupport.h"
#include "support/EditorTestSupport.h"

#include "Scene.h"
#include "Project.h"
#include "audio/AudioMixer.h"
#include "camera/CameraType.h"
#include "component/AudioSourceComponent.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "object/GameObject.h"
#include "object/sound/AudioSource.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include "LuaBridge.h"
#include "soloud.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace audio_spatial_blend_test {

    namespace fs = std::filesystem;

    // a 16-bit mono PCM WAV of a sine
    void writeSine(const fs::path& path, double seconds, int rate = 44100) {
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
            const double value = 0.5 * std::sin(2.0 * M_PI * 1000.0 * i / rate);
            u16((uint16_t)(int16_t)std::lround(value * 32767.0));
        }
    }

    // the pan of a 2D voice whose channels are as loud, one to the other, as `left` and `right`
    // (SoLoud's 2D pan law: left cos, right sin of (pan + 1) pi / 4)
    float panOf(float left, float right) {
        return (float)(std::atan2(right, left) * 4.0 / M_PI - 1.0);
    }

    class AudioSpatialBlendTest : public scriptdata::ScriptDataTest {
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
            AudioSystem::setListenerPaused(false);
            AudioMixer::setMasterPaused(false);
            AudioMixer::unload();
            SoundPool::clear();
            writeSine(root / "assets" / "tone.wav", 1.0);
        }

        void TearDown() override {
            if (scene) {
                Engine::removeScene(scene);
                if (owned) delete scene;
                scene = nullptr;
            }
            AudioSystem::close();
            AudioMixer::unload();
            SoundPool::clear();
            Engine::setAsyncLoading(wasAsync);
            ScriptDataTest::TearDown();
        }

        bool owned = false;

        // the game's main scene, its camera at the origin looking down -Z (the 3D listener)
        void makeScene(Scene* existing = nullptr) {
            owned = existing == nullptr;
            scene = existing ? existing : new Scene();
            Engine::setScene(scene);
        }

        void addCamera(Entity entity = NULL_ENTITY) {
            if (entity == NULL_ENTITY) {
                entity = scene->createEntity();
                scene->addComponent<Transform>(entity, Transform());
                CameraComponent camera;
                camera.type = CameraType::CAMERA_PERSPECTIVE;
                camera.autoResize = false;
                scene->addComponent<CameraComponent>(entity, camera);
            }
            scene->setCamera(entity);
            scene->getComponent<Transform>(entity).worldPosition = Vector3(0, 0, 0);
            CameraComponent& camera = scene->getComponent<CameraComponent>(entity);
            camera.worldTarget = Vector3(0, 0, -1);
            camera.steadyWorldUp = Vector3(0, 1, 0);
        }

        // one frame: the mixer mixes `dt` of audio, the scene's system updates
        void step(double dt = 0.02) {
            AudioMixer::update(dt, dt);
            scene->getSystem<AudioSystem>()->update(dt);
        }

        void run(double seconds) {
            for (double t = 0.0; t < seconds - 1e-9; t += 0.02) step(0.02);
        }

        // a looping source 10 units to the right of the listener, linear rolloff from 0 to 20:
        // its 3D gain is 0.5
        AudioSource spatial(float blend) {
            AudioSource sound(scene, true);
            AudioSourceComponent& audio = sound.getComponent<AudioSourceComponent>();
            audio.attenuationModel = AudioSourceAttenuation::LINEAR_DISTANCE;
            audio.minDistance = 0.0f;
            audio.maxDistance = 20.0f;
            audio.spatialBlend = blend;
            sound.setLooping(true);
            sound.loadSound("tone.wav");
            sound.getComponent<Transform>().worldPosition = Vector3(10, 0, 0);
            return sound;
        }

        SoLoud::Soloud& soloud() { return AudioSystem::getSoloud(); }
        unsigned int handleOf(AudioSource& sound) { return sound.getComponent<AudioSourceComponent>().handle; }
        float channel(unsigned int handle, int index) {
            return soloud().mVoice[soloud().getVoiceFromHandle_internal(handle)]->mChannelVolume[index];
        }
    };

    // AC18: a scene saved before S97 (no spatialBlend key). Recorded before the change, on its code:
    // Flat (no Transform, pan 0.25, volume 0.8) played 2D with pan 0.25 and gain 0.8; Spatial (a
    // Transform 10 units right of the camera, linear 0..20, volume 0.8) played 3D with gain 0.4 and
    // channel volumes 0.0264 (left) and 0.4736 (right)
    const char* const kSceneBeforeS97 =
        "formatVersion: 4\nid: 97\nname: BeforeS97\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\nentities:\n"
        "  - type: Entity\n    entity: 1001\n    name: Camera\n    components:\n"
        "      transform:\n        position: [0, 0, 0]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n"
        "      camera:\n        type: camera_perspective\n"
        "  - type: Entity\n    entity: 1002\n    name: Flat\n    components:\n"
        "      audiosource:\n        filename: tone.wav\n        state: playing\n        looping: true\n"
        "        volume: 0.8\n        pan: 0.25\n"
        "  - type: Entity\n    entity: 1003\n    name: Spatial\n    components:\n"
        "      transform:\n        position: [10, 0, 0]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n"
        "      audiosource:\n        filename: tone.wav\n        state: playing\n        looping: true\n"
        "        volume: 0.8\n        minDistance: 0\n        maxDistance: 20\n        attenuationModel: linear_distance\n";

    Entity named(Scene* scene, const std::vector<Entity>& entities, const std::string& name) {
        for (Entity entity : entities) {
            if (scene->getEntityName(entity) == name) return entity;
        }
        return NULL_ENTITY;
    }

}

using namespace audio_spatial_blend_test;

TEST_F(AudioSpatialBlendTest, BlendScalesGainAndPan) {
    makeScene();
    addCamera();
    AudioSource flat = spatial(0.0f);
    AudioSource full = spatial(1.0f);
    AudioSource half = spatial(0.5f);
    flat.play();
    full.play();
    half.play();
    run(0.1);
    const unsigned int flatVoice = handleOf(flat);
    const unsigned int fullVoice = handleOf(full);
    const unsigned int halfVoice = handleOf(half);
    ASSERT_NE(flatVoice, 0u);
    ASSERT_NE(fullVoice, 0u);
    ASSERT_NE(halfVoice, 0u);

    // 0: 2D, its own pan, no attenuation
    EXPECT_FLOAT_EQ(soloud().getPan(flatVoice), 0.0f);
    EXPECT_NEAR(soloud().getOverallVolume(flatVoice), 1.0f, 1e-5f);
    // 1: SoLoud's 3D, as before Spatial Blend existed
    EXPECT_NEAR(soloud().getOverallVolume(fullVoice), 0.5f, 1e-4f);
    const float pan3D = panOf(channel(fullVoice, 0), channel(fullVoice, 1));
    EXPECT_GT(pan3D, 0.9f) << "10 units to the right";
    // 0.5: a 2D voice at lerp(1, 0.5, 0.5) and halfway to the 3D pan
    EXPECT_NEAR(soloud().getOverallVolume(halfVoice), 0.75f, 0.01f);
    EXPECT_NEAR(soloud().getPan(halfVoice), 0.5f * pan3D, 0.02f);

    // the pan follows the source (to the left now), in the frame it moves
    half.getComponent<Transform>().worldPosition = Vector3(-10, 0, 0);
    step();
    EXPECT_NEAR(soloud().getPan(halfVoice), -0.5f * pan3D, 0.02f);
    EXPECT_NEAR(soloud().getOverallVolume(halfVoice), 0.75f, 0.01f);
}

TEST_F(AudioSpatialBlendTest, ABlendChangeMovesAPlayingVoice) {
    makeScene();
    addCamera();
    AudioSource sound = spatial(1.0f);
    sound.play();
    run(0.1);
    const unsigned int voice = handleOf(sound);
    ASSERT_NE(voice, 0u);
    EXPECT_NEAR(soloud().getOverallVolume(voice), 0.5f, 1e-4f);

    // to 2D while it plays: the same voice, at its own pan and volume
    sound.getComponent<AudioSourceComponent>().spatialBlend = 0.0f;
    sound.getComponent<AudioSourceComponent>().pan = -0.5f;
    sound.getComponent<AudioSourceComponent>().needUpdate = true;
    step();
    ASSERT_EQ(handleOf(sound), voice);
    EXPECT_NEAR(soloud().getOverallVolume(voice), 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(soloud().getPan(voice), -0.5f);
    EXPECT_GT(channel(voice, 0), channel(voice, 1)) << "panned left";

    // and back to 3D
    sound.getComponent<AudioSourceComponent>().spatialBlend = 1.0f;
    step();
    ASSERT_EQ(handleOf(sound), voice);
    EXPECT_NEAR(soloud().getOverallVolume(voice), 0.5f, 1e-4f);
    EXPECT_GT(channel(voice, 1), channel(voice, 0)) << "heard on the right again";
}

TEST_F(AudioSpatialBlendTest, OldScenesSoundTheSame) {
    editor::test::DecodedScene decoded = editor::test::decodeSceneText(kSceneBeforeS97, "BeforeS97.scene");
    ASSERT_NE(decoded.scene, nullptr);
    makeScene(decoded.scene->scene);
    const std::vector<Entity>& entities = decoded.scene->entities;
    addCamera(named(scene, entities, "Camera"));
    const Entity flatEntity = named(scene, entities, "Flat");
    const Entity spatialEntity = named(scene, entities, "Spatial");
    ASSERT_NE(flatEntity, NULL_ENTITY);
    ASSERT_NE(spatialEntity, NULL_ENTITY);
    EXPECT_FLOAT_EQ(scene->getComponent<AudioSourceComponent>(flatEntity).spatialBlend, 1.0f) << "absent: the default";
    EXPECT_FLOAT_EQ(scene->getComponent<AudioSourceComponent>(spatialEntity).spatialBlend, 1.0f);
    EXPECT_EQ(scene->findComponent<Transform>(flatEntity), nullptr) << "a bare 2D entity stays bare";

    run(0.1);
    const unsigned int flatVoice = scene->getComponent<AudioSourceComponent>(flatEntity).handle;
    const unsigned int spatialVoice = scene->getComponent<AudioSourceComponent>(spatialEntity).handle;
    ASSERT_NE(flatVoice, 0u);
    ASSERT_NE(spatialVoice, 0u);
    EXPECT_FLOAT_EQ(soloud().getPan(flatVoice), 0.25f);
    EXPECT_NEAR(soloud().getOverallVolume(flatVoice), 0.8f, 1e-5f);
    EXPECT_NEAR(soloud().getOverallVolume(spatialVoice), 0.4f, 1e-4f);
    EXPECT_NEAR(channel(spatialVoice, 0), 0.0264f, 1e-4f);
    EXPECT_NEAR(channel(spatialVoice, 1), 0.4736f, 1e-4f);
    Engine::removeScene(scene);
    scene = nullptr;
}

TEST_F(AudioSpatialBlendTest, NewTwoDSourceHasAGameObject) {
    makeScene();
    addCamera();
    AudioSource sound(scene);
    sound.setLooping(true);
    sound.loadSound("tone.wav");
    ASSERT_NE(scene->findComponent<Transform>(sound.getEntity()), nullptr) << "a GameObject";
    EXPECT_FLOAT_EQ(sound.getComponent<AudioSourceComponent>().spatialBlend, 0.0f);
    EXPECT_FALSE(sound.isSound3D());
    EXPECT_NO_THROW(sound.getGameObject());

    // parented under a moving object it still plays 2D: centred, no attenuation
    const Entity parent = scene->createEntity();
    scene->addComponent<Transform>(parent, Transform());
    scene->addEntityChild(parent, sound.getEntity(), false);
    sound.play();
    for (int i = 1; i <= 5; i++) {
        scene->getComponent<Transform>(parent).worldPosition = Vector3(40.0f * i, 0, 0);
        sound.getComponent<Transform>().worldPosition = Vector3(40.0f * i, 0, 0);
        step();
    }
    const unsigned int voice = handleOf(sound);
    ASSERT_NE(voice, 0u);
    EXPECT_FLOAT_EQ(soloud().getPan(voice), 0.0f);
    EXPECT_NEAR(soloud().getOverallVolume(voice), 1.0f, 1e-5f);

    // sound3D follows the blend and keeps the Transform
    sound.setSound3D(true);
    EXPECT_FLOAT_EQ(sound.getComponent<AudioSourceComponent>().spatialBlend, 1.0f);
    EXPECT_TRUE(sound.isSound3D());
    sound.setSound3D(false);
    EXPECT_FLOAT_EQ(sound.getComponent<AudioSourceComponent>().spatialBlend, 0.0f);
    EXPECT_FALSE(sound.isSound3D());
    EXPECT_NE(scene->findComponent<Transform>(sound.getEntity()), nullptr);

    // sound3D on an entity without a Transform adds one
    AudioSource bare(scene, scene->createEntity());
    bare.addComponent<AudioSourceComponent>();
    bare.setSound3D(true);
    EXPECT_NE(scene->findComponent<Transform>(bare.getEntity()), nullptr);
    EXPECT_FLOAT_EQ(bare.getComponent<AudioSourceComponent>().spatialBlend, 1.0f);
}

TEST_F(AudioSpatialBlendTest, AnOldBareSourceGetsAGameObjectAndStaysTwoD) {
    makeScene();
    addCamera();
    // what AudioSource(scene) made before S97: an entity with only the component (blend 1)
    AudioSource sound(scene, scene->createEntity());
    sound.addComponent<AudioSourceComponent>();
    ASSERT_FALSE(sound.isSound3D());
    GameObject object = sound.getGameObject();
    EXPECT_EQ(object.getEntity(), sound.getEntity());
    EXPECT_NE(scene->findComponent<Transform>(sound.getEntity()), nullptr);
    EXPECT_FLOAT_EQ(sound.getComponent<AudioSourceComponent>().spatialBlend, 0.0f);
    EXPECT_FALSE(sound.isSound3D());
}

TEST_F(AudioSpatialBlendTest, LuaReadsAndClampsTheBlend) {
    makeScene();
    AudioSource sound(scene);
    ASSERT_TRUE(luabridge::push(L, sound));
    lua_setglobal(L, "blendSource");
    EXPECT_EQ(eval("blendSource.spatialBlend = 0.25; return blendSource.spatialBlend == 0.25"), "true");
    EXPECT_EQ(eval("return blendSource.sound3D"), "true");
    EXPECT_EQ(eval("blendSource.spatialBlend = 3; return blendSource.spatialBlend == 1"), "true");
    EXPECT_EQ(eval("blendSource.spatialBlend = -1; return blendSource.spatialBlend == 0 and not blendSource.sound3D"), "true");
    EXPECT_EQ(eval("blendSource.sound3D = true; return blendSource.spatialBlend == 1"), "true");
    EXPECT_EQ(eval("return blendSource:getGameObject() ~= nil"), "true");
    lua_pushnil(L);
    lua_setglobal(L, "blendSource");
}
