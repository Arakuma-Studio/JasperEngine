// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Capsule shadows' geometry (render/CapsuleShadows.h, docs/specs/S57-forge-lighting.md R2): Fit To
// Skeleton on a humanoid rig, the bone-space shapes and the occlusion the shaders evaluate.

#include "render/CapsuleShadows.h"

#include <gtest/gtest.h>

#include <map>

using namespace jasperengine;

namespace {

    // a 1.8 m Mixamo-named rig standing at the origin, its bones in world space with identity
    // rotations (every bone's +Y up the body)
    std::vector<CapsuleShadows::Bone> mixamoRig(float scale = 1.0f){
        struct Entry{ const char* name; const char* parent; float x, y, z; };
        static const Entry entries[] = {
            {"mixamorig:Hips", nullptr, 0.0f, 1.0f, 0.0f},
            {"mixamorig:Spine", "mixamorig:Hips", 0.0f, 1.1f, 0.0f},
            {"mixamorig:Spine1", "mixamorig:Spine", 0.0f, 1.22f, 0.0f},
            {"mixamorig:Spine2", "mixamorig:Spine1", 0.0f, 1.35f, 0.0f},
            {"mixamorig:Neck", "mixamorig:Spine2", 0.0f, 1.5f, 0.0f},
            {"mixamorig:Head", "mixamorig:Neck", 0.0f, 1.6f, 0.0f},
            {"mixamorig:HeadTop_End", "mixamorig:Head", 0.0f, 1.8f, 0.0f},
            {"mixamorig:LeftShoulder", "mixamorig:Spine2", 0.08f, 1.42f, 0.0f},
            {"mixamorig:LeftArm", "mixamorig:LeftShoulder", 0.18f, 1.42f, 0.0f},
            {"mixamorig:LeftForeArm", "mixamorig:LeftArm", 0.45f, 1.42f, 0.0f},
            {"mixamorig:LeftHand", "mixamorig:LeftForeArm", 0.7f, 1.42f, 0.0f},
            {"mixamorig:RightShoulder", "mixamorig:Spine2", -0.08f, 1.42f, 0.0f},
            {"mixamorig:RightArm", "mixamorig:RightShoulder", -0.18f, 1.42f, 0.0f},
            {"mixamorig:RightForeArm", "mixamorig:RightArm", -0.45f, 1.42f, 0.0f},
            {"mixamorig:RightHand", "mixamorig:RightForeArm", -0.7f, 1.42f, 0.0f},
            {"mixamorig:LeftUpLeg", "mixamorig:Hips", 0.1f, 0.95f, 0.0f},
            {"mixamorig:LeftLeg", "mixamorig:LeftUpLeg", 0.1f, 0.5f, 0.0f},
            {"mixamorig:LeftFoot", "mixamorig:LeftLeg", 0.1f, 0.08f, 0.0f},
            {"mixamorig:LeftToeBase", "mixamorig:LeftFoot", 0.1f, 0.02f, 0.14f},
            {"mixamorig:RightUpLeg", "mixamorig:Hips", -0.1f, 0.95f, 0.0f},
            {"mixamorig:RightLeg", "mixamorig:RightUpLeg", -0.1f, 0.5f, 0.0f},
            {"mixamorig:RightFoot", "mixamorig:RightLeg", -0.1f, 0.08f, 0.0f},
            {"mixamorig:RightToeBase", "mixamorig:RightFoot", -0.1f, 0.02f, 0.14f},
        };
        std::vector<CapsuleShadows::Bone> bones;
        std::map<std::string, int> indexOf;
        for (const Entry& entry : entries){
            CapsuleShadows::Bone bone;
            bone.name = entry.name;
            bone.parent = entry.parent ? indexOf.at(entry.parent) : -1;
            bone.position = Vector3(entry.x, entry.y, entry.z) * scale;
            bone.scale = Vector3(1.0f, 1.0f, 1.0f);
            indexOf[entry.name] = (int)bones.size();
            bones.push_back(bone);
        }
        return bones;
    }

    // the world capsule of a fitted shape on its bone of the rig
    CapsuleShadows::World worldOf(const CapsuleShadowShape& shape, const std::vector<CapsuleShadows::Bone>& bones){
        for (const CapsuleShadows::Bone& bone : bones){
            if (bone.name == shape.bone){
                const Matrix4 model = Matrix4::translateMatrix(bone.position) * bone.rotation.getRotationMatrix() * Matrix4::scaleMatrix(bone.scale);
                return CapsuleShadows::toWorld(shape, model);
            }
        }
        ADD_FAILURE() << "no bone " << shape.bone;
        return CapsuleShadows::World();
    }

    bool closeTo(const Vector3& a, const Vector3& b, float tolerance){
        return (a - b).length() <= tolerance;
    }

}

TEST(CapsuleFit, HumanoidRigGetsTheForgesEightCapsules){
    const std::vector<CapsuleShadows::Bone> bones = mixamoRig();
    std::vector<CapsuleShadowShape> capsules;
    std::string error;
    ASSERT_TRUE(CapsuleShadows::fitToSkeleton(bones, capsules, &error)) << error;
    ASSERT_EQ(capsules.size(), 8u);

    // head, spine, then each leg's upper leg, lower leg and foot
    const char* expectedBones[] = {"mixamorig:Neck", "mixamorig:Hips", "mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig:LeftFoot",
                                   "mixamorig:RightUpLeg", "mixamorig:RightLeg", "mixamorig:RightFoot"};
    for (size_t i = 0; i < 8; i++)
        EXPECT_EQ(capsules[i].bone, expectedBones[i]);

    // the segments run joint to joint (the head to its end bone)
    const CapsuleShadows::World head = worldOf(capsules[0], bones);
    EXPECT_TRUE(closeTo(head.a, Vector3(0.0f, 1.5f, 0.0f), 1e-4f) || closeTo(head.b, Vector3(0.0f, 1.5f, 0.0f), 1e-4f));
    EXPECT_TRUE(closeTo(head.a, Vector3(0.0f, 1.8f, 0.0f), 1e-4f) || closeTo(head.b, Vector3(0.0f, 1.8f, 0.0f), 1e-4f));
    const CapsuleShadows::World shin = worldOf(capsules[3], bones);
    EXPECT_TRUE(closeTo(shin.a, Vector3(0.1f, 0.5f, 0.0f), 1e-4f) || closeTo(shin.b, Vector3(0.1f, 0.5f, 0.0f), 1e-4f));
    EXPECT_TRUE(closeTo(shin.a, Vector3(0.1f, 0.08f, 0.0f), 1e-4f) || closeTo(shin.b, Vector3(0.1f, 0.08f, 0.0f), 1e-4f));
    // the spine to the upper chest (Spine2)
    const CapsuleShadows::World spine = worldOf(capsules[1], bones);
    EXPECT_NEAR(std::max(spine.a.y, spine.b.y), 1.35f, 1e-4f);

    // radii are shares of the body's height: the torso widest, the feet narrowest
    EXPECT_GT(capsules[1].radius, capsules[0].radius);
    EXPECT_GT(capsules[2].radius, capsules[3].radius);
    EXPECT_GT(capsules[3].radius, capsules[4].radius);
    // the height: the head above the hips (0.6 m) and a leg (the foot below them, 0.93 m)
    EXPECT_NEAR(capsules[1].radius, 0.09f * 1.525f, 0.005f);
    // a leg's bone points along +Y: its capsule lies along the bone's Y
    EXPECT_EQ(capsules[2].direction, CapsuleDirection::Y_AXIS);
}

TEST(CapsuleFit, RadiiFollowTheRigsScale){
    std::vector<CapsuleShadowShape> small;
    std::vector<CapsuleShadowShape> large;
    ASSERT_TRUE(CapsuleShadows::fitToSkeleton(mixamoRig(1.0f), small));
    ASSERT_TRUE(CapsuleShadows::fitToSkeleton(mixamoRig(100.0f), large));
    ASSERT_EQ(small.size(), large.size());
    for (size_t i = 0; i < small.size(); i++){
        EXPECT_NEAR(large[i].radius, small[i].radius * 100.0f, small[i].radius * 0.01f);
        EXPECT_NEAR(large[i].length, small[i].length * 100.0f, small[i].length * 0.01f);
    }
}

TEST(CapsuleFit, BonesWithoutAHumanoidAreRefused){
    std::vector<CapsuleShadows::Bone> bones;
    for (const char* name : {"root", "tail1", "tail2"}){
        CapsuleShadows::Bone bone;
        bone.name = name;
        bone.parent = (int)bones.size() - 1;
        bone.position = Vector3(0.0f, (float)bones.size(), 0.0f);
        bones.push_back(bone);
    }
    std::vector<CapsuleShadowShape> capsules(1);
    std::string error;
    EXPECT_FALSE(CapsuleShadows::fitToSkeleton(bones, capsules, &error));
    EXPECT_FALSE(error.empty());
}

TEST(CapsuleFit, ShapeRoundTripsThroughARotatedScaledBone){
    CapsuleShadows::Bone bone;
    bone.name = "arm";
    bone.position = Vector3(1.0f, 2.0f, 3.0f);
    bone.rotation = Quaternion(90.0f, Vector3(0.0f, 0.0f, 1.0f));
    bone.scale = Vector3(2.0f, 2.0f, 2.0f);
    const Vector3 a(1.0f, 2.5f, 3.0f);
    const Vector3 b(0.0f, 2.5f, 3.0f);
    const CapsuleShadowShape shape = CapsuleShadows::fromWorld("arm", bone, a, b, 0.2f);
    EXPECT_NEAR(shape.radius, 0.1f, 1e-5f);
    const Matrix4 model = Matrix4::translateMatrix(bone.position) * bone.rotation.getRotationMatrix() * Matrix4::scaleMatrix(bone.scale);
    const CapsuleShadows::World world = CapsuleShadows::toWorld(shape, model);
    EXPECT_NEAR(world.radius, 0.2f, 1e-5f);
    EXPECT_TRUE((closeTo(world.a, a, 1e-4f) && closeTo(world.b, b, 1e-4f)) || (closeTo(world.a, b, 1e-4f) && closeTo(world.b, a, 1e-4f)));
}

TEST(CapsuleFit, ReachCullsReceiversOutsideTheShadow){
    CapsuleShadows::World capsule;
    capsule.a = Vector3(0.0f, 1.0f, 0.0f);
    capsule.b = Vector3(0.0f, 1.8f, 0.0f);
    capsule.radius = 0.15f;
    const Vector3 up(0.0f, 1.0f, 0.0f);
    AABB ground;
    ground.setExtents(Vector3(-5.0f, -0.1f, -5.0f), Vector3(5.0f, 0.0f, 5.0f));
    AABB faraway;
    faraway.setExtents(Vector3(50.0f, -0.1f, 50.0f), Vector3(60.0f, 0.0f, 60.0f));
    AABB above;
    above.setExtents(Vector3(-1.0f, 5.0f, -1.0f), Vector3(1.0f, 6.0f, 1.0f));
    EXPECT_TRUE(CapsuleShadows::reaches(capsule, up, 0.25f, ground));
    EXPECT_FALSE(CapsuleShadows::reaches(capsule, up, 0.25f, faraway));
    EXPECT_FALSE(CapsuleShadows::reaches(capsule, up, 0.25f, above));
}
