// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Humanoid retargeting in the editor (anim-retarget spec 14, test 13): the Avatar Configuration
// window's actions (map a slot, a twist, Apply writes the sidecar as one undo step), Convert to
// Humanoid Clip writes a version 2 clip, Record on a humanoid clip keys the slot's E, and the
// Animation Player entry flags round-trip through the scene stream. Headless project as in
// ClipRecordCurvesTest.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Project.h"
#include "Stream.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AvatarMaskWeights.h"
#include "animation/ClipRecordHooks.h"
#include "animation/HumanoidClip.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "animation/ClipBinding.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/ClipSceneActions.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "object/Model.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseRigProvider.h"
#include "pose/PoseMath.h"
#include "subsystem/MeshSystem.h"
#include "window/AvatarWindow.h"
#include "window/InspectorAnimLayers.h"

#include "yaml-cpp/yaml.h"

#include "support/PosixCompat.h"

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
            // the support library's engine (TestMain's TestSystem) with this project's paths: a
            // second Engine::systemInit with an editor Platform crashed the tree
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
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

    // an empty model GameObject (Transform, Mesh, Model) of a scene, a root of its hierarchy
    Entity addModelObject(uint32_t sceneId, const std::string& name) {
        SceneProject* sceneProject = project()->getScene(sceneId);
        Scene* scene = sceneProject->scene;
        const Entity entity = scene->createUserEntity();
        scene->setEntityName(entity, name);
        scene->addComponent<Transform>(entity, Transform());
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<ModelComponent>(entity, {});
        sceneProject->entities.push_back(entity);
        return entity;
    }

    // a model GameObject loaded by ModelLoadCmd (deletable, undoable): its scene and entity
    ModelScene loadedModel(const std::string& file, const std::string& name) {
        ModelScene s;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene(name + "Scene", SceneType::SCENE_3D);
        s.id = p->getSelectedSceneId();
        SceneProject* sceneProject = p->getScene(s.id);
        s.scene = sceneProject->scene;
        s.model = addModelObject(s.id, name);
        Engine::setAsyncLoading(false);     // the load finishes inside the command
        EXPECT_TRUE(CommandHandle::get(s.id)->addCommand(new ModelLoadCmd(p, s.id, s.model, file)));
        EXPECT_FALSE(s.scene->getComponent<ModelComponent>(s.model).filename.empty()) << file << " loaded";
        return s;
    }

    double degreesBetween(const Quaternion& a, const Quaternion& b) {
        return pose::radToDeg(pose::angleBetween(pose::Quatd(a).normalized(), pose::Quatd(b).normalized()));
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

    // undo puts the bytes back (here: no sidecar), redo writes the same bytes again
    const std::string applied = readAll(sidecar);
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FALSE(fs::exists(sidecar));
    ASSERT_TRUE(project()->getProjectCommandHistory()->redo());
    ASSERT_TRUE(fs::exists(sidecar));
    EXPECT_EQ(readAll(sidecar), applied);
    EXPECT_EQ(applied, pose::writeHumanoidOverrides(written));

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
    // a model with animations loads in its node hierarchy (Record refuses the legacy layout)
    copyFixture("hero_animated.gltf", "models/record/hero.gltf");
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
    // independent expectations (not the formula under test): E is relative to the neutral pose,
    // so the arm in its neutral pose keys the identity, and the neutral pose turned 40 degrees
    // about the bone's own Z keys a 40 degree E (E = N R N^-1 for local = left * E * N)
    const size_t slot = (size_t)pose::boneIndex(pose::HumanBone::LeftUpperArm);
    const Quaternion bridgeInverse = humanoid->target->bridgeRotationInverse[slot];
    const pose::Quatd neutral(humanoid->target->rig->neutralPose().rotations[slot]);
    auto recordLocal = [&](const pose::Quatd& skeletonLocal) {
        const Quaternion transformLocal = (bridgeInverse * skeletonLocal.normalized().toQuaternion()).normalized();
        ClipRecordValue value;
        value.channel = ClipChannel::Rotation;
        value.value = {transformLocal.x, transformLocal.y, transformLocal.z, transformLocal.w};
        EXPECT_TRUE(ClipRecordHooks::record(s.id, arm, value, ""));
        const ClipData& recorded = c.getDocument()->data();
        const int track = recorded.findHumanoidTrack(ClipChannel::Humanoid, (int)slot);
        EXPECT_GE(track, 0) << "a humanoid track, not a path track";
        EXPECT_LT(recorded.findTrack("Hero/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/mixamorig:LeftShoulder/mixamorig:LeftArm",
                                     ClipChannel::Rotation), 0);
        if (track < 0) return pose::Quatd();
        const std::vector<float>& E = recorded.tracks[(size_t)track].keys[0].value;
        return pose::Quatd(E[3], E[0], E[1], E[2]).normalized();
    };
    const pose::Quatd atNeutral = recordLocal(neutral);
    EXPECT_LT(pose::radToDeg(pose::angleBetween(atNeutral, pose::Quatd())), 0.01) << "the neutral pose is E = identity";
    const pose::Quatd turned = recordLocal(neutral * pose::axisAngleRad(pose::Vec3d(0, 0, 1), pose::degToRad(40.0)));
    EXPECT_NEAR(pose::radToDeg(pose::angleBetween(turned, pose::Quatd())), 40.0, 0.01) << "a 40 degree turn from neutral";
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

// ---- review fixes ----

TEST_F(HumanoidEditorTest, ConvertUseInThisPlayerAndItsUndo) {
    copyFixture("hero_animated.gltf", "models/use/hero.gltf");
    ModelScene s = modelScene("models/use/hero.gltf", "UseHero");
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "models/use/hero.gltf#Walk";
    entry.retarget = true;
    player.clips = {entry};
    s.scene->addComponent<AnimationPlayerComponent>(s.model, player);
    const std::string output = ClipSceneActions::humanoidClipPath(project(), "models/use/hero.gltf#Walk");
    ClipActionResult result;
    std::string error;
    ASSERT_TRUE(ClipSceneActions::convertToHumanoid(project(), s.id, s.model, 0, "models/use/hero.gltf#Walk", "", output, result, error)) << error;
    EXPECT_FALSE(result.projectStep) << "the file and the entry: one scene step";
    const fs::path file = project()->getProjectPath() / output;
    ASSERT_TRUE(fs::exists(file));
    const AnimationPlayerClip& used = s.scene->getComponent<AnimationPlayerComponent>(s.model).clips[0];
    EXPECT_EQ(used.clip, "models/use/Walk (Humanoid).anim");
    EXPECT_FALSE(used.retarget) << "the file is humanoid: no retarget";
    // undo: the entry back as it was, the file gone; redo: both again
    ASSERT_TRUE(CommandHandle::get(s.id)->undo());
    EXPECT_FALSE(fs::exists(file));
    EXPECT_EQ(s.scene->getComponent<AnimationPlayerComponent>(s.model).clips[0], entry);
    ASSERT_TRUE(CommandHandle::get(s.id)->redo());
    EXPECT_TRUE(fs::exists(file));
    EXPECT_EQ(s.scene->getComponent<AnimationPlayerComponent>(s.model).clips[0].clip, "models/use/Walk (Humanoid).anim");

    // the file alone (no player): a project step, undone by the project history
    const std::string alone = "assets/models/use/Walk alone.anim";
    ASSERT_TRUE(ClipSceneActions::convertToHumanoid(project(), 0, NULL_ENTITY, SIZE_MAX, "models/use/hero.gltf#Walk", "", alone, result, error)) << error;
    EXPECT_TRUE(result.projectStep);
    ASSERT_TRUE(fs::exists(project()->getProjectPath() / alone));
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FALSE(fs::exists(project()->getProjectPath() / alone));
}

TEST_F(HumanoidEditorTest, AvatarPreviewOwnsTheRootSoADeleteNeverKeepsTheNeutralPose) {
    copyFixture("hero_tpose.gltf", "models/owner/hero.gltf");
    ModelScene s = loadedModel("models/owner/hero.gltf", "OwnerHero");
    ASSERT_NE(s.model, NULL_ENTITY);
    const fs::path sceneFile = project()->getProjectPath() / "AvatarOwner.scene";
    ASSERT_TRUE(project()->saveSceneToPath(s.id, sceneFile));
    const std::string unpreviewed = readAll(sceneFile);

    AvatarWindow window(project());
    std::string error;
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;
    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(s.scene, s.model);
    ASSERT_TRUE(slots.target) << slots.error;
    const Entity arm = slots.target->slotEntities[(size_t)pose::boneIndex(pose::HumanBone::LeftUpperArm)];
    const std::string armName = s.scene->getEntityName(arm);
    const Quaternion rest = s.scene->getComponent<Transform>(arm).rotation;
    window.setPreview(true);
    ASSERT_TRUE(window.isPreviewing());
    ASSERT_GT(degreesBetween(s.scene->getComponent<Transform>(arm).rotation, rest), 1.0) << "the neutral pose is on";
    EXPECT_NE(PreviewOwner::ownerOf(s.id, s.model).token, 0) << "the preview owns the model root";

    // delete the model: the preview restores first; undo brings the rest back; save writes it
    ASSERT_TRUE(CommandHandle::get(s.id)->addCommand(new DeleteEntityCmd(project(), s.id, s.model)));
    EXPECT_FALSE(window.isPreviewing());
    ASSERT_TRUE(CommandHandle::get(s.id)->undo());
    const Entity model = s.scene->findEntity("OwnerHero");
    ASSERT_NE(model, NULL_ENTITY);
    const Entity armAgain = s.scene->findEntity(armName);
    ASSERT_NE(armAgain, NULL_ENTITY);
    EXPECT_LT(degreesBetween(s.scene->getComponent<Transform>(armAgain).rotation, rest), 1e-3) << "the undone delete is at rest";
    ASSERT_TRUE(project()->saveSceneToPath(s.id, sceneFile));
    EXPECT_EQ(readAll(sceneFile), unpreviewed) << "no neutral pose saved";

    // another owner of the root (the Animation window, a pose edit) stops the Avatar preview
    ASSERT_TRUE(window.openModel(s.id, model, &error)) << error;
    window.setPreview(true);
    ASSERT_TRUE(window.isPreviewing());
    const PreviewOwner::Token other = PreviewOwner::acquire(s.id, model, s.scene, "test", [](){});
    EXPECT_FALSE(window.isPreviewing());
    EXPECT_LT(degreesBetween(s.scene->getComponent<Transform>(armAgain).rotation, rest), 1e-3);
    PreviewOwner::release(other);
}

TEST_F(HumanoidEditorTest, AvatarWorkingCopyFollowsTheProject) {
    copyFixture("hero_tpose.gltf", "models/follow/hero.gltf");
    copyFixture("hero_tpose.gltf", "models/follow/other.gltf");
    ModelScene s = loadedModel("models/follow/hero.gltf", "FollowHero");
    ASSERT_NE(s.model, NULL_ENTITY);
    const fs::path sidecar = assets() / "models/follow/hero.gltf.humanoid";
    AvatarWindow window(project());
    std::string error;
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;

    // Apply, then its undo: the saved copy and the clean working copy follow the file
    window.setTwist(pose::HumanBone::LeftLowerArm, 20.0f);
    ASSERT_TRUE(window.apply(&error)) << error;
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    ASSERT_FALSE(fs::exists(sidecar));
    window.syncWithProject();
    EXPECT_FALSE(window.isDirty());
    EXPECT_FALSE(window.overrides().twist[(size_t)pose::boneIndex(pose::HumanBone::LeftLowerArm)].has_value())
        << "the undone twist is gone from the working copy (the next Apply would have written it back)";

    // a sidecar written elsewhere (set_humanoid, Map Bones, the Inspector) while editing: the
    // edits stay, Discard takes the file
    window.setTwist(pose::HumanBone::RightLowerArm, 10.0f);
    pose::HumanoidOverrides external;
    external.animationType = pose::AnimationType::Humanoid;
    std::ofstream(sidecar) << pose::writeHumanoidOverrides(external);
    window.syncWithProject();
    EXPECT_TRUE(window.isDirty());
    EXPECT_EQ(window.overrides().twist[(size_t)pose::boneIndex(pose::HumanBone::RightLowerArm)], std::optional<float>(10.0f));
    window.discard();
    EXPECT_TRUE(window.overrides().isHumanoid()) << "Discard takes the file as it is now";

    // Configure Avatar... again on the same model keeps the edits; another model asks first
    window.setTwist(pose::HumanBone::RightLowerArm, 12.0f);
    ASSERT_TRUE(window.requestOpen(s.id, s.model, &error)) << error;
    EXPECT_TRUE(window.isDirty()) << "the bound model again: its working copy stays";
    const Entity other = addModelObject(s.id, "FollowOther");
    ASSERT_TRUE(CommandHandle::get(s.id)->addCommand(new ModelLoadCmd(project(), s.id, other, "models/follow/other.gltf")));
    ASSERT_NE(other, NULL_ENTITY);
    ASSERT_TRUE(window.requestOpen(s.id, other, &error)) << error;
    EXPECT_TRUE(window.hasPendingOpen()) << "unsaved edits: asked before opening another model";
    EXPECT_EQ(window.boundEntity(), s.model);
    EXPECT_TRUE(window.isDirty());
    window.discard();
    ASSERT_TRUE(window.requestOpen(s.id, other, &error)) << error;
    EXPECT_EQ(window.boundEntity(), other) << "no edits: opens directly";
    EXPECT_FALSE(window.hasPendingOpen());

    // a reloaded model (new bone entities): the window rebinds, the preview writes the new bones
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;
    window.setTwist(pose::HumanBone::LeftLowerArm, 5.0f);
    ASSERT_TRUE(CommandHandle::get(s.id)->addCommand(new ModelLoadCmd(project(), s.id, s.model, "models/follow/other.gltf")));
    ASSERT_TRUE(CommandHandle::get(s.id)->addCommand(new ModelLoadCmd(project(), s.id, s.model, "models/follow/hero.gltf")));
    window.syncWithProject();
    EXPECT_TRUE(window.isDirty()) << "the edits survive the reload";
    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(s.scene, s.model);
    ASSERT_TRUE(slots.target) << slots.error;
    const Entity arm = slots.target->slotEntities[(size_t)pose::boneIndex(pose::HumanBone::LeftUpperArm)];
    const Quaternion rest = s.scene->getComponent<Transform>(arm).rotation;
    window.setPreview(true);
    EXPECT_GT(degreesBetween(s.scene->getComponent<Transform>(arm).rotation, rest), 1.0) << "the preview writes the reloaded bones";
    window.stopPreview();
    EXPECT_LT(degreesBetween(s.scene->getComponent<Transform>(arm).rotation, rest), 1e-3);
    window.discard();
}

TEST_F(HumanoidEditorTest, InvalidHandsAndHeadOverridesAreRefusedAndNeverCostTheRig) {
    copyFixture("hero_tpose.gltf", "models/extra/hero.gltf");
    ModelScene s = modelScene("models/extra/hero.gltf", "ExtraHero");
    const fs::path sidecar = assets() / "models/extra/hero.gltf.humanoid";
    AvatarWindow window(project());
    std::string error;
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;
    // a body slot's bone as a finger: the window says so and Apply refuses
    window.setExtraBone(pose::HumanExtraBone::LeftIndexProximal, std::string("mixamorig:LeftArm"));
    EXPECT_FALSE(window.resolved().extraError.empty());
    ASSERT_TRUE(window.resolved().rig) << "the body map still works";
    EXPECT_FALSE(window.apply(&error));
    EXPECT_NE(error.find("Hands & Head"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(sidecar));
    window.discard();

    // such a sidecar written by hand: the runtime keeps the rig (auto-detected fingers), as the
    // editor shows it
    pose::HumanoidOverrides bad;
    bad.animationType = pose::AnimationType::Humanoid;
    bad.extraBones[(size_t)pose::extraIndex(pose::HumanExtraBone::LeftIndexProximal)] = std::string("mixamorig:LeftArm");
    std::ofstream(sidecar) << pose::writeHumanoidOverrides(bad);
    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(s.scene, s.model);
    ASSERT_TRUE(slots.target) << slots.error;
    EXPECT_GE(slots.target->extra.mappedCount(0, pose::HUMAN_FINGER_BONE_COUNT), 12) << "auto-detected fingers";
    const std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forModelFile(project(), assets() / "models/extra/hero.gltf");
    EXPECT_TRUE(info->rig);
    EXPECT_FALSE(info->extraError.empty());
    EXPECT_EQ(info->extra.bones, slots.target->extra.bones) << "the editor and the runtime resolve the same hands";
    fs::remove(sidecar);
}

TEST_F(HumanoidEditorTest, TheRigInfoKeepsTheAvatarOfAnInvalidBodyMap) {
    copyFixture("hero_tpose.gltf", "models/invalid/hero.gltf");
    const fs::path sidecar = assets() / "models/invalid/hero.gltf.humanoid";
    // the hips unmapped (not a humanoid), with a twist and Humanoid type
    pose::HumanoidOverrides overrides;
    overrides.animationType = pose::AnimationType::Humanoid;
    overrides.bones[(size_t)pose::boneIndex(pose::HumanBone::Hips)] = std::string();
    overrides.twist[(size_t)pose::boneIndex(pose::HumanBone::LeftLowerArm)] = 30.0f;
    std::ofstream(sidecar) << pose::writeHumanoidOverrides(overrides);
    std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forModelFile(project(), assets() / "models/invalid/hero.gltf");
    EXPECT_FALSE(info->rig);
    EXPECT_TRUE(info->overrides.isHumanoid()) << "the Animation Type is read even when the map does not validate";
    EXPECT_EQ(info->overrides, overrides) << "what the Inspector's Animation Type combo rewrites from";
    EXPECT_TRUE(info->sidecarError.empty());
    // the Rig foldout's Animation Type rewrites the sidecar as read (the Avatar kept)
    std::string error;
    ASSERT_TRUE(PoseRigProvider::setAnimationType(project(), assets() / "models/invalid/hero.gltf", pose::AnimationType::Generic, error)) << error;
    pose::HumanoidOverrides generic = overrides;
    generic.animationType.reset();
    EXPECT_EQ(readAll(sidecar), pose::writeHumanoidOverrides(generic)) << "only animation_type changed";
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_EQ(readAll(sidecar), pose::writeHumanoidOverrides(overrides));

    // an unreadable sidecar: said, and no writer rebuilds it from nothing
    std::ofstream(sidecar) << "{ not json";
    info = PoseRigProvider::forModelFile(project(), assets() / "models/invalid/hero.gltf");
    EXPECT_FALSE(info->sidecarError.empty());
    EXPECT_FALSE(PoseRigProvider::setAnimationType(project(), assets() / "models/invalid/hero.gltf", pose::AnimationType::Humanoid, error));
    EXPECT_NE(error.find("fix or delete the file first"), std::string::npos) << error;
    EXPECT_EQ(readAll(sidecar), "{ not json") << "the Rig foldout left it alone";
    // the Avatar window's Apply too
    ModelScene s = modelScene("models/invalid/hero.gltf", "InvalidHero");
    AvatarWindow window(project());
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;
    window.setTwist(pose::HumanBone::LeftLowerArm, 10.0f);
    EXPECT_FALSE(window.apply(&error));
    EXPECT_NE(error.find("fix or delete the file first"), std::string::npos) << error;
    EXPECT_EQ(readAll(sidecar), "{ not json") << "Apply left it alone";
    // fixed on disk: the window reads it again and Apply works
    fs::remove(sidecar);
    window.syncWithProject();
    window.setTwist(pose::HumanBone::LeftLowerArm, 10.0f);
    EXPECT_TRUE(window.apply(&error)) << error;
    fs::remove(sidecar);
}

TEST_F(HumanoidEditorTest, AvatarTestClipOfItsOwnModelUsesTheWorkingCopyOnBothSides) {
    copyFixture("hero_animated.gltf", "models/testclip/hero.gltf");
    const fs::path sidecar = assets() / "models/testclip/hero.gltf.humanoid";
    // Animation Type: Humanoid (the pool serves its clips converted with the saved sidecar)
    pose::HumanoidOverrides humanoid;
    humanoid.animationType = pose::AnimationType::Humanoid;
    std::ofstream(sidecar) << pose::writeHumanoidOverrides(humanoid);
    AnimationClipDocument::checkDiskChanges(project());
    ModelScene s = modelScene("models/testclip/hero.gltf", "TestClipHero");
    AvatarWindow window(project());
    std::string error;
    ASSERT_TRUE(window.openModel(s.id, s.model, &error)) << error;
    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(s.scene, s.model);
    ASSERT_TRUE(slots.target) << slots.error;
    const Entity arm = slots.target->slotEntities[(size_t)pose::boneIndex(pose::HumanBone::LeftUpperArm)];
    // the generic walk's arm at 0.5 s: what the round trip on its own rig gives back, whatever the
    // twist (both sides twisted alike)
    ClipData generic;
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(anim::buildModelSubClip("models/testclip/hero.gltf#Walk", generic, warnings, &error)) << error;
    std::shared_ptr<const AnimationClip> walk = AnimationClip::build(generic, warnings, nullptr);
    std::string armPath;
    ASSERT_TRUE(anim::relativePath(s.scene, s.model, arm, armPath));
    const int track = walk->findTrack(armPath, ClipChannel::Rotation);
    ASSERT_GE(track, 0);
    std::vector<anim::TrackSample> samples;
    anim::sampleClipTracks(*walk, 0.5f, samples);
    const Quaternion expected = samples[(size_t)track].rotation;

    window.setTestClip("models/testclip/hero.gltf#Walk");
    window.setTestTime(0.5f);
    window.setPreview(true);
    ASSERT_TRUE(window.isPreviewing());
    EXPECT_LT(degreesBetween(s.scene->getComponent<Transform>(arm).rotation, expected), 0.1) << "saved Avatar";
    window.setTwist(pose::HumanBone::LeftUpperArm, 30.0f);
    window.updatePreview();
    EXPECT_LT(degreesBetween(s.scene->getComponent<Transform>(arm).rotation, expected), 0.1)
        << "an unsaved twist on both sides (the source's conversion too)";
    ASSERT_TRUE(window.apply(&error)) << error;
    window.updatePreview();
    EXPECT_LT(degreesBetween(s.scene->getComponent<Transform>(arm).rotation, expected), 0.1) << "after Apply";
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    window.syncWithProject();
    window.updatePreview();
    EXPECT_LT(degreesBetween(s.scene->getComponent<Transform>(arm).rotation, expected), 0.1) << "after its undo";
    window.stopPreview();
    fs::remove(sidecar);
}

TEST_F(HumanoidEditorTest, ControllerFromClipsKeepsTheHumanoidFlags) {
    AnimationPlayerComponent player;
    AnimationPlayerClip plain;
    plain.clip = "anim/walk.anim";
    AnimationPlayerClip flagged;
    flagged.clip = "models/hero.gltf#Run";
    flagged.retarget = true;
    flagged.mirror = true;
    flagged.footIK = true;
    player.clips = {plain, flagged};
    // S21's controller: the entries' flags on its states (the one retarget path plays them)
    const anim::ControllerDef data = animlayersui::controllerFromClips(player);
    ASSERT_EQ(data.layers.size(), 1u);
    const std::vector<anim::StateDef>& states = data.layers[0].machine.states;
    ASSERT_EQ(states.size(), 2u);
    EXPECT_FALSE(states[0].retarget || states[0].mirror || states[0].footIK);
    EXPECT_TRUE(states[1].retarget);
    EXPECT_TRUE(states[1].mirror);
    EXPECT_TRUE(states[1].footIK);
}
