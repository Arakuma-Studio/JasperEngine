// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Directional shadow casters that cannot shadow what the camera sees are left out of a cascade
// (RenderSystem::getShadowReceiverPlanes): the planes bound the cascade's receiver slice swept
// toward the light, so a caster outside the view that casts into it is kept and one whose
// shadow falls outside it is dropped.

#include <gtest/gtest.h>

#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Plane.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "subsystem/RenderSystem.h"

#include <cmath>
#include <cstdint>

using namespace jasperengine;

namespace {

    // as RenderSystem's caster test: a box wholly behind one plane is culled
    bool kept(const Plane* planes, int count, const Vector3& center, const Vector3& halfSize) {
        for (int i = 0; i < count; i++) {
            if (planes[i].getSide(center, halfSize) == Plane::NEGATIVE_SIDE)
                return false;
        }
        return true;
    }

    bool insideClip(const Matrix4& viewProjection, const Vector3& point) {
        const Vector4 clip = viewProjection * Vector4(point.x, point.y, point.z, 1.0f);
        return clip.w > 0.0f && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w && std::abs(clip.z) <= clip.w;
    }

    struct Cascade {
        Matrix4 receiver;
        Matrix4 lightProjection;
        Vector3 lightTravel;
        Plane planes[18];
        int count = 0;
    };

    // a camera at the origin looking down -Z (60 degrees, square), the cascade slice 1..10;
    // a 40 x 40 light projection of a 1024 slot (a texel is 0.04 units)
    Cascade makeSetup(const Vector3& lightTravel, float normalBias = 2.0f) {
        Cascade s;
        s.receiver = Matrix4::perspectiveMatrix(1.0471976f, 1.0f, 1.0f, 10.0f);
        s.lightProjection = Matrix4::orthoMatrix(-20.0f, 20.0f, -20.0f, 20.0f, -100.0f, 100.0f);
        s.lightTravel = lightTravel;
        s.count = RenderSystem::getShadowReceiverPlanes(s.receiver, s.lightProjection, 1024, normalBias, lightTravel, s.planes);
        return s;
    }

    uint32_t lcg(uint32_t& state) {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    float uniform(uint32_t& state, float lo, float hi) {
        return lo + (hi - lo) * (float)(lcg(state) >> 8) / 16777216.0f;
    }

}

TEST(ShadowReceiverCulling, KeepsWhatCastsIntoTheViewDropsTheRest) {
    // the light travels straight down
    const Cascade s = makeSetup(Vector3(0.0f, -1.0f, 0.0f));
    ASSERT_GT(s.count, 0);
    ASSERT_LE(s.count, 18);
    const Vector3 half(0.5f, 0.5f, 0.5f);

    // in the view
    EXPECT_TRUE(kept(s.planes, s.count, Vector3(0.0f, 0.0f, -5.0f), half));
    // high above the view, out of it, its shadow falls into it
    EXPECT_TRUE(kept(s.planes, s.count, Vector3(0.0f, 50.0f, -5.0f), half));
    EXPECT_TRUE(kept(s.planes, s.count, Vector3(1.5f, 200.0f, -8.0f), half));
    // below the view: its shadow goes further down
    EXPECT_FALSE(kept(s.planes, s.count, Vector3(0.0f, -50.0f, -5.0f), half));
    // off to the side, above or level
    EXPECT_FALSE(kept(s.planes, s.count, Vector3(50.0f, 0.0f, -5.0f), half));
    EXPECT_FALSE(kept(s.planes, s.count, Vector3(-50.0f, 30.0f, -5.0f), half));
    // behind the camera and past the slice's far end
    EXPECT_FALSE(kept(s.planes, s.count, Vector3(0.0f, 30.0f, 20.0f), half));
    EXPECT_FALSE(kept(s.planes, s.count, Vector3(0.0f, 30.0f, -40.0f), half));
    // a big box reaching into the swept volume is kept
    EXPECT_TRUE(kept(s.planes, s.count, Vector3(30.0f, 30.0f, -5.0f), Vector3(28.0f, 1.0f, 1.0f)));
}

TEST(ShadowReceiverCulling, ObliqueLightKeepsTheCastersUpTheLight) {
    // travelling +X and down: what shadows the view is up and toward -X
    const Cascade s = makeSetup(Vector3(1.0f, -1.0f, 0.0f));
    ASSERT_GT(s.count, 0);
    const Vector3 half(0.5f, 0.5f, 0.5f);
    EXPECT_TRUE(kept(s.planes, s.count, Vector3(-30.0f, 30.0f, -5.0f), half));
    // up and toward +X: its shadow lands at x = 60
    EXPECT_FALSE(kept(s.planes, s.count, Vector3(30.0f, 30.0f, -5.0f), half));
}

TEST(ShadowReceiverCulling, NeverDropsACasterWhoseShadowReachesTheSlice) {
    // random lights and points: a point whose ray along the light enters the slice is kept
    uint32_t state = 12345u;
    int checkedHits = 0;
    for (int light = 0; light < 24; light++) {
        Vector3 travel(uniform(state, -1.0f, 1.0f), uniform(state, -1.0f, -0.05f), uniform(state, -1.0f, 1.0f));
        const Cascade s = makeSetup(travel, uniform(state, 0.0f, 4.0f));
        ASSERT_GT(s.count, 0);
        const Vector3 direction = travel.normalized();
        for (int p = 0; p < 400; p++) {
            const Vector3 point(uniform(state, -40.0f, 40.0f), uniform(state, -10.0f, 60.0f), uniform(state, -40.0f, 20.0f));
            bool hits = false;
            // the slice stays above y = -6: the ray is followed down to there
            for (float t = 0.0f; t < 400.0f && !hits; t += 0.05f){
                const Vector3 along = point + direction * t;
                if (along.y < -6.0f)
                    break;
                hits = insideClip(s.receiver, along);
            }
            if (hits) {
                checkedHits++;
                EXPECT_TRUE(kept(s.planes, s.count, point, Vector3(0.0f, 0.0f, 0.0f)))
                    << "light " << light << " point " << point.x << " " << point.y << " " << point.z;
            }
        }
    }
    EXPECT_GT(checkedHits, 100);
}

TEST(ShadowReceiverCulling, DegenerateInputCullsNothing) {
    Plane planes[18];
    const Matrix4 receiver = Matrix4::perspectiveMatrix(1.0f, 1.0f, 1.0f, 10.0f);
    const Matrix4 lightProjection = Matrix4::orthoMatrix(-20.0f, 20.0f, -20.0f, 20.0f, -100.0f, 100.0f);
    EXPECT_EQ(RenderSystem::getShadowReceiverPlanes(receiver, lightProjection, 1024, 2.0f, Vector3(0.0f, 0.0f, 0.0f), planes), 0);
    Matrix4 broken = receiver;
    broken.set(0, 0, NAN);
    EXPECT_EQ(RenderSystem::getShadowReceiverPlanes(broken, lightProjection, 1024, 2.0f, Vector3(0.0f, -1.0f, 0.0f), planes), 0);
}
