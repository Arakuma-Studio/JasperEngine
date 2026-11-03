// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The module fields S20 took over from origin/main's version 2 particle systems
// (docs/specs/S20-particles.md, Upstream absorption): the shape's arc and radius modes and
// Align to Direction, the orbit's offset curves, Limit Velocity's separate axes, Inherit
// Velocity's Current mode, 3D Rotation by Speed, the noise's scroll, a random start delay,
// Random Color, burst counts over the emitter's time, and the Texture Sheet's flips. Ported
// from origin/main's ParticleEmissionTest, ParticleShapeTest, ParticleSimulationTest and
// MinMaxTest: the same behaviours, checked on S20's runtime.

#include "ParticleTestUtil.h"

#include "particles/ParticleRender.h"
#include "memory/Arena.h"
#include "particles/ParticleKernels.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

using namespace particletest;

namespace {

    constexpr float kPi = 3.14159265358979323846f;

    // a still emitter of particles that do not move by themselves, rate `rate` per second
    ParticleEffectDocument stillEmitter(float rate){
        ParticleEffectDocument d;
        d.tickRate = 60;
        d.duration = 10.0f;
        d.simulationSpace = SimulationSpace::World;
        d.maxParticles = 4096;
        d.main.startLifetime = MinMaxCurve(100.0f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.emission.rateOverTime = MinMaxCurve(rate);
        return d;
    }

    float angleOf(float x, float z){
        float a = std::atan2(z, x) * 180.0f / kPi;
        return a < 0.0f ? a + 360.0f : a;
    }

    float wrapDegrees(float a){
        while (a < 0.0f) a += 360.0f;
        while (a >= 360.0f) a -= 360.0f;
        return a;
    }

    // the particles by spawn order
    std::vector<uint32_t> bySpawn(const Sim& sim){
        std::vector<uint32_t> order(sim.count());
        for (uint32_t i = 0; i < sim.count(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b){ return sim.chu(ChSpawnIndex, a) < sim.chu(ChSpawnIndex, b); });
        return order;
    }

}

TEST(ParticleUpstreamModules, LoopArcAdvancesPerParticle){
    // Circle, arc Loop at one turn a second, 10 a second: each particle 36 degrees after the last
    ParticleEffectDocument d = stillEmitter(10.0f);
    d.shape.shape = ShapeType::Circle;
    d.shape.radius = 1.0f;
    d.shape.radiusThickness = 0.0f;
    d.shape.arcMode = ShapeMultiMode::Loop;
    d.shape.arcSpeed = MinMaxCurve(1.0f);
    Sim sim(d);
    sim.ticks(60);
    ASSERT_GE(sim.count(), 9u);
    const std::vector<uint32_t> order = bySpawn(sim);
    for (size_t k = 1; k < order.size(); k++){
        const float a0 = angleOf(sim.ch(ChPosX, order[k - 1]), sim.ch(ChPosZ, order[k - 1]));
        const float a1 = angleOf(sim.ch(ChPosX, order[k]), sim.ch(ChPosZ, order[k]));
        EXPECT_NEAR(wrapDegrees(a1 - a0), 36.0f, 1.0f) << k;
    }
}

TEST(ParticleUpstreamModules, PingPongArcTurnsBack){
    // half an arc (180 degrees) at one arc a second: back and forth, never past the arc
    ParticleEffectDocument d = stillEmitter(30.0f);
    d.shape.shape = ShapeType::Circle;
    d.shape.radiusThickness = 0.0f;
    d.shape.arc = 180.0f;
    d.shape.arcMode = ShapeMultiMode::PingPong;
    d.shape.arcSpeed = MinMaxCurve(0.5f);   // 180 degrees a second
    Sim sim(d);
    sim.ticks(120);
    const std::vector<uint32_t> order = bySpawn(sim);
    float highest = 0.0f;
    bool cameBack = false;
    for (size_t k = 1; k < order.size(); k++){
        const float z = sim.ch(ChPosZ, order[k]);
        EXPECT_GE(z, -1.0e-3f) << "inside the 180-degree arc";
        const float a = angleOf(sim.ch(ChPosX, order[k]), z);
        highest = std::max(highest, a);
        if (a < angleOf(sim.ch(ChPosX, order[k - 1]), sim.ch(ChPosZ, order[k - 1])) - 1.0f)
            cameBack = true;
    }
    EXPECT_GT(highest, 170.0f);
    EXPECT_TRUE(cameBack);
}

TEST(ParticleUpstreamModules, BurstSpreadSpacesABurstEvenly){
    ParticleEffectDocument d = stillEmitter(0.0f);
    d.shape.shape = ShapeType::Circle;
    d.shape.radiusThickness = 0.0f;
    d.shape.arcMode = ShapeMultiMode::BurstSpread;
    Burst b;
    b.count = MinMaxCurve(4.0f);
    d.emission.bursts = {b};
    Sim sim(d);
    sim.ticks(1);
    ASSERT_EQ(sim.count(), 4u);
    std::vector<float> angles;
    for (uint32_t i = 0; i < 4; i++) angles.push_back(angleOf(sim.ch(ChPosX, i), sim.ch(ChPosZ, i)));
    std::sort(angles.begin(), angles.end());
    for (int k = 0; k < 4; k++){
        const float expected = 90.0f * (float)k;
        EXPECT_NEAR(std::min(std::fabs(angles[(size_t)k] - expected), 360.0f - std::fabs(angles[(size_t)k] - expected)), 0.0f, 0.5f) << k;
    }
}

TEST(ParticleUpstreamModules, ArcSpreadHoldsMultiples){
    // a spread of 0.25 places random picks only at quarters of the arc
    ParticleEffectDocument d = stillEmitter(200.0f);
    d.shape.shape = ShapeType::Circle;
    d.shape.radiusThickness = 0.0f;
    d.shape.arcSpread = 0.25f;
    Sim sim(d);
    sim.ticks(30);
    ASSERT_GT(sim.count(), 50u);
    std::set<int> seen;
    for (uint32_t i = 0; i < sim.count(); i++){
        const float a = angleOf(sim.ch(ChPosX, i), sim.ch(ChPosZ, i));
        const float quarter = std::round(a / 90.0f) * 90.0f;
        EXPECT_NEAR(a, quarter, 0.5f) << i;
        seen.insert((int)quarter % 360);
    }
    EXPECT_GE(seen.size(), 3u);
}

TEST(ParticleUpstreamModules, RadiusLoopMovesAlongTheEdge){
    // an Edge with Loop radius: positions sweep from one end to the other at the speed
    ParticleEffectDocument d = stillEmitter(10.0f);
    d.shape.shape = ShapeType::Edge;
    d.shape.radius = 2.0f;
    d.shape.radiusMode = ShapeMultiMode::Loop;
    d.shape.radiusSpeed = MinMaxCurve(0.5f);
    Sim sim(d);
    sim.ticks(60);
    const std::vector<uint32_t> order = bySpawn(sim);
    ASSERT_GE(order.size(), 9u);
    for (size_t k = 1; k < order.size(); k++){
        const float x0 = sim.ch(ChPosX, order[k - 1]), x1 = sim.ch(ChPosX, order[k]);
        // 0.5 lengths a second, 10 a second: 0.05 of the 4-unit edge (0.2 units) apart
        EXPECT_NEAR(x1 - x0, 0.2f, 0.02f) << k;
    }
}

TEST(ParticleUpstreamModules, AlignToDirectionTurnsTheBillboard){
    // a cone's particles start rolled towards their direction: the roll is the direction's
    // angle from +Y in the view plane (a still view: X and Y)
    ParticleEffectDocument d = stillEmitter(100.0f);
    d.main.startSpeed = MinMaxCurve(1.0f);
    d.shape.shape = ShapeType::Cone;
    d.shape.angle = 45.0f;
    d.shape.radius = 1.0f;
    d.shape.radiusThickness = 0.0f;
    d.shape.alignToDirection = true;
    d.interpolate = false;
    Sim sim(d);
    sim.ticks(3);
    ASSERT_GT(sim.count(), 2u);
    memory::Arena arena("ParticleUpstreamModulesTest");
    const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
    ASSERT_EQ(pack.count, sim.count());
    bool rolled = false;
    for (uint32_t i = 0; i < pack.count; i++){
        const float roll = (float)(pack.instances[i].rotationFrame & 0xffffu) / 65535.0f;
        rolled = rolled || (roll > 0.01f && roll < 0.99f);
    }
    EXPECT_TRUE(rolled) << "tilted directions roll their billboards";

    d.shape.alignToDirection = false;
    Sim plain(d);
    plain.ticks(3);
    const PackResult still = ParticleRenderPrep::pack(plain.runtime, RenderView(), arena);
    for (uint32_t i = 0; i < still.count; i++){
        const float roll = (float)(still.instances[i].rotationFrame & 0xffffu) / 65535.0f;
        EXPECT_TRUE(roll < 1.0e-4f || roll > 1.0f - 1.0e-4f) << i;
    }
}

TEST(ParticleUpstreamModules, OrbitAroundAnOffsetCentre){
    // an orbit around (1, 0, 0) of the emitter: a particle starting at the emitter keeps its
    // distance to that centre and leaves the emitter
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.velocityOverLifetime.present = true;
    d.velocityOverLifetime.orbitalY = MinMaxCurve(90.0f);
    d.velocityOverLifetime.offsetX = MinMaxCurve(1.0f);
    Sim sim(d);
    sim.ticks(60);
    ASSERT_EQ(sim.count(), 1u);
    const float x = sim.ch(ChPosX), y = sim.ch(ChPosY), z = sim.ch(ChPosZ);
    EXPECT_NEAR(std::sqrt((x - 1.0f) * (x - 1.0f) + y * y + z * z), 1.0f, 0.05f);
    EXPECT_GT(std::sqrt(x * x + z * z), 0.5f);
}

TEST(ParticleUpstreamModules, OffsetCurvesMoveTheCentreOverTheLife){
    // the offset as a curve: the same particle with a constant offset ends elsewhere
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.main.startLifetime = MinMaxCurve(2.0f);
    d.velocityOverLifetime.present = true;
    d.velocityOverLifetime.orbitalY = MinMaxCurve(90.0f);
    d.velocityOverLifetime.offsetX = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f), 2.0f);
    Sim curved(d);
    curved.ticks(60);
    d.velocityOverLifetime.offsetX = MinMaxCurve(1.0f);
    Sim constant(d);
    constant.ticks(60);
    ASSERT_EQ(curved.count(), 1u);
    ASSERT_EQ(constant.count(), 1u);
    EXPECT_GT(std::fabs(curved.ch(ChPosX) - constant.ch(ChPosX)) + std::fabs(curved.ch(ChPosZ) - constant.ch(ChPosZ)), 1.0e-3f);
}

TEST(ParticleUpstreamModules, LimitVelocityPerAxis){
    ParticleEffectDocument d = singleParticle(Vector3(10.0f, 0.5f, -10.0f));
    d.limitVelocity.present = true;
    d.limitVelocity.separateAxes = true;
    d.limitVelocity.limit = MinMaxCurve(1.0f);
    d.limitVelocity.limitY = MinMaxCurve(5.0f);
    d.limitVelocity.limitZ = MinMaxCurve(2.0f);
    d.limitVelocity.dampen = 1.0f;
    Sim sim(d);
    sim.ticks(3);
    ASSERT_EQ(sim.count(), 1u);
    EXPECT_NEAR(sim.ch(ChVelX), 1.0f, 1e-3f);
    EXPECT_NEAR(sim.ch(ChVelY), 0.5f, 1e-3f) << "under its limit: kept";
    EXPECT_NEAR(sim.ch(ChVelZ), -2.0f, 1e-3f);

    // one speed limit without separate axes
    d.limitVelocity.separateAxes = false;
    d.limitVelocity.limit = MinMaxCurve(2.0f);
    Sim speed(d);
    speed.ticks(3);
    const float vx = speed.ch(ChVelX), vy = speed.ch(ChVelY), vz = speed.ch(ChVelZ);
    EXPECT_NEAR(std::sqrt(vx * vx + vy * vy + vz * vz), 2.0f, 1e-3f);
}

TEST(ParticleUpstreamModules, InheritVelocityCurrentFollowsTheEmittersChange){
    // Current: a particle born while the emitter is still gets the emitter's change of
    // velocity afterwards; Initial: only what it had at birth (nothing)
    for (const InheritVelocityMode mode : {InheritVelocityMode::Current, InheritVelocityMode::Initial}){
        ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
        d.main.inheritVelocity = MinMaxCurve(1.0f);
        d.main.inheritVelocityMode = mode;
        d.emitterVelocityMode = EmitterVelocityMode::Rigidbody;   // the velocity the input gives
        Sim sim(d);
        sim.input.hasVelocity = true;
        sim.ticks(5);
        ASSERT_EQ(sim.count(), 1u);
        EXPECT_NEAR(sim.ch(ChVelX), 0.0f, 1e-4f);
        sim.input.velocity[0] = 3.0f;
        sim.ticks(5);
        EXPECT_NEAR(sim.ch(ChVelX), mode == InheritVelocityMode::Current ? 3.0f : 0.0f, 1e-3f) << (int)mode;
    }
}

TEST(ParticleUpstreamModules, RotationBySpeedTurnsAboutEachAxis){
    // separate axes: 90 degrees a second about X at the top of the speed range turn the
    // particle's quaternion a quarter about X in a second
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 1.0f, 0.0f));
    d.renderer.renderMode = RenderMode::Mesh;
    MeshSource cube;
    d.renderer.meshes = {cube};
    d.rotationBySpeed.present = true;
    d.rotationBySpeed.separateAxes = true;
    d.rotationBySpeed.angularVelocityX = MinMaxCurve(90.0f);
    d.rotationBySpeed.angularVelocityY = MinMaxCurve(0.0f);
    d.rotationBySpeed.angularVelocity = MinMaxCurve(0.0f);
    d.rotationBySpeed.speedRange = Vector2(0.0f, 1.0f);
    Sim sim(d);
    sim.ticks(61);
    ASSERT_EQ(sim.count(), 1u);
    const float qx = sim.ch(ChQuatX), qy = sim.ch(ChQuatY), qz = sim.ch(ChQuatZ), qw = sim.ch(ChQuatW);
    const float half = 45.0f * kPi / 180.0f;
    EXPECT_NEAR(std::fabs(qx), std::sin(half), 0.02f);
    EXPECT_NEAR(qy, 0.0f, 0.02f);
    EXPECT_NEAR(qz, 0.0f, 0.02f);
    EXPECT_NEAR(std::fabs(qw), std::cos(half), 0.02f);
}

TEST(ParticleUpstreamModules, NoiseScrollChangesTheField){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.noise.present = true;
    d.noise.strength = MinMaxCurve(1.0f);
    d.noise.frequency = 1.0f;
    d.noise.octaves = 4;
    Sim still(d);
    still.ticks(60);
    d.noise.scrollSpeed = MinMaxCurve(2.0f);
    Sim scrolled(d);
    scrolled.ticks(60);
    ASSERT_EQ(still.count(), 1u);
    ASSERT_EQ(scrolled.count(), 1u);
    const float dx = still.ch(ChPosX) - scrolled.ch(ChPosX), dy = still.ch(ChPosY) - scrolled.ch(ChPosY);
    EXPECT_GT(std::fabs(dx) + std::fabs(dy), 1.0e-3f);
    // four octaves compile (origin/main's range is 1 to 4)
    d.noise.octaves = 4;
    EXPECT_TRUE(ParticleCompiler::compile(d, {})->valid);
}

TEST(ParticleUpstreamModules, RandomStartDelayPicksPerPlay){
    ParticleEffectDocument d = stillEmitter(60.0f);
    d.startDelay = MinMaxCurve::twoConstants(0.5f, 1.0f);
    std::set<uint32_t> firstTicks;
    for (uint32_t seed = 1; seed <= 6; seed++){
        Sim sim(d, seed);
        uint32_t tick = 0;
        while (sim.count() == 0 && tick < 200){
            sim.ticks(1);
            tick++;
        }
        EXPECT_GE(tick, 30u) << "not before 0.5 s";
        EXPECT_LE(tick, 61u) << "by 1 s";
        firstTicks.insert(tick);
    }
    EXPECT_GE(firstTicks.size(), 2u) << "a random delay per play";
}

TEST(ParticleUpstreamModules, RandomColorPicksAlongTheGradient){
    // Random Color: each particle the gradient at a random point (black to white: greys)
    ParticleEffectDocument d = stillEmitter(200.0f);
    d.main.startColor.mode = GradientValueMode::RandomColor;
    d.main.startColor.gradient = Gradient::twoColors(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
    Sim sim(d);
    sim.ticks(30);
    ASSERT_GT(sim.count(), 50u);
    std::set<uint32_t> reds;
    for (uint32_t i = 0; i < sim.count(); i++){
        const uint32_t c = sim.chu(ChStartColor, i);
        const uint32_t r = c & 255u, g = (c >> 8) & 255u, b = (c >> 16) & 255u;
        EXPECT_NEAR((float)r, (float)g, 1.0f);
        EXPECT_NEAR((float)g, (float)b, 1.0f);
        reds.insert(r / 32u);
    }
    EXPECT_GE(reds.size(), 5u) << "spread over the gradient";
}

TEST(ParticleUpstreamModules, BurstCountCurveReadsTheEmitterTime){
    // a burst half way through a 2 s effect whose count curve rises from 0 to 10: 5
    ParticleEffectDocument d = stillEmitter(0.0f);
    d.duration = 2.0f;
    d.looping = false;
    Burst b;
    b.time = 1.0f;
    b.count = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f), 10.0f);
    d.emission.bursts = {b};
    Sim sim(d);
    sim.ticks(70);
    EXPECT_EQ(sim.count(), 5u);
}

TEST(ParticleUpstreamModules, TextureSheetFlipsMirrorAShare){
    // flipU 1: every particle's quad mirrored horizontally (a negative width)
    ParticleEffectDocument d = stillEmitter(60.0f);
    d.textureSheetAnimation.present = true;
    d.textureSheetAnimation.tilesX = 2;
    d.textureSheetAnimation.tilesY = 2;
    d.textureSheetAnimation.flipU = 1.0f;
    d.interpolate = false;
    Sim sim(d);
    sim.ticks(10);
    ASSERT_GT(sim.count(), 0u);
    memory::Arena arena("ParticleUpstreamModulesTest");
    const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
    for (uint32_t i = 0; i < pack.count; i++){
        EXPECT_LT(ParticleRenderPrep::fromHalf((uint16_t)(pack.instances[i].size & 0xffffu)), 0.0f) << i;
        EXPECT_GT(ParticleRenderPrep::fromHalf((uint16_t)(pack.instances[i].size >> 16)), 0.0f) << i;
    }
}

TEST(ParticleUpstreamModules, TwoConstantsKeepTheirOrder){
    // origin/main's MinMaxTest: a range whose first value exceeds its second still lerps from
    // the first (S20's constant) to the second (constantMax)
    ParticleEffectDocument d = stillEmitter(500.0f);
    d.main.startLifetime = MinMaxCurve::twoConstants(4.0f, 2.0f);
    Sim sim(d);
    sim.ticks(10);
    ASSERT_GT(sim.count(), 50u);
    float lo = 1.0e9f, hi = 0.0f;
    for (uint32_t i = 0; i < sim.count(); i++){
        const float life = 1.0f / sim.ch(ChInvLife, i);
        lo = std::min(lo, life);
        hi = std::max(hi, life);
    }
    EXPECT_GE(lo, 2.0f - 1e-3f);
    EXPECT_LE(hi, 4.0f + 1e-3f);
    EXPECT_GT(hi - lo, 1.0f);
}

// ---------------------------------------------------------------- origin/main's MinMaxTest on S20's values

TEST(ParticleUpstreamMinMax, ConstantAndTwoConstants){
    const MinMaxCurve c(3.5f);
    EXPECT_EQ(c.mode, CurveMode::Constant);
    EXPECT_FLOAT_EQ(c.evaluate(0.0f, 0.0f), 3.5f);
    EXPECT_FLOAT_EQ(c.evaluate(0.7f, 0.9f), 3.5f);
    EXPECT_TRUE(c.isConstant());
    const MinMaxCurve two = MinMaxCurve::twoConstants(2.0f, 6.0f);
    EXPECT_FLOAT_EQ(two.evaluate(0.3f, 0.0f), 2.0f);
    EXPECT_FLOAT_EQ(two.evaluate(0.3f, 1.0f), 6.0f);
    EXPECT_FLOAT_EQ(two.evaluate(0.9f, 0.25f), 3.0f);
    // the first above the second: the lerp keeps the order (Unity)
    const MinMaxCurve reversed = MinMaxCurve::twoConstants(6.0f, 2.0f);
    EXPECT_FLOAT_EQ(reversed.evaluate(0.0f, 0.25f), 5.0f);
    float lo = 0.0f, hi = 0.0f;
    reversed.range(lo, hi);
    EXPECT_FLOAT_EQ(lo, 2.0f);
    EXPECT_FLOAT_EQ(hi, 6.0f);
}

TEST(ParticleUpstreamMinMax, CurvesTimesTheMultiplier){
    const MinMaxCurve c = MinMaxCurve::fromCurve(Curve::linear(0.0f, 0.0f, 1.0f, 2.0f), 3.0f);
    EXPECT_FLOAT_EQ(c.evaluate(0.0f, 0.5f), 0.0f);
    EXPECT_NEAR(c.evaluate(0.5f, 0.5f), 3.0f, 1e-5f);
    EXPECT_NEAR(c.evaluate(1.0f, 0.0f), 6.0f, 1e-5f);
    EXPECT_FALSE(c.isConstant());
    float lo = 0.0f, hi = 0.0f;
    c.range(lo, hi);
    EXPECT_NEAR(hi, 6.0f, 1e-5f);
    // two curves blend by the random, then the multiplier
    MinMaxCurve two = MinMaxCurve::fromCurve(Curve::constant(1.0f), 2.0f);
    two.mode = CurveMode::TwoCurves;
    two.curveMax = Curve::linear(0.0f, 2.0f, 1.0f, 4.0f);
    EXPECT_NEAR(two.evaluate(0.5f, 0.0f), 2.0f, 1e-5f);
    EXPECT_NEAR(two.evaluate(0.5f, 1.0f), 6.0f, 1e-5f);
    EXPECT_NEAR(two.evaluate(0.5f, 0.5f), 4.0f, 1e-5f);
}

TEST(ParticleUpstreamMinMax, BakedMatchesTheExactCurve){
    // the compiled LUT of a size curve against the curve itself, over a particle's life
    Curve curve;
    CurveKey a, b, c;
    a.time = 0.0f; a.value = 0.0f; a.outTangent = 2.0f;
    b.time = 0.6f; b.value = 1.0f;
    c.time = 1.0f; c.value = 0.2f; c.inTangent = -2.0f; c.outTangent = -2.0f;
    curve.keys = {a, b, c};
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.main.startLifetime = MinMaxCurve(1.0f);
    d.interpolate = false;
    d.sizeOverLifetime.present = true;
    d.sizeOverLifetime.size = MinMaxCurve::fromCurve(curve, 1.5f);
    Sim sim(d);
    memory::Arena arena("ParticleUpstreamModulesTest");
    for (int tick = 1; tick < 60; tick++){
        sim.ticks(1);
        ASSERT_EQ(sim.count(), 1u) << tick;
        const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
        const float t = sim.ch(ChAge) * sim.ch(ChInvLife);
        EXPECT_NEAR(ParticleRenderPrep::fromHalf((uint16_t)(pack.instances[0].size & 0xffffu)), curve.evaluate(t) * 1.5f, 0.01f) << t;
        arena.reset();
    }
}

TEST(ParticleUpstreamMinMax, TheFiveGradientModes){
    // S20's colours are sRGB as edited; evaluate() returns linear RGBA
    const Vector4 red(1, 0, 0, 1), blue(0, 0, 1, 0.5f);
    const MinMaxGradient color(red);
    EXPECT_EQ(color.evaluate(0.3f, 0.8f), Color::sRGBToLinear(red));

    MinMaxGradient two;
    two.mode = GradientValueMode::TwoColors;
    two.color = red;
    two.colorMax = blue;
    const Vector4 half = two.evaluate(0.9f, 0.5f);
    EXPECT_NEAR(half.x, Color::sRGBToLinear(Vector3(0.5f, 0.5f, 0.5f)).x, 1e-5f);
    EXPECT_NEAR(half.w, 0.75f, 1e-6f);

    const Gradient g = Gradient::twoColors(Vector3(1, 0, 0), Vector3(0, 0, 1), 1.0f, 0.5f);
    MinMaxGradient gradient;
    gradient.mode = GradientValueMode::Gradient;
    gradient.gradient = g;
    EXPECT_EQ(gradient.evaluate(0.0f, 0.9f), g.evaluate(0.0f));
    EXPECT_EQ(gradient.evaluate(1.0f, 0.1f), g.evaluate(1.0f));

    MinMaxGradient twoGradients;
    twoGradients.mode = GradientValueMode::TwoGradients;
    twoGradients.gradient = Gradient::constant(Vector3(1, 1, 1));
    twoGradients.gradientMax = g;
    const Vector4 mid = twoGradients.evaluate(1.0f, 0.5f);
    EXPECT_NEAR(mid.x, 0.5f, 1e-5f);
    EXPECT_NEAR(mid.z, 1.0f, 1e-5f);

    MinMaxGradient random = gradient;
    random.mode = GradientValueMode::RandomColor;
    // the random picks the point of the gradient, the time does not matter
    EXPECT_EQ(random.evaluate(0.0f, 1.0f), g.evaluate(1.0f));
    EXPECT_EQ(random.evaluate(0.7f, 0.0f), g.evaluate(0.0f));
    EXPECT_TRUE(random.usesGradient());
}

TEST(ParticleUpstreamMinMax, ParticleRandomsAreStable){
    // a particle's value for a stream never changes, streams are independent of each other
    for (uint32_t particle : {1u, 77u, 123456789u}){
        const float a = random01(0x1234u, particle);
        EXPECT_EQ(a, random01(0x1234u, particle));
        EXPECT_GE(a, 0.0f);
        EXPECT_LT(a, 1.0f);
        EXPECT_NE(a, random01(0x5678u, particle));
    }
    // roughly uniform
    double sum = 0.0;
    for (uint32_t particle = 0; particle < 10000; particle++) sum += random01(0x9abcu, particle);
    EXPECT_NEAR(sum / 10000.0, 0.5, 0.02);
}
