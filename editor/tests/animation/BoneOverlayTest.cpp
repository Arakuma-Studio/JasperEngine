// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S74 R26, AC12: under Write Bones: Exposed the overlay draws the Animator's pose, not the Transforms
// the Animator does not write; a root whose parent is no bone still gets its marker.

#include <gtest/gtest.h>

#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorInstance.h"
#include "animation/BoneOverlay.h"
#include "component/AnimatorComponent.h"
#include "component/BoneComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "subsystem/AnimatorSystem.h"
#include "subsystem/RenderSystem.h"

#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    ClipKey key(float time, std::vector<float> value){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        return k;
    }

    ClipTrackData track(const std::string& path, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = ClipChannel::Position;
        t.keys = std::move(keys);
        return t;
    }

    class BoneOverlayTest : public ::testing::Test {
    protected:
        Scene scene;
        fs::path directory;
        Entity root = NULL_ENTITY, armature = NULL_ENTITY, hips = NULL_ENTITY, spine = NULL_ENTITY;
        ModelComponent model;

        Entity node(Entity parent, const std::string& name, Vector3 position, bool bone){
            const Entity entity = scene.createEntity();
            scene.setEntityName(entity, name);
            Transform transform;
            transform.position = position;
            scene.addComponent<Transform>(entity, transform);
            if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
            if (bone){
                scene.addComponent<BoneComponent>(entity, {});
                model.bonesIdMapping[(int)model.bonesIdMapping.size()] = entity;
            }
            return entity;
        }

        void SetUp() override {
            directory = fs::temp_directory_path() / ("je_bone_overlay_" + std::to_string((long long)::testing::UnitTest::GetInstance()->random_seed()));
            fs::create_directories(directory);
            AnimationClipPool::clear();
            root = node(NULL_ENTITY, "Hero", Vector3(10, 0, 0), false);
            armature = node(root, "Armature", Vector3(0, 0, 0), false);
            hips = node(armature, "Hips", Vector3(0, 1, 0), true);
            spine = node(hips, "Spine", Vector3(0, 0.3f, 0), true);
        }

        void TearDown() override {
            AnimationClipPool::clear();
            AnimatorAssetPool::clear();
            fs::remove_all(directory);
        }

        void resolveTransforms(){
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                transform.needUpdate = false;
            }
        }

        AnimatorComponent& playing(AnimatorWriteBones writeBones, std::vector<std::string> exposed = {}){
            ClipData data;
            data.wrap = ClipWrapMode::Loop;
            data.tracks = {track("Armature/Hips", {key(0, {0, 1, 0}), key(1, {2, 1, 0})}),
                           track("Armature/Hips/Spine", {key(0, {0, 0.3f, 0}), key(1, {0, 0.3f, 2})})};
            const fs::path clip = directory / "walk.anim";
            std::ofstream(clip, std::ios::binary) << anim::writeClip(data);
            anim::ControllerDef controller;
            anim::StateDef state;
            state.name = "Play";
            state.motion.clip.path = clip.generic_string();
            anim::LayerDef layer;
            layer.name = "Base";
            layer.weight = 1.0f;
            layer.machine.defaultState = "Play";
            layer.machine.states = {state};
            controller.layers = {layer};
            const fs::path file = directory / "c.controller";
            std::ofstream(file, std::ios::binary) << anim::writeController(controller);
            scene.addComponent<AnimatorComponent>(root, {});
            AnimatorComponent& component = scene.getComponent<AnimatorComponent>(root);
            component.controller = file.generic_string();
            component.needsLoad = true;
            component.writeBones = writeBones;
            component.exposedBones = std::move(exposed);
            scene.getSystem<AnimatorSystem>()->update(0.0);
            scene.getSystem<AnimatorSystem>()->update(0.5);
            resolveTransforms();
            return component;
        }
    };

}

TEST_F(BoneOverlayTest, UnderExposedTheOverlayDrawsTheRigPose){
    playing(AnimatorWriteBones::Exposed);
    // the Transforms were left at rest, and that is what reading them would draw
    EXPECT_NEAR(scene.getComponent<Transform>(hips).worldPosition.x, 10.0f, 1e-5f);
    const anim::AnimatorInstance* instance = scene.getSystem<AnimatorSystem>()->instance(root);
    ASSERT_NE(instance, nullptr);
    const int hipsBone = instance->rig.findBone(hips);
    const int spineBone = instance->rig.findBone(spine);
    ASSERT_GE(hipsBone, 0);
    ASSERT_GE(spineBone, 0);
    ASSERT_GT(instance->rigWorld().size(), (size_t)spineBone);

    const std::vector<BoneOverlaySegment> segments = boneOverlaySegments(&scene, model);
    ASSERT_EQ(segments.size(), 2u);
    const Vector3 rigHips = instance->rigWorld()[(size_t)hipsBone].translation();
    const Vector3 rigSpine = instance->rigWorld()[(size_t)spineBone].translation();
    EXPECT_NEAR(rigHips.x, 1.0f, 1e-4f) << "the clip's pose at 0.5 s";
    const BoneOverlaySegment& hipsSegment = segments[0];
    const BoneOverlaySegment& spineSegment = segments[1];
    EXPECT_NEAR(hipsSegment.bone.x, rigHips.x + 10.0f, 1e-4f) << "drawn at the pose, under the Animator's Transform";
    EXPECT_NEAR(hipsSegment.bone.y, rigHips.y, 1e-4f);
    EXPECT_NEAR(spineSegment.bone.x, rigSpine.x + 10.0f, 1e-4f);
    EXPECT_NEAR(spineSegment.bone.z, rigSpine.z, 1e-4f);
    ASSERT_TRUE(spineSegment.hasParent);
    EXPECT_NEAR(spineSegment.parent.x, hipsSegment.bone.x, 1e-5f);
    EXPECT_TRUE(isUnwrittenAnimatorBone(&scene, hips));
}

TEST_F(BoneOverlayTest, UnderAllTheTransformsAreThePose){
    playing(AnimatorWriteBones::All);
    const std::vector<BoneOverlaySegment> segments = boneOverlaySegments(&scene, model);
    ASSERT_EQ(segments.size(), 2u);
    EXPECT_NEAR(segments[0].bone.x, scene.getComponent<Transform>(hips).worldPosition.x, 1e-5f);
    EXPECT_NEAR(segments[0].bone.x, 11.0f, 1e-4f);
    EXPECT_FALSE(isUnwrittenAnimatorBone(&scene, hips)) << "written: the Inspector says nothing";
}

TEST_F(BoneOverlayTest, AnExposedListedBoneIsWritten){
    playing(AnimatorWriteBones::Exposed, {"Armature/Hips/Spine"});
    EXPECT_TRUE(isUnwrittenAnimatorBone(&scene, hips));
    EXPECT_FALSE(isUnwrittenAnimatorBone(&scene, spine)) << "listed: its Transform is written";
}

TEST_F(BoneOverlayTest, ARootUnderANodeThatIsNoBoneHasItsMarker){
    resolveTransforms();
    const std::vector<BoneOverlaySegment> segments = boneOverlaySegments(&scene, model);
    ASSERT_EQ(segments.size(), 2u);
    EXPECT_FALSE(segments[0].hasParent) << "Hips sits under Armature, a plain node: a marker, no segment";
    EXPECT_TRUE(segments[1].hasParent);
    EXPECT_NEAR(segments[1].parent.y, 1.0f, 1e-5f);
    EXPECT_FALSE(isUnwrittenAnimatorBone(&scene, hips)) << "no Animator: nothing to say";
}
