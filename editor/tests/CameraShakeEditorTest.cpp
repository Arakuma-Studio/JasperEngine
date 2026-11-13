// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Camera shake and camera follow in the editor (spec 14, editor tests): registration and the
// Catalog, the scene YAML round trip (old scenes without the keys read the defaults), the
// exported-build code (the emitted text compiled here), cloning, the .shake asset write with its
// undo, and the edit-mode preview leaving the scene unmodified.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "Catalog.h"
#include "Factory.h"
#include "Platform.h"
#include "Project.h"
#include "ScriptDataTestSupport.h"
#include "Scene.h"
#include "Stream.h"
#include "camera/ShakePreset.h"
#include "camera/ShakePresets.h"
#include "camera/ShakerRuntime.h"
#include "command/CommandHistory.h"
#include "component/CameraComponent.h"
#include "component/CameraFollowComponent.h"
#include "component/CameraShakerComponent.h"
#include "component/Transform.h"
#include "registry/EntityCloner.h"
#include "subsystem/CameraControlSystem.h"
#include "util/ProjectUtils.h"
#include "window/InspectorWindowCamera.h"

#include <filesystem>
#include <fstream>
#include "support/PosixCompat.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    CameraShakerComponent authoredShaker(){
        CameraShakerComponent shaker;
        shaker.enabled = false;
        shaker.global = false;
        shaker.unscaledTime = true;
        shaker.mode = CameraShakeMode::Mode2D;
        shaker.positionScale = 0.5f;
        shaker.rotationScale = 2.0f;
        shaker.startPreset = "drunk";
        shaker.traumaDecay = 0.75f;
        shaker.traumaPower = 3.0f;
        shaker.traumaMaxOffset = Vector3(0.2f, 0.3f, 0.4f);
        shaker.traumaMaxAngle = Vector3(1.0f, 2.0f, 6.0f);
        shaker.traumaFrequency = 20.0f;
        return shaker;
    }

    CameraFollowComponent authoredFollow(Entity target){
        CameraFollowComponent follow;
        follow.enabled = false;
        follow.target = target;
        follow.offset = Vector3(1, 3, -9);
        follow.offsetSpace = FollowOffsetSpace::TargetYaw;
        follow.damping = Vector3(0.1f, 0.4f, 0.0f);
        follow.maxSpeed = 12.0f;
        follow.lookAt = false;
        follow.lookAtOffset = Vector3(0, 2, 0);
        follow.rotationDamping = 0.25f;
        follow.deadZone = Vector2(2, 1);
        follow.useBounds = true;
        follow.boundsMin = Vector3(-5, -1, -50);
        follow.boundsMax = Vector3(5, 10, 50);
        follow.unscaledTime = true;
        return follow;
    }

    void expectSameShaker(const CameraShakerComponent& a, const CameraShakerComponent& b){
        EXPECT_EQ(a.enabled, b.enabled);
        EXPECT_EQ(a.global, b.global);
        EXPECT_EQ(a.unscaledTime, b.unscaledTime);
        EXPECT_EQ(a.mode, b.mode);
        EXPECT_FLOAT_EQ(a.positionScale, b.positionScale);
        EXPECT_FLOAT_EQ(a.rotationScale, b.rotationScale);
        EXPECT_EQ(a.startPreset, b.startPreset);
        EXPECT_FLOAT_EQ(a.traumaDecay, b.traumaDecay);
        EXPECT_FLOAT_EQ(a.traumaPower, b.traumaPower);
        EXPECT_EQ(a.traumaMaxOffset, b.traumaMaxOffset);
        EXPECT_EQ(a.traumaMaxAngle, b.traumaMaxAngle);
        EXPECT_FLOAT_EQ(a.traumaFrequency, b.traumaFrequency);
    }

    void expectSameFollow(const CameraFollowComponent& a, const CameraFollowComponent& b){
        EXPECT_EQ(a.enabled, b.enabled);
        EXPECT_EQ(a.target, b.target);
        EXPECT_EQ(a.offset, b.offset);
        EXPECT_EQ(a.offsetSpace, b.offsetSpace);
        EXPECT_EQ(a.damping, b.damping);
        EXPECT_FLOAT_EQ(a.maxSpeed, b.maxSpeed);
        EXPECT_EQ(a.lookAt, b.lookAt);
        EXPECT_EQ(a.lookAtOffset, b.lookAtOffset);
        EXPECT_FLOAT_EQ(a.rotationDamping, b.rotationDamping);
        EXPECT_EQ(a.deadZone, b.deadZone);
        EXPECT_EQ(a.useBounds, b.useBounds);
        EXPECT_EQ(a.boundsMin, b.boundsMin);
        EXPECT_EQ(a.boundsMax, b.boundsMax);
        EXPECT_EQ(a.unscaledTime, b.unscaledTime);
    }

    // the code Factory emits for authoredShaker() / authoredFollow(7), pasted: it must compile
    // against the engine headers and build the same components (EMITTED_* hold its text)
    void emittedShaker(Scene* scene, Entity entity){
        CameraShakerComponent camerashaker;
        camerashaker.enabled = false;
        camerashaker.global = false;
        camerashaker.unscaledTime = true;
        camerashaker.mode = CameraShakeMode::Mode2D;
        camerashaker.positionScale = 0.500000f;
        camerashaker.rotationScale = 2.000000f;
        camerashaker.startPreset = "drunk";
        camerashaker.traumaDecay = 0.750000f;
        camerashaker.traumaPower = 3.000000f;
        camerashaker.traumaMaxOffset = Vector3(0.200000f, 0.300000f, 0.400000f);
        camerashaker.traumaMaxAngle = Vector3(1.000000f, 2.000000f, 6.000000f);
        camerashaker.traumaFrequency = 20.000000f;
        scene->addComponent<CameraShakerComponent>(entity, camerashaker);
    }

    const char* const EMITTED_SHAKER =
        "CameraShakerComponent camerashaker;\n"
        "camerashaker.enabled = false;\n"
        "camerashaker.global = false;\n"
        "camerashaker.unscaledTime = true;\n"
        "camerashaker.mode = CameraShakeMode::Mode2D;\n"
        "camerashaker.positionScale = 0.500000f;\n"
        "camerashaker.rotationScale = 2.000000f;\n"
        "camerashaker.startPreset = \"drunk\";\n"
        "camerashaker.traumaDecay = 0.750000f;\n"
        "camerashaker.traumaPower = 3.000000f;\n"
        "camerashaker.traumaMaxOffset = Vector3(0.200000f, 0.300000f, 0.400000f);\n"
        "camerashaker.traumaMaxAngle = Vector3(1.000000f, 2.000000f, 6.000000f);\n"
        "camerashaker.traumaFrequency = 20.000000f;\n"
        "scene->addComponent<CameraShakerComponent>(entity, camerashaker);\n";

    void emittedFollow(Scene* scene, Entity entity){
        CameraFollowComponent camerafollow;
        camerafollow.enabled = false;
        camerafollow.target = 7;
        camerafollow.offset = Vector3(1.000000f, 3.000000f, -9.000000f);
        camerafollow.offsetSpace = FollowOffsetSpace::TargetYaw;
        camerafollow.damping = Vector3(0.100000f, 0.400000f, 0.000000f);
        camerafollow.maxSpeed = 12.000000f;
        camerafollow.lookAt = false;
        camerafollow.lookAtOffset = Vector3(0.000000f, 2.000000f, 0.000000f);
        camerafollow.rotationDamping = 0.250000f;
        camerafollow.deadZone = Vector2(2.000000f, 1.000000f);
        camerafollow.useBounds = true;
        camerafollow.boundsMin = Vector3(-5.000000f, -1.000000f, -50.000000f);
        camerafollow.boundsMax = Vector3(5.000000f, 10.000000f, 50.000000f);
        camerafollow.unscaledTime = true;
        scene->addComponent<CameraFollowComponent>(entity, camerafollow);
    }

    const char* const EMITTED_FOLLOW =
        "CameraFollowComponent camerafollow;\n"
        "camerafollow.enabled = false;\n"
        "camerafollow.target = 7;\n"
        "camerafollow.offset = Vector3(1.000000f, 3.000000f, -9.000000f);\n"
        "camerafollow.offsetSpace = FollowOffsetSpace::TargetYaw;\n"
        "camerafollow.damping = Vector3(0.100000f, 0.400000f, 0.000000f);\n"
        "camerafollow.maxSpeed = 12.000000f;\n"
        "camerafollow.lookAt = false;\n"
        "camerafollow.lookAtOffset = Vector3(0.000000f, 2.000000f, 0.000000f);\n"
        "camerafollow.rotationDamping = 0.250000f;\n"
        "camerafollow.deadZone = Vector2(2.000000f, 1.000000f);\n"
        "camerafollow.useBounds = true;\n"
        "camerafollow.boundsMin = Vector3(-5.000000f, -1.000000f, -50.000000f);\n"
        "camerafollow.boundsMax = Vector3(5.000000f, 10.000000f, 50.000000f);\n"
        "camerafollow.unscaledTime = true;\n"
        "scene->addComponent<CameraFollowComponent>(entity, camerafollow);\n";

    // ---- one headless project for the asset and preview tests

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    Headless& headless(){
        static Headless h = [](){
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-camera-shake-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: camerashake\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            // the editor's Platform as System::instance() for this suite's tests (the binary's engine
            // is TestMain's: a second Engine::systemInit would restart its jobs and logger)
            headlesssystem::use(out.platform.get());
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
        // another test file's project may have taken the engine's Platform since
        headlesssystem::use(h.platform.get());
        return h;
    }

    std::string readAll(const fs::path& path){
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

}

TEST(CameraShakeComponents, RegistrationAndCatalog){
    // appended after the Touch Area (oe-port keeps the Ragdoll between them: its ordinals stay)
    EXPECT_GT(static_cast<int>(ComponentType::CameraShakerComponent), static_cast<int>(ComponentType::TouchAreaComponent));
    EXPECT_EQ(static_cast<int>(ComponentType::CameraFollowComponent), static_cast<int>(ComponentType::CameraShakerComponent) + 1);
    // appended after the older types (prefab override masks use the ordinal); later features append after them
    EXPECT_GE(static_cast<int>(LastComponentType), static_cast<int>(ComponentType::CameraFollowComponent));
    EXPECT_EQ(Catalog::getComponentName(ComponentType::CameraShakerComponent, true), "camerashaker");
    EXPECT_EQ(Catalog::getComponentName(ComponentType::CameraFollowComponent, true), "camerafollow");
    EXPECT_EQ(Catalog::getComponentDisplayName(ComponentType::CameraShakerComponent), "Camera Shaker");
    EXPECT_EQ(Catalog::getComponentDisplayName(ComponentType::CameraFollowComponent), "Camera Follow");
    EXPECT_EQ(Catalog::getComponentType("camerashaker"), ComponentType::CameraShakerComponent);
    EXPECT_EQ(Catalog::getComponentType("CameraFollowComponent"), ComponentType::CameraFollowComponent);
    EXPECT_TRUE(Catalog::hasEnabledProperty(ComponentType::CameraShakerComponent));
    EXPECT_TRUE(Catalog::hasEnabledProperty(ComponentType::CameraFollowComponent));

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<CameraShakerComponent>(entity, authoredShaker());
    scene.addComponent<CameraFollowComponent>(entity, authoredFollow(NULL_ENTITY));
    const std::vector<ComponentType> found = Catalog::findComponents(&scene, entity);
    EXPECT_NE(std::find(found.begin(), found.end(), ComponentType::CameraShakerComponent), found.end());
    EXPECT_NE(std::find(found.begin(), found.end(), ComponentType::CameraFollowComponent), found.end());
    const PropertyData mode = Catalog::findProperty(&scene, entity, ComponentType::CameraShakerComponent, "mode");
    EXPECT_EQ(mode.type, PropertyType::Enum);
    EXPECT_EQ(*static_cast<int*>(mode.ref), (int)CameraShakeMode::Mode2D);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, ComponentType::CameraFollowComponent, "target").type, PropertyType::Entity);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, ComponentType::CameraShakerComponent, "runtime").ref) << "the runtime is no property";
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, ComponentType::CameraFollowComponent, "velocity").ref);
}

TEST(CameraShakeComponents, SceneYamlRoundTripAndOldScenes){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<CameraShakerComponent>(entity, authoredShaker());
    scene.addComponent<CameraFollowComponent>(entity, authoredFollow(5));
    const YAML::Node components = Stream::encodeComponents(entity, &scene, scene.getSignature(entity));
    ASSERT_TRUE(components["camerashaker"]);
    ASSERT_TRUE(components["camerafollow"]);
    EXPECT_EQ(components["camerashaker"]["mode"].as<std::string>(), "2d");
    EXPECT_EQ(components["camerafollow"]["offsetSpace"].as<std::string>(), "target_yaw");

    Scene other;
    const Entity copy = other.createUserEntity();
    Stream::decodeComponents(copy, NULL_ENTITY, &other, components);
    ASSERT_TRUE(other.findComponent<CameraShakerComponent>(copy));
    ASSERT_TRUE(other.findComponent<CameraFollowComponent>(copy));
    expectSameShaker(other.getComponent<CameraShakerComponent>(copy), authoredShaker());
    expectSameFollow(other.getComponent<CameraFollowComponent>(copy), authoredFollow(5));
    EXPECT_EQ(YAML::Dump(Stream::encodeCameraShakerComponent(other.getComponent<CameraShakerComponent>(copy))),
              YAML::Dump(components["camerashaker"])) << "a second trip writes the same text";

    // a scene written before a key existed (or by hand) reads the defaults
    expectSameShaker(Stream::decodeCameraShakerComponent(YAML::Load("{enabled: true}")), CameraShakerComponent());
    expectSameFollow(Stream::decodeCameraFollowComponent(YAML::Load("{target: 3}")), [](){
        CameraFollowComponent f;
        f.target = 3;
        return f;
    }());
    // out-of-range values are clamped as the setters do
    const CameraShakerComponent clamped = Stream::decodeCameraShakerComponent(YAML::Load("{traumaPower: 9, positionScale: -2, mode: sideways}"));
    EXPECT_FLOAT_EQ(clamped.traumaPower, 4.0f);
    EXPECT_FLOAT_EQ(clamped.positionScale, 0.0f);
    EXPECT_EQ(clamped.mode, CameraShakeMode::Auto);

    // decoded over a playing shaker: its shakes play on
    CameraShakerComponent playing;
    playing.runtime = std::make_shared<camera::ShakerRuntime>();
    const CameraShakerComponent decoded = Stream::decodeCameraShakerComponent(components["camerashaker"], &playing);
    EXPECT_EQ(decoded.runtime, playing.runtime);
    expectSameShaker(decoded, authoredShaker());

    // Add Component's remove + undo keeps the authored values
    std::vector<Entity> entities{entity};
    const YAML::Node removed = ProjectUtils::removeEntityComponent(&scene, entity, ComponentType::CameraShakerComponent, entities, true);
    EXPECT_FALSE(scene.findComponent<CameraShakerComponent>(entity));
    ProjectUtils::addEntityComponent(&scene, entity, ComponentType::CameraShakerComponent, entities, removed);
    ASSERT_TRUE(scene.findComponent<CameraShakerComponent>(entity));
    expectSameShaker(scene.getComponent<CameraShakerComponent>(entity), authoredShaker());
}

TEST(CameraShakeComponents, FactoryEmitsCodeThatBuildsTheSameComponents){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<CameraShakerComponent>(entity, authoredShaker());
    scene.addComponent<CameraFollowComponent>(entity, authoredFollow(7));
    EXPECT_EQ(Factory::createComponent(0, &scene, entity, ComponentType::CameraShakerComponent, "", "scene", "entity"), EMITTED_SHAKER);
    EXPECT_EQ(Factory::createComponent(0, &scene, entity, ComponentType::CameraFollowComponent, "", "scene", "entity"), EMITTED_FOLLOW);

    Scene built;
    const Entity builtEntity = built.createUserEntity();
    emittedShaker(&built, builtEntity);
    emittedFollow(&built, builtEntity);
    expectSameShaker(built.getComponent<CameraShakerComponent>(builtEntity), authoredShaker());
    expectSameFollow(built.getComponent<CameraFollowComponent>(builtEntity), authoredFollow(7));

    // defaults emit no mode / space / start preset line
    Scene plain;
    const Entity plainEntity = plain.createUserEntity();
    plain.addComponent<CameraShakerComponent>(plainEntity, {});
    const std::string code = Factory::createComponent(0, &plain, plainEntity, ComponentType::CameraShakerComponent, "", "scene", "entity");
    EXPECT_EQ(code.find(".mode"), std::string::npos) << code;
    EXPECT_EQ(code.find(".startPreset"), std::string::npos) << code;
}

TEST(CameraShakeComponents, CloningResetsTheRuntimeAndRemapsTheTarget){
    Scene scene;
    const Entity root = scene.createUserEntity();
    scene.addComponent<Transform>(root, {});
    const Entity target = scene.createUserEntity();
    scene.addComponent<Transform>(target, {});
    scene.addEntityChild(root, target, false);
    const Entity outside = scene.createUserEntity();
    scene.addComponent<Transform>(outside, {});

    CameraComponent cameraComponent;
    cameraComponent.hasViewOffset = true;
    scene.addComponent<CameraComponent>(root, cameraComponent);
    CameraShakerComponent shaker = authoredShaker();
    shaker.runtime = std::make_shared<camera::ShakerRuntime>();
    scene.addComponent<CameraShakerComponent>(root, shaker);
    CameraFollowComponent follow = authoredFollow(target);
    follow.placed = true;
    follow.velocity = Vector3(1, 2, 3);
    scene.addComponent<CameraFollowComponent>(root, follow);

    const Entity copy = EntityCloner::clone(&scene, EntityCloner::collectHierarchy(&scene, root), &scene)[0];
    ASSERT_NE(copy, NULL_ENTITY);
    EXPECT_FALSE(scene.getComponent<CameraShakerComponent>(copy).runtime);
    expectSameShaker(scene.getComponent<CameraShakerComponent>(copy), authoredShaker());
    const CameraFollowComponent& copied = scene.getComponent<CameraFollowComponent>(copy);
    EXPECT_NE(copied.target, target) << "a target inside the copy points at the copy's own";
    EXPECT_NE(copied.target, NULL_ENTITY);
    EXPECT_FALSE(copied.placed);
    EXPECT_EQ(copied.velocity, Vector3(0, 0, 0));
    EXPECT_FALSE(scene.getComponent<CameraComponent>(copy).hasViewOffset);

    scene.getComponent<CameraFollowComponent>(root).target = outside;
    const Entity second = EntityCloner::clone(&scene, EntityCloner::collectHierarchy(&scene, root), &scene)[0];
    EXPECT_EQ(scene.getComponent<CameraFollowComponent>(second).target, outside) << "a target outside it stays";
}

// the editor in edit mode: the game's events paused for each test, and given back as they were after
// it (left paused, they held the music of MusicPlayerTest, which runs later in this binary)
class CameraShakeEditor : public ::testing::Test {
protected:
    bool wasPaused = false;
    void SetUp() override {
        wasPaused = Engine::areGameEventsPaused();
        Engine::pauseGameEvents(true);
    }
    void TearDown() override {
        Engine::pauseGameEvents(wasPaused);
    }
};

TEST_F(CameraShakeEditor, ShakeAssetEditIsOneProjectUndoStep){
    ASSERT_TRUE(headless().ready);
    Project* project = headless().project.get();
    const fs::path file = headless().dir / "shakes" / "boom.shake";
    fs::create_directories(file.parent_path());
    camera::ShakePresetFile original;
    original.preset = *camera::findBuiltinShakePreset("explosion");
    std::string error;
    ASSERT_TRUE(inspectorcamera::writeShakePresetFile(project, file, original, error)) << error;
    const std::string first = readAll(file);
    CommandHistory* history = project->getProjectCommandHistory();
    const size_t steps = history->position();

    camera::ShakePresetFile edited = original;
    edited.preset.strength = 0.6f;
    edited.preset.type = camera::ShakeType::Sustained;
    ASSERT_TRUE(inspectorcamera::writeShakePresetFile(project, file, edited, error)) << error;
    EXPECT_EQ(history->position(), steps + 1) << "one project undo step";
    camera::ShakeParseResult parsed = camera::parseShakePreset(readAll(file));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.file->preset, edited.preset);

    ASSERT_TRUE(inspectorcamera::writeShakePresetFile(project, file, edited, error));
    EXPECT_EQ(history->position(), steps + 1) << "the same text adds no step";

    history->undo();
    EXPECT_EQ(readAll(file), first) << "undo puts the previous values back";
    history->redo();
    EXPECT_EQ(camera::parseShakePreset(readAll(file)).file->preset, edited.preset);
}

TEST_F(CameraShakeEditor, EditModePreviewLeavesTheSceneUnmodified){
    ASSERT_TRUE(headless().ready);
    Project* project = headless().project.get();
    project->createNewScene("ShakePreview", SceneType::SCENE_3D);
    SceneProject* sceneProject = project->getScene(project->getSelectedSceneId());
    ASSERT_TRUE(sceneProject && sceneProject->scene);
    Scene* scene = sceneProject->scene;
    const Entity camera = scene->createEntity();
    Transform transform;
    transform.position = Vector3(0, 1, 5);
    scene->addComponent<Transform>(camera, transform);
    scene->addComponent<CameraComponent>(camera, {});
    scene->addComponent<CameraShakerComponent>(camera, {});
    sceneProject->entities.push_back(camera);
    sceneProject->isModified = false;
    const std::string before = YAML::Dump(Stream::encodeComponents(camera, scene, scene->getSignature(camera)));

    std::shared_ptr<CameraControlSystem> system = scene->getSystem<CameraControlSystem>();
    ASSERT_TRUE(system->isPaused()) << "edit mode pauses the camera system";
    ASSERT_TRUE(system->previewShake(camera, *camera::findBuiltinShakePreset("drunk"), "drunk"));
    // the editor's frames (the preview runs on its own clock); a paused update moves no follower
    for (int i = 0; i < 4; i++) system->update(1.0 / 60.0);
    EXPECT_TRUE(system->isPreviewing());
    EXPECT_FALSE(sceneProject->isModified);
    EXPECT_FALSE(scene->getComponent<CameraShakerComponent>(camera).runtime) << "the component's runtime is untouched";
    EXPECT_EQ(scene->getComponent<Transform>(camera).position, Vector3(0, 1, 5));
    EXPECT_EQ(YAML::Dump(Stream::encodeComponents(camera, scene, scene->getSignature(camera))), before)
        << "nothing of the preview is saved";

    project->stopCameraShakePreviews();
    EXPECT_FALSE(scene->getComponent<CameraComponent>(camera).hasViewOffset) << "stopping clears the view offset";
    EXPECT_FALSE(system->isPreviewing());
}
