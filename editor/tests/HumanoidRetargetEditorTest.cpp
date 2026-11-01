// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Humanoid retargeting in the editor (anim-retarget spec 14, test 13): the Avatar Configuration
// window's actions (map a slot, a twist, Apply writes the sidecar as one undo step), Convert to
// Humanoid Clip writes a version 2 clip, Record on a humanoid clip keys the slot's E, and the
// Animation Player entry flags round-trip through the scene stream. Headless project as in
// ClipRecordCurvesTest.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Platform.h"
#include "Project.h"
#include "Stream.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AvatarMask.h"
#include "animation/ClipRecordHooks.h"
#include "animation/HumanoidClip.h"
#include "command/type/ClipSceneActions.h"
#include "component/ModelComponent.h"
#include "object/Model.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseMath.h"
#include "subsystem/MeshSystem.h"
#include "window/AvatarWindow.h"

#include "yaml-cpp/yaml.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Host : public EditorHost {
        void stopTransientPreviews() override {
            if (animationClipAppHooks().stopTransientPreviews) animationClipAppHooks().stopTransientPreviews();
        }
        void clearSceneWindowState(uint32_t sceneId) override {
            if (animationClipAppHooks().sceneClosed) animationClipAppHooks().sceneClosed(sceneId);
        }
        void prepareForProjectSwitch() override {
            if (animationClipAppHooks().projectClosed) animationClipAppHooks().projectClosed();
        }
    };

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-humanoid-editor-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: humanoid\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            static char arg0[] = "jasperengine-editor-tests";
            static char* argv[] = {arg0, nullptr};
            Engine::systemInit(1, argv, out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        return h;
    }

    bool headlessStarted = false;

    Project* project() {
        headlessStarted = true;
        return headless().project.get();
    }
    fs::path assets() { return project()->getAssetsPath(); }

    struct Cleanup : public ::testing::Environment {
        void TearDown() override {
            if (!headlessStarted) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const cleanup = ::testing::AddGlobalTestEnvironment(new Cleanup);

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    void copyFixture(const std::string& name, const std::string& to) {
        fs::create_directories((assets() / to).parent_path());
        fs::copy_file(fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "tools" / "fixtures" / "pose-animation" / "assets" /
                      "models" / name, assets() / to, fs::copy_options::overwrite_existing);
    }

    struct ModelScene {
        uint32_t id = 0;
        Scene* scene = nullptr;
        Entity model = NULL_ENTITY;
        std::shared_ptr<Model> handle;      // owns the model entity
    };

    ModelScene modelScene(const std::string& file, const std::string& name) {
        ModelScene s;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene(name, SceneType::SCENE_3D);
        s.id = p->getSelectedSceneId();
        SceneProject* sceneProject = p->getScene(s.id);
        s.scene = sceneProject->scene;
        s.handle = std::make_shared<Model>(s.scene);
        s.model = s.handle->getEntity();
        s.scene->setEntityName(s.model, name);
        sceneProject->entities.push_back(s.model);
        EXPECT_TRUE(s.scene->getSystem<MeshSystem>()->loadGLTF(s.model, (assets() / file).string(), false, false, false));
        s.scene->getComponent<ModelComponent>(s.model).filename = file;
        s.scene->getComponent<ModelComponent>(s.model).needUpdateModel = false;
        return s;
    }

    class HumanoidEditorTest : public ::testing::Test {
    protected:
        Host host;
        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            setEditorHost(&host);
        }
        void TearDown() override {
            ClipRecordHooks::endAllGestures();
            AnimationClipController::release(project());
            setEditorHost(nullptr);
        }
    };

}

TEST_F(HumanoidEditorTest, AvatarWindowAppliesTheSidecarAsOneUndoStep) {
    copyFixture("hero_tpose.gltf", "models/avatar/hero.gltf");
    ModelScene s = modelScene("models/avatar/hero.gltf", "AvatarHero");
    const fs::path sidecar = assets() / "models/avatar/hero.gltf.humanoid";
    ASSERT_FALSE(fs::exists(sidecar));

    AvatarWindow window(project());
    std::string error;
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;
    ASSERT_TRUE(window.resolved().rig) << window.resolved().error;
    EXPECT_FALSE(window.isDirty());
    EXPECT_GE(window.resolved().extra.mappedCount(0, pose::HUMAN_FINGER_BONE_COUNT), 12) << "Mixamo fingers auto-detect";

    // map a slot (the same bone auto-detect found, now forced), set a twist, Humanoid type
    window.setBone(pose::HumanBone::LeftUpperArm, std::string("mixamorig:LeftArm"));
    window.setTwist(pose::HumanBone::LeftLowerArm, 15.0f);
    window.setAnimationType(pose::AnimationType::Humanoid);
    ASSERT_TRUE(window.isDirty());
    ASSERT_TRUE(window.resolved().rig);
    EXPECT_FLOAT_EQ(window.resolved().rig->twist(pose::HumanBone::LeftLowerArm), 15.0f) << "the preview rig has the twist";
    ASSERT_TRUE(window.apply(&error)) << error;
    EXPECT_FALSE(window.isDirty());
    ASSERT_TRUE(fs::exists(sidecar));
    pose::HumanoidOverrides written;
    std::vector<std::string> warnings;
    ASSERT_TRUE(pose::parseHumanoidOverrides(readAll(sidecar), written, warnings, &error)) << error;
    EXPECT_TRUE(written.isHumanoid());
    EXPECT_EQ(written.bones[(size_t)pose::boneIndex(pose::HumanBone::LeftUpperArm)], std::optional<std::string>("mixamorig:LeftArm"));
    EXPECT_EQ(written.twist[(size_t)pose::boneIndex(pose::HumanBone::LeftLowerArm)], std::optional<float>(15.0f));

    // undo puts the bytes back (here: no sidecar), redo writes it again
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FALSE(fs::exists(sidecar));
    ASSERT_TRUE(project()->getProjectCommandHistory()->redo());
    EXPECT_TRUE(fs::exists(sidecar));

    // a second edit over an existing sidecar: undo restores its exact bytes
    const std::string before = readAll(sidecar);
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;
    window.setTwist(pose::HumanBone::LeftLowerArm, 0.0f);
    window.autoMapAll();
    ASSERT_TRUE(window.apply(&error)) << error;
    EXPECT_NE(readAll(sidecar), before);
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_EQ(readAll(sidecar), before);

    // the preview writes the neutral pose and puts the bones back
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;
    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(s.scene, s.model);
    ASSERT_TRUE(slots.target);
    const Entity arm = slots.target->slotEntities[(size_t)pose::boneIndex(pose::HumanBone::LeftUpperArm)];
    const Quaternion rest = s.scene->getComponent<Transform>(arm).rotation;
    window.setPreview(true);
    EXPECT_GT(std::fabs(1.0f - std::fabs(s.scene->getComponent<Transform>(arm).rotation.dot(rest))), 1e-4f) << "T-pose arms go down";
    window.stopPreview();
    EXPECT_EQ(s.scene->getComponent<Transform>(arm).rotation, rest);
}

TEST_F(HumanoidEditorTest, ConvertToHumanoidClipWritesAVersionTwoClip) {
    copyFixture("hero_animated.gltf", "models/convert/hero.gltf");
    ClipActionResult result;
    std::string error;
    const std::string output = ClipSceneActions::humanoidClipPath(project(), "models/convert/hero.gltf#Walk");
    EXPECT_EQ(output, "assets/models/convert/Walk (Humanoid).anim");
    ASSERT_TRUE(ClipSceneActions::convertToHumanoid(project(), 0, NULL_ENTITY, SIZE_MAX, "models/convert/hero.gltf#Walk", "", output, result, error)) << error;
    EXPECT_TRUE(result.projectStep);
    const std::string text = readAll(project()->getProjectPath() / output);
    EXPECT_NE(text.find("\"version\": 2,"), std::string::npos);
    anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(parsed.data->isHumanoid());
    EXPECT_EQ(parsed.data->humanoid.sourceModel, "models/convert/hero.gltf");
    EXPECT_GE(parsed.data->findHumanoidTrack(ClipChannel::Humanoid, pose::boneIndex(pose::HumanBone::LeftUpperLeg)), 0);
    EXPECT_GE(parsed.data->findHumanoidTrack(ClipChannel::HumanoidPosition, HUMANOID_POSITION_LEFT_FOOT), 0);
    // deterministic: the same bytes again (apart from the new GUID)
    ClipActionResult again;
    const std::string second = "assets/models/convert/Walk again.anim";
    ASSERT_TRUE(ClipSceneActions::convertToHumanoid(project(), 0, NULL_ENTITY, SIZE_MAX, "models/convert/hero.gltf#Walk", "", second, again, error)) << error;
    anim::ClipParseResult other = anim::parseClip(readAll(project()->getProjectPath() / second));
    ASSERT_TRUE(other.ok());
    other.data->guid = parsed.data->guid;
    EXPECT_EQ(anim::writeClip(*other.data), anim::writeClip(*parsed.data));
    // a clip without a source model is refused
    ClipData plain;
    std::string plainText = anim::writeClip(plain);
    fs::create_directories(assets() / "anim");
    std::ofstream(assets() / "anim/plain.anim") << plainText;
    AnimationClipDocument::checkDiskChanges(project());
    EXPECT_FALSE(ClipSceneActions::convertToHumanoid(project(), 0, NULL_ENTITY, SIZE_MAX, "anim/plain.anim", "", "assets/anim/plain h.anim", result, error));
    EXPECT_NE(error.find("names no source model"), std::string::npos) << error;
}

TEST_F(HumanoidEditorTest, RecordOnAHumanoidClipKeysTheSlotE) {
    copyFixture("hero_tpose.gltf", "models/record/hero.gltf");
    ModelScene s = modelScene("models/record/hero.gltf", "RecordHero");
    // a humanoid clip (one hips key) played by the model
    ClipData data;
    ClipTrackData hips;
    hips.channel = ClipChannel::Humanoid;
    hips.humanBone = pose::boneIndex(pose::HumanBone::Hips);
    ClipKey key;
    key.value = {0, 0, 0, 1};
    hips.keys = {key};
    data.tracks = {hips};
    fs::create_directories(assets() / "anim");
    std::ofstream(assets() / "anim/rec_h.anim") << AnimationClipDocument::fileText(data);
    AnimationClipDocument::checkDiskChanges(project());
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "anim/rec_h.anim";
    player.clips = {entry};
    s.scene->addComponent<AnimationPlayerComponent>(s.model, player);

    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.model, &error)) << error;
    std::shared_ptr<const anim::HumanoidSlots> humanoid = c.boundHumanoid();
    ASSERT_TRUE(humanoid) << "the bound root's humanoid rig";
    const Entity arm = humanoid->target->slotEntities[(size_t)pose::boneIndex(pose::HumanBone::LeftUpperArm)];
    c.setRecording(true);
    ASSERT_TRUE(ClipRecordHooks::claims(s.id, arm, ClipChannel::Rotation));
    // the gizmo turns the upper arm 40 degrees about the model's Z
    const Quaternion rest = s.scene->getComponent<Transform>(arm).rotation;
    const pose::Quatd turned = pose::Quatd(rest) * pose::axisAngleRad(pose::Vec3d(0, 0, 1), pose::degToRad(40.0));
    ClipRecordValue value;
    value.channel = ClipChannel::Rotation;
    value.value = {(float)turned.x, (float)turned.y, (float)turned.z, (float)turned.w};
    ASSERT_TRUE(ClipRecordHooks::record(s.id, arm, value, ""));
    const ClipData& recorded = c.getDocument()->data();
    const int track = recorded.findHumanoidTrack(ClipChannel::Humanoid, pose::boneIndex(pose::HumanBone::LeftUpperArm));
    ASSERT_GE(track, 0) << "a humanoid track, not a path track";
    EXPECT_LT(recorded.findTrack("Hero/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/mixamorig:LeftShoulder/mixamorig:LeftArm",
                                 ClipChannel::Rotation), 0);
    const std::vector<float>& E = recorded.tracks[(size_t)track].keys[0].value;
    const pose::Quatd local = pose::Quatd(humanoid->target->bridgeRotationInverse[(size_t)pose::boneIndex(pose::HumanBone::LeftUpperArm)].inverse() *
                                          turned.toQuaternion()).normalized();
    const pose::Quatd expected = humanoid->target->rig->jointRotationFromLocal(pose::HumanBone::LeftUpperArm, local);
    EXPECT_LT(pose::radToDeg(pose::angleBetween(pose::Quatd(E[3], E[0], E[1], E[2]), expected)), 0.01);
    c.stopPreview();
}

TEST_F(HumanoidEditorTest, EntryFlagsRoundTripThroughTheStream) {
    AnimationPlayerComponent player;
    AnimationPlayerClip plain;
    plain.clip = "anim/walk.anim";
    AnimationPlayerClip flagged;
    flagged.clip = "models/hero.gltf#Walk";
    flagged.retarget = true;
    flagged.mirror = true;
    flagged.footIK = true;
    player.clips = {plain, flagged};
    const YAML::Node node = Stream::encodeAnimationPlayerComponent(player);
    const std::string text = YAML::Dump(node);
    EXPECT_FALSE(node["clips"][0]["retarget"]) << "unset flags are not written";
    EXPECT_TRUE(node["clips"][1]["retarget"]);
    const AnimationPlayerComponent decoded = Stream::decodeAnimationPlayerComponent(YAML::Load(text));
    ASSERT_EQ(decoded.clips.size(), 2u);
    EXPECT_FALSE(decoded.clips[0].retarget);
    EXPECT_FALSE(decoded.clips[0].mirror);
    EXPECT_TRUE(decoded.clips[1].retarget);
    EXPECT_TRUE(decoded.clips[1].mirror);
    EXPECT_TRUE(decoded.clips[1].footIK);
    EXPECT_EQ(decoded.clips[1], flagged);
    EXPECT_NE(decoded.clips[1], plain);
    // a v1 scene (no flags) decodes as before and encodes byte-identically
    AnimationPlayerComponent old;
    old.clips = {plain};
    const std::string oldText = YAML::Dump(Stream::encodeAnimationPlayerComponent(old));
    EXPECT_EQ(oldText.find("retarget"), std::string::npos);
    EXPECT_EQ(YAML::Dump(Stream::encodeAnimationPlayerComponent(Stream::decodeAnimationPlayerComponent(YAML::Load(oldText)))), oldText);
}
