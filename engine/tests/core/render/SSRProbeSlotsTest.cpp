// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The SSR composite's reflection codes and per-view probe slots
// (docs/superpowers/specs/2026-11-04-ssr-probes-design.md, 6.4 and 10.1).

#include "render/SSRProbeSlots.h"

#include "util/Angle.h"

#include <gtest/gtest.h>

#include <set>
#include <vector>

using namespace jasperengine;

namespace{

    std::vector<SSRProbeCandidate> ssrCandidates(std::initializer_list<std::pair<Entity, float>> list){
        std::vector<SSRProbeCandidate> out;
        for (const auto& [probe, coverage] : list)
            out.push_back({probe, coverage});
        return out;
    }

    Matrix4 ssrViewProjection(){
        // looking down -Z from the origin, 90 degrees vertical, square
        return Matrix4::perspectiveMatrix(Angle::degToRad(90.0f), 1.0f, 0.1f, 100.0f) *
               Matrix4::lookAtMatrix(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -1.0f), Vector3(0.0f, 1.0f, 0.0f));
    }

}

TEST(SSRProbeSlots, CodesFitOneByteAndRoundTrip){
    EXPECT_EQ(SSRCode::SKIP, 0);
    EXPECT_EQ(SSRCode::ADD, 1);
    EXPECT_EQ(SSRCode::AMBIENT, 2);
    EXPECT_EQ(SSRCode::SKY, 3);
    EXPECT_EQ(SSRCode::PROBE_NO_SLOT, 4);

    std::set<int> seen;
    for (int slot = 0; slot < SSR_PROBE_SLOTS; slot++){
        for (int step = 1; step <= SSR_PROBE_WEIGHT_STEPS; step++){
            const uint8_t code = SSRCode::probe(slot, (float)step / (float)SSR_PROBE_WEIGHT_STEPS);
            EXPECT_GE(code, 5);
            EXPECT_LE(code, 252);
            EXPECT_TRUE(seen.insert(code).second) << "code " << (int)code << " used twice";
            int decodedSlot = -1, decodedStep = -1;
            ASSERT_TRUE(SSRCode::decodeProbe(code, decodedSlot, decodedStep));
            EXPECT_EQ(decodedSlot, slot);
            EXPECT_EQ(decodedStep, step);
        }
    }
    int slot = 0, step = 0;
    for (int code = 0; code < 5; code++)
        EXPECT_FALSE(SSRCode::decodeProbe((uint8_t)code, slot, step));
    for (int code = 253; code < 256; code++)
        EXPECT_FALSE(SSRCode::decodeProbe((uint8_t)code, slot, step));
}

TEST(SSRProbeSlots, WeightTakesAtLeastOneStep){
    int slot = 0, step = 0;
    ASSERT_TRUE(SSRCode::decodeProbe(SSRCode::probe(2, 0.002f), slot, step));
    EXPECT_EQ(slot, 2);
    EXPECT_EQ(step, 1);
    ASSERT_TRUE(SSRCode::decodeProbe(SSRCode::probe(0, 1.0f), slot, step));
    EXPECT_EQ(step, SSR_PROBE_WEIGHT_STEPS);
    ASSERT_TRUE(SSRCode::decodeProbe(SSRCode::probe(0, 1.7f), slot, step));
    EXPECT_EQ(step, SSR_PROBE_WEIGHT_STEPS);
}

TEST(SSRProbeSlots, LargestCoverageGetsTheSlots){
    SSRProbeAssignment none;
    SSRProbeAssignment out;
    assignSSRProbeSlots(ssrCandidates({{10, 0.05f}, {11, 0.40f}, {12, 0.01f}, {13, 0.20f}, {14, 0.10f}, {15, 0.30f}}), none, out);
    EXPECT_EQ(out.count(), 4);
    EXPECT_GE(out.slotOf(11), 0);
    EXPECT_GE(out.slotOf(15), 0);
    EXPECT_GE(out.slotOf(13), 0);
    EXPECT_GE(out.slotOf(14), 0);
    EXPECT_EQ(out.slotOf(10), -1);
    EXPECT_EQ(out.slotOf(12), -1);
    // the largest first, in order, when nothing held a slot
    EXPECT_EQ(out.slots[0], 11u);
    EXPECT_EQ(out.slots[1], 15u);
    EXPECT_EQ(out.slots[2], 13u);
    EXPECT_EQ(out.slots[3], 14u);

    // the same frame again: every holder keeps its index
    SSRProbeAssignment again;
    assignSSRProbeSlots(ssrCandidates({{14, 0.10f}, {13, 0.20f}, {12, 0.01f}, {11, 0.40f}, {10, 0.05f}, {15, 0.30f}}), out, again);
    for (int s = 0; s < SSR_PROBE_SLOTS; s++)
        EXPECT_EQ(again.slots[s], out.slots[s]);
}

TEST(SSRProbeSlots, ProbesSharingACubemapTakeTwoSlots){
    // two probes of one authored cubemap (Doriax 1ed266ea3): the slot is the probe, not its texture
    SSRProbeAssignment none;
    SSRProbeAssignment out;
    assignSSRProbeSlots(ssrCandidates({{21, 0.2f}, {22, 0.2f}}), none, out);
    EXPECT_EQ(out.count(), 2);
    EXPECT_NE(out.slotOf(21), out.slotOf(22));
}

TEST(SSRProbeSlots, HysteresisKeepsLastFramesHolder){
    SSRProbeAssignment previous;
    previous.slots[0] = 1;
    previous.slots[1] = 2;
    previous.slots[2] = 3;
    previous.slots[3] = 4; // the holder at 0.30

    SSRProbeAssignment out;
    assignSSRProbeSlots(ssrCandidates({{1, 0.9f}, {2, 0.8f}, {3, 0.7f}, {4, 0.30f}, {5, 0.36f}}), previous, out);
    EXPECT_EQ(out.slotOf(4), 3);
    EXPECT_EQ(out.slotOf(5), -1);

    assignSSRProbeSlots(ssrCandidates({{1, 0.9f}, {2, 0.8f}, {3, 0.7f}, {4, 0.30f}, {5, 0.40f}}), previous, out);
    EXPECT_EQ(out.slotOf(4), -1);
    // the newcomer takes the slot that was freed
    EXPECT_EQ(out.slotOf(5), 3);
    EXPECT_EQ(out.slotOf(1), 0);
}

TEST(SSRProbeSlots, TiesBreakByEntity){
    SSRProbeAssignment none;
    SSRProbeAssignment out;
    assignSSRProbeSlots(ssrCandidates({{9, 0.1f}, {7, 0.1f}, {8, 0.1f}, {5, 0.1f}, {6, 0.1f}}), none, out);
    EXPECT_EQ(out.slots[0], 5u);
    EXPECT_EQ(out.slots[1], 6u);
    EXPECT_EQ(out.slots[2], 7u);
    EXPECT_EQ(out.slots[3], 8u);
    EXPECT_EQ(out.slotOf(9), -1);
}

TEST(SSRProbeSlots, ScreenCoverageClampsAndCountsTheEyeInsideAsFull){
    const Matrix4 vp = ssrViewProjection();

    // at distance 10 a 90-degree view spans -10..10: a box from x 0..10, y -10..10 is half of it
    EXPECT_NEAR(screenCoverage(AABB(Vector3(0.0f, -10.0f, -10.0f), Vector3(10.0f, 10.0f, -10.0f)), vp), 0.5f, 0.01f);
    // beside the frustum
    EXPECT_NEAR(screenCoverage(AABB(Vector3(30.0f, -1.0f, -11.0f), Vector3(32.0f, 1.0f, -9.0f)), vp), 0.0f, 1e-6f);
    // around the eye
    EXPECT_NEAR(screenCoverage(AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(1.0f, 1.0f, 1.0f)), vp), 1.0f, 1e-6f);
    // larger than the screen: clamped to all of it
    EXPECT_NEAR(screenCoverage(AABB(Vector3(-100.0f, -100.0f, -10.0f), Vector3(100.0f, 100.0f, -10.0f)), vp), 1.0f, 1e-6f);
}

TEST(SSRProbeSlots, ScreenCoverageClipsBoxesReachingBehindTheEye){
    const Matrix4 vp = ssrViewProjection();

    // beside the camera and reaching behind it: off screen to the right, not the whole screen
    EXPECT_LT(screenCoverage(AABB(Vector3(2.0f, -1.0f, -2.0f), Vector3(4.0f, 1.0f, 2.0f)), vp), 0.05f);
    // a floor under the camera running behind it: the lower part of the screen only
    const float floor = screenCoverage(AABB(Vector3(-5.0f, -2.0f, -10.0f), Vector3(5.0f, -1.0f, 5.0f)), vp);
    EXPECT_GT(floor, 0.1f);
    EXPECT_LT(floor, 0.9f);
    // wholly behind the eye: nothing
    EXPECT_EQ(screenCoverage(AABB(Vector3(-1.0f, -1.0f, 1.0f), Vector3(1.0f, 1.0f, 3.0f)), vp), 0.0f);
}
