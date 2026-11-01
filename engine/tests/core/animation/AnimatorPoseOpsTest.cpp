// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's SoA pose kernels (docs/specs/S21-animation.md AC4, R3, R17, D3): the SIMD back
// end equals the scalar one, masked override keeps unmasked bones exactly, additive at weight 0
// is bit-identical, an additive clip at its reference frame changes nothing, rig space keeps
// non-uniform scale like the render system's matrices.

#include <gtest/gtest.h>

#include "animation/AnimatorPose.h"
#include "math/Matrix4.h"

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace {

    Quaternion randomRotation(std::mt19937& random){
        std::normal_distribution<float> n(0.0f, 1.0f);
        Quaternion q(n(random), n(random), n(random), n(random));
        q.normalize();
        return q;
    }

    void randomPose(Pose& pose, std::mt19937& random){
        std::uniform_real_distribution<float> u(-2.0f, 2.0f);
        std::uniform_real_distribution<float> s(0.5f, 1.5f);
        for (size_t b = 0; b < pose.boneCount(); b++){
            pose.setPosition(b, Vector3(u(random), u(random), u(random)));
            pose.setRotation(b, randomRotation(random));
            pose.setScale(b, Vector3(s(random), s(random), s(random)));
        }
        for (size_t c = 0; c < pose.channelCount(); c++) pose.channelData()[c] = u(random);
    }

    struct BackendScope{
        PoseBackend previous = poseOpsBackend();
        explicit BackendScope(PoseBackend backend){ setPoseOpsBackend(backend); }
        ~BackendScope(){ setPoseOpsBackend(previous); }
    };

    float maxDifference(const Pose& a, const Pose& b){
        float worst = 0.0f;
        for (size_t i = 0; i < a.floatCount(); i++) worst = std::max(worst, std::fabs(a.raw()[i] - b.raw()[i]));
        return worst;
    }

}

TEST(AnimatorPoseOps, SimdMatchesScalar){
    std::mt19937 random(42);
    std::uniform_real_distribution<float> w(0.0f, 1.0f);
    const size_t bones = 37;
    const size_t channels = 6;
    float worst = 0.0f;
    std::vector<float> mask(40, 0.0f);
    std::vector<float> channelMask(8, 0.0f);
    for (int iteration = 0; iteration < 10000; iteration++){
        Pose a(bones, channels), b(bones, channels), d(bones, channels);
        randomPose(a, random);
        randomPose(b, random);
        poseMakeDelta(d, b, a);
        for (size_t i = 0; i < bones; i++) mask[i] = w(random);
        for (size_t i = 0; i < channels; i++) channelMask[i] = w(random);
        const float weight = w(random);
        Pose results[2][4];
        for (int backend = 0; backend < 2; backend++){
            BackendScope scope(backend == 0 ? PoseBackend::Scalar : PoseBackend::Native);
            Pose& blend = results[backend][0];
            blend.resize(bones, channels);
            poseBlend(blend, a, b, weight, mask.data(), channelMask.data());
            Pose& additive = results[backend][1];
            additive.resize(bones, channels);
            poseAdditive(additive, a, d, weight, mask.data(), channelMask.data());
            Pose& acc = results[backend][2];
            acc.resize(bones, channels);
            poseClear(acc);
            poseAccumulate(acc, a, weight);
            poseAccumulate(acc, b, 1.0f - weight + 0.01f);
            poseNormalize(acc, 1.01f, a);
            Pose& delta = results[backend][3];
            delta.resize(bones, channels);
            poseMakeDelta(delta, a, b);
        }
        for (int k = 0; k < 4; k++) worst = std::max(worst, maxDifference(results[0][k], results[1][k]));
    }
    EXPECT_LE(worst, 1e-5f) << "back end " << poseNativeBackendName();
}

TEST(AnimatorPoseOps, MaskOverrideKeepsUnmaskedBonesExactly){
    std::mt19937 random(7);
    Pose base(8, 2), layer(8, 2), out(8, 2);
    randomPose(base, random);
    randomPose(layer, random);
    std::vector<float> mask = {1, 0, 1, 0, 1, 0, 1, 0};
    std::vector<float> channelMask = {0, 1, 0, 0};
    poseBlend(out, base, layer, 1.0f, mask.data(), channelMask.data());
    for (size_t b = 0; b < 8; b++){
        const Pose& expected = mask[b] > 0.0f ? layer : base;
        Quaternion q = out.rotation(b), e = expected.rotation(b);
        if (q.dot(e) < 0.0f) q = -q;
        EXPECT_NEAR(q.w, e.w, 1e-6f);
        EXPECT_NEAR(q.x, e.x, 1e-6f);
        if (mask[b] == 0.0f){
            EXPECT_EQ(std::memcmp(&out.array(Pose::QX)[b], &base.array(Pose::QX)[b], sizeof(float)), 0) << "bit-identical";
            EXPECT_EQ(out.position(b), base.position(b));
            EXPECT_EQ(out.scale(b), base.scale(b));
        }else{
            EXPECT_EQ(out.position(b), layer.position(b));
        }
    }
    EXPECT_EQ(out.channelData()[0], base.channelData()[0]);
    EXPECT_EQ(out.channelData()[1], layer.channelData()[1]);
}

TEST(AnimatorPoseOps, AdditiveZeroWeightIsBitIdentical){
    std::mt19937 random(9);
    Pose base(12, 3), delta(12, 3), out(12, 3);
    randomPose(base, random);
    randomPose(delta, random);
    // -0 positions are kept as -0 (no 0 added)
    base.setPosition(3, Vector3(-0.0f, -0.0f, 1.0f));
    for (PoseBackend backend : {PoseBackend::Scalar, PoseBackend::Native}){
        BackendScope scope(backend);
        poseAdditive(out, base, delta, 0.0f, nullptr, nullptr);
        EXPECT_EQ(std::memcmp(out.raw(), base.raw(), base.floatCount() * sizeof(float)), 0);
        std::vector<float> zeroMask(12, 0.0f);
        poseAdditive(out, base, delta, 1.0f, zeroMask.data(), zeroMask.data());
        EXPECT_EQ(std::memcmp(out.raw(), base.raw(), base.floatCount() * sizeof(float)), 0);
    }
}

TEST(AnimatorPoseOps, AdditiveAtItsReferenceChangesNothing){
    std::mt19937 random(11);
    Pose base(10, 2), reference(10, 2), delta(10, 2), out(10, 2);
    randomPose(base, random);
    randomPose(reference, random);
    // the clip sampled at its reference frame is the reference: an identity delta
    poseMakeDelta(delta, reference, reference);
    poseAdditive(out, base, delta, 1.0f, nullptr, nullptr);
    EXPECT_LE(maxDifference(out, base), 1e-6f);

    // a real delta applied fully: base * (ref^-1 * pose)
    Pose pose(10, 2);
    randomPose(pose, random);
    poseMakeDelta(delta, pose, reference);
    poseAdditive(out, base, delta, 1.0f, nullptr, nullptr);
    for (size_t b = 0; b < 10; b++){
        Quaternion expected = (base.rotation(b) * (reference.rotation(b).inverse() * pose.rotation(b))).normalized();
        Quaternion got = out.rotation(b);
        if (got.dot(expected) < 0.0f) got = -got;
        EXPECT_NEAR(got.w, expected.w, 1e-5f);
        EXPECT_NEAR(got.y, expected.y, 1e-5f);
        const Vector3 t = base.position(b) + (pose.position(b) - reference.position(b));
        EXPECT_NEAR(out.position(b).x, t.x, 1e-5f);
    }
}

TEST(AnimatorPoseOps, RigSpaceKeepsNonUniformScale){
    // the render system's parent.modelMatrix * T * R * S, three levels with non-uniform scale
    Pose local(3, 0);
    const int parents[3] = {-1, 0, 1};
    local.setPosition(0, Vector3(1, 2, 3));
    local.setRotation(0, Quaternion(0.9238795f, 0, 0.3826834f, 0));
    local.setScale(0, Vector3(2, 1, 0.5f));
    local.setPosition(1, Vector3(0, 1, 0));
    local.setRotation(1, Quaternion(0.9238795f, 0.3826834f, 0, 0));
    local.setScale(1, Vector3(1, 3, 1));
    local.setPosition(2, Vector3(0.5f, 0, 0.25f));
    local.setRotation(2, Quaternion(0.7071068f, 0, 0, 0.7071068f));
    local.setScale(2, Vector3(1, 1, 1));
    std::vector<Affine> world(3);
    std::vector<Vector3> positions(3), scales(3);
    std::vector<Quaternion> rotations(3);
    poseToRigSpace(local, parents, RigSpace{world.data(), positions.data(), rotations.data(), scales.data()});
    Matrix4 expected;
    for (int i = 0; i < 3; i++){
        const Matrix4 m = Matrix4::translateMatrix(local.position(i)) * local.rotation(i).getRotationMatrix() * Matrix4::scaleMatrix(local.scale(i));
        expected = i == 0 ? m : expected * m;
        for (int c = 0; c < 3; c++){
            for (int r = 0; r < 3; r++) EXPECT_NEAR(world[i].m[c * 3 + r], expected[c][r], 1e-5f) << "bone " << i;
            EXPECT_NEAR(world[i].m[9 + c], expected[3][c], 1e-5f);
        }
        const Vector3 p = expected * Vector3(0, 0, 0);
        EXPECT_NEAR(positions[i].x, p.x, 1e-5f);
        EXPECT_NEAR(positions[i].y, p.y, 1e-5f);
    }
    const Affine inverse = world[2].inverse();
    const Affine identity = world[2] * inverse;
    for (int k = 0; k < 12; k++) EXPECT_NEAR(identity.m[k], Affine().m[k], 1e-5f);
}

// The one-element operations (the Animation Player's layers, docs/specs/S21-animation.md
// "Upstream absorption") are the scalar lanes of poseBlend / poseAdditive: the same bits as the
// Scalar back end, weight by weight (a per-bone mask carries each bone's weight)
TEST(AnimatorPoseOps, ElementOpsMatchTheKernels){
    BackendScope scalar(PoseBackend::Scalar);
    std::mt19937 random(7);
    std::uniform_real_distribution<float> w(0.0f, 1.0f);
    const size_t bones = 29;
    const size_t channels = 5;
    Pose base(bones, channels), layer(bones, channels), blended(bones, channels), added(bones, channels);
    randomPose(base, random);
    randomPose(layer, random);
    // (masks span the pose's padded strides, as the kernels read them in lanes)
    std::vector<float> mask(base.stride(), 0.0f), channelMask(base.channelStride(), 0.0f);
    for (size_t b = 0; b < bones; b++) mask[b] = w(random);
    mask[0] = 0.0f;
    mask[1] = 1.0f;
    for (size_t c = 0; c < channels; c++) channelMask[c] = w(random);
    poseBlend(blended, base, layer, 1.0f, mask.data(), channelMask.data());
    poseAdditive(added, base, layer, 1.0f, mask.data(), channelMask.data());
    auto same = [](float a, float b){ return std::memcmp(&a, &b, sizeof(float)) == 0; };
    for (size_t b = 0; b < bones; b++){
        const Quaternion q = poseBlendRotation(base.rotation(b), layer.rotation(b), mask[b]);
        const Quaternion a = poseAdditiveRotation(base.rotation(b), layer.rotation(b), mask[b]);
        const Quaternion kq = blended.rotation(b), ka = added.rotation(b);
        EXPECT_TRUE(same(q.x, kq.x) && same(q.y, kq.y) && same(q.z, kq.z) && same(q.w, kq.w)) << "blend rotation, bone " << b;
        EXPECT_TRUE(same(a.x, ka.x) && same(a.y, ka.y) && same(a.z, ka.z) && same(a.w, ka.w)) << "additive rotation, bone " << b;
        for (Pose::Array t : {Pose::TX, Pose::TY, Pose::TZ}){
            EXPECT_TRUE(same(poseBlendValue(base.array(t)[b], layer.array(t)[b], mask[b]), blended.array(t)[b])) << "blend translation " << b;
            EXPECT_TRUE(same(poseAdditiveValue(base.array(t)[b], layer.array(t)[b], mask[b]), added.array(t)[b])) << "additive translation " << b;
        }
        for (Pose::Array s : {Pose::SX, Pose::SY, Pose::SZ}){
            EXPECT_TRUE(same(poseBlendValue(base.array(s)[b], layer.array(s)[b], mask[b]), blended.array(s)[b])) << "blend scale " << b;
            EXPECT_TRUE(same(poseAdditiveScale(base.array(s)[b], layer.array(s)[b], mask[b]), added.array(s)[b])) << "additive scale " << b;
        }
    }
    for (size_t c = 0; c < channels; c++){
        EXPECT_TRUE(same(poseBlendValue(base.channelData()[c], layer.channelData()[c], channelMask[c]), blended.channelData()[c])) << "channel " << c;
        EXPECT_TRUE(same(poseAdditiveValue(base.channelData()[c], layer.channelData()[c], channelMask[c]), added.channelData()[c])) << "channel " << c;
    }
}
