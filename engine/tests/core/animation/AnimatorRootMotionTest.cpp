// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Root Transform options on the Animator (docs/specs/S94-rendering-anim-gaps.md F32, F33,
// AC31): Based Upon Body Orientation turns a clip's start heading out of the pose, Offset turns it
// back in, Feet keeps the feet where the model stands, and a clip with a root_motion block stays
// in place with Apply Root Motion off. Written red first: the yaw stayed, the drift was kept.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include "animation/ClipBinding.h"
#include "animation/RootMotion.h"
#include "component/ModelComponent.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    constexpr float kAnimatorRootDt = 1.0f / 30.0f;
    constexpr float kRadToDeg = 57.29577951308232f;

    std::string readFixture(const std::string& name){
        std::ifstream file(std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models/") + name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    class AnimatorRootMotionTest : public ::testing::Test, public AnimatorFixture {
    protected:
        std::vector<std::unique_ptr<Model>> models;
        std::string tpose;

        void SetUp() override{
            tpose = prefix + "hero_tpose.gltf";
            animtest::writeAsset(tpose, readFixture("hero_tpose.gltf"));
        }

        Entity loadHero(){
            models.push_back(std::make_unique<Model>(&scene));
            const Entity entity = models.back()->getEntity();
            EXPECT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(entity, tpose, false, false, false));
            scene.getComponent<ModelComponent>(entity).needUpdateModel = false;
            scene.getComponent<ModelComponent>(entity).filename = tpose;
            return entity;
        }

        Entity findByName(Entity from, const std::string& name){
            std::vector<Entity> stack = {from};
            while (!stack.empty()){
                const Entity e = stack.back();
                stack.pop_back();
                if (scene.getEntityNameRef(e) == name) return e;
                auto transforms = scene.getComponentArray<Transform>();
                for (size_t i = 0; i < transforms->size(); i++){
                    if (transforms->getComponentFromIndex(i).parent == e) stack.push_back(transforms->getEntity(i));
                }
            }
            return NULL_ENTITY;
        }

        // the hero playing one clip of a held hips transform with `rootMotion` (JSON), Apply Root Motion off
        Entity playHeldHips(const Quaternion& hipsRotation, const Vector3& hipsPosition, const std::string& rootMotion){
            const Entity hero = loadHero();
            const Entity hipsBone = findByName(hero, "mixamorig:Hips");
            std::string hipsPath;
            EXPECT_TRUE(relativePath(&scene, hero, hipsBone, hipsPath));
            ClipData data;
            data.wrap = ClipWrapMode::Loop;
            data.previewModel = tpose;
            const Quaternion& q = hipsRotation;
            data.tracks = {track(hipsPath, ClipChannel::Rotation, {key(0, {q.x, q.y, q.z, q.w}), key(1, {q.x, q.y, q.z, q.w})}),
                           track(hipsPath, ClipChannel::Position, {key(0, {hipsPosition.x, hipsPosition.y, hipsPosition.z}),
                                                                   key(1, {hipsPosition.x, hipsPosition.y, hipsPosition.z})})};
            if (!rootMotion.empty()) data.unknownKeys.push_back({"root_motion", rootMotion});
            // a file per call: the clip pool keeps a path's first clip
            const std::string name = "held" + std::to_string(models.size());
            AnimatorComponent& component = animator(controllerFile(name, oneStateController(clip(name, data))), hero);
            component.retarget = AnimatorRetarget::Off;
            step(0.0);
            step(kAnimatorRootDt, 3);
            return hipsBone;
        }

        static ControllerDef oneStateController(const std::string& path){
            return controller({}, {state("Play", path)});
        }

        float yawDegrees(Entity entity){
            return rootYawOf(rotation(entity)) * kRadToDeg;
        }
    };

}

// AC31: hips yawed 30 degrees at frame 0 face the root under Body Orientation; Offset 10 gives 10
TEST_F(AnimatorRootMotionTest, BodyOrientationRemovesStartYaw){
    const Entity original = playHeldHips(aboutY(30.0f), Vector3(0, 0.95f, 0), "{\"bake_rotation\": true}");
    EXPECT_NEAR(yawDegrees(original), 30.0f, 1.0f) << "Original keeps the clip's heading";

    const Entity body = playHeldHips(aboutY(30.0f), Vector3(0, 0.95f, 0),
                                     "{\"bake_rotation\": true, \"rotation_based_upon\": \"body_orientation\"}");
    EXPECT_NEAR(yawDegrees(body), 0.0f, 1.0f) << "Body Orientation: the body faces the root's forward";

    const Entity offset = playHeldHips(aboutY(30.0f), Vector3(0, 0.95f, 0),
                                       "{\"bake_rotation\": true, \"rotation_based_upon\": \"body_orientation\", \"rotation_offset\": 10}");
    EXPECT_NEAR(yawDegrees(offset), 10.0f, 1.0f) << "Offset turns the pose";
}

// Based Upon Feet: a clip whose hips are 0.2 m low at frame 0 is lifted so the feet stand where
// the model's rest pose has them; Y Offset adds to it
TEST_F(AnimatorRootMotionTest, FeetBasisKeepsTheFeetOnTheGround){
    const Entity original = playHeldHips(Quaternion(), Vector3(0, 0.75f, 0), "{}");
    EXPECT_NEAR(position(original).y, 0.75f, 1e-4f);

    const Entity feet = playHeldHips(Quaternion(), Vector3(0, 0.75f, 0), "{\"position_y_based_upon\": \"feet\"}");
    EXPECT_NEAR(position(feet).y, 0.95f, 1e-4f) << "the feet at their rest height";

    const Entity raised = playHeldHips(Quaternion(), Vector3(0, 0.75f, 0), "{\"position_y_based_upon\": \"feet\", \"position_y_offset\": 0.1}");
    EXPECT_NEAR(position(raised).y, 1.05f, 1e-4f);
}

// AC31: Apply Root Motion off and a clip with a root_motion block: the unbaked XZ motion leaves
// the pose (Unity), and the GameObject does not move
TEST_F(AnimatorRootMotionTest, ApplyOffDiscardsUnbakedMotion){
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {0.5f, 1, 2})})};
    data.unknownKeys.push_back({"root_motion", "{}"});
    AnimatorComponent& component = animator(controllerFile("c", controller({}, {state("Walk", clip("walk", data))})));
    ASSERT_FALSE(component.applyRootMotion);
    step(0.0);
    for (int i = 0; i < 40; i++){
        step(kAnimatorRootDt);
        EXPECT_LT(std::fabs(position(hips).x), 0.001f) << "frame " << i;
        EXPECT_LT(std::fabs(position(hips).z), 0.001f) << "frame " << i;
    }
    EXPECT_NEAR(position(hips).y, 1.0f, 1e-5f) << "bake_position_y (the default) keeps the height";
    EXPECT_LT(position(root).length(), 1e-6f) << "Apply Root Motion off: the GameObject stays";
}

// an Animator saved before that rule (Legacy Root Drift, set by the scene and prefab migrations)
// plays the same clip as it did: Apply Root Motion off keeps the drift in the pose
TEST_F(AnimatorRootMotionTest, LegacyRootDriftKeepsTheDrift){
    ClipData data;
    data.wrap = ClipWrapMode::Once;
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {0, 1, 2})})};
    data.unknownKeys.push_back({"root_motion", "{}"});
    AnimatorComponent& component = animator(controllerFile("c", controller({}, {state("Walk", clip("walk", data))})));
    component.legacyRootDrift = true;
    step(0.0);
    step(0.25, 2);
    EXPECT_NEAR(position(hips).z, 1.0f, 1e-4f) << "the drift stays in the pose";
    EXPECT_LT(position(root).length(), 1e-6f) << "and the GameObject stays";
}

// a clip without a root_motion block plays as before with Apply Root Motion off: its drift stays
TEST_F(AnimatorRootMotionTest, ApplyOffWithoutBlockKeepsTheDrift){
    ClipData data;
    data.wrap = ClipWrapMode::Once;
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {0, 1, 2})})};
    animator(controllerFile("c", controller({}, {state("Walk", clip("walk", data))})));
    step(0.0);
    step(0.25, 2);
    EXPECT_NEAR(position(hips).z, 1.0f, 1e-4f);
}

// a Based Upon that needs a body acts as Original on a rig without a humanoid
TEST_F(AnimatorRootMotionTest, BodyBasisWithoutHumanoidActsAsOriginal){
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {0, 1, 2})})};
    data.unknownKeys.push_back({"root_motion", "{\"position_xz_based_upon\": \"center_of_mass\"}"});
    AnimatorComponent& component = animator(controllerFile("c", controller({}, {state("Walk", clip("walk", data))})));
    component.applyRootMotion = true;
    step(0.0);
    step(0.25, 2);
    EXPECT_NEAR(position(root).z, 1.0f, 1e-3f) << "the hips' motion moved the GameObject";
    EXPECT_NEAR(position(hips).z, 0.0f, 1e-4f);
}

// the Based Upon members round-trip through the .anim's root_motion block (absent when Original)
TEST(RootMotionBlock, BasedUponRoundTrips){
    ClipRootMotion rm;
    rm.present = true;
    EXPECT_EQ(writeClipRootMotion(rm).find("based_upon"), std::string::npos) << "defaults write the S21 block unchanged";
    rm.rotationBasedUpon = RootRotationBasis::BodyOrientation;
    rm.rotationOffset = 12.5f;
    rm.positionYBasedUpon = RootPositionYBasis::Feet;
    rm.positionYOffset = -0.25f;
    rm.positionXZBasedUpon = RootPositionXZBasis::CenterOfMass;
    std::vector<ClipWarning> warnings;
    const ClipRootMotion back = readClipRootMotion({{"root_motion", writeClipRootMotion(rm)}}, &warnings);
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(back, rm);
    readClipRootMotion({{"root_motion", "{\"rotation_based_upon\": \"hips\"}"}}, &warnings);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].message.find("body_orientation"), std::string::npos) << "the warning names the valid values";
}
