// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Several Audio Sources on one GameObject (docs/specs/S97-audio-gaps.md REQ-24 to REQ-29, AC23,
// AC26): each further source is a slot, a hidden child entity with its own Audio Source. The
// runtime half (AudioSourceSlots, the Lua API, persistence) plays on SoLoud's null driver; the
// editor half (the slot commands, their undo, the listings, the load-time repair) on a Project.

#include "ScriptTestHarness.h"

#include "LogCapture.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AudioSourceSlotCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "picking/ScenePicker.h"
#include "component/AudioSourceComponent.h"
#include "camera/CameraType.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "manager/SceneManager.h"
#include "object/GameObject.h"
#include "object/sound/AudioSource.h"
#include "object/sound/AudioSourceSlots.h"
#include "subsystem/AudioSystem.h"
#include "support/TestTempDir.h"
#include "support/EditorTestSupport.h"
#include "log/Logger.h"

#include "soloud.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace audio_source_slots_test {

    namespace fs = std::filesystem;

    // a mono 16-bit PCM WAV of silence
    void writeSilence(const fs::path& path, int samples) {
        std::ofstream out(path, std::ios::binary);
        auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        const uint32_t dataSize = static_cast<uint32_t>(samples) * 2;
        out.write("RIFF", 4); u32(36 + dataSize); out.write("WAVE", 4);
        out.write("fmt ", 4); u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
        out.write("data", 4); u32(dataSize);
        for (int i = 0; i < samples; i++) u16(0);
    }

    // the runtime: scenes built in code, frames of the engine, the null audio driver
    class AudioSourceSlotsTest : public ScriptTest {
    protected:
        AudioBackend backend = AudioBackend::Null;
        bool asyncLoading = false;

        void SetUp() override {
            ScriptTest::SetUp();
            SceneManager::setPersistentSceneFactory(nullptr, nullptr);
            backend = AudioSystem::getBackend();
            AudioSystem::close();
            AudioSystem::setBackend(AudioBackend::Null);
            // the clips load at once: frameUntilPlaying counts frames, which a background decode
            // can outlast on a slow runner
            asyncLoading = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            writeSilence(dir / "loop.wav", 8000 * 5);
        }

        void TearDown() override {
            ScriptTest::TearDown();
            AudioSystem::close();
            AudioSystem::setBackend(backend);
            Engine::setAsyncLoading(asyncLoading);
        }

        // a camera at the origin: the 3D listener
        void addCamera(Scene* scene) {
            const Entity camera = entity(scene, "Camera");
            CameraComponent component;
            component.type = CameraType::CAMERA_PERSPECTIVE;
            scene->addComponent<CameraComponent>(camera, component);
            scene->setCamera(camera);
        }

        // a playing looping source of loop.wav
        static void playLoop(AudioSource source) {
            AudioSourceComponent& audio = source.getComponent<AudioSourceComponent>();
            audio.filename = "loop.wav";
            audio.looping = true;
            audio.state = AudioSourceState::Playing;
        }

        // frames until both sources have a voice
        void frameUntilPlaying(Scene* scene, Entity a, Entity b) {
            for (int i = 0; i < 30; i++) {
                if (scene->getComponent<AudioSourceComponent>(a).handle != 0 && scene->getComponent<AudioSourceComponent>(b).handle != 0) return;
                frame();
            }
        }
    };

    // the editor: a copy of a fixture project, its main scene
    class AudioSourceSlotsEditorTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"audio-source-slots"};
        std::unique_ptr<editor::Project> project;
        editor::SceneProject* scene = nullptr;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<editor::Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (editor::SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) {
                    project->loadScene(candidate.filepath, false, false, true);
                }
            }
            for (editor::SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            project->setSelectedSceneId(scene->id);
        }

        void TearDown() override {
            if (project) {
                for (editor::SceneProject& candidate : project->getScenes()) {
                    editor::CommandHandle::remove(candidate.id);
                }
                project->releaseScenes();
            }
            project.reset();
        }

        editor::CommandHistory* history() { return editor::CommandHandle::get(scene->id); }

        // a root GameObject with an Audio Source, made outside the history
        Entity makeCar(const std::string& name) {
            Scene* registry = scene->scene;
            const Entity car = registry->createUserEntity();
            registry->setEntityName(car, name);
            registry->addComponent<Transform>(car, Transform());
            registry->addComponent<AudioSourceComponent>(car, AudioSourceComponent());
            scene->entities.push_back(car);
            return car;
        }

        Entity addSlot(Entity owner) {
            auto* cmd = new editor::AddAudioSourceSlotCmd(project.get(), scene->id, owner);
            if (!history()->addCommandNoMerge(cmd)) return NULL_ENTITY;
            return cmd->getSlotEntity();
        }

        bool listed(Entity entity) {
            return std::find(scene->entities.begin(), scene->entities.end(), entity) != scene->entities.end();
        }
    };

}

using namespace audio_source_slots_test;

// AC23 (runtime): both sources of a GameObject play, and the slot's 3D position follows it
TEST_F(AudioSourceSlotsTest, BothSourcesPlayAndTheSlotFollowsItsGameObject) {
    Scene* scene = newScene();
    addCamera(scene);
    const Entity car = entity(scene, "Car");
    GameObject carObject(scene, car);
    AudioSource engine = AudioSourceSlots::add(carObject);
    AudioSource horn = AudioSourceSlots::add(carObject);
    ASSERT_EQ(engine.getEntity(), car) << "the first is the GameObject's own";
    ASSERT_NE(horn.getEntity(), car) << "the second is a slot";
    EXPECT_TRUE(AudioSourceSlots::isSlot(scene, horn.getEntity()));
    EXPECT_EQ(scene->getComponent<AudioSourceComponent>(horn.getEntity()).slot, 1);
    EXPECT_EQ(scene->getComponent<Transform>(horn.getEntity()).parent, car);
    playLoop(engine);
    playLoop(horn);
    Engine::setScene(scene);
    frameUntilPlaying(scene, car, horn.getEntity());
    const unsigned int engineVoice = scene->getComponent<AudioSourceComponent>(car).handle;
    const unsigned int hornVoice = scene->getComponent<AudioSourceComponent>(horn.getEntity()).handle;
    ASSERT_NE(engineVoice, 0u);
    ASSERT_NE(hornVoice, 0u);
    EXPECT_NE(engineVoice, hornVoice) << "two voices";
    EXPECT_TRUE(AudioSystem::getSoloud().isValidVoiceHandle(engineVoice));
    EXPECT_TRUE(AudioSystem::getSoloud().isValidVoiceHandle(hornVoice));

    carObject.setPosition(Vector3(5, 0, -3));
    frame();
    frame();
    EXPECT_EQ(scene->getComponent<AudioSourceComponent>(horn.getEntity()).lastPosition, Vector3(5, 0, -3))
        << "the slot plays where its GameObject is";
}

// AC25's engine half (REQ-27): getAll in order, add, remove, and findObjectOfType sees slots
TEST_F(AudioSourceSlotsTest, LuaGetsAddsAndRemovesSources) {
    Scene* scene = newScene();
    const Entity car = entity(scene, "Car");
    Engine::setScene(scene);
    setScene("S", scene);
    lua("car = GameObject(S, " + std::to_string(car) + ")\n"
        "own = AudioSource.add(car)\n"
        "first = AudioSource.add(car)\n"
        "second = AudioSource.add(car)");
    EXPECT_EQ(lua("return own.entity == car.entity"), "true");
    EXPECT_EQ(lua("local all = AudioSource.getAll(car); return #all .. ' ' .. tostring(all[1].entity == own.entity) .. ' ' .. "
                  "tostring(all[2].entity == first.entity) .. ' ' .. tostring(all[3].entity == second.entity)"),
              "3 true true true");
    EXPECT_EQ(lua("return first:getAudioSourceComponent().slot .. ' ' .. second:getAudioSourceComponent().slot"), "1 2");
    lua("AudioSource.remove(first)");
    EXPECT_EQ(lua("local all = AudioSource.getAll(car); return #all .. ' ' .. tostring(all[2].entity == second.entity)"), "2 true");
    lua("AudioSource.remove(own)");
    EXPECT_EQ(scene->findComponent<AudioSourceComponent>(car), nullptr) << "the GameObject's own source is gone";
    EXPECT_EQ(lua("local all = AudioSource.getAll(car); return #all .. ' ' .. tostring(all[1].entity == second.entity)"), "1 true");
    EXPECT_EQ(lua("return GameObject.findObjectOfType('AudioSource').entity == second.entity"), "true") << "a slot is found as a source";
    lua("again = AudioSource.add(car)");
    EXPECT_EQ(lua("return again.entity == car.entity"), "true") << "a GameObject without its own source gets it back first";
}

// REQ-25: a further Audio Source is a component, as in Unity: the child APIs and GameObject.find
// leave its slot entity out, while the GameObject's real children stay
TEST_F(AudioSourceSlotsTest, ScriptsWalkingChildrenSeeNoSlots) {
    Scene* scene = newScene();
    const Entity car = entity(scene, "Car");
    const Entity wheel = entity(scene, "Wheel", car);
    GameObject carObject(scene, car);
    AudioSourceSlots::add(carObject);
    const Entity horn = AudioSourceSlots::add(carObject).getEntity();
    ASSERT_TRUE(AudioSourceSlots::isSlot(scene, horn));
    const std::string hornName = scene->getEntityName(horn);
    EXPECT_EQ(carObject.getChildCount(), 1u);
    EXPECT_EQ(carObject.getChild(0)->getEntity(), wheel);
    EXPECT_FALSE(carObject.getChild(1).has_value());
    EXPECT_FALSE(carObject.findChild(hornName).has_value());

    Engine::setScene(scene);
    setScene("S", scene);
    lua("car = GameObject(S, " + std::to_string(car) + ")\nHORN = '" + hornName + "'");
    EXPECT_EQ(lua("local children = car:getChildren(); return car.childCount .. ' ' .. #children .. ' ' .. children[1].name"), "1 1 Wheel");
    EXPECT_EQ(lua("return tostring(car:findChild(HORN))"), "nil");
    EXPECT_EQ(lua("return tostring(GameObject.find(HORN)) .. ' ' .. tostring(GameObject.find('Car/' .. HORN))"), "nil nil");
    EXPECT_EQ(lua("return GameObject.find('Car/Wheel').name"), "Wheel");
    EXPECT_EQ(lua("return #AudioSource.getAll(car)"), "2") << "the sources still hold both";
}

// AC26 (REQ-24, S5): a persistent GameObject keeps the voices of both its sources across a load
TEST_F(AudioSourceSlotsTest, SlotsMoveWithDontDestroyOnLoad) {
    Scene* first = nullptr;
    Entity car = NULL_ENTITY;
    SceneManager::registerScene(1, "First", [this, &first, &car]() {
        first = newScene();
        addCamera(first);
        car = entity(first, "Car");
        GameObject carObject(first, car);
        playLoop(AudioSourceSlots::add(carObject));
        playLoop(AudioSourceSlots::add(carObject));
        Engine::setScene(first);
    });
    SceneManager::registerScene(2, "Second", [this, &first]() {
        Engine::setScene(newScene());
        LuaBinding::cleanupLuaScripts(first);
        ObjectManager::forgetScene(first);
        scenes.erase(std::remove_if(scenes.begin(), scenes.end(), [first](const std::unique_ptr<Scene>& scene) { return scene.get() == first; }),
                     scenes.end());
        first = nullptr;
    });
    ASSERT_TRUE(SceneManager::loadScene("First"));
    frame();
    ASSERT_NE(first, nullptr);
    const std::vector<Entity> slots = AudioSourceSlots::getSlots(first, car);
    ASSERT_EQ(slots.size(), 1u);
    frameUntilPlaying(first, car, slots[0]);
    const unsigned int engineVoice = first->getComponent<AudioSourceComponent>(car).handle;
    const unsigned int hornVoice = first->getComponent<AudioSourceComponent>(slots[0]).handle;
    ASSERT_NE(engineVoice, 0u);
    ASSERT_NE(hornVoice, 0u);

    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(first, car));
    ASSERT_TRUE(SceneManager::loadScene("Second"));
    frame();
    frame();
    Scene* persistent = SceneManager::getPersistentScene();
    ASSERT_NE(persistent, nullptr);
    Entity moved = NULL_ENTITY;
    for (Entity e : persistent->getEntityList()) {
        if (persistent->getEntityName(e) == "Car") moved = e;
    }
    ASSERT_NE(moved, NULL_ENTITY);
    const std::vector<Entity> movedSlots = AudioSourceSlots::getSlots(persistent, moved);
    ASSERT_EQ(movedSlots.size(), 1u) << "the slot went with its GameObject";
    EXPECT_EQ(persistent->getComponent<AudioSourceComponent>(moved).handle, engineVoice);
    EXPECT_EQ(persistent->getComponent<AudioSourceComponent>(movedSlots[0]).handle, hornVoice);
    EXPECT_TRUE(AudioSystem::getSoloud().isValidVoiceHandle(engineVoice)) << "the GameObject's own voice plays on";
    EXPECT_TRUE(AudioSystem::getSoloud().isValidVoiceHandle(hornVoice)) << "the slot's voice plays on";
    Engine::removeAllScenes();
}

// AC23 (editor): Add Component adds a hidden child that stays out of the listings; duplicate,
// delete, reset and Remove Component keep it a slot of its GameObject, each one undo step
TEST_F(AudioSourceSlotsEditorTest, SecondSourceIsAHiddenChildThatFollows) {
    Scene* registry = scene->scene;
    const Entity car = makeCar("Car");
    project->setSelectedEntity(scene->id, car);

    const Entity horn = addSlot(car);
    ASSERT_NE(horn, NULL_ENTITY);
    EXPECT_TRUE(AudioSourceSlots::isSlot(registry, horn));
    EXPECT_EQ(registry->getComponent<Transform>(horn).parent, car);
    EXPECT_EQ(registry->getComponent<Transform>(horn).position, Vector3(0, 0, 0)) << "at the local origin";
    EXPECT_EQ(registry->getComponent<AudioSourceComponent>(horn).slot, 1);
    EXPECT_EQ(project->getSelectedEntities(scene->id), std::vector<Entity>{car}) << "the GameObject stays selected";
    EXPECT_EQ(editor::ScenePicker::pickedEntity(registry, horn), car) << "picked as its GameObject, never by itself";
    EXPECT_EQ(editor::ScenePicker::pickedEntity(registry, car), car);

    const Entity siren = addSlot(car);
    EXPECT_EQ(registry->getComponent<AudioSourceComponent>(siren).slot, 2);
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(registry->isEntityCreated(siren));
    ASSERT_TRUE(history()->redo());
    ASSERT_TRUE(registry->isEntityCreated(siren));
    EXPECT_EQ(registry->getComponent<AudioSourceComponent>(siren).slot, 2) << "the redo is the same slot";
    EXPECT_EQ(AudioSourceSlots::getSlots(registry, car), (std::vector<Entity>{horn, siren}));

    // Reset and Paste Component Values never take a source out of its place
    registry->getComponent<AudioSourceComponent>(horn).volume = 0.25;
    editor::Command* reset = editor::ComponentValuesCmd::buildReset(project.get(), scene->id, {horn}, editor::ComponentType::AudioSourceComponent);
    ASSERT_NE(reset, nullptr);
    ASSERT_TRUE(history()->addCommandNoMerge(reset));
    EXPECT_DOUBLE_EQ(registry->getComponent<AudioSourceComponent>(horn).volume, 1.0);
    EXPECT_EQ(registry->getComponent<AudioSourceComponent>(horn).slot, 1) << "Reset keeps the slot";

    // Move Down swaps the two slots
    editor::Command* moveDown = editor::buildMoveAudioSourceSlot(project.get(), scene->id, horn, 1);
    ASSERT_NE(moveDown, nullptr);
    ASSERT_TRUE(history()->addCommandNoMerge(moveDown));
    EXPECT_EQ(AudioSourceSlots::getSlots(registry, car), (std::vector<Entity>{siren, horn}));
    EXPECT_EQ(editor::buildMoveAudioSourceSlot(project.get(), scene->id, horn, 1), nullptr) << "the last moves no further down";
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(AudioSourceSlots::getSlots(registry, car), (std::vector<Entity>{horn, siren}));

    // a duplicate has its own two slots
    auto* duplicate = new editor::DuplicateEntityCmd(project.get(), scene->id, {car});
    ASSERT_TRUE(history()->addCommandNoMerge(duplicate));
    const std::vector<Entity> copies = duplicate->getCreatedEntities();
    ASSERT_EQ(copies.size(), 3u) << "the copy and its two slots";
    const auto copy = std::find_if(copies.begin(), copies.end(), [registry](Entity e) { return !AudioSourceSlots::isSlot(registry, e); });
    ASSERT_NE(copy, copies.end());
    const std::vector<Entity> copiedSlots = AudioSourceSlots::getSlots(registry, *copy);
    ASSERT_EQ(copiedSlots.size(), 2u) << "2 + 2 sources";
    EXPECT_EQ(registry->getComponent<AudioSourceComponent>(copiedSlots[0]).slot, 1);
    EXPECT_EQ(registry->getComponent<AudioSourceComponent>(copiedSlots[1]).slot, 2);
    ASSERT_TRUE(history()->undo());
    for (Entity created : copies) EXPECT_FALSE(registry->isEntityCreated(created));

    // Remove Component on a slot takes its entity; the undo brings it back
    ASSERT_TRUE(history()->addCommandNoMerge(new editor::RemoveAudioSourceSlotCmd(project.get(), scene->id, siren)));
    EXPECT_FALSE(registry->isEntityCreated(siren));
    EXPECT_FALSE(listed(siren));
    ASSERT_TRUE(history()->undo());
    ASSERT_TRUE(registry->isEntityCreated(siren));
    EXPECT_EQ(registry->getComponent<AudioSourceComponent>(siren).slot, 2);

    // deleting the GameObject deletes its sources; the undo restores them as slots
    ASSERT_TRUE(history()->addCommandNoMerge(new editor::DeleteEntityCmd(project.get(), scene->id, car)));
    EXPECT_FALSE(registry->isEntityCreated(car));
    EXPECT_FALSE(registry->isEntityCreated(horn));
    EXPECT_FALSE(registry->isEntityCreated(siren));
    ASSERT_TRUE(history()->undo());
    ASSERT_TRUE(registry->isEntityCreated(horn));
    EXPECT_EQ(AudioSourceSlots::getSlots(registry, car), (std::vector<Entity>{horn, siren}));

    // a slot takes no slot of its own; a GameObject without a Transform takes no second source
    EXPECT_FALSE(editor::AddAudioSourceSlotCmd::refusal(registry, horn).empty());
}

// AC23 (REQ-29): a slot without a GameObject over it, and two slots with one number, load with a
// warning instead of failing
TEST_F(AudioSourceSlotsEditorTest, BrokenSlotsLoadWithAWarning) {
    Scene* registry = scene->scene;
    const Entity car = makeCar("Car");
    const Entity horn = addSlot(car);
    const Entity siren = addSlot(car);
    ASSERT_NE(siren, NULL_ENTITY);
    registry->getComponent<AudioSourceComponent>(siren).slot = 1;   // both 1, as a merge could leave them
    const Entity stray = makeCar("Stray");
    registry->getComponent<AudioSourceComponent>(stray).slot = 3;   // a slot at the root
    const fs::path saved = dir.path() / "assets" / "broken-slots.scene";
    ASSERT_TRUE(project->saveSceneToPath(scene->id, saved));

    // decoded as Project::loadScene decodes it
    jasperengine::test::LogCapture log;
    editor::test::DecodedScene loaded = editor::test::decodeSceneFile(saved);
    jasperengine::Logger::flush();
    ASSERT_NE(loaded.scene, nullptr);
    Scene* again = loaded.scene->scene;
    ASSERT_NE(again, nullptr);
    ASSERT_TRUE(again->isEntityCreated(stray));
    EXPECT_EQ(again->getComponent<AudioSourceComponent>(stray).slot, 0) << "an ordinary GameObject now";
    EXPECT_EQ(log.count(D_LOG_WARN, "'Stray'"), 1u) << "named in a warning";
    EXPECT_EQ(again->getComponent<AudioSourceComponent>(horn).slot, 1);
    EXPECT_EQ(again->getComponent<AudioSourceComponent>(siren).slot, 2) << "numbered again in entity order";
    EXPECT_EQ(log.count(D_LOG_WARN, "share slot numbers"), 1u);
}
