// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The version 2 simulation (particles spec 6.3, 7, 9): gravity, force in world and local
// space, velocity over lifetime against the analytic integral, limit velocity at 30 and 60
// fps alike, drag, noise bounded and seed-stable, the by-speed modules, the texture sheet's
// time modes, prewarm, stop() and its behaviours, determinism with a fixed seed. ActionSystem:
// playOnAwake, the stop actions, children, a deactivated GameObject, the renderer it fills.

#include <gtest/gtest.h>

#include "Scene.h"
#include "action/ParticleSystem.h"
#include "manager/ObjectManager.h"
#include "component/ActionComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "particles/ParticleSimulation.h"
#include "subsystem/ActionSystem.h"

#include <cmath>

using namespace jasperengine;

namespace {

    ParticleSystemComponent base() {
        ParticleSystemComponent ps;
        ps.version = 2;
        ps.main.autoRandomSeed = false;
        ps.main.randomSeed = 11;
        ps.main.startSpeed = MinMaxCurve(0.0f);
        ps.main.startLifetime = MinMaxCurve(100.0f);
        ps.main.duration = 100.0f;
        ps.shape.enabled = false;
        ps.emission.rateOverTime = MinMaxCurve(0.0f);
        return ps;
    }

    // one particle at the emitter, simulated `seconds` at `fps`
    ParticleSystemComponent single(ParticleSystemComponent ps, float seconds, float fps,
                                   const ParticleEmitterFrame& emitter = ParticleEmitterFrame(), const ParticleEnvironment& env = ParticleEnvironment()) {
        particles::play(ps, emitter, env);
        particles::emit(ps, 1, emitter, env);
        const int steps = (int)std::lround(seconds * fps);
        for (int i = 0; i < steps; i++) particles::step(ps, 1.0f / fps, emitter, env);
        return ps;
    }

}

TEST(ParticleSimulation, GravityModifier) {
    ParticleSystemComponent ps = base();
    ps.main.gravityModifier = MinMaxCurve(1.0f);
    ParticleEnvironment env;
    env.gravity3D = Vector3(0.0f, -10.0f, 0.0f);
    ps = single(ps, 1.0f, 60.0f, ParticleEmitterFrame(), env);
    ASSERT_EQ(particles::particleCount(ps), 1u);
    EXPECT_NEAR(ps.state.velocity[0].y, -10.0f, 1e-3f);
    // semi-implicit Euler: within 2 % of the analytic g t^2 / 2
    EXPECT_NEAR(ps.state.position[0].y, -5.0f, 0.1f);

    // the 2D source: the 2D world's gravity, in the scene's units
    ParticleSystemComponent two = base();
    two.main.gravityModifier = MinMaxCurve(0.5f);
    two.main.gravitySource = ParticleSystemGravitySource::Physics2D;
    env.gravity2D = Vector3(0.0f, -640.0f, 0.0f);
    two = single(two, 1.0f, 60.0f, ParticleEmitterFrame(), env);
    EXPECT_NEAR(two.state.velocity[0].y, -320.0f, 0.05f);
}

TEST(ParticleSimulation, ForceWorldAndLocal) {
    // the emitter turned 90 degrees about Z: its local +X is the world's +Y
    ParticleEmitterFrame emitter;
    emitter.rotation = particles::eulerRadians(Vector3(0.0f, 0.0f, 1.5707963f));
    ParticleSystemComponent local = base();
    local.main.simulationSpace = ParticleSystemSimulationSpace::World;
    local.forceOverLifetime.enabled = true;
    local.forceOverLifetime.x = MinMaxCurve(2.0f);
    local.forceOverLifetime.space = ParticleSystemSimulationSpace::Local;
    local = single(local, 1.0f, 60.0f, emitter);
    EXPECT_NEAR(local.state.velocity[0].y, 2.0f, 1e-3f);
    EXPECT_NEAR(local.state.velocity[0].x, 0.0f, 1e-3f);

    ParticleSystemComponent world = base();
    world.main.simulationSpace = ParticleSystemSimulationSpace::World;
    world.forceOverLifetime.enabled = true;
    world.forceOverLifetime.x = MinMaxCurve(2.0f);
    world.forceOverLifetime.space = ParticleSystemSimulationSpace::World;
    world = single(world, 1.0f, 60.0f, emitter);
    EXPECT_NEAR(world.state.velocity[0].x, 2.0f, 1e-3f);
}

TEST(ParticleSimulation, VelocityOverLifetimeIsAdditive) {
    ParticleSystemComponent ps = base();
    ps.main.startSpeed = MinMaxCurve(1.0f);
    ps.shape.enabled = true;
    ps.shape.shapeType = ParticleSystemShapeType::Cone;
    ps.shape.angle = 0.0f;
    ps.shape.radius = 0.0f;
    ps.velocityOverLifetime.enabled = true;
    ps.velocityOverLifetime.x = MinMaxCurve(2.0f);
    ps.velocityOverLifetime.z = MinMaxCurve(-1.0f);
    ps = single(ps, 2.0f, 60.0f);
    // start speed 1 up plus (2, 0, -1): the exact line
    EXPECT_NEAR(ps.state.position[0].x, 4.0f, 1e-3f);
    EXPECT_NEAR(ps.state.position[0].y, 2.0f, 1e-3f);
    EXPECT_NEAR(ps.state.position[0].z, -2.0f, 1e-3f);
    // the animated velocity is not integrated into the particle's own
    EXPECT_NEAR(ps.state.velocity[0].x, 0.0f, 1e-5f);
    EXPECT_NEAR(ps.state.totalVelocity[0].x, 2.0f, 1e-5f);

    // speed modifier scales both
    ParticleSystemComponent slow = ps;
    slow.state = ParticleSimState();
    slow.velocityOverLifetime.speedModifier = MinMaxCurve(0.5f);
    slow = single(slow, 2.0f, 60.0f);
    EXPECT_NEAR(slow.state.position[0].y, 1.0f, 1e-3f);
}

TEST(ParticleSimulation, LimitVelocityIsFrameRateIndependent) {
    for (float dampen : {1.0f, 0.1f}) {
        ParticleSystemComponent ps = base();
        ps.main.startSpeed = MinMaxCurve(10.0f);
        ps.shape.enabled = true;
        ps.shape.shapeType = ParticleSystemShapeType::Cone;
        ps.shape.angle = 0.0f;
        ps.shape.radius = 0.0f;
        ps.limitVelocityOverLifetime.enabled = true;
        ps.limitVelocityOverLifetime.limit = MinMaxCurve(2.0f);
        ps.limitVelocityOverLifetime.dampen = dampen;
        const ParticleSystemComponent at30 = single(ps, 1.0f, 30.0f);
        const ParticleSystemComponent at60 = single(ps, 1.0f, 60.0f);
        const float v30 = at30.state.velocity[0].y, v60 = at60.state.velocity[0].y;
        EXPECT_NEAR(v30, v60, 0.01f * std::max(v30, v60)) << "dampen " << dampen;
        if (dampen == 1.0f) EXPECT_NEAR(v30, 2.0f, 1e-3f);
        if (dampen == 0.1f) {
            // the excess keeps 0.9^30 of itself after a second
            EXPECT_NEAR(v30, 2.0f + 8.0f * std::pow(0.9f, 30.0f), 0.05f);
        }
    }
}

TEST(ParticleSimulation, Drag) {
    ParticleSystemComponent ps = base();
    ps.main.startSpeed = MinMaxCurve(4.0f);
    ps.shape.enabled = true;
    ps.shape.shapeType = ParticleSystemShapeType::Cone;
    ps.shape.angle = 0.0f;
    ps.shape.radius = 0.0f;
    ps.limitVelocityOverLifetime.enabled = true;
    ps.limitVelocityOverLifetime.limit = MinMaxCurve(1000.0f);
    ps.limitVelocityOverLifetime.drag = MinMaxCurve(0.5f);
    ps.limitVelocityOverLifetime.multiplyDragByParticleSize = false;
    ps.limitVelocityOverLifetime.multiplyDragByParticleVelocity = false;
    ps = single(ps, 1.0f, 60.0f);
    // v' = -0.5 v: about e^-0.5 of it after a second
    EXPECT_NEAR(ps.state.velocity[0].y, 4.0f * std::exp(-0.5f), 0.05f);
}

TEST(ParticleSimulation, NoiseIsBoundedAndSeedStable) {
    ParticleSystemComponent ps = base();
    ps.emission.rateOverTime = MinMaxCurve(50.0f);
    ps.main.startLifetime = MinMaxCurve(2.0f);
    ps.shape.enabled = true;
    ps.shape.shapeType = ParticleSystemShapeType::Sphere;
    ps.noise.enabled = true;
    ps.noise.strength = MinMaxCurve(1.0f);
    ps.noise.frequency = 1.0f;
    ps.noise.damping = false;
    ps.noise.octaves = 3;
    ParticleSystemComponent a = ps, b = ps;
    particles::play(a, ParticleEmitterFrame(), ParticleEnvironment());
    particles::play(b, ParticleEmitterFrame(), ParticleEnvironment());
    for (int i = 0; i < 90; i++) {
        particles::step(a, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
        particles::step(b, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
    }
    ASSERT_EQ(particles::particleCount(a), particles::particleCount(b));
    ASSERT_GT(particles::particleCount(a), 0u);
    for (size_t i = 0; i < particles::particleCount(a); i++) {
        EXPECT_EQ(a.state.position[i], b.state.position[i]);
        const Vector3& v = a.state.totalVelocity[i];
        // strength 1 per axis: the noise velocity stays within it
        EXPECT_LE(std::fabs(v.x), 1.0f + 1e-4f);
        EXPECT_LE(std::fabs(v.y), 1.0f + 1e-4f);
        EXPECT_LE(std::fabs(v.z), 1.0f + 1e-4f);
    }
}

TEST(ParticleSimulation, BySpeedModules) {
    ParticleSystemComponent ps = base();
    ps.main.startSpeed = MinMaxCurve(5.0f);
    ps.shape.enabled = true;
    ps.shape.shapeType = ParticleSystemShapeType::Cone;
    ps.shape.angle = 0.0f;
    ps.shape.radius = 0.0f;
    ps.sizeBySpeed.enabled = true;
    ps.sizeBySpeed.size = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 1.0f));
    ps.sizeBySpeed.range = Vector2(0.0f, 10.0f);
    ps.rotationBySpeed.enabled = true;
    ps.rotationBySpeed.z = MinMaxCurve::fromCurve(AnimationCurve::linear(0.0f, 0.0f, 1.0f, 2.0f));
    ps.rotationBySpeed.range = Vector2(0.0f, 10.0f);
    ps.colorBySpeed.enabled = true;
    ps.colorBySpeed.color = MinMaxGradient::fromGradient(Gradient::twoColors(Vector4(1, 0, 0, 1), Vector4(0, 0, 1, 1)));
    ps.colorBySpeed.range = Vector2(0.0f, 10.0f);
    ps = single(ps, 0.5f, 60.0f);
    // speed 5 of 0..10: half way on each curve
    EXPECT_NEAR(ps.state.size[0].x, 0.5f, 0.02f);
    EXPECT_NEAR(ps.state.rotation[0].z, 1.0f * 0.5f, 0.03f) << "1 rad/s for 0.5 s";
    // gradients blend their keys in sRGB (as Unity): half way is sRGB 0.5, linear 0.214
    EXPECT_NEAR(ps.state.color[0].x, 0.214f, 0.02f);
    EXPECT_NEAR(ps.state.color[0].z, 0.214f, 0.02f);
}

TEST(ParticleSimulation, TextureSheetTimeModes) {
    ParticleSystemComponent ps = base();
    ps.textureSheetAnimation.enabled = true;
    ps.textureSheetAnimation.numTilesX = 5;
    ps.textureSheetAnimation.numTilesY = 10;
    ps.textureSheetAnimation.timeMode = ParticleSystemAnimationTimeMode::FPS;
    ps.textureSheetAnimation.fps = 25.0f;
    ParticleSystemComponent run = ps;
    particles::play(run, ParticleEmitterFrame(), ParticleEnvironment());
    particles::emit(run, 1, ParticleEmitterFrame(), ParticleEnvironment());
    int advances = 0;
    float last = run.state.frame[0];
    for (int i = 0; i < 100; i++) {
        particles::step(run, 0.01f, ParticleEmitterFrame(), ParticleEnvironment());
        if (run.state.frame[0] != last) advances++;
        last = run.state.frame[0];
    }
    EXPECT_EQ(advances, 25) << "FPS 25 over 1 s";
    EXPECT_EQ((int)last, 25);

    // Lifetime: the frame over the particle's life, cycles times
    ParticleSystemComponent life = ps;
    life.main.startLifetime = MinMaxCurve(1.0f);
    life.textureSheetAnimation.numTilesX = 2;
    life.textureSheetAnimation.numTilesY = 2;
    life.textureSheetAnimation.timeMode = ParticleSystemAnimationTimeMode::Lifetime;
    life.textureSheetAnimation.cycles = 2;
    life = single(life, 0.3f, 100.0f);
    // t 0.3, two cycles: 0.6 of the 4 frames -> frame 2
    EXPECT_EQ((int)life.state.frame[0], 2);
    const Rect rect = particles::textureSheetRect(life.textureSheetAnimation, 2, 0);
    EXPECT_FLOAT_EQ(rect.getX(), 0.0f);
    EXPECT_FLOAT_EQ(rect.getY(), 0.5f);
    EXPECT_FLOAT_EQ(rect.getWidth(), 0.5f);

    // Speed: the speed range picks the frame
    ParticleSystemComponent speed = ps;
    speed.textureSheetAnimation.numTilesX = 4;
    speed.textureSheetAnimation.numTilesY = 1;
    speed.textureSheetAnimation.timeMode = ParticleSystemAnimationTimeMode::Speed;
    speed.textureSheetAnimation.speedRange = Vector2(0.0f, 4.0f);
    speed.main.startSpeed = MinMaxCurve(2.5f);
    speed.shape.enabled = true;
    speed.shape.shapeType = ParticleSystemShapeType::Cone;
    speed.shape.angle = 0.0f;
    speed.shape.radius = 0.0f;
    speed = single(speed, 0.1f, 60.0f);
    EXPECT_EQ((int)speed.state.frame[0], 2);
}

TEST(ParticleSimulation, PrewarmEqualsOneDuration) {
    ParticleSystemComponent ps = base();
    ps.main.duration = 1.0f;
    ps.main.looping = true;
    ps.main.startLifetime = MinMaxCurve(0.8f);
    ps.main.startSpeed = MinMaxCurve(1.0f);
    ps.shape.enabled = true;
    ps.emission.rateOverTime = MinMaxCurve(30.0f);
    ParticleSystemComponent warm = ps;
    warm.main.prewarm = true;
    particles::play(warm, ParticleEmitterFrame(), ParticleEnvironment());
    ParticleSystemComponent cold = ps;
    particles::play(cold, ParticleEmitterFrame(), ParticleEnvironment());
    for (int i = 0; i < 30; i++) particles::step(cold, 1.0f / 30.0f, ParticleEmitterFrame(), ParticleEnvironment());
    ASSERT_EQ(particles::particleCount(warm), particles::particleCount(cold));
    for (size_t i = 0; i < particles::particleCount(warm); i++) {
        EXPECT_NEAR((warm.state.position[i] - cold.state.position[i]).length(), 0.0f, 1e-4f);
    }
    EXPECT_GT(particles::particleCount(warm), 20u);
}

TEST(ParticleSimulation, StopBehaviours) {
    ParticleSystemComponent ps = base();
    ps.main.startLifetime = MinMaxCurve(0.5f);
    ps.emission.rateOverTime = MinMaxCurve(20.0f);
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    for (int i = 0; i < 20; i++) particles::step(ps, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
    ASSERT_GT(particles::particleCount(ps), 0u);

    // StopEmitting: the particles live out, then it stops
    ParticleSystemComponent emitting = ps;
    EXPECT_FALSE(particles::stop(emitting, ParticleSystemStopBehavior::StopEmitting));
    EXPECT_TRUE(emitting.state.playing);
    EXPECT_FALSE(emitting.state.emitting);
    bool finished = false;
    const size_t before = particles::particleCount(emitting);
    for (int i = 0; i < 60 && !finished; i++) finished = particles::step(emitting, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
    EXPECT_TRUE(finished);
    EXPECT_FALSE(emitting.state.playing);
    EXPECT_EQ(particles::particleCount(emitting), 0u);
    EXPECT_GT(before, 0u);

    // StopEmittingAndClear: stopped and empty now
    ParticleSystemComponent cleared = ps;
    EXPECT_TRUE(particles::stop(cleared, ParticleSystemStopBehavior::StopEmittingAndClear));
    EXPECT_FALSE(cleared.state.playing);
    EXPECT_EQ(particles::particleCount(cleared), 0u);

    // play() again restarts the clock
    particles::play(cleared, ParticleEmitterFrame(), ParticleEnvironment());
    EXPECT_TRUE(cleared.state.playing);
    EXPECT_FLOAT_EQ(cleared.state.time, 0.0f);
}

TEST(ParticleSimulation, NonLoopingFinishes) {
    ParticleSystemComponent ps = base();
    ps.main.looping = false;
    ps.main.duration = 0.5f;
    ps.main.startLifetime = MinMaxCurve(0.25f);
    ps.emission.rateOverTime = MinMaxCurve(20.0f);
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    bool finished = false;
    int frames = 0;
    while (!finished && frames < 600) {
        finished = particles::step(ps, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
        frames++;
    }
    EXPECT_TRUE(finished);
    // the emission ends at 0.5 s, the last particles die 0.25 s later
    EXPECT_NEAR(frames / 60.0f, 0.75f, 0.05f);
}

TEST(ParticleSimulation, FixedSeedReplaysExactly) {
    ParticleSystemComponent ps = base();
    ps.main.startLifetime = MinMaxCurve::between(0.5f, 1.5f);
    ps.main.startSpeed = MinMaxCurve::between(1.0f, 3.0f);
    ps.main.startColor = MinMaxGradient::between(Vector4(1, 0, 0, 1), Vector4(0, 1, 0, 1));
    ps.shape.enabled = true;
    ps.shape.shapeType = ParticleSystemShapeType::Sphere;
    ps.emission.rateOverTime = MinMaxCurve(40.0f);
    ParticleSystemComponent a = ps, b = ps;
    particles::play(a, ParticleEmitterFrame(), ParticleEnvironment());
    particles::play(b, ParticleEmitterFrame(), ParticleEnvironment());
    for (int i = 0; i < 120; i++) {
        particles::step(a, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
        particles::step(b, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
    }
    ASSERT_EQ(particles::particleCount(a), particles::particleCount(b));
    for (size_t i = 0; i < particles::particleCount(a); i++) {
        EXPECT_EQ(a.state.position[i], b.state.position[i]);
        EXPECT_EQ(a.state.color[i], b.state.color[i]);
    }
    // an auto seed differs from play to play
    ParticleSystemComponent c = ps, d = ps;
    c.main.autoRandomSeed = d.main.autoRandomSeed = true;
    particles::play(c, ParticleEmitterFrame(), ParticleEnvironment());
    particles::play(d, ParticleEmitterFrame(), ParticleEnvironment());
    EXPECT_NE(c.state.seedUsed, d.state.seedUsed);
}

TEST(ParticleSimulation, LocalSpaceMovesWithTheEmitterWorldStays) {
    ParticleSystemComponent local = base();
    ParticleSystemComponent world = base();
    world.main.simulationSpace = ParticleSystemSimulationSpace::World;
    ParticleEmitterFrame emitter;
    particles::play(local, emitter, ParticleEnvironment());
    particles::play(world, emitter, ParticleEnvironment());
    particles::emit(local, 1, emitter, ParticleEnvironment());
    particles::emit(world, 1, emitter, ParticleEnvironment());
    emitter.position = Vector3(5, 0, 0);
    particles::step(local, 0.1f, emitter, ParticleEnvironment());
    particles::step(world, 0.1f, emitter, ParticleEnvironment());
    // a local particle stays at the emitter's origin (now at x 5), a world one where it was
    EXPECT_EQ(local.state.position[0], Vector3(0, 0, 0));
    EXPECT_NEAR((particles::simulationToWorld(local, emitter) * local.state.position[0]).x, 5.0f, 1e-5f);
    EXPECT_EQ(world.state.position[0], Vector3(0, 0, 0));
}

// ------------------------------------------------------------------ ActionSystem

namespace {

    void step(Scene& scene, double dt, int count = 1) {
        for (int i = 0; i < count; i++) {
            scene.getSystem<ActionSystem>()->update(dt);
        }
    }

    Entity makeSystem(Scene& scene, const ParticleSystemComponent& values, Entity parent = NULL_ENTITY) {
        Entity entity = scene.createEntity();
        scene.addComponent<ActionComponent>(entity, ActionComponent());
        scene.addComponent<ParticleSystemComponent>(entity, values);
        ParticleSystem::setupDefaultRenderer(&scene, entity);
        if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
        return entity;
    }

}

TEST(ParticleSystemAction, PlayOnAwakeFillsTheInstances) {
    Scene scene;
    ParticleSystemComponent values = base();
    values.emission.rateOverTime = MinMaxCurve(10.0f);
    const Entity entity = makeSystem(scene, values);
    step(scene, 0.1, 10);
    const ParticleSystemComponent& ps = scene.getComponent<ParticleSystemComponent>(entity);
    EXPECT_TRUE(ps.state.playing);
    EXPECT_EQ(particles::particleCount(ps), 10u);
    EXPECT_EQ(scene.getComponent<ActionComponent>(entity).state, ActionState::Running);
    const InstancedMeshComponent& instmesh = scene.getComponent<InstancedMeshComponent>(entity);
    EXPECT_EQ(instmesh.instances.size(), 10u);
    EXPECT_EQ(instmesh.particle.alignment, InstanceAlignment::View);
    EXPECT_EQ(instmesh.maxInstances, values.main.maxParticles);
}

TEST(ParticleSystemAction, NoPlayOnAwakeWaits) {
    Scene scene;
    ParticleSystemComponent values = base();
    values.main.playOnAwake = false;
    values.emission.rateOverTime = MinMaxCurve(10.0f);
    const Entity entity = makeSystem(scene, values);
    step(scene, 0.1, 5);
    EXPECT_EQ(particles::particleCount(scene.getComponent<ParticleSystemComponent>(entity)), 0u);
    ParticleSystem ps(&scene, entity);
    EXPECT_TRUE(ps.isStopped());
    ps.play();
    EXPECT_TRUE(ps.isPlaying()) << "play() is immediate";
    step(scene, 0.1, 5);
    EXPECT_EQ(ps.getParticleCount(), 5u);
    // emit() while stopped: alive, not emitting
    ps.stop(true, ParticleSystemStopBehavior::StopEmittingAndClear);
    step(scene, 0.1);
    EXPECT_TRUE(ps.isStopped());
    EXPECT_EQ(ps.emit(3), 3);
    EXPECT_TRUE(ps.isAlive());
    EXPECT_FALSE(ps.isEmitting());
    step(scene, 0.1);
    EXPECT_EQ(ps.getParticleCount(), 3u);
}

TEST(ParticleSystemAction, StopActionDisableAndDestroy) {
    Scene scene;
    ParticleSystemComponent values = base();
    values.main.looping = false;
    values.main.duration = 0.2f;
    values.main.startLifetime = MinMaxCurve(0.2f);
    values.emission.rateOverTime = MinMaxCurve(50.0f);
    values.main.stopAction = ParticleSystemStopAction::Disable;
    const Entity disabled = makeSystem(scene, values);
    values.main.stopAction = ParticleSystemStopAction::Destroy;
    const Entity destroyed = makeSystem(scene, values);
    step(scene, 0.05, 20);
    // Destroy is GameObject.destroy: at the end of the frame
    ObjectManager::update(0.0);
    EXPECT_FALSE(scene.isEntityActive(disabled));
    EXPECT_FALSE(scene.isEntityCreated(destroyed));
}

TEST(ParticleSystemAction, ChildrenFollowPlayAndStop) {
    Scene scene;
    ParticleSystemComponent values = base();
    values.main.playOnAwake = false;
    values.emission.rateOverTime = MinMaxCurve(10.0f);
    const Entity root = makeSystem(scene, values);
    const Entity child = makeSystem(scene, values, root);
    ParticleSystem ps(&scene, root);
    ps.play(true);
    step(scene, 0.1, 3);
    EXPECT_TRUE(ParticleSystem(&scene, child).isPlaying());
    ps.stop(true, ParticleSystemStopBehavior::StopEmittingAndClear);
    step(scene, 0.1);
    EXPECT_TRUE(ParticleSystem(&scene, child).isStopped());
    // without children
    ps.play(false);
    step(scene, 0.1);
    EXPECT_TRUE(ps.isPlaying());
    EXPECT_TRUE(ParticleSystem(&scene, child).isStopped());
}

TEST(ParticleSystemAction, DeactivatedGameObjectClearsAndReplaysOnReactivation) {
    Scene scene;
    ParticleSystemComponent values = base();
    values.emission.rateOverTime = MinMaxCurve(10.0f);
    const Entity entity = makeSystem(scene, values);
    step(scene, 0.1, 5);
    ASSERT_EQ(particles::particleCount(scene.getComponent<ParticleSystemComponent>(entity)), 5u);
    scene.setEntityActive(entity, false);
    step(scene, 0.1);
    EXPECT_EQ(particles::particleCount(scene.getComponent<ParticleSystemComponent>(entity)), 0u);
    EXPECT_EQ(scene.getComponent<InstancedMeshComponent>(entity).instances.size(), 0u);
    scene.setEntityActive(entity, true);
    step(scene, 0.1, 3);
    EXPECT_TRUE(scene.getComponent<ParticleSystemComponent>(entity).state.playing);
    EXPECT_EQ(particles::particleCount(scene.getComponent<ParticleSystemComponent>(entity)), 3u);
}

TEST(ParticleSystemAction, AgeSortOrdersTheInstances) {
    // particles rising at 1 unit/s: the older the higher; the instances draw in their order
    ParticleSystemComponent values = base();
    values.emission.rateOverTime = MinMaxCurve(10.0f);
    values.main.startSpeed = MinMaxCurve(1.0f);
    values.shape.enabled = true;
    values.shape.shapeType = ParticleSystemShapeType::Cone;
    values.shape.angle = 0.0f;
    values.shape.radius = 0.0f;
    for (ParticleSystemSortMode mode : {ParticleSystemSortMode::None, ParticleSystemSortMode::YoungestInFront, ParticleSystemSortMode::OldestInFront}) {
        Scene scene;
        values.renderer.sortMode = mode;
        const Entity entity = makeSystem(scene, values);
        step(scene, 0.1, 6);
        const InstancedMeshComponent& instmesh = scene.getComponent<InstancedMeshComponent>(entity);
        ASSERT_GE(instmesh.instances.size(), 3u);
        const float first = instmesh.instances.front().position.y, last = instmesh.instances.back().position.y;
        if (mode == ParticleSystemSortMode::OldestInFront) {
            EXPECT_LT(first, last) << "the oldest (highest) drawn last";
        } else {
            EXPECT_GT(first, last) << "spawn order: the youngest drawn last";
        }
        EXPECT_EQ(instmesh.particle.sort, InstanceSortMode::Keep);
    }
}
