// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// particles/ParticleShapes (particles spec 6.3.1, 9): 10,000 seeded samples per shape land in
// its volume, on its surface (thickness 0) or edges, with the expected radial distribution;
// cone directions tilt by the distance from the axis; arc limits and the Loop / PingPong /
// BurstSpread sequences; random direction amount 1 is uniform on the sphere; the shape's own
// transform applies.

#include <gtest/gtest.h>

#include "particles/ParticleShapes.h"
#include "particles/ParticleSimulation.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr int SAMPLES = 10000;
    constexpr float DEG = 3.14159265f / 180.0f;

    struct Rng {
        uint64_t state = 0, inc = 0;
        explicit Rng(uint64_t seed) { pcgSeed(state, inc, seed, 54u); }
        Pcg32 get() { return Pcg32(state, inc); }
    };

    ParticleShapeModule shapeOf(ParticleSystemShapeType type) {
        ParticleShapeModule shape;
        shape.shapeType = type;
        return shape;
    }

    float azimuthDegrees(const Vector3& p) {
        float a = std::atan2(p.z, p.x) / DEG;
        if (a < 0.0f) a += 360.0f;
        return a;
    }

}

TEST(ParticleShape, SphereSurfaceAndVolume) {
    Rng rng(1);
    Pcg32 r = rng.get();
    ParticleShapeModule shape = shapeOf(ParticleSystemShapeType::Sphere);
    shape.radius = 2.0f;
    shape.radiusThickness = 0.0f;
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(shape, ShapeContext(), r);
        ASSERT_NEAR(s.position.length(), 2.0f, 1e-4f);
        ASSERT_NEAR(s.direction.dotProduct(s.position / 2.0f), 1.0f, 1e-4f) << "outward";
    }
    // the whole volume: uniform, so the share inside half the radius is (1/2)^3
    shape.radiusThickness = 1.0f;
    int inside = 0;
    for (int i = 0; i < SAMPLES; i++) {
        const float d = sampleShape(shape, ShapeContext(), r).position.length();
        ASSERT_LE(d, 2.0f + 1e-4f);
        if (d < 1.0f) inside++;
    }
    EXPECT_NEAR((float)inside / SAMPLES, 0.125f, 0.02f);
    // thickness 0.5: nothing inside the inner radius
    shape.radiusThickness = 0.5f;
    for (int i = 0; i < SAMPLES; i++) {
        ASSERT_GE(sampleShape(shape, ShapeContext(), r).position.length(), 1.0f - 1e-4f);
    }
}

TEST(ParticleShape, HemisphereStaysAbove) {
    Rng rng(2);
    Pcg32 r = rng.get();
    ParticleShapeModule shape = shapeOf(ParticleSystemShapeType::Hemisphere);
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(shape, ShapeContext(), r);
        ASSERT_GE(s.position.y, -1e-5f);
        ASSERT_GE(s.direction.y, -1e-5f);
    }
}

TEST(ParticleShape, ConeDirectionsTiltWithTheDistanceFromTheAxis) {
    Rng rng(3);
    Pcg32 r = rng.get();
    ParticleShapeModule shape = shapeOf(ParticleSystemShapeType::Cone);
    shape.radius = 1.0f;
    shape.angle = 30.0f;
    shape.radiusThickness = 0.0f;
    // on the rim: every direction leaves at the cone angle
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(shape, ShapeContext(), r);
        ASSERT_NEAR(s.position.y, 0.0f, 1e-5f);
        ASSERT_NEAR(Vector3(s.position.x, 0, s.position.z).length(), 1.0f, 1e-4f);
        ASSERT_NEAR(std::acos(std::min(1.0f, s.direction.y)) / DEG, 30.0f, 0.5f);
        // the tilt points outward
        ASSERT_GT(Vector3(s.direction.x, 0, s.direction.z).dotProduct(s.position), 0.0f);
    }
    // the whole base: the centre goes straight up, the area is uniform
    shape.radiusThickness = 1.0f;
    int centre = 0, inner = 0;
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(shape, ShapeContext(), r);
        const float d = Vector3(s.position.x, 0, s.position.z).length();
        const float tilt = std::acos(std::min(1.0f, s.direction.y)) / DEG;
        ASSERT_NEAR(tilt, 30.0f * d, 0.5f);
        if (d < 0.02f) {
            centre++;
            ASSERT_LT(tilt, 0.7f);
        }
        if (d < 0.5f) inner++;
    }
    EXPECT_GT(centre, 0);
    EXPECT_NEAR((float)inner / SAMPLES, 0.25f, 0.02f) << "area sampling";
}

TEST(ParticleShape, PointConeAndConeVolume) {
    Rng rng(4);
    Pcg32 r = rng.get();
    ParticleShapeModule shape = shapeOf(ParticleSystemShapeType::Cone);
    shape.radius = 0.0f;
    shape.angle = 20.0f;
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(shape, ShapeContext(), r);
        ASSERT_NEAR(s.position.length(), 0.0f, 1e-6f);
        ASSERT_LE(std::acos(std::min(1.0f, s.direction.y)) / DEG, 20.0f + 1e-3f);
    }
    shape.shapeType = ParticleSystemShapeType::ConeVolume;
    shape.length = 3.0f;
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(shape, ShapeContext(), r);
        // on its ray from the apex, no farther than the length
        const float along = s.position.dotProduct(s.direction);
        ASSERT_GE(along, -1e-5f);
        ASSERT_LE(along, 3.0f + 1e-4f);
        ASSERT_NEAR((s.position - s.direction * along).length(), 0.0f, 1e-4f);
    }
}

TEST(ParticleShape, BoxShellAndEdge) {
    Rng rng(5);
    Pcg32 r = rng.get();
    ParticleShapeModule shape = shapeOf(ParticleSystemShapeType::Box);
    shape.scale = Vector3(2.0f, 4.0f, 6.0f);
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(shape, ShapeContext(), r);
        ASSERT_LE(std::fabs(s.position.x), 1.0f + 1e-5f);
        ASSERT_LE(std::fabs(s.position.y), 2.0f + 1e-5f);
        ASSERT_LE(std::fabs(s.position.z), 3.0f + 1e-5f);
        ASSERT_NEAR(s.direction.y, 1.0f, 1e-6f);
    }
    shape.shapeType = ParticleSystemShapeType::BoxShell;
    for (int i = 0; i < SAMPLES; i++) {
        const Vector3 p = sampleShape(shape, ShapeContext(), r).position;
        const int onFace = (std::fabs(std::fabs(p.x) - 1.0f) < 1e-4f) + (std::fabs(std::fabs(p.y) - 2.0f) < 1e-4f) + (std::fabs(std::fabs(p.z) - 3.0f) < 1e-4f);
        ASSERT_GE(onFace, 1) << p.x << " " << p.y << " " << p.z;
    }
    shape.shapeType = ParticleSystemShapeType::BoxEdge;
    for (int i = 0; i < SAMPLES; i++) {
        const Vector3 p = sampleShape(shape, ShapeContext(), r).position;
        const int onFace = (std::fabs(std::fabs(p.x) - 1.0f) < 1e-4f) + (std::fabs(std::fabs(p.y) - 2.0f) < 1e-4f) + (std::fabs(std::fabs(p.z) - 3.0f) < 1e-4f);
        ASSERT_GE(onFace, 2) << "an edge is on two faces";
    }
}

TEST(ParticleShape, CircleEdgeRectangleDonut) {
    Rng rng(6);
    Pcg32 r = rng.get();
    ParticleShapeModule circle = shapeOf(ParticleSystemShapeType::Circle);
    circle.radius = 1.5f;
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(circle, ShapeContext(), r);
        ASSERT_NEAR(s.position.y, 0.0f, 1e-6f);
        ASSERT_LE(s.position.length(), 1.5f + 1e-4f);
        ASSERT_NEAR(s.direction.y, 0.0f, 1e-6f) << "radial in the plane";
    }
    ParticleShapeModule edge = shapeOf(ParticleSystemShapeType::Edge);
    edge.radius = 2.0f;
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(edge, ShapeContext(), r);
        ASSERT_LE(std::fabs(s.position.x), 2.0f + 1e-5f);
        ASSERT_NEAR(s.position.y, 0.0f, 1e-6f);
        ASSERT_NEAR(s.position.z, 0.0f, 1e-6f);
        ASSERT_NEAR(s.direction.y, 1.0f, 1e-6f);
    }
    ParticleShapeModule rect = shapeOf(ParticleSystemShapeType::Rectangle);
    rect.scale = Vector3(4.0f, 1.0f, 2.0f);
    for (int i = 0; i < SAMPLES; i++) {
        const Vector3 p = sampleShape(rect, ShapeContext(), r).position;
        ASSERT_LE(std::fabs(p.x), 2.0f + 1e-5f);
        ASSERT_NEAR(p.y, 0.0f, 1e-6f);
        ASSERT_LE(std::fabs(p.z), 1.0f + 1e-5f);
    }
    ParticleShapeModule donut = shapeOf(ParticleSystemShapeType::Donut);
    donut.radius = 2.0f;
    donut.donutRadius = 0.25f;
    for (int i = 0; i < SAMPLES; i++) {
        const ShapeSample s = sampleShape(donut, ShapeContext(), r);
        const Vector3 ringDir = Vector3(s.position.x, 0, s.position.z).normalize();
        const Vector3 ring = ringDir * 2.0f;
        ASSERT_LE((s.position - ring).length(), 0.25f + 1e-4f);
    }
}

TEST(ParticleShape, ArcLimitsAndModes) {
    Rng rng(7);
    Pcg32 r = rng.get();
    ParticleShapeModule shape = shapeOf(ParticleSystemShapeType::Circle);
    shape.radiusThickness = 0.0f;
    shape.arc = 90.0f;
    for (int i = 0; i < SAMPLES; i++) {
        const float a = azimuthDegrees(sampleShape(shape, ShapeContext(), r).position);
        ASSERT_LE(a, 90.0f + 0.01f);
    }
    // Loop: the angle advances with the phase and wraps at the arc
    shape.arcMode = ParticleSystemShapeMultiModeValue::Loop;
    ShapeContext context;
    context.arcPhase = 45.0f;
    EXPECT_NEAR(azimuthDegrees(sampleShape(shape, context, r).position), 45.0f, 0.01f);
    context.arcPhase = 135.0f;
    EXPECT_NEAR(azimuthDegrees(sampleShape(shape, context, r).position), 45.0f, 0.01f);
    // PingPong: back from the end
    shape.arcMode = ParticleSystemShapeMultiModeValue::PingPong;
    context.arcPhase = 135.0f;
    EXPECT_NEAR(azimuthDegrees(sampleShape(shape, context, r).position), 45.0f, 0.01f);
    context.arcPhase = 150.0f;
    EXPECT_NEAR(azimuthDegrees(sampleShape(shape, context, r).position), 30.0f, 0.01f);
    // BurstSpread: evenly within the burst
    shape.arc = 360.0f;
    shape.arcMode = ParticleSystemShapeMultiModeValue::BurstSpread;
    context.spreadCount = 4;
    for (int k = 0; k < 4; k++) {
        context.spreadIndex = k;
        EXPECT_NEAR(azimuthDegrees(sampleShape(shape, context, r).position), 90.0f * k, 0.01f);
    }
    // spread quantizes a random arc
    shape.arcMode = ParticleSystemShapeMultiModeValue::Random;
    shape.arcSpread = 0.25f;
    for (int i = 0; i < 1000; i++) {
        const float a = azimuthDegrees(sampleShape(shape, ShapeContext(), r).position);
        const float steps = a / 90.0f;
        ASSERT_NEAR(steps, std::round(steps), 1e-3f);
    }
}

TEST(ParticleShape, RandomDirectionIsUniformAndTheShapeTransformApplies) {
    Rng rng(8);
    Pcg32 r = rng.get();
    ParticleShapeModule shape = shapeOf(ParticleSystemShapeType::Cone);
    shape.randomDirectionAmount = 1.0f;
    Vector3 sum(0, 0, 0);
    for (int i = 0; i < SAMPLES; i++) sum += sampleShape(shape, ShapeContext(), r).direction;
    EXPECT_LT((sum / (float)SAMPLES).length(), 0.03f);

    // a cone turned 90 degrees around X emits along +Z, moved by the position
    ParticleShapeModule turned = shapeOf(ParticleSystemShapeType::Cone);
    turned.angle = 0.0f;
    turned.radius = 0.0f;
    turned.rotation = Vector3(90.0f, 0.0f, 0.0f);
    turned.position = Vector3(1.0f, 2.0f, 3.0f);
    const ShapeSample s = sampleShape(turned, ShapeContext(), r);
    EXPECT_NEAR(s.direction.z, 1.0f, 1e-5f);
    EXPECT_NEAR((s.position - Vector3(1, 2, 3)).length(), 0.0f, 1e-5f);

    // disabled: the origin, straight up
    ParticleShapeModule off = shapeOf(ParticleSystemShapeType::Sphere);
    off.enabled = false;
    const ShapeSample o = sampleShape(off, ShapeContext(), r);
    EXPECT_EQ(o.position, Vector3(0, 0, 0));
    EXPECT_EQ(o.direction, Vector3(0, 1, 0));
}

TEST(ParticleShape, EulerOrderIsXThenYThenZ) {
    // X 90 then Z 90: +Y -> +Z (X) -> +Z (Z leaves it)
    const Quaternion q = eulerRadians(Vector3(90.0f * DEG, 0.0f, 90.0f * DEG));
    const Vector3 v = q * Vector3(0, 1, 0);
    EXPECT_NEAR(v.z, 1.0f, 1e-5f);
    // and +X -> +X (X) -> +Y (Z)
    const Vector3 w = q * Vector3(1, 0, 0);
    EXPECT_NEAR(w.y, 1.0f, 1e-5f);
}
