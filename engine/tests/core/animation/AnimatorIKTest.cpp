// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's IK solvers (docs/specs/S21-animation.md AC5, R11, D11): two-bone reaches within
// 1 mm with the mid joint on the pole side, straightens toward an unreachable target, FABRIK
// converges, look-at never passes its clamp, non-uniform scale chains, the spring's half-life.
// S77 R32-R34: the soft reach, hinge axis and twist, reached, the look-at chain walk with an aim
// offset, the ankle's height on a slope, and the degenerate cases of each solver.

#include <gtest/gtest.h>

#include "animation/AnimatorIK.h"

#include <cmath>
#include <random>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace {

    // a chain of `count` bones along +y, `length` apart (bone 0 at the origin)
    struct Chain{
        Pose local;
        std::vector<int> parents;
        std::vector<Affine> world;
        std::vector<Vector3> positions;
        std::vector<Quaternion> rotations;
        std::vector<Vector3> scales;
        IKContext context;

        Chain(int count, float length, Vector3 scale = Vector3(1, 1, 1)): local((size_t)count, 0){
            parents.resize((size_t)count);
            for (int i = 0; i < count; i++){
                parents[(size_t)i] = i - 1;
                local.setPosition((size_t)i, i == 0 ? Vector3(0, 0, 0) : Vector3(0, length, 0));
                local.setScale((size_t)i, scale);
            }
            world.resize((size_t)count);
            positions.resize((size_t)count);
            rotations.resize((size_t)count);
            scales.resize((size_t)count);
            context.local = &local;
            context.parents = parents.data();
            context.space = RigSpace{world.data(), positions.data(), rotations.data(), scales.data()};
            refreshRigSpace(context, 0);
        }
    };

    float angleBetween(const Vector3& a, const Vector3& b){
        const float d = a.normalized().dotProduct(b.normalized());
        return std::acos(std::min(1.0f, std::max(-1.0f, d))) * 57.29577951f;
    }

    Quaternion about(const Vector3& axis, float degrees){
        const Vector3 unit = axis.normalized();
        const float half = degrees * 0.5f * 0.0174532925f;
        return Quaternion(std::cos(half), unit.x * std::sin(half), unit.y * std::sin(half), unit.z * std::sin(half));
    }

    bool finite(const Vector3& v){ return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    void expectFinitePose(const Chain& chain){
        for (const Vector3& position : chain.positions) EXPECT_TRUE(finite(position));
        for (const Quaternion& q : chain.rotations){
            EXPECT_TRUE(std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z));
        }
    }

    // the bend of the joint: 0 on a straight chain
    float bendOf(const Chain& chain){
        return angleBetween(chain.positions[1] - chain.positions[0], chain.positions[2] - chain.positions[1]);
    }

    // the part of `v` across the line along `axis`
    Vector3 across(const Vector3& v, const Vector3& axis){
        const Vector3 unit = axis.normalized();
        return v - unit * v.dotProduct(unit);
    }

    // the signed angle from `from` to `to` about `axis`
    float signedAngle(const Vector3& from, const Vector3& to, const Vector3& axis){
        return std::atan2(axis.normalized().dotProduct(from.crossProduct(to)), from.dotProduct(to)) * 57.29577951f;
    }

    // a two-bone leg of two `length` bones, the knee bent `bend` degrees: the frame's fresh pose
    // (IK always starts from the animated one)
    struct LegSolve{
        Vector3 knee;
        Vector3 end;
        float bend = 0.0f;
        IKResult result;
    };
    LegSolve solveLeg(float length, float bend, const Vector3& target, const TwoBoneOptions& options, float weight = 1.0f){
        Chain chain(3, length);
        chain.local.setRotation(1, about(Vector3(1, 0, 0), bend));
        refreshRigSpace(chain.context, 0);
        LegSolve out;
        out.result = solveTwoBone(chain.context, 0, 1, 2, target, nullptr, nullptr, weight, options);
        out.knee = chain.positions[1];
        out.end = chain.positions[2];
        out.bend = bendOf(chain);
        return out;
    }

    constexpr int kLookBones[4] = {0, 1, 2, 3};

    // the angle between the aim ray of the chain's last bone and the way to the target
    float aimError(const Chain& chain, int last, const Vector3& forward, const Vector3& offset, const Vector3& target){
        const Vector3 eye = chain.positions[(size_t)last] + chain.rotations[(size_t)last] * offset;
        return angleBetween(chain.rotations[(size_t)last] * forward, target - eye);
    }

    // a chain leaning forward (+z, the way it faces), its bones `lengths` apart
    void leanForward(Chain& chain, float degrees, const std::vector<float>& lengths){
        for (size_t i = 0; i < lengths.size(); i++) chain.local.setPosition(i + 1, Vector3(0, lengths[i], 0));
        chain.local.setRotation(0, about(Vector3(1, 0, 0), degrees));
        refreshRigSpace(chain.context, 0);
    }

    // `yaw` and `pitch` degrees off the forward of a chain's head, `distance` away from it
    Vector3 targetOff(const Chain& chain, int head, const Vector3& forward, float yaw, float pitch, float distance){
        const Quaternion rig = chain.rotations[(size_t)head];
        const Vector3 direction = rig * (about(Vector3(0, 1, 0), yaw) * (about(Vector3(1, 0, 0), -pitch) * forward));
        return chain.positions[(size_t)head] + direction * distance;
    }

}

TEST(AnimatorIK, TwoBoneReachesWithThePoleSide){
    Chain chain(3, 1.0f);
    // a slight bend so the chain has a plane
    chain.local.setRotation(1, Quaternion(0.9998477f, 0.0174524f, 0, 0));
    refreshRigSpace(chain.context, 0);
    const Vector3 target(0.5f, 1.2f, 0.3f);
    const Vector3 pole(0, 1, 2);
    solveTwoBone(chain.context, 0, 1, 2, target, &pole, nullptr, 1.0f);
    EXPECT_LE((chain.positions[2] - target).length(), 0.001f);
    // the mid joint lies on the pole's side of the line from the root to the target
    const Vector3 axis = (target - chain.positions[0]).normalized();
    Vector3 mid = chain.positions[1] - axis * chain.positions[1].dotProduct(axis);
    Vector3 side = pole - axis * pole.dotProduct(axis);
    EXPECT_GT(mid.dotProduct(side), 0.0f);
    // the bones keep their lengths
    EXPECT_NEAR((chain.positions[1] - chain.positions[0]).length(), 1.0f, 1e-4f);
    EXPECT_NEAR((chain.positions[2] - chain.positions[1]).length(), 1.0f, 1e-4f);
}

TEST(AnimatorIK, TwoBoneUnreachableTargetStraightensTheChain){
    Chain chain(3, 1.0f);
    chain.local.setRotation(1, Quaternion(0.9238795f, 0.3826834f, 0, 0));
    refreshRigSpace(chain.context, 0);
    const Vector3 target(5, 0, 0);
    solveTwoBone(chain.context, 0, 1, 2, target, nullptr, nullptr, 1.0f);
    const Vector3 direction = (target - chain.positions[0]).normalized();
    EXPECT_LE(angleBetween(chain.positions[2] - chain.positions[0], direction), 0.1f);
    EXPECT_LE(angleBetween(chain.positions[1] - chain.positions[0], direction), 5.0f) << "almost straight (0.999 of the reach)";
    EXPECT_NEAR((chain.positions[2] - chain.positions[0]).length(), 1.998f, 1e-3f);
}

TEST(AnimatorIK, TwoBoneWeightBlendsAndZeroDoesNothing){
    Chain chain(3, 1.0f);
    chain.local.setRotation(1, Quaternion(0.9238795f, 0.3826834f, 0, 0));
    refreshRigSpace(chain.context, 0);
    const Vector3 before = chain.positions[2];
    solveTwoBone(chain.context, 0, 1, 2, Vector3(1, 0.5f, 0), nullptr, nullptr, 0.0f);
    EXPECT_EQ(chain.positions[2], before);
    solveTwoBone(chain.context, 0, 1, 2, Vector3(1, 0.5f, 0), nullptr, nullptr, 0.5f);
    EXPECT_GT((chain.positions[2] - before).length(), 0.01f);
    EXPECT_GT((chain.positions[2] - Vector3(1, 0.5f, 0)).length(), 0.01f);
}

TEST(AnimatorIK, FabrikConverges){
    std::mt19937 random(5);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int converged = 0;
    for (int test = 0; test < 8; test++){
        Chain chain(6, 0.5f);
        for (int i = 1; i < 6; i++){
            Quaternion q(1.0f, u(random) * 0.2f, u(random) * 0.2f, u(random) * 0.2f);
            q.normalize();
            chain.local.setRotation((size_t)i, q);
        }
        refreshRigSpace(chain.context, 0);
        // a reachable target in front of the chain: 30 % to 75 % of the reach
        Vector3 target(u(random), std::fabs(u(random)) + 0.5f, u(random));
        target = target.normalized() * (2.5f * (0.3f + 0.45f * std::fabs(u(random))));
        const int bones[6] = {0, 1, 2, 3, 4, 5};
        solveFabrik(chain.context, bones, 6, target, 10, 0.001f, 1.0f);
        if ((chain.positions[5] - target).length() <= 0.001f) converged++;
        for (int i = 0; i < 5; i++) EXPECT_NEAR((chain.positions[(size_t)i + 1] - chain.positions[(size_t)i]).length(), 0.5f, 1e-4f);
    }
    EXPECT_EQ(converged, 8);
}

TEST(AnimatorIK, LookAtNeverExceedsItsClamp){
    const int bones[3] = {0, 1, 2};
    const float weights[3] = {0.2f, 0.3f, 0.5f};
    const Vector3 forward(0, 0, 1);
    for (int i = 0; i < 36; i++){
        Chain c(3, 0.3f);
        const float angle = (float)i * 10.0f * 0.0174532925f;
        const Vector3 target = c.positions[2] + Vector3(std::sin(angle), 0.3f, std::cos(angle)) * 3.0f;
        const Vector3 before = c.rotations[2] * forward;
        solveLookAt(c.context, bones, weights, 3, forward, target, 45.0f, 1.0f);
        const Vector3 after = c.rotations[2] * forward;
        EXPECT_LE(angleBetween(before, after), 45.0f + 0.05f) << "target at " << i * 10 << " degrees";
        if (i == 0){
            EXPECT_LE(angleBetween(after, target - c.positions[2]), 0.5f) << "an in-range target is reached";
        }
    }
}

TEST(AnimatorIK, NonUniformScaleChains){
    Chain chain(3, 1.0f, Vector3(1, 2, 1));
    // world bone lengths are scaled by the parents' y scale: 2, then 4
    EXPECT_NEAR(chain.positions[1].y, 2.0f, 1e-5f);
    EXPECT_NEAR(chain.positions[2].y, 6.0f, 1e-5f);
    chain.local.setRotation(1, Quaternion(0.9998477f, 0.0174524f, 0, 0));
    refreshRigSpace(chain.context, 0);
    const Vector3 target(1.5f, 3.0f, 1.0f);
    solveTwoBone(chain.context, 0, 1, 2, target, nullptr, nullptr, 1.0f);
    EXPECT_LE((chain.positions[2] - target).length(), 0.01f);
}

TEST(AnimatorIK, SpringHalfLife){
    float velocity = 0.0f;
    float x = 1.0f;
    const float halfLife = 0.08f;
    float t = 0.0f;
    while (t < halfLife - 1e-6f){
        x = springDamp(x, 0.0f, velocity, halfLife, 0.001f);
        t += 0.001f;
    }
    EXPECT_NEAR(x, 0.5f, 0.01f);
    for (int i = 0; i < 2000; i++) x = springDamp(x, 0.0f, velocity, halfLife, 0.001f);
    EXPECT_NEAR(x, 0.0f, 1e-4f) << "critically damped: no overshoot";
    EXPECT_GE(x, -1e-6f);
    EXPECT_EQ(shortestArc(Vector3(1, 0, 0), Vector3(1, 0, 0)).w, 1.0f);
    const Quaternion half = shortestArc(Vector3(1, 0, 0), Vector3(-1, 0, 0));
    EXPECT_NEAR(std::fabs(half.w), 0.0f, 1e-6f);
}

// ---- S77 R32: two-bone soften, hinge axis, twist, reached

// The red case: a hard limit moves the knee 2.7 degrees per mm of target just before 0.999 of the
// reach, then freezes it for good. Softened, the knee is continuous and keeps moving past the reach.
TEST(AnimatorIK, TwoBoneSoftenKeepsTheKneeMoving){
    const float length = 0.45f;
    const float reach = 2.0f * length * 0.999f;
    TwoBoneOptions options;
    options.soften = 0.05f;
    const float step = 0.0001f;
    float fastest = 0.0f;
    float previousEnd = 0.0f;
    LegSolve previous = solveLeg(length, 10.0f, Vector3(0, reach * 0.90f, 0), options);
    for (float d = reach * 0.90f + step; d < reach * 1.15f; d += step){
        const LegSolve now = solveLeg(length, 10.0f, Vector3(0, d, 0), options);
        fastest = std::max(fastest, std::fabs(now.bend - previous.bend) / (step * 1000.0f));
        const float end = now.end.length();
        EXPECT_GE(end, previousEnd - 2e-5f) << "the end only moves out, at " << d;
        EXPECT_LT(end, reach + 1e-5f) << "the eased distance never passes the hard limit, at " << d;
        previousEnd = end;
        previous = now;
    }
    EXPECT_LE(fastest, 0.5f) << "degrees of knee per mm of target";
    const LegSolve atReach = solveLeg(length, 10.0f, Vector3(0, reach, 0), options);
    const LegSolve past = solveLeg(length, 10.0f, Vector3(0, reach * 1.15f, 0), options);
    EXPECT_GE(atReach.bend - past.bend, 5.0f) << "past the reach the knee still straightens";
    EXPECT_GT(past.bend, 1.0f) << "and never gets to the limit";
}

// ozz's curve has slope 4/3 at its start (1 - (3 / (a + 3))^4, despite its comment): the end speeds
// up by a third where softening starts. Ours is the same family shaped for slope 1.
TEST(AnimatorIK, TwoBoneSoftenFollowsItsCurveWithSlopeOneAtOnset){
    const float length = 0.5f;
    const float reach = 2.0f * length * 0.999f;
    TwoBoneOptions options;
    options.soften = 0.1f;
    const float ds = reach * options.soften;
    const float da = reach - ds;
    EXPECT_NEAR(solveLeg(length, 20.0f, Vector3(0, da - 0.01f, 0), options).end.length(), da - 0.01f, 2e-5f) << "untouched before the onset";
    // the slope just past the onset, and the curve further out
    const float small = ds * 0.02f;
    const float a0 = solveLeg(length, 20.0f, Vector3(0, da, 0), options).end.length();
    const float a1 = solveLeg(length, 20.0f, Vector3(0, da + small, 0), options).end.length();
    EXPECT_NEAR((a1 - a0) / small, 1.0f, 0.04f) << "slope 1 at onset (ozz's 4/3 fails this)";
    for (float a : {0.25f, 1.0f, 4.0f, 16.0f}){
        const float r = 4.0f / (a + 4.0f);
        const float expected = da + ds * (1.0f - r * r * r * r);
        EXPECT_NEAR(solveLeg(length, 20.0f, Vector3(0, da + a * ds, 0), options).end.length(), expected, 2e-5f) << "alpha " << a;
    }
}

TEST(AnimatorIK, TwoBoneZeroSoftenTwistAndAxisAreThePlainSolve){
    const Vector3 target(0.3f, 1.4f, 0.2f);
    const LegSolve plain = solveLeg(1.0f, 15.0f, target, TwoBoneOptions());
    TwoBoneOptions zero;
    zero.soften = 0.0f;
    zero.twist = 0.0f;
    zero.hingeAxis = Vector3(0, 0, 0);
    const LegSolve same = solveLeg(1.0f, 15.0f, target, zero);
    EXPECT_EQ(same.knee, plain.knee);
    EXPECT_EQ(same.end, plain.end);
    // the hard limit stays where it was
    const LegSolve far = solveLeg(1.0f, 15.0f, Vector3(0, 5, 0), TwoBoneOptions());
    EXPECT_NEAR(far.end.length(), 1.998f, 1e-3f);
}

TEST(AnimatorIK, TwoBoneReachedIsMeasuredOnTheFinalPose){
    const TwoBoneOptions plain;
    // on the target
    IKResult r = solveLeg(1.0f, 15.0f, Vector3(0.3f, 1.4f, 0.2f), plain).result;
    EXPECT_TRUE(r.reached);
    EXPECT_LE(r.error, kIKReachTolerance);
    // beyond the chain: the error is the rest of the way
    r = solveLeg(1.0f, 15.0f, Vector3(0, 3, 0), plain).result;
    EXPECT_FALSE(r.reached);
    EXPECT_NEAR(r.error, 3.0f - 1.998f, 1e-3f);
    // a partial weight leaves the end short of a target it could reach
    r = solveLeg(1.0f, 15.0f, Vector3(0.3f, 1.4f, 0.2f), plain, 0.5f).result;
    EXPECT_FALSE(r.reached);
    EXPECT_GT(r.error, 0.01f);
    // weight 0 solves nothing, and says so
    r = solveLeg(1.0f, 15.0f, Vector3(0.3f, 1.4f, 0.2f), plain, 0.0f).result;
    EXPECT_FALSE(r.reached);
    EXPECT_EQ(r.error, 0.0f);
    // closer than the difference of the bones
    Chain unequal(3, 1.0f);
    unequal.local.setPosition(2, Vector3(0, 0.4f, 0));
    unequal.local.setRotation(1, about(Vector3(1, 0, 0), 30.0f));
    refreshRigSpace(unequal.context, 0);
    r = solveTwoBone(unequal.context, 0, 1, 2, Vector3(0, 0.2f, 0), nullptr, nullptr, 1.0f);
    EXPECT_FALSE(r.reached);
    EXPECT_NEAR(r.error, 0.4f, 2e-3f);
    // soft: reached while the eased distance is within the tolerance, not from the onset on (ozz)
    TwoBoneOptions soft;
    soft.soften = 0.1f;
    const float reach = 1.998f;
    const float da = reach * 0.9f;
    r = solveLeg(1.0f, 15.0f, Vector3(0, da - 0.05f, 0), soft).result;
    EXPECT_TRUE(r.reached);
    r = solveLeg(1.0f, 15.0f, Vector3(0, da + 0.005f, 0), soft).result;
    EXPECT_TRUE(r.reached) << "0.005 m past the onset the end is still within 1 mm: " << r.error;
    r = solveLeg(1.0f, 15.0f, Vector3(0, reach, 0), soft).result;
    EXPECT_FALSE(r.reached);
    EXPECT_GT(r.error, 0.01f);
    // the error is in world metres under a scaled rig: 2 mm in a rig of 0.01 is not 1 mm's miss
    Chain scaled(3, 1.0f);
    const Matrix4 world = Matrix4::scaleMatrix(0.01f);
    scaled.context.rigToWorld = &world;
    r = solveTwoBone(scaled.context, 0, 1, 2, Vector3(0, 3, 0), nullptr, nullptr, 1.0f);
    EXPECT_NEAR(r.error, (3.0f - 1.998f) * 0.01f, 1e-4f);
    EXPECT_FALSE(r.reached) << "1 cm off in world";
    r = solveTwoBone(scaled.context, 0, 1, 2, Vector3(0, 2.05f, 0), nullptr, nullptr, 1.0f);
    EXPECT_TRUE(r.reached) << "under 1 mm in world, 5 cm in the rig: " << r.error;
}

// A straight chain has no bend plane; the hinge axis picks the side, and the mid joint turns only
// about it, so its twist survives
TEST(AnimatorIK, TwoBoneHingeAxisBendsAStraightChainToItsSide){
    const Vector3 target(0, 1.2f, 0);
    for (float sign : {1.0f, -1.0f}){
        Chain chain(3, 1.0f);
        TwoBoneOptions options;
        options.hingeAxis = Vector3(0, 0, sign);
        const IKResult r = solveTwoBone(chain.context, 0, 1, 2, target, nullptr, nullptr, 1.0f, options);
        EXPECT_TRUE(r.reached) << r.error;
        EXPECT_LE((chain.positions[2] - target).length(), 0.001f);
        EXPECT_LT(chain.positions[1].x * sign, -0.1f) << "a positive turn opens the joint: the knee on the other side";
        EXPECT_NEAR(chain.positions[1].z, 0.0f, 1e-4f) << "in the plane across the axis";
    }
    // the mid joint's own twist survives: its local change is a turn about the axis alone
    Chain twisted(3, 1.0f);
    const Quaternion before = about(Vector3(0, 1, 0), 30.0f);
    twisted.local.setRotation(1, before);
    refreshRigSpace(twisted.context, 0);
    TwoBoneOptions options;
    options.hingeAxis = Vector3(0, 0, 1);
    EXPECT_TRUE(solveTwoBone(twisted.context, 0, 1, 2, target, nullptr, nullptr, 1.0f, options).reached);
    const Quaternion change = before.inverse() * twisted.local.rotation(1);
    EXPECT_NEAR(change.x, 0.0f, 1e-4f);
    EXPECT_NEAR(change.y, 0.0f, 1e-4f);
    EXPECT_GT(std::fabs(change.z), 0.1f);
}

TEST(AnimatorIK, TwoBoneHingeAxisCorrectsABackwardBendAndKeepsItsSideThroughStraight){
    TwoBoneOptions options;
    options.hingeAxis = Vector3(0, 0, 1);
    // bent the wrong way (+40 about the axis closes the joint past straight)
    Chain back(3, 1.0f);
    back.local.setRotation(1, about(Vector3(0, 0, 1), 40.0f));
    refreshRigSpace(back.context, 0);
    const Vector3 target(0, 1.5f, 0);
    EXPECT_TRUE(solveTwoBone(back.context, 0, 1, 2, target, nullptr, nullptr, 1.0f, options).reached);
    EXPECT_LT(back.positions[1].x, -0.1f);
    // near straight the side does not jump, and the knee moves smoothly
    float previous = 0.0f;
    bool first = true;
    for (float d = 1.9f; d < 2.1f; d += 0.001f){
        Chain chain(3, 1.0f);
        solveTwoBone(chain.context, 0, 1, 2, Vector3(0, d, 0), nullptr, nullptr, 1.0f, options);
        EXPECT_LT(chain.positions[1].x, 0.0f) << "at " << d;
        if (!first){
            EXPECT_LE(std::fabs(chain.positions[1].x - previous), 0.02f) << "at " << d;
        }
        previous = chain.positions[1].x;
        first = false;
    }
}

// A bone off the plane across the axis: the joint's circle is measured, not assumed
TEST(AnimatorIK, TwoBoneHingeAxisReachesWithABoneOffItsPlane){
    Chain chain(3, 1.0f);
    chain.local.setPosition(2, Vector3(0, 1, 0.3f));
    refreshRigSpace(chain.context, 0);
    TwoBoneOptions options;
    options.hingeAxis = Vector3(0, 0, 1);
    const Vector3 target(0.8f, 1.2f, 0.5f);
    const IKResult r = solveTwoBone(chain.context, 0, 1, 2, target, nullptr, nullptr, 1.0f, options);
    EXPECT_TRUE(r.reached) << r.error;
    EXPECT_LE((chain.positions[2] - target).length(), 0.001f);
}

TEST(AnimatorIK, TwoBoneHingeAxisWithPoleAndDegenerateAxis){
    // the pole picks the plane, the axis the side of the joint
    Chain chain(3, 1.0f);
    TwoBoneOptions options;
    options.hingeAxis = Vector3(0, 0, 1);
    const Vector3 target(0, 1.5f, 0);
    const Vector3 pole(0.2f, 0.7f, 2.0f);
    EXPECT_TRUE(solveTwoBone(chain.context, 0, 1, 2, target, &pole, nullptr, 1.0f, options).reached);
    const Vector3 axis = (target - chain.positions[0]).normalized();
    EXPECT_LE(angleBetween(across(chain.positions[1], axis), across(pole, axis)), 0.5f);
    // an axis along a bone defines no plane: as auto
    Chain along(3, 1.0f);
    along.local.setRotation(1, about(Vector3(1, 0, 0), 10.0f));
    refreshRigSpace(along.context, 0);
    TwoBoneOptions degenerate;
    degenerate.hingeAxis = Vector3(0, 1, 0);
    const IKResult r = solveTwoBone(along.context, 0, 1, 2, target, nullptr, nullptr, 1.0f, degenerate);
    EXPECT_TRUE(r.reached) << r.error;
    expectFinitePose(along);
    // non-uniform scale: the passes still get there
    Chain scaled(3, 1.0f, Vector3(1, 2, 1));
    const Vector3 far(1.5f, 3.0f, 1.0f);
    solveTwoBone(scaled.context, 0, 1, 2, far, nullptr, nullptr, 1.0f, options);
    EXPECT_LE((scaled.positions[2] - far).length(), 0.01f);
}

TEST(AnimatorIK, TwoBoneTwistTurnsTheChainAboutTheTargetLine){
    const Vector3 target(0.5f, 1.2f, 0.3f);
    const Vector3 pole(0, 1, 2);
    Chain plain(3, 1.0f);
    plain.local.setRotation(1, about(Vector3(1, 0, 0), 2.0f));
    refreshRigSpace(plain.context, 0);
    solveTwoBone(plain.context, 0, 1, 2, target, &pole, nullptr, 1.0f);
    for (float twist : {90.0f, -35.0f}){
        Chain chain(3, 1.0f);
        chain.local.setRotation(1, about(Vector3(1, 0, 0), 2.0f));
        refreshRigSpace(chain.context, 0);
        TwoBoneOptions options;
        options.twist = twist;
        const IKResult r = solveTwoBone(chain.context, 0, 1, 2, target, &pole, nullptr, 1.0f, options);
        EXPECT_TRUE(r.reached) << r.error;
        EXPECT_LE((chain.positions[2] - target).length(), 0.001f);
        EXPECT_NEAR((chain.positions[1] - chain.positions[0]).length(), 1.0f, 1e-4f);
        EXPECT_NEAR((chain.positions[2] - chain.positions[1]).length(), 1.0f, 1e-4f);
        EXPECT_NEAR(signedAngle(across(plain.positions[1], target), across(chain.positions[1], target), target), twist, 0.1f);
    }
}

// ozz's aim job lerps the twisted rotation unfixed when weighted (ik_aim_job.cc:202). Roll toward
// the pole +170 plus twist +170 is a -20 degree turn the short way; half of it is -10, not +170.
TEST(AnimatorIK, TwoBoneWeightedTwistTakesTheShortWay){
    Chain chain(3, 1.0f);
    chain.local.setRotation(1, about(Vector3(1, 0, 0), 60.0f));
    refreshRigSpace(chain.context, 0);
    // the end is on the target and the joint open as it should be: only the roll and twist remain
    const Vector3 target = chain.positions[2];
    const Vector3 axis = target.normalized();
    const Vector3 knee = across(chain.positions[1], axis);
    const Vector3 pole = axis + about(axis, 170.0f) * knee;
    TwoBoneOptions options;
    options.hingeAxis = Vector3(-1, 0, 0);
    options.twist = 170.0f;
    solveTwoBone(chain.context, 0, 1, 2, target, &pole, nullptr, 0.5f, options);
    EXPECT_NEAR(signedAngle(knee, across(chain.positions[1], axis), axis), -10.0f, 0.5f);
}

// ---- S77: the edge cases of ozz's two-bone tests (ZeroLength, Aligned, PoleTargetAlignment, ZeroScale)

TEST(AnimatorIK, TwoBoneZeroLengthTargetAndBones){
    TwoBoneOptions options;
    options.hingeAxis = Vector3(0, 0, 1);
    options.soften = 0.1f;
    options.twist = 20.0f;
    for (const TwoBoneOptions& o : {TwoBoneOptions(), options}){
        // the target on the upper joint: no direction to reach along
        Chain atRoot(3, 1.0f);
        atRoot.local.setRotation(1, about(Vector3(1, 0, 0), 30.0f));
        refreshRigSpace(atRoot.context, 0);
        const IKResult r = solveTwoBone(atRoot.context, 0, 1, 2, atRoot.positions[0], nullptr, nullptr, 1.0f, o);
        expectFinitePose(atRoot);
        EXPECT_EQ(r.reached, r.error <= kIKReachTolerance);
        // a bone of no length: nothing can move the end
        Chain noUpper(3, 1.0f);
        noUpper.local.setPosition(1, Vector3(0, 0, 0));
        refreshRigSpace(noUpper.context, 0);
        EXPECT_FALSE(solveTwoBone(noUpper.context, 0, 1, 2, Vector3(1, 1, 0), nullptr, nullptr, 1.0f, o).reached);
        expectFinitePose(noUpper);
        Chain noLower(3, 1.0f);
        noLower.local.setPosition(2, Vector3(0, 0, 0));
        refreshRigSpace(noLower.context, 0);
        EXPECT_FALSE(solveTwoBone(noLower.context, 0, 1, 2, Vector3(1, 1, 0), nullptr, nullptr, 1.0f, o).reached);
        expectFinitePose(noLower);
    }
}

TEST(AnimatorIK, TwoBoneAlignedJointsAndTarget){
    // a straight chain, a target on its line
    Chain chain(3, 1.0f);
    const Vector3 target(0, 1.5f, 0);
    const IKResult r = solveTwoBone(chain.context, 0, 1, 2, target, nullptr, nullptr, 1.0f);
    EXPECT_TRUE(r.reached) << r.error;
    EXPECT_LE((chain.positions[2] - target).length(), 0.001f);
    expectFinitePose(chain);
    // ... with a pole on the same line: no plane from either
    const Vector3 pole(0, 3, 0);
    Chain aligned(3, 1.0f);
    EXPECT_TRUE(solveTwoBone(aligned.context, 0, 1, 2, target, &pole, nullptr, 1.0f).reached);
    expectFinitePose(aligned);
}

TEST(AnimatorIK, TwoBonePoleAlignedWithTheTarget){
    TwoBoneOptions hinged;
    hinged.hingeAxis = Vector3(0, 0, 1);
    for (const TwoBoneOptions& options : {TwoBoneOptions(), hinged}){
        Chain chain(3, 1.0f);
        chain.local.setRotation(1, about(Vector3(0, 0, 1), -40.0f));
        refreshRigSpace(chain.context, 0);
        const Vector3 target(1.0f, 0.5f, 0);
        const Vector3 pole = target * 2.0f;
        EXPECT_TRUE(solveTwoBone(chain.context, 0, 1, 2, target, &pole, nullptr, 1.0f, options).reached);
        expectFinitePose(chain);
        EXPECT_LE((chain.positions[2] - target).length(), 0.001f);
    }
}

TEST(AnimatorIK, TwoBoneZeroScale){
    TwoBoneOptions hinged;
    hinged.hingeAxis = Vector3(0, 0, 1);
    hinged.twist = 15.0f;
    for (const TwoBoneOptions& options : {TwoBoneOptions(), hinged}){
        for (size_t scaled = 0; scaled < 3; scaled++){
            Chain chain(3, 1.0f);
            chain.local.setRotation(1, about(Vector3(1, 0, 0), 20.0f));
            chain.local.setScale(scaled, Vector3(0, 0, 0));
            refreshRigSpace(chain.context, 0);
            const IKResult r = solveTwoBone(chain.context, 0, 1, 2, Vector3(0.5f, 1, 0.5f), nullptr, nullptr, 1.0f, options);
            expectFinitePose(chain);
            EXPECT_TRUE(std::isfinite(r.error));
        }
    }
}

TEST(AnimatorIK, TwoBoneNonFiniteInputsLeaveAFinitePose){
    const float nan = std::nanf("");
    TwoBoneOptions soft;
    soft.soften = nan;
    soft.twist = nan;
    soft.hingeAxis = Vector3(nan, 0, 0);
    Chain chain(3, 1.0f);
    chain.local.setRotation(1, about(Vector3(1, 0, 0), 20.0f));
    refreshRigSpace(chain.context, 0);
    IKResult r = solveTwoBone(chain.context, 0, 1, 2, Vector3(0.5f, 1, 0.5f), nullptr, nullptr, 1.0f, soft);
    expectFinitePose(chain);
    EXPECT_TRUE(r.reached) << "non-finite options read as off";
    r = solveTwoBone(chain.context, 0, 1, 2, Vector3(nan, 1, 0), nullptr, nullptr, 1.0f);
    expectFinitePose(chain);
    EXPECT_FALSE(r.reached);
}

// ---- S77 R33: the look-at chain walk

// The red case. An upright chain yawing cannot fail (the axis passes through the head); a chain
// leaning forward can: the parents carry the head off the line the turn was measured from.
TEST(AnimatorIK, LookAtForwardLeaningChainHitsItsTarget){
    const Vector3 forward(0, 0, 1);
    const struct { std::vector<float> lengths; std::vector<float> weights; } chains[] = {
        {{0.18f, 0.08f}, {0.2f, 0.3f, 0.5f}},   // the benchmark's neck
        {{0.3f, 0.3f}, {1.0f, 1.0f, 1.0f}},
        {{0.5f}, {0.5f, 0.5f}},                 // a torso-length lever
    };
    for (const auto& shape : chains){
        const int count = (int)shape.weights.size();
        for (float distance : {1.0f, 2.0f}){
            for (float pitch : {0.0f, 15.0f, 30.0f, 45.0f, 60.0f, -40.0f}){
                for (float yaw : {0.0f, 15.0f, 60.0f, -30.0f}){
                    Chain chain(count, 0.3f);
                    leanForward(chain, 30.0f, shape.lengths);
                    const Vector3 target = targetOff(chain, count - 1, forward, yaw, pitch, distance);
                    const IKResult r = solveLookAt(chain.context, kLookBones, shape.weights.data(), count, forward, target, 180.0f, 1.0f);
                    EXPECT_LE(aimError(chain, count - 1, forward, Vector3(0, 0, 0), target), kIKAimTolerance)
                        << count << " bones, yaw " << yaw << ", pitch " << pitch << ", at " << distance << " m";
                    EXPECT_TRUE(r.reached) << "yaw " << yaw << ", pitch " << pitch;
                }
            }
        }
    }
}

// The eyes sit off the last bone: their forward ray goes through the target, not the bone's
TEST(AnimatorIK, LookAtAimOffsetPutsTheEyesOnTheTarget){
    const Vector3 forward(0, 0, 1);
    const Vector3 offset(0.03f, 0.1f, 0.08f);
    const float weights[3] = {0.2f, 0.3f, 0.5f};
    for (float pitch : {0.0f, 30.0f, -45.0f}){
        for (float yaw : {0.0f, 45.0f, -60.0f}){
            Chain chain(3, 0.2f);
            leanForward(chain, 25.0f, {0.18f, 0.08f});
            const Vector3 target = targetOff(chain, 2, forward, yaw, pitch, 1.0f);
            const IKResult r = solveLookAt(chain.context, kLookBones, weights, 3, forward, target, 180.0f, 1.0f, offset);
            EXPECT_LE(aimError(chain, 2, forward, offset, target), kIKAimTolerance) << "yaw " << yaw << ", pitch " << pitch;
            EXPECT_TRUE(r.reached);
        }
    }
    // none: the bone's own forward, as without the argument
    Chain plain(3, 0.2f);
    Chain zero(3, 0.2f);
    const Vector3 target(0.4f, 0.8f, 1.0f);
    solveLookAt(plain.context, kLookBones, weights, 3, forward, target, 180.0f, 1.0f);
    solveLookAt(zero.context, kLookBones, weights, 3, forward, target, 180.0f, 1.0f, Vector3(0, 0, 0));
    EXPECT_EQ(plain.rotations[2], zero.rotations[2]);
}

// Each joint keeps the share its weight gave it: on an upright yaw the shares are exact angles
TEST(AnimatorIK, LookAtKeepsTheEngineShares){
    const Vector3 forward(0, 0, 1);
    const float weights[3] = {0.2f, 0.3f, 0.5f};
    Chain chain(3, 0.3f);
    const float yaw = 50.0f;
    const Vector3 target = targetOff(chain, 2, forward, yaw, 0.0f, 3.0f);
    solveLookAt(chain.context, kLookBones, weights, 3, forward, target, 180.0f, 1.0f);
    const float degrees[3] = {yaw * 0.2f, yaw * 0.3f, yaw * 0.5f};
    for (size_t i = 0; i < 3; i++){
        const Quaternion q = chain.local.rotation(i);
        EXPECT_NEAR(2.0f * std::atan2(q.y, q.w) * 57.29577951f, degrees[i], 0.1f) << "bone " << i;
    }
    // a last bone with no weight stays where the animation had it; its parents do all the work
    const float noHead[3] = {1.0f, 1.0f, 0.0f};
    Chain still(3, 0.3f);
    leanForward(still, 20.0f, {0.3f, 0.3f});
    const Quaternion head = still.local.rotation(2);
    const Vector3 aim = targetOff(still, 2, forward, 30.0f, 10.0f, 1.0f);
    EXPECT_TRUE(solveLookAt(still.context, kLookBones, noHead, 3, forward, aim, 180.0f, 1.0f).reached);
    EXPECT_EQ(still.local.rotation(2), head);
    EXPECT_LE(aimError(still, 2, forward, Vector3(0, 0, 0), aim), kIKAimTolerance);
    // no weight anywhere: nothing turns
    const float none[2] = {0.0f, 0.0f};
    Chain idle(2, 0.3f);
    EXPECT_FALSE(solveLookAt(idle.context, kLookBones, none, 2, forward, aim, 180.0f, 1.0f).reached);
    EXPECT_EQ(idle.local.rotation(1), Quaternion());
}

TEST(AnimatorIK, LookAtClampHoldsOnALeaningChain){
    const Vector3 forward(0, 0, 1);
    const Vector3 offset(0.02f, 0.1f, 0.08f);
    const float weights[3] = {0.5f, 0.3f, 0.2f};
    for (float clamp : {0.0f, 20.0f, 45.0f}){
        for (float yaw : {60.0f, 120.0f, 175.0f, -90.0f}){
            Chain chain(3, 0.3f);
            leanForward(chain, 30.0f, {0.3f, 0.3f});
            const Vector3 before = chain.rotations[2] * forward;
            const Vector3 target = targetOff(chain, 2, forward, yaw, 20.0f, 1.0f);
            const IKResult r = solveLookAt(chain.context, kLookBones, weights, 3, forward, target, clamp, 1.0f, offset);
            const float turned = angleBetween(before, chain.rotations[2] * forward);
            EXPECT_LE(turned, clamp + 0.05f) << "clamp " << clamp << ", yaw " << yaw;
            EXPECT_GE(turned, clamp - 0.05f) << "the whole clamp is used";
            EXPECT_FALSE(r.reached) << "a clamped aim misses";
            EXPECT_GT(r.error, 0.01f);
        }
    }
    // a target in range does not feel the clamp
    Chain chain(3, 0.3f);
    leanForward(chain, 30.0f, {0.3f, 0.3f});
    const Vector3 target = targetOff(chain, 2, forward, 20.0f, 0.0f, 1.0f);
    EXPECT_TRUE(solveLookAt(chain.context, kLookBones, weights, 3, forward, target, 60.0f, 1.0f).reached);
}

// Weight is a part of the turn along its arc, the short way (the fix-up ozz skips at
// ik_aim_job.cc:202): whatever the sign the incoming quaternion carries.
TEST(AnimatorIK, LookAtWeightBlendsTheShortWay){
    const Vector3 forward(0, 0, 1);
    const float weights[1] = {1.0f};
    for (float sign : {1.0f, -1.0f}){
        for (float weight : {0.5f, 0.25f}){
            Chain chain(1, 0.3f);
            const Quaternion incoming = about(Vector3(0, 1, 0), 20.0f);
            chain.local.setRotation(0, Quaternion(incoming.w * sign, incoming.x * sign, incoming.y * sign, incoming.z * sign));
            refreshRigSpace(chain.context, 0);
            const Vector3 before = chain.rotations[0] * forward;
            const Vector3 target = chain.positions[0] + about(Vector3(0, 1, 0), 170.0f) * before * 3.0f;
            const IKResult r = solveLookAt(chain.context, kLookBones, weights, 1, forward, target, 180.0f, weight);
            EXPECT_FALSE(r.reached);
            EXPECT_NEAR(signedAngle(before, chain.rotations[0] * forward, Vector3(0, 1, 0)), 170.0f * weight, 0.1f);
        }
    }
    // weight 0: nothing moves, nothing reports
    Chain still(1, 0.3f);
    const Quaternion unchanged = still.local.rotation(0);
    const IKResult r = solveLookAt(still.context, kLookBones, weights, 1, forward, Vector3(5, 0, 0), 180.0f, 0.0f);
    EXPECT_EQ(still.local.rotation(0), unchanged);
    EXPECT_FALSE(r.reached);
    EXPECT_EQ(r.error, 0.0f);
}

// ---- S77: the edge cases of ozz's aim tests (TargetTooClose, Aligned, Forward, ZeroScale)

TEST(AnimatorIK, LookAtEdgeCases){
    const Vector3 forward(0, 0, 1);
    const float weights[2] = {1.0f, 1.0f};
    // a target closer to the head than the eyes' distance across their ray: no ray reaches it
    {
        Chain chain(2, 0.3f);
        const Vector3 offset(0.5f, 0, 0);
        const IKResult r = solveLookAt(chain.context, kLookBones, weights, 2, forward, chain.positions[1] + Vector3(0, 0, 0.1f), 180.0f, 1.0f, offset);
        expectFinitePose(chain);
        EXPECT_FALSE(r.reached);
    }
    // the target on the eyes: no direction
    {
        Chain chain(2, 0.3f);
        const Quaternion unchanged = chain.local.rotation(1);
        const IKResult r = solveLookAt(chain.context, kLookBones, weights, 2, forward, chain.positions[1], 180.0f, 1.0f);
        EXPECT_FALSE(r.reached);
        EXPECT_EQ(chain.local.rotation(1), unchanged);
    }
    // straight behind: an arbitrary half turn, then on target
    {
        Chain chain(2, 0.3f);
        const Vector3 target = chain.positions[1] - forward * 2.0f;
        EXPECT_TRUE(solveLookAt(chain.context, kLookBones, weights, 2, forward, target, 180.0f, 1.0f).reached);
        expectFinitePose(chain);
    }
    // already aimed: nothing turns
    {
        Chain chain(2, 0.3f);
        const IKResult r = solveLookAt(chain.context, kLookBones, weights, 2, forward, chain.positions[1] + forward * 2.0f, 180.0f, 1.0f);
        EXPECT_TRUE(r.reached);
        EXPECT_NEAR(std::fabs(chain.local.rotation(1).w), 1.0f, 1e-6f);
    }
    // a forward axis of any length; none at all turns nothing
    {
        Chain unit(2, 0.3f);
        Chain longer(2, 0.3f);
        const Vector3 target(1, 1, 1);
        solveLookAt(unit.context, kLookBones, weights, 2, forward, target, 180.0f, 1.0f);
        solveLookAt(longer.context, kLookBones, weights, 2, forward * 2.0f, target, 180.0f, 1.0f);
        EXPECT_NEAR(angleBetween(unit.rotations[1] * forward, longer.rotations[1] * forward), 0.0f, 0.05f);
        Chain none(2, 0.3f);
        EXPECT_FALSE(solveLookAt(none.context, kLookBones, weights, 2, Vector3(0, 0, 0), target, 180.0f, 1.0f).reached);
        EXPECT_EQ(none.local.rotation(1), Quaternion());
    }
    // zero scale on any bone
    for (size_t scaled = 0; scaled < 2; scaled++){
        Chain chain(2, 0.3f);
        chain.local.setScale(scaled, Vector3(0, 0, 0));
        refreshRigSpace(chain.context, 0);
        const IKResult r = solveLookAt(chain.context, kLookBones, weights, 2, forward, Vector3(1, 1, 1), 180.0f, 1.0f, Vector3(0, 0.1f, 0));
        expectFinitePose(chain);
        EXPECT_TRUE(std::isfinite(r.error));
    }
    // a non-finite target
    {
        Chain chain(2, 0.3f);
        const IKResult r = solveLookAt(chain.context, kLookBones, weights, 2, forward, Vector3(std::nanf(""), 0, 0), 180.0f, 1.0f);
        expectFinitePose(chain);
        EXPECT_FALSE(r.reached);
    }
    // non-uniform parent scale: the turn is found in the parent's space
    {
        Chain chain(2, 0.3f, Vector3(1, 2, 1));
        const Vector3 target(0.5f, 1.5f, 1.0f);
        solveLookAt(chain.context, kLookBones, weights, 2, forward, target, 180.0f, 1.0f);
        expectFinitePose(chain);
    }
}

// The aim's outcome reads in world metres like a chain's: the target's distance off the aim ray
TEST(AnimatorIK, LookAtReachedReportsTheMissAcrossTheRay){
    const Vector3 forward(0, 0, 1);
    const float weights[1] = {1.0f};
    Chain chain(1, 0.3f);
    // 30 degrees off, clamped to 10: 20 degrees short of a target 2 m away
    const Vector3 target = chain.positions[0] + about(Vector3(0, 1, 0), 30.0f) * forward * 2.0f;
    const IKResult r = solveLookAt(chain.context, kLookBones, weights, 1, forward, target, 10.0f, 1.0f);
    EXPECT_FALSE(r.reached);
    EXPECT_NEAR(r.error, 2.0f * std::sin(20.0f * 0.0174532925f), 1e-3f);
}

// ---- S77 R34: the ankle's height on a slope

TEST(AnimatorIK, FootAnkleHeightKeepsTheSoleOnTheSlope){
    const float h = 0.08f;
    auto normal = [](float degrees){ return about(Vector3(1, 0, 0), degrees) * Vector3(0, 1, 0); };
    auto cosd = [](float degrees){ return std::cos(degrees * 0.0174532925f); };
    EXPECT_NEAR(footAnkleHeight(h, Vector3(0, 1, 0), 30.0f), h, 1e-6f) << "flat ground: nothing changes";
    EXPECT_NEAR(footAnkleHeight(h, normal(20.0f), 30.0f), h / cosd(20.0f), 1e-5f);
    EXPECT_NEAR(footAnkleHeight(h, normal(30.0f), 30.0f), h / cosd(30.0f), 1e-5f);
    // past the clamp the foot turns 30 degrees only: the centre of the sole is what stays on it
    EXPECT_NEAR(footAnkleHeight(h, normal(45.0f), 30.0f), h * cosd(15.0f) / cosd(45.0f), 1e-5f);
    EXPECT_NEAR(footAnkleHeight(h, normal(-30.0f), 30.0f), h / cosd(30.0f), 1e-5f) << "either way of a slope";
    // continuous across the clamp
    EXPECT_NEAR(footAnkleHeight(h, normal(30.01f), 30.0f), footAnkleHeight(h, normal(29.99f), 30.0f), 1e-4f);
    // no turn allowed: the foot stays flat with its centre on the plane
    EXPECT_NEAR(footAnkleHeight(h, normal(25.0f), 0.0f), h, 1e-5f);
    // a clamp beyond 60 degrees holds up to that clamp, a wall stays bounded, junk reads as flat
    EXPECT_NEAR(footAnkleHeight(h, normal(70.0f), 75.0f), h / cosd(70.0f), 1e-4f);
    EXPECT_LE(footAnkleHeight(h, normal(89.0f), 30.0f), 4.0f * h);
    EXPECT_LE(footAnkleHeight(h, normal(90.0f), 90.0f), 25.0f * h);
    EXPECT_LE(footAnkleHeight(h, normal(120.0f), 30.0f), 4.0f * h);
    EXPECT_GE(footAnkleHeight(h, normal(120.0f), 30.0f), 0.0f);
    EXPECT_NEAR(footAnkleHeight(h, Vector3(std::nanf(""), std::nanf(""), 0), 30.0f), h, 1e-6f);
}
