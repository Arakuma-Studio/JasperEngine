// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// "Upgrade to modules" (particles/ParticleUpgrade, spec 6.7, 9): each mapping rule, exact cases
// simulating to the statistics of version 1 (mean and spread of the positions at 1 s within
// 2 %), approximated modifiers reporting their error, a position modifier refusing the upgrade
// unless forced.

#include <gtest/gtest.h>

#include "Scene.h"
#include "particles/ParticleSimulation.h"
#include "particles/ParticleUpgrade.h"
#include "subsystem/ActionSystem.h"
#include "component/ActionComponent.h"
#include "component/PointsComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "action/ParticleSystem.h"
#include "component/Transform.h"
#include "util/Color.h"

#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    // many particles: the statistics of two random streams agree within a few per mille
    ParticleSystemComponent legacy() {
        ParticleSystemComponent p;
        p.maxParticles = 20000;
        p.rate = 20000;
        p.maxPerUpdate = 1000;
        p.loop = true;
        p.lifeInitializer.minLife = 2.0f;
        p.lifeInitializer.maxLife = 2.0f;
        return p;
    }

    struct Stats {
        Vector3 mean;
        Vector3 spread;   // standard deviation per axis
        size_t count = 0;
    };

    Stats stats(const std::vector<Vector3>& positions) {
        Stats s;
        s.count = positions.size();
        if (positions.empty()) return s;
        for (const Vector3& p : positions) s.mean += p;
        s.mean = s.mean / (float)positions.size();
        Vector3 var;
        for (const Vector3& p : positions) {
            const Vector3 d = p - s.mean;
            var += Vector3(d.x * d.x, d.y * d.y, d.z * d.z);
        }
        var = var / (float)positions.size();
        s.spread = Vector3(std::sqrt(var.x), std::sqrt(var.y), std::sqrt(var.z));
        return s;
    }

    // the legacy system on a Points target at the origin, 1 s at 60 fps
    std::vector<Vector3> runLegacy(const ParticleSystemComponent& values) {
        Scene scene;
        Entity target = scene.createEntity();
        scene.addComponent<Transform>(target, Transform());
        scene.addComponent<PointsComponent>(target, PointsComponent());
        Entity system = scene.createEntity();
        ActionComponent action;
        action.target = target;
        action.startTrigger = true;
        scene.addComponent<ActionComponent>(system, action);
        scene.addComponent<ParticleSystemComponent>(system, values);
        std::srand(3);
        for (int i = 0; i < 60; i++) scene.getSystem<ActionSystem>()->update(1.0 / 60.0);
        std::vector<Vector3> out;
        for (const PointData& p : scene.getComponent<PointsComponent>(target).points) {
            if (p.visible) out.push_back(p.position);
        }
        return out;
    }

    std::vector<Vector3> runUpgraded(const ParticleSystemComponent& values) {
        ParticleSystemComponent ps = upgradeLegacy(values, true).component;
        ps.main.autoRandomSeed = false;
        ps.main.randomSeed = 5;
        particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
        for (int i = 0; i < 60; i++) particles::step(ps, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
        return ps.state.position;
    }

    void expectSameStatistics(const ParticleSystemComponent& values, float tolerance) {
        const Stats a = stats(runLegacy(values));
        const Stats b = stats(runUpgraded(values));
        ASSERT_GT(a.count, 10000u);
        EXPECT_NEAR((float)b.count, (float)a.count, (float)a.count * 0.05f + 1.0f);
        const float scale = std::max(1.0f, std::max(a.spread.length(), a.mean.length()));
        EXPECT_NEAR(b.mean.x, a.mean.x, tolerance * scale);
        EXPECT_NEAR(b.mean.y, a.mean.y, tolerance * scale);
        EXPECT_NEAR(b.mean.z, a.mean.z, tolerance * scale);
        EXPECT_NEAR(b.spread.x, a.spread.x, tolerance * scale);
        EXPECT_NEAR(b.spread.y, a.spread.y, tolerance * scale);
        EXPECT_NEAR(b.spread.z, a.spread.z, tolerance * scale);
    }

}

TEST(ParticleUpgrade, TheSystemFields) {
    ParticleSystemComponent p = legacy();
    p.localSpace = true;
    p.bursts = {{0.5f, 3, 6}};
    const UpgradeResult r = upgradeLegacy(p, true);
    const ParticleSystemComponent& c = r.component;
    EXPECT_EQ(c.version, 2);
    EXPECT_TRUE(r.report.upgraded);
    EXPECT_EQ(c.main.maxParticles, 20000u);
    EXPECT_TRUE(c.main.looping);
    EXPECT_FLOAT_EQ(c.main.duration, 1.0f) << "the legacy cycle: maxParticles / rate";
    EXPECT_FLOAT_EQ(c.emission.rateOverTime.evaluate(0, 0), 20000.0f);
    EXPECT_EQ(c.main.simulationSpace, ParticleSystemSimulationSpace::Local);
    EXPECT_EQ(c.main.scalingMode, ParticleSystemScalingMode::Hierarchy);
    EXPECT_FLOAT_EQ(c.main.startSpeed.evaluate(0, 1), 0.0f);
    ASSERT_EQ(c.emission.bursts.size(), 1u);
    EXPECT_FLOAT_EQ(c.emission.bursts[0].time, 0.5f);
    // the rounded counts cover 3..6 evenly
    EXPECT_EQ((int)std::floor(c.emission.bursts[0].count.evaluate(0, 0.0f) + 0.5f), 3);
    EXPECT_EQ((int)std::floor(c.emission.bursts[0].count.evaluate(0, 0.999f) + 0.5f), 6);
    EXPECT_FALSE(r.report.hasStatus(UpgradeStatus::Dropped));

    // a world system: positions scaled at spawn only (Shape scaling)
    p.localSpace = false;
    const UpgradeResult w = upgradeLegacy(p, true);
    EXPECT_EQ(w.component.main.simulationSpace, ParticleSystemSimulationSpace::World);
    EXPECT_EQ(w.component.main.scalingMode, ParticleSystemScalingMode::Shape);
}

TEST(ParticleUpgrade, ShapesAndTheirStatistics) {
    // box
    ParticleSystemComponent box = legacy();
    box.positionInitializer.shape = ParticleEmitterShape::Box;
    box.positionInitializer.minPosition = Vector3(-2, 0, -1);
    box.positionInitializer.maxPosition = Vector3(2, 1, 3);
    const UpgradeResult b = upgradeLegacy(box, true);
    EXPECT_EQ(b.component.shape.shapeType, ParticleSystemShapeType::Box);
    EXPECT_EQ(b.component.shape.position, Vector3(0, 0.5f, 1));
    EXPECT_EQ(b.component.shape.scale, Vector3(4, 1, 4));
    EXPECT_EQ(b.report.find("positionInitializer")->status, UpgradeStatus::Exact);
    expectSameStatistics(box, 0.02f);

    // sphere with an inner radius
    ParticleSystemComponent sphere = legacy();
    sphere.positionInitializer.shape = ParticleEmitterShape::Sphere;
    sphere.positionInitializer.radius = 2.0f;
    sphere.positionInitializer.innerRadius = 1.0f;
    const UpgradeResult s = upgradeLegacy(sphere, true);
    EXPECT_EQ(s.component.shape.shapeType, ParticleSystemShapeType::Sphere);
    EXPECT_FLOAT_EQ(s.component.shape.radius, 2.0f);
    EXPECT_FLOAT_EQ(s.component.shape.radiusThickness, 0.5f);
    expectSameStatistics(sphere, 0.03f);

    // circle and hemisphere keep their axes
    ParticleSystemComponent circle = legacy();
    circle.positionInitializer.shape = ParticleEmitterShape::Circle;
    circle.positionInitializer.radius = 1.5f;
    EXPECT_EQ(upgradeLegacy(circle, true).component.shape.shapeType, ParticleSystemShapeType::Circle);
    expectSameStatistics(circle, 0.03f);

    // the legacy cone: an approximation (reported)
    ParticleSystemComponent cone = legacy();
    cone.positionInitializer.shape = ParticleEmitterShape::Cone;
    cone.positionInitializer.coneAngle = 30.0f;
    cone.positionInitializer.coneHeight = 2.0f;
    const UpgradeResult c = upgradeLegacy(cone, true);
    EXPECT_EQ(c.component.shape.shapeType, ParticleSystemShapeType::ConeVolume);
    EXPECT_FLOAT_EQ(c.component.shape.length, 2.0f);
    EXPECT_EQ(c.report.find("positionInitializer")->status, UpgradeStatus::Approximated);
}

TEST(ParticleUpgrade, VelocityAndAccelerationAreExact) {
    ParticleSystemComponent p = legacy();
    p.localSpace = true;
    p.velocityInitializer.minVelocity = Vector3(-1, 2, 0);
    p.velocityInitializer.maxVelocity = Vector3(1, 3, 0);
    p.accelerationInitializer.minAcceleration = Vector3(0, -2, 0);
    p.accelerationInitializer.maxAcceleration = Vector3(0, -2, 0);
    const UpgradeResult r = upgradeLegacy(p, true);
    EXPECT_TRUE(r.component.velocityOverLifetime.enabled);
    EXPECT_EQ(r.component.velocityOverLifetime.x.mode, ParticleSystemCurveMode::TwoConstants);
    EXPECT_TRUE(r.component.forceOverLifetime.enabled);
    EXPECT_EQ(r.report.find("velocityInitializer")->status, UpgradeStatus::Exact);
    EXPECT_EQ(r.report.find("accelerationInitializer")->status, UpgradeStatus::Exact);
    expectSameStatistics(p, 0.02f);
}

TEST(ParticleUpgrade, ColoursAndTheGradient) {
    ParticleSystemComponent p = legacy();
    p.colorInitializer.minColor = Vector3(1, 0.5f, 0);
    p.colorInitializer.maxColor = Vector3(1, 0.5f, 0);
    p.alphaInitializer.minAlpha = 0.5f;
    p.alphaInitializer.maxAlpha = 0.5f;
    const UpgradeResult r = upgradeLegacy(p, true);
    const Vector3 linear = Color::sRGBToLinear(Vector3(1, 0.5f, 0));
    EXPECT_EQ(r.component.main.startColor.mode, ParticleSystemGradientMode::Color);
    EXPECT_NEAR(r.component.main.startColor.color.y, linear.y, 1e-6f);
    EXPECT_FLOAT_EQ(r.component.main.startColor.color.w, 0.5f);

    // a gradient replaces the start colour's rgb and becomes colour over lifetime
    p.colorGradient.stops = {{0.0f, Vector3(1, 0, 0)}, {1.0f, Vector3(0, 0, 1)}};
    const UpgradeResult g = upgradeLegacy(p, true);
    EXPECT_TRUE(g.component.colorOverLifetime.enabled);
    EXPECT_EQ(g.component.main.startColor.color.x, 1.0f);
    EXPECT_EQ(g.component.main.startColor.color.y, 1.0f);
    ASSERT_NE(g.report.find("colorGradient"), nullptr);
    const Vector4 end = g.component.colorOverLifetime.color.evaluate(1.0f, 0.0f);
    EXPECT_NEAR(end.z, 1.0f, 1e-5f);
}

TEST(ParticleUpgrade, ModifiersBecomeCurvesWithTheirError) {
    // a linear size modifier over the whole (constant) life: exact
    ParticleSystemComponent p = legacy();
    p.sizeModifier.fromTime = 0.0f;
    p.sizeModifier.toTime = 2.0f;
    p.sizeModifier.fromSize = 1.0f;
    p.sizeModifier.toSize = 3.0f;
    const UpgradeResult exact = upgradeLegacy(p, true);
    EXPECT_TRUE(exact.component.sizeOverLifetime.enabled);
    EXPECT_NEAR(exact.component.sizeOverLifetime.size.evaluate(0.5f, 0.0f), 2.0f, 1e-4f);
    EXPECT_EQ(exact.report.find("sizeModifier")->status, UpgradeStatus::Exact);

    // the life varies: approximated at the mean life, the error reported
    p.lifeInitializer.minLife = 1.0f;
    p.lifeInitializer.maxLife = 3.0f;
    const UpgradeResult approx = upgradeLegacy(p, true);
    const UpgradeItem* item = approx.report.find("sizeModifier");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->status, UpgradeStatus::Approximated);
    EXPECT_GT(item->maxError, 0.1f);

    // an alpha modifier: the colour over lifetime's alpha keys
    ParticleSystemComponent a = legacy();
    a.alphaModifier.fromTime = 0.0f;
    a.alphaModifier.toTime = 1.0f;
    a.alphaModifier.fromAlpha = 1.0f;
    a.alphaModifier.toAlpha = 0.0f;
    const UpgradeResult alpha = upgradeLegacy(a, true);
    EXPECT_TRUE(alpha.component.colorOverLifetime.enabled);
    // half way through the 2 s life the modifier has ended at 0
    EXPECT_NEAR(alpha.component.colorOverLifetime.color.evaluate(0.5f, 0.0f).w, 0.0f, 1e-3f);
    EXPECT_NEAR(alpha.component.colorOverLifetime.color.evaluate(0.25f, 0.0f).w, 0.5f, 0.02f);
}

TEST(ParticleUpgrade, PositionModifierNeedsForce) {
    ParticleSystemComponent p = legacy();
    p.positionModifier.fromTime = 0.0f;
    p.positionModifier.toTime = 1.0f;
    p.positionModifier.toPosition = Vector3(0, 5, 0);
    const UpgradeResult refused = upgradeLegacy(p, true, false);
    EXPECT_FALSE(refused.report.upgraded);
    EXPECT_EQ(refused.component.version, 1);
    EXPECT_EQ(refused.report.find("positionModifier")->status, UpgradeStatus::Dropped);
    const UpgradeResult forced = upgradeLegacy(p, true, true);
    EXPECT_TRUE(forced.report.upgraded);
    EXPECT_EQ(forced.component.version, 2);
}

TEST(ParticleUpgrade, MeshTargetsUseTheScale) {
    ParticleSystemComponent p = legacy();
    p.scaleInitializer.minScale = Vector3(1, 2, 3);
    p.scaleInitializer.maxScale = Vector3(1, 2, 3);
    const UpgradeResult r = upgradeLegacy(p, false);
    EXPECT_TRUE(r.component.main.startSize3D);
    EXPECT_FLOAT_EQ(r.component.main.startSizeY.evaluate(0, 0), 2.0f);
    EXPECT_FLOAT_EQ(r.component.main.startSizeZ.evaluate(0, 0), 3.0f);
}

// what the legacy target drew decides the render mode (a mesh stays a mesh, not a billboard)
TEST(ParticleUpgrade, TheTargetDecidesTheRenderMode) {
    ParticleSystemComponent p = legacy();
    p.localSpace = true;
    const UpgradeResult mesh = upgradeLegacy(p, LegacyTarget::Mesh);
    EXPECT_EQ(mesh.component.renderer.renderMode, ParticleSystemRenderMode::Mesh);
    EXPECT_EQ(mesh.component.renderer.alignment, ParticleSystemRenderSpace::Local);
    EXPECT_FLOAT_EQ(mesh.component.renderer.maxParticleSize, 0.0f) << "no on-screen size limit";
    ASSERT_NE(mesh.report.find("renderer"), nullptr);
    EXPECT_EQ(mesh.report.find("renderer")->status, UpgradeStatus::Exact);
    p.localSpace = false;
    EXPECT_EQ(upgradeLegacy(p, LegacyTarget::Mesh).component.renderer.alignment, ParticleSystemRenderSpace::World);
    EXPECT_EQ(upgradeLegacy(p, LegacyTarget::Billboard).component.renderer.renderMode, ParticleSystemRenderMode::Billboard);
    EXPECT_EQ(upgradeLegacy(p, LegacyTarget::VerticalBillboard).component.renderer.renderMode, ParticleSystemRenderMode::VerticalBillboard);
    EXPECT_EQ(upgradeLegacy(p, LegacyTarget::Points).component.renderer.renderMode, ParticleSystemRenderMode::Billboard);
}

TEST(ParticleUpgrade, MeshTargetsInAScene) {
    Scene scene;
    const Entity target = scene.createEntity();
    scene.addComponent<Transform>(target, Transform());
    scene.addComponent<MeshComponent>(target, MeshComponent());
    scene.addComponent<InstancedMeshComponent>(target, InstancedMeshComponent());
    EXPECT_EQ(ParticleSystem::legacyTargetOf(&scene, target), LegacyTarget::Mesh);
    scene.getComponent<InstancedMeshComponent>(target).instancedBillboard = true;
    EXPECT_EQ(ParticleSystem::legacyTargetOf(&scene, target), LegacyTarget::Billboard);
    scene.getComponent<InstancedMeshComponent>(target).instancedCylindricalBillboard = true;
    EXPECT_EQ(ParticleSystem::legacyTargetOf(&scene, target), LegacyTarget::VerticalBillboard);
    scene.getComponent<InstancedMeshComponent>(target).instancedBillboard = false;
    scene.getComponent<InstancedMeshComponent>(target).instancedCylindricalBillboard = false;

    const Entity system = scene.createEntity();
    ActionComponent action;
    action.target = target;
    scene.addComponent<ActionComponent>(system, action);
    ParticleSystemComponent p = legacy();
    p.localSpace = false;
    scene.addComponent<ParticleSystemComponent>(system, p);
    ParticleSystem(&scene, system).upgrade(true);
    const ParticleSystemComponent& c = scene.getComponent<ParticleSystemComponent>(system);
    EXPECT_EQ(c.version, 2);
    EXPECT_EQ(c.renderer.renderMode, ParticleSystemRenderMode::Mesh);
    EXPECT_EQ(c.renderer.alignment, ParticleSystemRenderSpace::World);
}

// a legacy loop whose last burst is its cycle: that burst still fires every loop
TEST(ParticleUpgrade, ALastBurstAtTheCycleEndKeepsFiring) {
    ParticleSystemComponent p;
    p.maxParticles = 1000;
    p.rate = 0;
    p.loop = true;
    p.lifeInitializer.minLife = 100.0f;
    p.lifeInitializer.maxLife = 100.0f;
    p.bursts = {{0.0f, 1, 1}, {1.0f, 2, 2}};
    UpgradeResult r = upgradeLegacy(p, true);
    ParticleSystemComponent& c = r.component;
    EXPECT_FLOAT_EQ(c.main.duration, 1.0f);
    c.main.autoRandomSeed = false;
    particles::play(c, ParticleEmitterFrame(), ParticleEnvironment());
    for (int i = 0; i < 150; i++) particles::step(c, 1.0f / 60.0f, ParticleEmitterFrame(), ParticleEnvironment());
    EXPECT_EQ(particles::particleCount(c), 7u) << "1 at 0 s, 2 at 1 s, 1 at 1 s (the next loop), 2 at 2 s, 1 at 2 s";
}
