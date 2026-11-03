// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// origin/main's version 2 particle tests (ParticleEmissionTest, ParticleShapeTest,
// ParticleSimulationTest and its ParticleSystemAction scene tests) on S20's runtime, which
// absorbed its systems (docs/specs/S20-particles.md, Upstream absorption): the same Unity
// behaviours, checked at S20's fixed ticks. Where S20 differs on purpose the test says how (a
// cone's tilt is tan-proportional to the radius here; the shape's Euler order is Z, X, Y).

#include "ParticleTestUtil.h"

#include "Scene.h"
#include "manager/ObjectManager.h"
#include "object/ParticleSystem.h"
#include "particles/ParticleRender.h"
#include "subsystem/ParticleSubsystem.h"
#include "memory/Arena.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

using namespace particletest;

namespace {

    constexpr float kDegree = 0.017453292519943295f;

    // a still emitter at 60 ticks a second: nothing moves, nothing dies
    ParticleEffectDocument counter(){
        ParticleEffectDocument d;
        d.tickRate = 60;
        d.duration = 100.0f;
        d.simulationSpace = SimulationSpace::World;
        d.maxParticles = 100000;
        d.main.startLifetime = MinMaxCurve(100.0f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.shape.enabled = false;
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        return d;
    }

    // `count` particles born at once with speed 1 (their direction), positions where they spawned
    ParticleEffectDocument burstOf(const ShapeModule& shape, uint32_t count = 4000){
        ParticleEffectDocument d = counter();
        d.maxParticles = count;
        d.main.startSpeed = MinMaxCurve(1.0f);
        d.shape = shape;
        Burst b;
        b.count = MinMaxCurve((float)count);
        d.emission.bursts = {b};
        return d;
    }

    struct Sample {
        Vector3 position;
        Vector3 direction;
    };

    std::vector<Sample> samples(const ParticleEffectDocument& d, uint32_t seed = 1){
        Sim sim(d, seed);
        sim.ticks(1);
        std::vector<Sample> out;
        for (uint32_t i = 0; i < sim.count(); i++){
            const Vector3 v(sim.ch(ChVelX, i), sim.ch(ChVelY, i), sim.ch(ChVelZ, i));
            out.push_back({Vector3(sim.ch(ChPosX, i), sim.ch(ChPosY, i), sim.ch(ChPosZ, i)), v.normalized()});
        }
        return out;
    }

    ShapeModule shapeOf(ShapeType type, ShapeEmitFrom from = ShapeEmitFrom::Volume){
        ShapeModule s;
        s.shape = type;
        s.emitFrom = from;
        return s;
    }

    float tiltDegrees(const Vector3& direction){
        return std::acos(std::min(1.0f, direction.y)) / kDegree;
    }

}

// ---------------------------------------------------------------- emission

TEST(ParticleUpstreamEmission, RateOverTimeCountsExactly){
    ParticleEffectDocument d = counter();
    d.emission.rateOverTime = MinMaxCurve(10.0f);
    Sim sim(d);
    sim.ticks(60);
    EXPECT_EQ(sim.count(), 10u);
    sim.ticks(60);
    EXPECT_EQ(sim.count(), 20u);
}

TEST(ParticleUpstreamEmission, FractionsAccumulateAcrossFrames){
    // 3 a second at 60 ticks: 0.05 a tick
    ParticleEffectDocument d = counter();
    d.emission.rateOverTime = MinMaxCurve(3.0f);
    Sim sim(d);
    sim.ticks(18);
    EXPECT_EQ(sim.count(), 0u) << "0.9 of one so far";
    sim.ticks(3);
    EXPECT_EQ(sim.count(), 1u);
    sim.ticks(159);
    EXPECT_EQ(sim.count(), 9u);
}

TEST(ParticleUpstreamEmission, SpawnsSpreadOverTheTickAreAged){
    // a second at 60 ticks: 4 a second spawn 0.25 s apart, each aged by what is left of its tick
    ParticleEffectDocument d = counter();
    d.emission.rateOverTime = MinMaxCurve(4.0f);
    Sim sim(d);
    sim.ticks(60);
    ASSERT_EQ(sim.count(), 4u);
    std::vector<float> ages;
    for (uint32_t i = 0; i < 4; i++) ages.push_back(sim.ch(ChAge, i));
    std::sort(ages.begin(), ages.end());
    for (int k = 0; k < 4; k++)
        EXPECT_NEAR(ages[(size_t)k], 0.25f * (float)k, 1.0f / 60.0f + 1e-4f) << k;
}

TEST(ParticleUpstreamEmission, RateOverDistanceAlongThePathAndTeleports){
    ParticleEffectDocument d = counter();
    d.emission.rateOverDistance = MinMaxCurve(5.0f);
    Sim sim(d);
    // 2 units in 20 ticks: 10 particles, one every 0.2 units along the path
    for (int i = 1; i <= 20; i++){
        sim.moveTo(0.1f * (float)i, 0.0f, 0.0f);
        sim.ticks(1);
    }
    // (the first tick has no motion of its own: it starts the path)
    ASSERT_GE(sim.count(), 9u);
    ASSERT_LE(sim.count(), 10u);
    const uint32_t spawned = sim.count();
    std::vector<float> xs;
    for (uint32_t i = 0; i < sim.count(); i++) xs.push_back(sim.ch(ChPosX, i));
    std::sort(xs.begin(), xs.end());
    // along the path, about every 0.2 units (S20 spreads a tick's spawns over the tick's path)
    EXPECT_GE(xs.front(), -1e-4f);
    EXPECT_LE(xs.back(), 2.0f + 1e-4f);
    EXPECT_NEAR((xs.back() - xs.front()) / (float)(xs.size() - 1), 0.2f, 0.05f);
    for (size_t i = 1; i < xs.size(); i++)
        EXPECT_LT(xs[i] - xs[i - 1], 0.35f) << i;
    // a teleport spawns nothing (and is said once)
    EXPECT_FALSE(sim.runtime.jumpedTooFar());
    sim.moveTo(500.0f, 0.0f, 0.0f);
    sim.ticks(1);
    EXPECT_EQ(sim.count(), spawned);
    EXPECT_TRUE(sim.runtime.jumpedTooFar());
}

TEST(ParticleUpstreamEmission, BurstCyclesAndIntervals){
    ParticleEffectDocument d = counter();
    Burst burst;
    burst.time = 0.2f;
    burst.count = MinMaxCurve(5.0f);
    burst.cycles = 3;
    burst.interval = 0.5f;
    d.emission.bursts = {burst};
    Sim sim(d);
    sim.ticks(11);
    EXPECT_EQ(sim.count(), 0u);
    sim.ticks(2);
    EXPECT_EQ(sim.count(), 5u) << "at 0.2";
    sim.ticks(30);
    EXPECT_EQ(sim.count(), 10u) << "at 0.7";
    sim.ticks(30);
    EXPECT_EQ(sim.count(), 15u) << "at 1.2";
    sim.ticks(120);
    EXPECT_EQ(sim.count(), 15u) << "3 cycles only";
}

TEST(ParticleUpstreamEmission, BurstProbability){
    ParticleEffectDocument d = counter();
    Burst burst;
    burst.count = MinMaxCurve(1.0f);
    burst.cycles = 1000;
    burst.interval = 1.0f / 60.0f;
    burst.probability = 0.5f;
    d.emission.bursts = {burst};
    Sim sim(d);
    sim.ticks(1100);
    EXPECT_NEAR((float)sim.count() / 1000.0f, 0.5f, 0.06f);
}

TEST(ParticleUpstreamEmission, LoopingReArmsBursts){
    ParticleEffectDocument d = counter();
    d.duration = 1.0f;
    Burst burst;
    burst.count = MinMaxCurve(4.0f);
    d.emission.bursts = {burst};
    Sim sim(d);
    sim.ticks(150);
    EXPECT_EQ(sim.count(), 12u) << "at 0, 1 and 2 s";
}

TEST(ParticleUpstreamEmission, MaxParticlesCap){
    ParticleEffectDocument d = counter();
    d.maxParticles = 25;
    d.emission.rateOverTime = MinMaxCurve(100.0f);
    Sim sim(d);
    sim.ticks(60);
    EXPECT_EQ(sim.count(), 25u);
}

TEST(ParticleUpstreamEmission, StartDelayHoldsTheClock){
    ParticleEffectDocument d = counter();
    d.startDelay = MinMaxCurve(0.5f);
    d.emission.rateOverTime = MinMaxCurve(10.0f);
    Sim sim(d);
    sim.ticks(30);
    EXPECT_EQ(sim.count(), 0u);
    sim.ticks(60);
    EXPECT_EQ(sim.count(), 10u);
}

TEST(ParticleUpstreamEmission, BurstAtTheCycleEndFiresEveryLoop){
    // Unity fires a burst at time == duration at the end of each cycle
    ParticleEffectDocument d = counter();
    d.duration = 1.0f;
    Burst end;
    end.time = 1.0f;
    end.count = MinMaxCurve(2.0f);
    d.emission.bursts = {end};
    Sim sim(d);
    sim.ticks(150);
    EXPECT_EQ(sim.count(), 4u) << "at 1 s and 2 s";
    // a non-looping one fires it once
    d.looping = false;
    Sim once(d);
    once.ticks(150);
    EXPECT_EQ(once.count(), 2u);
}

TEST(ParticleUpstreamEmission, ArcSpeedRangeIsRandomPerPlay){
    // a two-constants arc speed picks one value of its range per play: the step between two
    // particles differs from seed to seed, inside the range
    std::set<int> steps;
    for (uint32_t seed = 1; seed <= 12; seed++){
        ParticleEffectDocument d = counter();
        d.emission.rateOverTime = MinMaxCurve(10.0f);
        d.shape.enabled = true;
        d.shape.shape = ShapeType::Circle;
        d.shape.radiusThickness = 0.0f;
        d.shape.arcMode = ShapeMultiMode::Loop;
        d.shape.arcSpeed = MinMaxCurve::twoConstants(0.0f, 1.0f);
        Sim sim(d, seed);
        sim.ticks(13);
        ASSERT_EQ(sim.count(), 2u);
        uint32_t a = 0, b = 1;
        if (sim.chu(ChSpawnIndex, a) > sim.chu(ChSpawnIndex, b)) std::swap(a, b);
        float step = (std::atan2(sim.ch(ChPosZ, b), sim.ch(ChPosX, b)) - std::atan2(sim.ch(ChPosZ, a), sim.ch(ChPosX, a))) / kDegree;
        while (step < 0.0f) step += 360.0f;
        EXPECT_LE(step, 36.0f + 0.5f) << seed;
        steps.insert((int)std::lround(step));
    }
    EXPECT_GE(steps.size(), 3u);
}

// ---------------------------------------------------------------- shapes

TEST(ParticleUpstreamShape, SphereSurfaceAndVolume){
    ShapeModule shape = shapeOf(ShapeType::Sphere);
    shape.radius = 2.0f;
    shape.radiusThickness = 0.0f;
    for (const Sample& s : samples(burstOf(shape))){
        ASSERT_NEAR(s.position.length(), 2.0f, 1e-4f);
        ASSERT_NEAR(s.direction.dotProduct(s.position / 2.0f), 1.0f, 1e-3f) << "outward";
    }
    // the whole volume: uniform, so the share inside half the radius is (1/2)^3
    shape.radiusThickness = 1.0f;
    int inside = 0;
    const std::vector<Sample> volume = samples(burstOf(shape));
    for (const Sample& s : volume){
        ASSERT_LE(s.position.length(), 2.0f + 1e-4f);
        if (s.position.length() < 1.0f) inside++;
    }
    EXPECT_NEAR((float)inside / (float)volume.size(), 0.125f, 0.02f);
}

TEST(ParticleUpstreamShape, ConeDirectionsTiltWithTheDistanceFromTheAxis){
    ShapeModule shape = shapeOf(ShapeType::Cone, ShapeEmitFrom::Base);
    shape.radius = 1.0f;
    shape.angle = 30.0f;
    shape.radiusThickness = 0.0f;
    // on the rim: every direction leaves at the cone angle, tilted outward
    for (const Sample& s : samples(burstOf(shape))){
        ASSERT_NEAR(s.position.y, 0.0f, 1e-5f);
        ASSERT_NEAR(Vector3(s.position.x, 0, s.position.z).length(), 1.0f, 1e-4f);
        ASSERT_NEAR(tiltDegrees(s.direction), 30.0f, 0.5f);
        ASSERT_GT(Vector3(s.direction.x, 0, s.direction.z).dotProduct(s.position), 0.0f);
    }
    // the whole base: the centre goes straight up, the area is uniform; S20 tilts a base point at
    // normalized radius r by atan(r tan(angle)) (the frustum's own rays)
    shape.radiusThickness = 1.0f;
    int inner = 0;
    const std::vector<Sample> base = samples(burstOf(shape));
    for (const Sample& s : base){
        const float d = Vector3(s.position.x, 0, s.position.z).length();
        ASSERT_NEAR(tiltDegrees(s.direction), std::atan(d * std::tan(30.0f * kDegree)) / kDegree, 0.5f);
        if (d < 0.5f) inner++;
    }
    EXPECT_NEAR((float)inner / (float)base.size(), 0.25f, 0.02f) << "area sampling";
}

TEST(ParticleUpstreamShape, PointConeAndConeVolume){
    ShapeModule shape = shapeOf(ShapeType::Cone, ShapeEmitFrom::Base);
    shape.radius = 0.0f;
    shape.angle = 20.0f;
    for (const Sample& s : samples(burstOf(shape))){
        ASSERT_NEAR(s.position.length(), 0.0f, 1e-6f);
        ASSERT_LE(tiltDegrees(s.direction), 20.0f + 1e-2f);
    }
    // Cone Volume: on its ray from the apex, no higher than the length
    shape.emitFrom = ShapeEmitFrom::Volume;
    shape.length = 3.0f;
    for (const Sample& s : samples(burstOf(shape))){
        const float along = s.position.dotProduct(s.direction);
        ASSERT_GE(along, -1e-5f);
        ASSERT_LE(s.position.y, 3.0f + 1e-4f);
        ASSERT_NEAR((s.position - s.direction * along).length(), 0.0f, 1e-3f);
    }
}

TEST(ParticleUpstreamShape, RandomDirectionIsUniformAndTheShapeTransformApplies){
    ShapeModule shape = shapeOf(ShapeType::Cone, ShapeEmitFrom::Base);
    shape.randomizeDirection = 1.0f;
    Vector3 sum(0, 0, 0);
    const std::vector<Sample> random = samples(burstOf(shape));
    for (const Sample& s : random) sum = sum + s.direction;
    EXPECT_LT((sum / (float)random.size()).length(), 0.05f);

    // a cone turned 90 degrees around X emits along +Z, moved by the position
    ShapeModule turned = shapeOf(ShapeType::Cone, ShapeEmitFrom::Base);
    turned.angle = 0.0f;
    turned.radius = 0.0f;
    turned.rotation = Vector3(90.0f, 0.0f, 0.0f);
    turned.position = Vector3(1.0f, 2.0f, 3.0f);
    const std::vector<Sample> one = samples(burstOf(turned, 4));
    ASSERT_FALSE(one.empty());
    EXPECT_NEAR(one[0].direction.z, 1.0f, 1e-5f);
    EXPECT_NEAR((one[0].position - Vector3(1, 2, 3)).length(), 0.0f, 1e-5f);

    // disabled: the origin, straight up
    ShapeModule off = shapeOf(ShapeType::Sphere);
    off.enabled = false;
    const std::vector<Sample> o = samples(burstOf(off, 4));
    ASSERT_FALSE(o.empty());
    EXPECT_NEAR(o[0].position.length(), 0.0f, 1e-6f);
    EXPECT_NEAR(o[0].direction.y, 1.0f, 1e-6f);
}

TEST(ParticleUpstreamShape, EulerOrderIsZThenXThenY){
    // S20's shape turn applies Z, then X, then Y (origin/main's applied X, Y, Z: its files are
    // converted, stream/UpstreamParticles): X 90 and Z 90 send +Y to -X, and +X to +Z
    ShapeModule turned = shapeOf(ShapeType::Cone, ShapeEmitFrom::Base);
    turned.angle = 0.0f;
    turned.radius = 0.0f;
    turned.rotation = Vector3(90.0f, 0.0f, 90.0f);
    const std::vector<Sample> s = samples(burstOf(turned, 4));
    ASSERT_FALSE(s.empty());
    EXPECT_NEAR(s[0].direction.x, -1.0f, 1e-5f);
    ShapeModule edge = shapeOf(ShapeType::Edge);
    edge.radius = 1.0f;
    edge.rotation = Vector3(90.0f, 0.0f, 90.0f);
    for (const Sample& e : samples(burstOf(edge, 50))){
        // the edge's X axis now runs along Z
        ASSERT_NEAR(e.position.x, 0.0f, 1e-5f);
        ASSERT_NEAR(e.position.y, 0.0f, 1e-5f);
    }
}

// ---------------------------------------------------------------- simulation

TEST(ParticleUpstreamSimulation, BySpeedModules){
    // speed 5 of the range 0..10: half way on each curve
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 5.0f, 0.0f));
    d.interpolate = false;
    d.sizeBySpeed.present = true;
    d.sizeBySpeed.size = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
    d.sizeBySpeed.speedRange = Vector2(0.0f, 10.0f);
    d.rotationBySpeed.present = true;
    d.rotationBySpeed.angularVelocity = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f), 2.0f / kDegree);
    d.rotationBySpeed.speedRange = Vector2(0.0f, 10.0f);
    d.colorBySpeed.present = true;
    d.colorBySpeed.color.mode = GradientValueMode::Gradient;
    d.colorBySpeed.color.gradient = Gradient::twoColors(Vector3(1, 0, 0), Vector3(0, 0, 1));
    d.colorBySpeed.speedRange = Vector2(0.0f, 10.0f);
    Sim sim(d);
    sim.ticks(31);
    ASSERT_EQ(sim.count(), 1u);
    // 1 rad/s for 0.5 s
    EXPECT_NEAR(sim.ch(ChRotation), 0.5f, 0.03f);
    memory::Arena arena("ParticleUpstreamBehaviourTest");
    const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
    ASSERT_EQ(pack.count, 1u);
    EXPECT_NEAR(ParticleRenderPrep::fromHalf((uint16_t)(pack.instances[0].size & 0xffffu)), 0.5f, 0.02f);
    // gradients blend their keys in sRGB (as Unity): half way is sRGB 0.5 (the instance's colour is sRGB)
    EXPECT_NEAR((float)(pack.instances[0].color & 255u) / 255.0f, 0.5f, 0.02f);
    EXPECT_NEAR((float)((pack.instances[0].color >> 16) & 255u) / 255.0f, 0.5f, 0.02f);
}

TEST(ParticleUpstreamSimulation, PrewarmEqualsOneDuration){
    ParticleEffectDocument d = counter();
    d.duration = 1.0f;
    d.looping = true;
    d.tickRate = 30;
    d.main.startLifetime = MinMaxCurve(0.8f);
    d.main.startSpeed = MinMaxCurve(1.0f);
    d.shape.enabled = true;
    d.emission.rateOverTime = MinMaxCurve(30.0f);
    ParticleEffectDocument warmDoc = d;
    warmDoc.prewarm = true;
    warmDoc.prewarmSync = true;   // all of it at play (else it spreads over the first frames)
    Sim warm(warmDoc);
    // the prewarm runs when time first passes (a frame): its duration at once
    warm.runtime.advance(warm.input, 1.0 / 30.0);
    Sim cold(d);
    cold.ticks(31);
    EXPECT_GT(warm.count(), 20u);
    EXPECT_NEAR((float)warm.count(), (float)cold.count(), 1.0f);
}

TEST(ParticleUpstreamSimulation, StopBehaviours){
    ParticleEffectDocument d = counter();
    d.main.startLifetime = MinMaxCurve(0.5f);
    d.emission.rateOverTime = MinMaxCurve(20.0f);
    // StopEmitting: the particles live out, then it stops
    Sim emitting(d);
    emitting.ticks(20);
    ASSERT_GT(emitting.count(), 0u);
    emitting.runtime.stop(false);
    EXPECT_FALSE(emitting.runtime.isEmitting());
    emitting.ticks(40);
    EXPECT_EQ(emitting.count(), 0u);
    EXPECT_FALSE(emitting.runtime.isPlaying());
    // StopEmittingAndClear: stopped and empty now; play() again restarts the clock
    Sim cleared(d);
    cleared.ticks(20);
    cleared.runtime.stop(true);
    EXPECT_EQ(cleared.count(), 0u);
    EXPECT_FALSE(cleared.runtime.isPlaying());
    cleared.runtime.play();
    EXPECT_TRUE(cleared.runtime.isPlaying());
}

TEST(ParticleUpstreamSimulation, NonLoopingFinishes){
    ParticleEffectDocument d = counter();
    d.looping = false;
    d.duration = 0.5f;
    d.main.startLifetime = MinMaxCurve(0.25f);
    d.emission.rateOverTime = MinMaxCurve(20.0f);
    Sim sim(d);
    int ticks = 0;
    do {
        sim.ticks(1);
        ticks++;
    } while ((sim.runtime.isPlaying() || sim.count() > 0) && ticks < 600);
    // the emission ends at 0.5 s, the last particles die 0.25 s later
    EXPECT_NEAR((float)ticks / 60.0f, 0.75f, 0.05f);
}

TEST(ParticleUpstreamSimulation, FixedSeedReplaysExactly){
    ParticleEffectDocument d = counter();
    d.main.startLifetime = MinMaxCurve::twoConstants(0.5f, 1.5f);
    d.main.startSpeed = MinMaxCurve::twoConstants(1.0f, 3.0f);
    d.main.startColor.mode = GradientValueMode::TwoColors;
    d.main.startColor.color = Vector4(1, 0, 0, 1);
    d.main.startColor.colorMax = Vector4(0, 1, 0, 1);
    d.shape.enabled = true;
    d.shape.shape = ShapeType::Sphere;
    d.emission.rateOverTime = MinMaxCurve(40.0f);
    Sim a(d, 9), b(d, 9);
    a.ticks(120);
    b.ticks(120);
    ASSERT_EQ(a.count(), b.count());
    for (uint32_t i = 0; i < a.count(); i++){
        EXPECT_EQ(a.ch(ChPosX, i), b.ch(ChPosX, i));
        EXPECT_EQ(a.chu(ChStartColor, i), b.chu(ChStartColor, i));
    }
    // another seed: another play
    Sim c(d, 10);
    c.ticks(120);
    bool differs = c.count() != a.count();
    for (uint32_t i = 0; !differs && i < a.count(); i++) differs = a.ch(ChPosX, i) != c.ch(ChPosX, i);
    EXPECT_TRUE(differs);
}

TEST(ParticleUpstreamSimulation, LocalSpaceMovesWithTheEmitterWorldStays){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.simulationSpace = SimulationSpace::Local;
    Sim local(d);
    local.ticks(2);
    local.moveTo(5.0f, 0.0f, 0.0f);
    local.ticks(2);
    // local particles keep their place in the emitter (simulation space = the emitter's)
    EXPECT_NEAR(local.ch(ChPosX), 0.0f, 1e-4f);
    d.simulationSpace = SimulationSpace::World;
    Sim world(d);
    world.ticks(2);
    world.moveTo(5.0f, 0.0f, 0.0f);
    world.ticks(2);
    EXPECT_NEAR(world.ch(ChPosX), 0.0f, 1e-4f) << "world particles stay where they were born";
}

TEST(ParticleUpstreamSimulation, AgeSortOrdersTheInstances){
    // particles rising at 1 unit a second: their height is their age; the instances are in draw
    // order (the last drawn on top)
    ParticleEffectDocument d = counter();
    d.emission.rateOverTime = MinMaxCurve(30.0f);
    d.main.startSpeed = MinMaxCurve(1.0f);
    d.shape.enabled = true;
    d.shape.shape = ShapeType::Cone;
    d.shape.emitFrom = ShapeEmitFrom::Base;
    d.shape.angle = 0.0f;
    d.shape.radius = 0.0f;
    d.interpolate = false;
    for (const SortMode mode : {SortMode::OldestInFront, SortMode::YoungestInFront}){
        d.renderer.sortMode = mode;
        Sim sim(d);
        sim.ticks(60);
        memory::Arena arena("ParticleUpstreamBehaviourTest");
        RenderView view;
        view.sort = true;
        const PackResult pack = ParticleRenderPrep::pack(sim.runtime, view, arena);
        ASSERT_EQ(pack.count, sim.count());
        ASSERT_GT(pack.count, 10u);
        for (uint32_t i = 1; i < pack.count; i++){
            const float below = pack.instances[i - 1].position[1], above = pack.instances[i].position[1];
            if (mode == SortMode::OldestInFront)
                EXPECT_GE(above, below - 1e-5f) << "the oldest (highest) last: " << i;
            else
                EXPECT_LE(above, below + 1e-5f) << "the youngest (lowest) last: " << i;
        }
    }
}

TEST(ParticleUpstreamSimulation, StretchedBillboardsFollowTheTotalVelocity){
    // a still particle moved only by Velocity over Lifetime: a stretched billboard draws along
    // that motion (origin/main stretched along totalVelocity, as Unity), a plain one stores none
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.interpolate = false;
    d.velocityOverLifetime.present = true;
    d.velocityOverLifetime.space = ValueSpace::World;
    d.velocityOverLifetime.x = MinMaxCurve(0.5f);
    {
        Sim plain(d);
        EXPECT_FALSE(plain.runtime.program()->channels & channelBit(ChAnimVelX)) << "only drawn along the velocity";
    }
    d.renderer.renderMode = RenderMode::StretchedBillboard;
    Sim sim(d);
    ASSERT_TRUE(sim.runtime.program()->channels & channelBit(ChAnimVelX));
    sim.ticks(10);
    ASSERT_EQ(sim.count(), 1u);
    EXPECT_GT(sim.ch(ChPosX), 0.05f) << "moved by Velocity over Lifetime";
    EXPECT_NEAR(sim.ch(ChVelX), 0.0f, 1e-6f) << "its stored velocity stays 0";
    memory::Arena arena("ParticleUpstreamBehaviourTest");
    const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
    ASSERT_EQ(pack.count, 1u);
    EXPECT_NEAR(ParticleRenderPrep::fromHalf((uint16_t)(pack.instances[0].velocityXY & 0xffffu)), 0.5f, 1e-3f);
    EXPECT_NEAR(ParticleRenderPrep::fromHalf((uint16_t)(pack.instances[0].velocityXY >> 16)), 0.0f, 1e-3f);
}

// ---------------------------------------------------------------- the scene (origin/main's ParticleSystemAction)

namespace {

    const char* const kCounterEffect = R"({"version": 1, "duration": 1, "looping": false, "seed": 3, "maxParticles": 100,
        "modules": [{"type": "main", "startLifetime": 0.2, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 60},
                    {"type": "shape", "enabled": false}]})";

    void frames(Scene& scene, int n){
        for (int i = 0; i < n; i++) scene.getSystem<ParticleSubsystem>()->update(1.0 / 60.0);
    }

}

TEST(ParticleUpstreamScene, PlayOnAwakeFillsAndNoPlayOnAwakeWaits){
    Scene scene;
    ParticleSystem awake(&scene);
    ASSERT_TRUE(awake.setInlineEffect(kCounterEffect));
    ParticleSystem waiting(&scene);
    waiting.setPlayOnAwake(false);
    ASSERT_TRUE(waiting.setInlineEffect(kCounterEffect));
    frames(scene, 10);
    EXPECT_GT(awake.getParticleCount(), 0u);
    EXPECT_EQ(waiting.getParticleCount(), 0u);
    waiting.play();
    frames(scene, 10);
    EXPECT_GT(waiting.getParticleCount(), 0u);
}

TEST(ParticleUpstreamScene, PlayStartsAsALoadAfterAnEditTimePreview){
    // the editor plays the edit-time scene in place: a system its preview stopped (a deselected
    // one) or played still starts with Play On Awake, from nothing (origin/main restarted them)
    Scene scene;
    ParticleSystem stopped(&scene);
    ASSERT_TRUE(stopped.setInlineEffect(kCounterEffect));
    ParticleSystem previewed(&scene);
    ASSERT_TRUE(previewed.setInlineEffect(kCounterEffect));
    std::shared_ptr<ParticleSubsystem> particles = scene.getSystem<ParticleSubsystem>();
    frames(scene, 1);
    particles->stop(stopped.getEntity(), true);
    frames(scene, 5);
    EXPECT_EQ(stopped.getParticleCount(), 0u);
    ASSERT_GT(previewed.getParticleCount(), 0u);

    particles->resetForPlay();
    EXPECT_EQ(previewed.getParticleCount(), 0u) << "no particle of the preview is left";
    frames(scene, 10);
    EXPECT_GT(stopped.getParticleCount(), 0u) << "Play On Awake plays it";
    EXPECT_GT(previewed.getParticleCount(), 0u);
    EXPECT_TRUE(stopped.isPlaying());
}

TEST(ParticleUpstreamScene, StopActionDisable){
    // a finished system with Stop Action Disable deactivates its GameObject
    Scene scene;
    ParticleSystem system(&scene);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 0.5, "looping": false, "seed": 3, "maxParticles": 100,
        "stopAction": "disable",
        "modules": [{"type": "main", "startLifetime": 0.2, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 60}, {"type": "shape", "enabled": false}]})"));
    frames(scene, 10);
    EXPECT_TRUE(system.isActive());
    frames(scene, 60);
    EXPECT_FALSE(system.isActive()) << "0.5 s of emission and 0.2 s of life later";
}

TEST(ParticleUpstreamScene, StopActionDestroy){
    // Destroy is GameObject.destroy: at the end of the frame
    Scene scene;
    ParticleSystem system(&scene);
    const Entity entity = system.getEntity();
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 0.2, "looping": false, "seed": 3, "maxParticles": 100,
        "stopAction": "destroy",
        "modules": [{"type": "main", "startLifetime": 0.2, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 50}, {"type": "shape", "enabled": false}]})"));
    frames(scene, 60);
    ObjectManager::update(0.0);
    EXPECT_FALSE(scene.isEntityCreated(entity));
}

TEST(ParticleUpstreamScene, ChildrenFollowPlayAndStop){
    Scene scene;
    ParticleSystem parent(&scene);
    parent.setPlayOnAwake(false);
    ASSERT_TRUE(parent.setInlineEffect(kCounterEffect));
    ParticleSystem child(&scene);
    child.setPlayOnAwake(false);
    ASSERT_TRUE(child.setInlineEffect(kCounterEffect));
    parent.addChild(&child);
    frames(scene, 2);
    parent.play(true);
    frames(scene, 5);
    EXPECT_TRUE(parent.isPlaying());
    EXPECT_TRUE(child.isPlaying());
    parent.stop(true, true);
    EXPECT_EQ(child.getParticleCount(), 0u);
    EXPECT_FALSE(child.isPlaying());
    // without the children: only itself
    parent.play(false);
    frames(scene, 2);
    EXPECT_TRUE(parent.isPlaying());
    EXPECT_FALSE(child.isPlaying());
}

TEST(ParticleUpstreamScene, DeactivatedGameObjectClearsAndReplaysOnReactivation){
    Scene scene;
    ParticleSystem system(&scene);
    // Always Simulate: no camera draws this scene, and S20's culling would pause a looping system
    // that has bounds once the shared frame counter (other tests' renders) moves past its last
    // seen frame; the test is about deactivation
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "seed": 3, "maxParticles": 100, "cullingMode": "alwaysSimulate",
        "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 60}, {"type": "shape", "enabled": false}]})"));
    frames(scene, 20);
    ASSERT_GT(system.getParticleCount(), 10u);
    system.setActive(false);
    frames(scene, 1);
    EXPECT_EQ(system.getParticleCount(), 0u);
    system.setActive(true);
    frames(scene, 10);
    EXPECT_GT(system.getParticleCount(), 0u) << "Play On Awake plays it again";
    EXPECT_LT(system.getParticleCount(), 20u);
}
