// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// origin/main's version 2 Particle Systems read into S20 (stream/UpstreamParticles,
// docs/specs/S20-particles.md, Upstream absorption): every module field of the version 2 YAML
// with its conversion (radians to degrees, linear colours to sRGB, a range's order, the shape's
// Euler order, the shape types and render alignments, the defaults that differ), the quad
// material in the Renderer module, and the scene and prefab migration around it (the target's
// MeshComponent, InstancedMesh and Points, prefab instance overrides). Ported from origin/main's
// ParticleStreamTest (the version 2 encoding: MinMax forms, every field) and ParticleUpgradeTest
// (the targets and their render modes); the real upstream-saved files are
// data_safety/UpstreamParticleFilesTest.cpp.

#include "Stream.h"
#include "StreamMigrations.h"
#include "component/ParticleSystemComponent.h"
#include "particles/ParticleEffectCodec.h"
#include "stream/UpstreamParticles.h"
#include "util/Color.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;
namespace up = jasperengine::editor::upstreamparticles;

namespace {

    constexpr float kDeg = 57.29577951308232f;

    // origin/main's own quad (library:unlit with its particle texture): nothing to note about it
    up::Target quadTarget(){
        up::Target target;
        target.kind = up::Target::Kind::Mesh;
        target.material.shader.path = "library:unlit";
        target.material.shaderFeatures = {"PARTICLE_TEXTURE"};
        target.material.blendMode = MaterialBlendMode::ALPHA;
        target.castShadows = false;
        return target;
    }

    up::Result convertText(const std::string& yaml, const up::Target& target = quadTarget()){
        return up::convert(YAML::Load(yaml), target);
    }

    float srgb(float linear){
        return Color::linearTosRGB(Vector3(linear, linear, linear)).x;
    }

    // the 3x3 turn (column-major) of S20's shape Euler degrees: Ry Rx Rz
    void s20Turn(const Vector3& e, const Vector3& v, Vector3& out){
        const float r = 1.0f / kDeg;
        const float cx = std::cos(e.x * r), sx = std::sin(e.x * r), cy = std::cos(e.y * r), sy = std::sin(e.y * r);
        const float cz = std::cos(e.z * r), sz = std::sin(e.z * r);
        const Vector3 a(cz * v.x - sz * v.y, sz * v.x + cz * v.y, v.z);
        const Vector3 b(a.x, cx * a.y - sx * a.z, sx * a.y + cx * a.z);
        out = Vector3(cy * b.x + sy * b.z, b.y, -sy * b.x + cy * b.z);
    }

    // origin/main's: Rz Ry Rx (X first)
    void upstreamTurn(const Vector3& e, const Vector3& v, Vector3& out){
        const float r = 1.0f / kDeg;
        const float cx = std::cos(e.x * r), sx = std::sin(e.x * r), cy = std::cos(e.y * r), sy = std::sin(e.y * r);
        const float cz = std::cos(e.z * r), sz = std::sin(e.z * r);
        const Vector3 a(v.x, cx * v.y - sx * v.z, sx * v.y + cx * v.z);
        const Vector3 b(cy * a.x + sy * a.z, a.y, -sy * a.x + cy * a.z);
        out = Vector3(cz * b.x - sz * b.y, sz * b.x + cz * b.y, b.z);
    }

    ParticleEffectDocument effectOf(const YAML::Node& component){
        ParticleEffectDocument d;
        std::vector<Diagnostic> diagnostics;
        EXPECT_TRUE(ParticleEffectCodec::read(component["inlineEffect"].as<std::string>(), d, diagnostics))
            << ParticleEffectCodec::format(diagnostics);
        return d;
    }

}

TEST(UpstreamParticles, RecognisesVersionTwo){
    EXPECT_TRUE(up::isVersion2(YAML::Load("{enabled: true, version: 2}")));
    EXPECT_FALSE(up::isVersion2(YAML::Load("{rate: 10, maxParticles: 50}"))) << "a version 1 (legacy) system";
    EXPECT_FALSE(up::isVersion2(YAML::Load("{inlineEffect: '{}', playOnAwake: true}"))) << "S20's own";
    EXPECT_FALSE(up::isVersion2(YAML::Load("{version: 1, rate: 3}")));
}

TEST(UpstreamParticles, MinMaxForms){
    // a number, two constants (min, max), a curve times its multiplier, two curves; rotations in
    // radians there, degrees here
    const up::Result r = convertText(
        "{version: 2, main: {startLifetime: 3,"
        " startSpeed: {mode: two_constants, min: 2, max: 6},"
        " startSize: {mode: curve, multiplier: 2, curve: {keys: [[0, 0.5], [1, 1]]}},"
        " startRotation: {mode: two_curves, multiplier: 1.5, min: {keys: [[0, 0]]}, max: {keys: [[0, 1], [1, 0]]}},"
        " gravityModifier: {mode: curve, multiplier: 1, curve: {keys: [[0, 0], [1, 1]]}}}}");
    const MainModule& m = r.effect.main;
    EXPECT_EQ(m.startLifetime.mode, CurveMode::Constant);
    EXPECT_FLOAT_EQ(m.startLifetime.constant, 3.0f);
    EXPECT_EQ(m.startSpeed.mode, CurveMode::TwoConstants);
    EXPECT_FLOAT_EQ(m.startSpeed.constant, 2.0f);
    EXPECT_FLOAT_EQ(m.startSpeed.constantMax, 6.0f);
    EXPECT_EQ(m.startSize.mode, CurveMode::Curve);
    EXPECT_FLOAT_EQ(m.startSize.multiplier, 2.0f);
    EXPECT_FLOAT_EQ(m.startSize.curve.evaluate(0.0f), 0.5f);
    EXPECT_EQ(m.startRotation.mode, CurveMode::TwoCurves);
    EXPECT_NEAR(m.startRotation.multiplier, 1.5f * kDeg, 1e-3f);
    EXPECT_FLOAT_EQ(m.startRotation.curve.evaluate(0.0f), 0.0f) << "the min curve first";
    EXPECT_FLOAT_EQ(m.startRotation.curveMax.evaluate(0.0f), 1.0f);
    // Gravity Modifier's curve runs over the emitter's time there
    EXPECT_EQ(m.gravityModifier.driver, CurveDriver::EmitterTime);
    EXPECT_TRUE(r.notes.empty()) << r.notes.front();
}

TEST(UpstreamParticles, GradientForms){
    // linear colours there ([r, g, b, a] arrays; hex is sRGB already), sRGB here
    const up::Result r = convertText(
        "{version: 2, main: {startColor: {mode: two_colors, min: '#FF8000', max: [0.5, 0.25, 1, 0.5]}},"
        " colorOverLifetime: {enabled: true, color: {mode: gradient, gradient: {mode: fixed,"
        "  colors: [[0, '#FFFFFF'], [1, [0, 0, 0.5]]], alphas: [[0, 1], [1, 0]]}}},"
        " colorBySpeed: {enabled: true, color: {mode: random_color, gradient: {colors: [[0, '#000000'], [1, '#FFFFFF']], alphas: [[0, 1]]}},"
        "  range: [1, 4]}}");
    const MinMaxGradient& start = r.effect.main.startColor;
    EXPECT_EQ(start.mode, GradientValueMode::TwoColors);
    EXPECT_NEAR(start.color.x, 1.0f, 1e-6f);
    EXPECT_NEAR(start.color.y, 128.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(start.colorMax.x, srgb(0.5f), 1e-5f);
    EXPECT_NEAR(start.colorMax.y, srgb(0.25f), 1e-5f);
    EXPECT_NEAR(start.colorMax.w, 0.5f, 1e-6f);
    const ColorOverLifetimeModule& col = r.effect.colorOverLifetime;
    EXPECT_TRUE(col.present && col.enabled);
    EXPECT_EQ(col.color.mode, GradientValueMode::Gradient);
    EXPECT_EQ(col.color.gradient.mode, GradientMode::Fixed);
    ASSERT_EQ(col.color.gradient.colorKeys.size(), 2u);
    EXPECT_NEAR(col.color.gradient.colorKeys[1].color.z, srgb(0.5f), 1e-5f);
    EXPECT_NEAR(col.color.gradient.evaluateAlpha(1.0f), 0.0f, 1e-6f);
    EXPECT_EQ(r.effect.colorBySpeed.color.mode, GradientValueMode::RandomColor);
    EXPECT_EQ(r.effect.colorBySpeed.speedRange, Vector2(1.0f, 4.0f));
}

TEST(UpstreamParticles, EveryModuleField){
    const up::Result r = convertText(
        "{enabled: true, version: 2, useUnscaledTime: true,"
        " main: {duration: 3, looping: false, prewarm: true, startDelay: {mode: two_constants, min: 0.1, max: 0.4},"
        "  startSize3D: true, startSizeY: 2, startSizeZ: 3, startRotation3D: true, startRotationX: 1, startRotationY: -1,"
        "  flipRotation: 0.25, gravitySource: Physics2D, simulationSpace: World, simulationSpeed: 1.5, scalingMode: Shape,"
        "  playOnAwake: false, maxParticles: 321, emitterVelocityMode: Rigidbody, stopAction: Callback,"
        "  autoRandomSeed: false, randomSeed: 77},"
        " emission: {enabled: false, rateOverTime: 25, rateOverDistance: 2,"
        "  bursts: [{time: 0.5, count: {mode: two_constants, min: 3, max: 6}, cycles: 0, interval: 0.2, probability: 0.75}]},"
        " shape: {shapeType: BoxEdge, radius: 0.5, radiusThickness: 0.5, radiusMode: PingPong, radiusSpread: 0.1, radiusSpeed: 2,"
        "  angle: 30, length: 4, arc: 180, arcMode: BurstSpread, arcSpread: 0.25, arcSpeed: 3, boxThickness: [0.1, 0.2, 0.3],"
        "  donutRadius: 0.4, position: [1, 2, 3], scale: [2, 1, 3], alignToDirection: true, randomDirectionAmount: 0.2,"
        "  sphericalDirectionAmount: 0.1, randomPositionAmount: 0.05},"
        " velocityOverLifetime: {enabled: true, x: 1, y: 2, z: 3, orbitalX: 1, orbitalY: 2, orbitalZ: 3,"
        "  orbitalOffsetX: 0.5, orbitalOffsetY: 0.25, orbitalOffsetZ: 0.125, radial: 0.25, speedModifier: 2, space: World},"
        " limitVelocityOverLifetime: {enabled: true, separateAxes: true, limitX: 3, limitY: 4, limitZ: 5, space: World, dampen: 0.5,"
        "  drag: 0.1, multiplyDragByParticleSize: false},"
        " inheritVelocity: {enabled: true, mode: Current, curve: 0.5},"
        " forceOverLifetime: {enabled: true, x: 1, y: -2, z: 3, space: World, randomized: true},"
        " sizeOverLifetime: {enabled: true, separateAxes: true, size: 2, y: 3, z: 4},"
        " sizeBySpeed: {enabled: true, size: 0.5, range: [2, 8]},"
        " rotationOverLifetime: {enabled: true, separateAxes: true, x: 1, y: 2, z: 3},"
        " rotationBySpeed: {enabled: true, separateAxes: true, x: 0.5, y: 1, z: 1.5, range: [0, 10]},"
        " noise: {enabled: true, separateAxes: true, strength: 0.5, strengthY: 0.6, strengthZ: 0.7, frequency: 1.5, scrollSpeed: 0.3,"
        "  damping: false, octaves: 3, octaveMultiplier: 0.4, octaveScale: 3, positionAmount: 0.8, rotationAmount: 0.2, sizeAmount: 0.1},"
        " textureSheetAnimation: {enabled: true, numTilesX: 4, numTilesY: 2, animation: SingleRow, rowMode: Random, rowIndex: 1,"
        "  timeMode: Speed, fps: 12, speedRange: [1, 5], startFrame: 2, cycles: 3, flipU: 0.5, flipV: 0.25},"
        " renderer: {renderMode: Stretch, cameraVelocityScale: 0.1, velocityScale: 0.5, lengthScale: 3, sortMode: YoungestInFront,"
        "  sortingFudge: 2, sortPriority: 4, minParticleSize: 0.01, maxParticleSize: 0.8, pivot: [0, 0.5, 0], flip: [0.5, 0, 0]}}");
    const ParticleEffectDocument& d = r.effect;
    EXPECT_TRUE(d.useUnscaledTime);
    EXPECT_FLOAT_EQ(d.duration, 3.0f);
    EXPECT_FALSE(d.looping);
    EXPECT_TRUE(d.prewarm);
    EXPECT_EQ(d.startDelay.mode, CurveMode::TwoConstants);
    EXPECT_FLOAT_EQ(d.startDelay.constant, 0.1f);
    EXPECT_FLOAT_EQ(d.startDelay.constantMax, 0.4f);
    EXPECT_TRUE(d.main.startSize3D);
    EXPECT_FLOAT_EQ(d.main.startSizeZ.constant, 3.0f);
    EXPECT_TRUE(d.main.startRotation3D);
    EXPECT_NEAR(d.main.startRotationX.constant, kDeg, 1e-3f);
    EXPECT_NEAR(d.main.startRotationY.constant, -kDeg, 1e-3f);
    EXPECT_FLOAT_EQ(d.main.flipRotation, 0.25f);
    EXPECT_EQ(d.main.gravitySource, GravitySource::Physics2D);
    EXPECT_EQ(d.simulationSpace, SimulationSpace::World);
    EXPECT_FLOAT_EQ(d.simulationSpeed, 1.5f);
    EXPECT_EQ(d.scalingMode, ScalingMode::Shape);
    EXPECT_FALSE(r.playOnAwake);
    EXPECT_EQ(d.maxParticles, 321u);
    EXPECT_EQ(d.emitterVelocityMode, EmitterVelocityMode::Rigidbody);
    EXPECT_EQ(d.stopAction, StopAction::Callback);
    EXPECT_EQ(r.seed, 77u);

    EXPECT_FALSE(d.emission.enabled);
    EXPECT_FLOAT_EQ(d.emission.rateOverDistance.constant, 2.0f);
    ASSERT_EQ(d.emission.bursts.size(), 1u);
    EXPECT_FLOAT_EQ(d.emission.bursts[0].time, 0.5f);
    EXPECT_FLOAT_EQ(d.emission.bursts[0].count.constantMax, 6.0f);
    EXPECT_EQ(d.emission.bursts[0].cycles, 0);
    EXPECT_FLOAT_EQ(d.emission.bursts[0].interval, 0.2f);
    EXPECT_FLOAT_EQ(d.emission.bursts[0].probability, 0.75f);

    const ShapeModule& s = d.shape;
    EXPECT_EQ(s.shape, ShapeType::Box);
    EXPECT_EQ(s.emitFrom, ShapeEmitFrom::Edge);
    EXPECT_FLOAT_EQ(s.radiusThickness, 0.5f);
    EXPECT_EQ(s.radiusMode, ShapeMultiMode::PingPong);
    EXPECT_FLOAT_EQ(s.radiusSpread, 0.1f);
    EXPECT_FLOAT_EQ(s.radiusSpeed.constant, 2.0f);
    EXPECT_FLOAT_EQ(s.angle, 30.0f);
    EXPECT_FLOAT_EQ(s.length, 4.0f);
    EXPECT_FLOAT_EQ(s.arc, 180.0f);
    EXPECT_EQ(s.arcMode, ShapeMultiMode::BurstSpread);
    EXPECT_FLOAT_EQ(s.arcSpread, 0.25f);
    EXPECT_FLOAT_EQ(s.arcSpeed.constant, 3.0f);
    EXPECT_EQ(s.boxThickness, Vector3(0.1f, 0.2f, 0.3f));
    EXPECT_FLOAT_EQ(s.donutRadius, 0.4f);
    EXPECT_EQ(s.position, Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(s.scale, Vector3(2.0f, 1.0f, 3.0f));
    EXPECT_TRUE(s.alignToDirection);
    EXPECT_FLOAT_EQ(s.randomizeDirection, 0.2f);
    EXPECT_FLOAT_EQ(s.spherizeDirection, 0.1f);
    EXPECT_FLOAT_EQ(s.randomizePosition, 0.05f);

    const VelocityOverLifetimeModule& v = d.velocityOverLifetime;
    EXPECT_TRUE(v.present && v.enabled);
    EXPECT_FLOAT_EQ(v.z.constant, 3.0f);
    EXPECT_NEAR(v.orbitalY.constant, 2.0f * kDeg, 1e-3f);
    EXPECT_FLOAT_EQ(v.offsetX.constant, 0.5f);
    EXPECT_FLOAT_EQ(v.offsetZ.constant, 0.125f);
    EXPECT_FLOAT_EQ(v.radial.constant, 0.25f);
    EXPECT_FLOAT_EQ(v.speedModifier.constant, 2.0f);
    EXPECT_EQ(v.space, ValueSpace::World);

    const LimitVelocityModule& l = d.limitVelocity;
    EXPECT_TRUE(l.present && l.enabled && l.separateAxes);
    EXPECT_FLOAT_EQ(l.limit.constant, 3.0f) << "the X limit";
    EXPECT_FLOAT_EQ(l.limitY.constant, 4.0f);
    EXPECT_FLOAT_EQ(l.limitZ.constant, 5.0f);
    EXPECT_EQ(l.space, ValueSpace::World);
    EXPECT_FLOAT_EQ(l.dampen, 0.5f);
    EXPECT_FLOAT_EQ(l.drag.constant, 0.1f);
    EXPECT_FALSE(l.multiplyBySize);
    EXPECT_TRUE(l.multiplyByVelocity) << "origin/main's default";

    EXPECT_FLOAT_EQ(d.main.inheritVelocity.constant, 0.5f);
    EXPECT_EQ(d.main.inheritVelocityMode, InheritVelocityMode::Current);
    EXPECT_FLOAT_EQ(d.forceOverLifetime.y.constant, -2.0f);
    EXPECT_EQ(d.forceOverLifetime.space, ValueSpace::World);
    EXPECT_TRUE(d.forceOverLifetime.randomizePerTick);
    EXPECT_TRUE(d.sizeOverLifetime.separateAxes);
    EXPECT_FLOAT_EQ(d.sizeOverLifetime.sizeZ.constant, 4.0f);
    EXPECT_EQ(d.sizeBySpeed.speedRange, Vector2(2.0f, 8.0f));
    EXPECT_NEAR(d.rotationOverLifetime.angularVelocity.constant, 3.0f * kDeg, 1e-3f);
    EXPECT_NEAR(d.rotationOverLifetime.angularVelocityX.constant, 1.0f * kDeg, 1e-3f);
    EXPECT_TRUE(d.rotationBySpeed.separateAxes);
    EXPECT_NEAR(d.rotationBySpeed.angularVelocityY.constant, 1.0f * kDeg, 1e-3f);
    EXPECT_EQ(d.rotationBySpeed.speedRange, Vector2(0.0f, 10.0f));

    const NoiseModule& n = d.noise;
    EXPECT_TRUE(n.separateAxes);
    EXPECT_FLOAT_EQ(n.strengthZ.constant, 0.7f);
    EXPECT_FLOAT_EQ(n.frequency, 1.5f);
    EXPECT_FLOAT_EQ(n.scrollSpeed.constant, 0.3f);
    EXPECT_FALSE(n.damping);
    EXPECT_EQ(n.octaves, 3);
    EXPECT_FLOAT_EQ(n.octaveMultiplier, 0.4f);
    EXPECT_FLOAT_EQ(n.octaveScale, 3.0f);
    EXPECT_FLOAT_EQ(n.positionAmount.constant, 0.8f);
    EXPECT_FLOAT_EQ(n.rotationAmount.constant, 0.2f) << "radians per second at amount 1 in both";
    EXPECT_FLOAT_EQ(n.sizeAmount.constant, 0.1f);

    const TextureSheetAnimationModule& t = d.textureSheetAnimation;
    EXPECT_EQ(t.tilesX, 4);
    EXPECT_EQ(t.tilesY, 2);
    EXPECT_EQ(t.animation, TextureSheetAnimation::SingleRow);
    EXPECT_EQ(t.rowMode, TextureSheetRowMode::Random);
    EXPECT_EQ(t.rowIndex, 1);
    EXPECT_EQ(t.timeMode, TextureSheetTimeMode::Speed);
    EXPECT_FLOAT_EQ(t.fps, 12.0f);
    EXPECT_EQ(t.speedRange, Vector2(1.0f, 5.0f));
    EXPECT_FLOAT_EQ(t.startFrame.constant, 2.0f);
    EXPECT_FLOAT_EQ(t.cycles, 3.0f);
    EXPECT_FLOAT_EQ(t.flipU, 0.5f);
    EXPECT_FLOAT_EQ(t.flipV, 0.25f);

    const RendererModule& rn = d.renderer;
    EXPECT_EQ(rn.renderMode, RenderMode::StretchedBillboard);
    EXPECT_FLOAT_EQ(rn.cameraScale, 0.1f);
    EXPECT_FLOAT_EQ(rn.speedScale, 0.5f);
    EXPECT_FLOAT_EQ(rn.lengthScale, 3.0f);
    EXPECT_EQ(rn.sortMode, SortMode::YoungestInFront);
    EXPECT_FLOAT_EQ(rn.sortingFudge, 2.0f);
    EXPECT_EQ(rn.sortPriority, 4);
    EXPECT_FLOAT_EQ(rn.minScreenSize, 0.01f);
    EXPECT_FLOAT_EQ(rn.maxScreenSize, 0.8f);
    EXPECT_EQ(rn.pivot, Vector3(0.0f, 0.5f, 0.0f));
    EXPECT_EQ(rn.flip, Vector3(0.5f, 0.0f, 0.0f));

    // and S20's codec writes and reads it back the same
    const std::string text = ParticleEffectCodec::write(d);
    ParticleEffectDocument again;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::read(text, again, diagnostics)) << ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(ParticleEffectCodec::write(again), text);
}

TEST(UpstreamParticles, DefaultsThatDiffer){
    // modules written with their own defaults left out: origin/main's 30 frames per second and
    // drag by velocity, not S20's
    const up::Result r = convertText("{version: 2, limitVelocityOverLifetime: {enabled: true}, textureSheetAnimation: {enabled: true}}");
    EXPECT_FLOAT_EQ(r.effect.textureSheetAnimation.fps, 30.0f);
    EXPECT_TRUE(r.effect.limitVelocity.multiplyByVelocity);
    // a module with no key stays hidden; Main, Emission, Shape keep their defaults
    const up::Result empty = convertText("{version: 2}");
    EXPECT_FALSE(empty.effect.noise.present);
    EXPECT_FALSE(empty.effect.velocityOverLifetime.present);
    EXPECT_FLOAT_EQ(empty.effect.emission.rateOverTime.constant, 10.0f);
    EXPECT_EQ(empty.effect.shape.shape, ShapeType::Cone);
    EXPECT_EQ(empty.effect.shape.emitFrom, ShapeEmitFrom::Base);
    EXPECT_TRUE(empty.playOnAwake);
    EXPECT_EQ(empty.seed, 0u) << "Auto Random Seed";
    // a module off: present, its values kept
    const up::Result off = convertText("{version: 2, noise: {strength: 2}}");
    EXPECT_TRUE(off.effect.noise.present);
    EXPECT_FALSE(off.effect.noise.enabled);
    EXPECT_FLOAT_EQ(off.effect.noise.strength.constant, 2.0f);
}

TEST(UpstreamParticles, ShapeTypesAndTheirEmitFrom){
    struct Case { const char* name; ShapeType shape; ShapeEmitFrom from; };
    const Case cases[] = {
        {"Sphere", ShapeType::Sphere, ShapeEmitFrom::Volume}, {"Hemisphere", ShapeType::Hemisphere, ShapeEmitFrom::Volume},
        {"Cone", ShapeType::Cone, ShapeEmitFrom::Base}, {"ConeVolume", ShapeType::Cone, ShapeEmitFrom::Volume},
        {"Box", ShapeType::Box, ShapeEmitFrom::Volume}, {"BoxShell", ShapeType::Box, ShapeEmitFrom::Shell},
        {"BoxEdge", ShapeType::Box, ShapeEmitFrom::Edge}, {"Circle", ShapeType::Circle, ShapeEmitFrom::Volume},
        {"Edge", ShapeType::Edge, ShapeEmitFrom::Volume}, {"Rectangle", ShapeType::Rectangle, ShapeEmitFrom::Volume},
        {"Donut", ShapeType::Donut, ShapeEmitFrom::Volume}};
    for (const Case& c : cases){
        const up::Result r = convertText(std::string("{version: 2, shape: {shapeType: ") + c.name + "}}");
        EXPECT_EQ(r.effect.shape.shape, c.shape) << c.name;
        EXPECT_EQ(r.effect.shape.emitFrom, c.from) << c.name;
    }
    // an unknown name keeps the cone, and says so
    const up::Result odd = convertText("{version: 2, shape: {shapeType: Teapot}}");
    EXPECT_EQ(odd.effect.shape.shape, ShapeType::Cone);
    ASSERT_FALSE(odd.notes.empty());
    EXPECT_NE(odd.notes[0].find("Teapot"), std::string::npos);
}

TEST(UpstreamParticles, ShapeRotationIsTheSameTurn){
    const Vector3 cases[] = {Vector3(30, 45, 60), Vector3(90, 0, 0), Vector3(0, 90, 0), Vector3(-45, 10, 170), Vector3(10, 90, 30)};
    for (const Vector3& e : cases){
        const Vector3 s = up::shapeRotation(e);
        for (const Vector3& axis : {Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)}){
            Vector3 a, b;
            upstreamTurn(e, axis, a);
            s20Turn(s, axis, b);
            EXPECT_NEAR(a.x, b.x, 2e-4f) << e.x << " " << e.y << " " << e.z;
            EXPECT_NEAR(a.y, b.y, 2e-4f) << e.x << " " << e.y << " " << e.z;
            EXPECT_NEAR(a.z, b.z, 2e-4f) << e.x << " " << e.y << " " << e.z;
        }
    }
    EXPECT_EQ(up::shapeRotation(Vector3(0, 0, 0)), Vector3(0, 0, 0));
}

TEST(UpstreamParticles, RenderAlignments){
    struct Case { const char* mode; const char* alignment; const char* space; RenderMode render; MeshAlignment mesh; };
    const Case cases[] = {
        {"Billboard", "View", "Local", RenderMode::Billboard, MeshAlignment::Simulation},
        {"Billboard", "World", "World", RenderMode::WorldOriented, MeshAlignment::Simulation},
        {"Billboard", "World", "Local", RenderMode::LocalOriented, MeshAlignment::Simulation},
        {"Billboard", "Local", "World", RenderMode::LocalOriented, MeshAlignment::Simulation},
        {"Billboard", "Facing", "Local", RenderMode::FacingCameraPosition, MeshAlignment::Simulation},
        {"Billboard", "Velocity", "Local", RenderMode::VelocityAligned, MeshAlignment::Simulation},
        {"HorizontalBillboard", "View", "Local", RenderMode::HorizontalBillboard, MeshAlignment::Simulation},
        {"VerticalBillboard", "View", "Local", RenderMode::VerticalBillboard, MeshAlignment::Simulation},
        {"Mesh", "View", "Local", RenderMode::Mesh, MeshAlignment::View},
        {"Mesh", "World", "Local", RenderMode::Mesh, MeshAlignment::Simulation},
        {"Mesh", "Local", "World", RenderMode::Mesh, MeshAlignment::Local},
        {"Mesh", "Facing", "Local", RenderMode::Mesh, MeshAlignment::Facing},
        {"Mesh", "Velocity", "Local", RenderMode::Mesh, MeshAlignment::Velocity}};
    for (const Case& c : cases){
        const up::Result r = convertText(std::string("{version: 2, main: {simulationSpace: ") + c.space + "}, renderer: {renderMode: " + c.mode +
                                         ", alignment: " + c.alignment + ", mesh: cube}}");
        SCOPED_TRACE(std::string(c.mode) + " " + c.alignment + " " + c.space);
        EXPECT_EQ(r.effect.renderer.renderMode, c.render);
        EXPECT_EQ(r.effect.renderer.meshAlignment, c.mesh);
    }
    // a mesh drawn without a MeshComponent of its own: the primitive by name, a model by path
    const up::Result cube = convertText("{version: 2, renderer: {renderMode: Mesh, mesh: capsule}}", up::Target());
    ASSERT_EQ(cube.effect.renderer.meshes.size(), 1u);
    EXPECT_EQ(cube.effect.renderer.meshes[0].type, MeshSourceType::Primitive);
    EXPECT_EQ(cube.effect.renderer.meshes[0].primitive, PrimitiveMesh::Capsule);
    const up::Result model = convertText("{version: 2, renderer: {renderMode: Mesh, mesh: models/rock.glb}}", up::Target());
    EXPECT_EQ(model.effect.renderer.meshes[0].type, MeshSourceType::Model);
    EXPECT_EQ(model.effect.renderer.meshes[0].model, "models/rock.glb");
    // with the GameObject's MeshComponent: that mesh, kept
    up::Target target;
    target.kind = up::Target::Kind::Mesh;
    const up::Result own = convertText("{version: 2, renderer: {renderMode: Mesh, mesh: cube}}", target);
    EXPECT_TRUE(own.drawsTargetMesh);
    EXPECT_EQ(own.effect.renderer.meshes[0].type, MeshSourceType::GameObject);
}

TEST(UpstreamParticles, TheQuadMaterialInTheRenderer){
    // origin/main's particle quad: library:unlit, PARTICLE_TEXTURE, the base map, its colour
    // (linear), tiling, soft particles, additive
    Material material;
    material.shader.path = "library:unlit";
    material.shaderFeatures = {"PARTICLE_TEXTURE", "SOFT"};
    material.shaderTextures = {{"u_baseTexture", Texture("textures/spark.png")}};
    material.shaderParams = {{"baseColor", Vector4(0.5f, 0.25f, 1.0f, 1.0f)}, {"tiling", Vector4(2.0f, 2.0f, 0.5f, 0.0f)},
                             {"softness", Vector4(0.75f, 0.0f, 0.0f, 0.0f)}};
    material.blendMode = MaterialBlendMode::ADDITIVE;
    RendererModule r;
    std::vector<std::string> notes;
    up::applyMaterial(material, r, notes);
    EXPECT_TRUE(notes.empty()) << notes.front();
    EXPECT_EQ(r.shader, "library:particles");
    EXPECT_EQ(r.texture, "textures/spark.png");
    EXPECT_EQ(r.blend, ParticleBlend::Additive);
    EXPECT_FALSE(r.depthWrite);
    EXPECT_EQ(r.textureTiling, Vector4(2.0f, 2.0f, 0.5f, 0.0f));
    EXPECT_EQ(r.shaderFeatures, std::vector<std::string>({"SOFT"}));
    ASSERT_EQ(r.shaderProperties.size(), 2u);
    EXPECT_EQ(r.shaderProperties[0].name, "tint");
    EXPECT_EQ(r.shaderProperties[0].value, Vector4(0.5f, 0.25f, 1.0f, 1.0f)) << "linear, as the material held it";
    EXPECT_EQ(r.shaderProperties[1].name, "softness");
    EXPECT_FLOAT_EQ(r.shaderProperties[1].value.x, 0.75f);

    // without a base map: the built-in soft particle (a disc), or a plain quad without PARTICLE_TEXTURE
    Material plain;
    plain.shader.path = "library:unlit";
    plain.shaderFeatures = {"PARTICLE_TEXTURE"};
    RendererModule disc;
    up::applyMaterial(plain, disc, notes);
    EXPECT_EQ(disc.untexturedShape, UntexturedShape::Disc);
    plain.shaderFeatures.clear();
    RendererModule square;
    up::applyMaterial(plain, square, notes);
    EXPECT_EQ(square.untexturedShape, UntexturedShape::Square);

    // what has no place is noted: an opaque blend, a custom shader, its other textures
    Material odd;
    odd.shader.path = "shaders/fire.frag";
    odd.blendMode = MaterialBlendMode::NONE;
    odd.shaderTextures = {{"u_noise", Texture("textures/noise.png")}};
    RendererModule r2;
    std::vector<std::string> oddNotes;
    up::applyMaterial(odd, r2, oddNotes);
    EXPECT_EQ(r2.blend, ParticleBlend::Alpha);
    EXPECT_TRUE(r2.depthWrite);
    EXPECT_EQ(oddNotes.size(), 3u);
}

TEST(UpstreamParticles, SceneMigrationMovesTheQuadIntoTheRenderer){
    // a version 2 system on its own GameObject (its action's target itself), drawn by the quad
    // MeshComponent and InstancedMesh: one S20 component, the quad and instances gone, the action
    // kept as its controller; Play On Awake and the action's speed carried over
    YAML::Node root = YAML::Load(
        "{entities: ["
        "{type: Entity, entity: 7, name: Sparks, components: {"
        " transform: {position: [1, 2, 3]},"
        " action: {target: 7, speed: 2},"
        " particlesystem: {enabled: true, version: 2, main: {startSpeed: 4, playOnAwake: false}, emission: {rateOverTime: 60}},"
        " mesh: {submeshes: [{material: {shader: library:unlit, shaderFeatures: [PARTICLE_TEXTURE], blendMode: ADDITIVE,"
        "  shaderParams: {baseColor: [1, 0.5, 0.25, 1]}}}]},"
        " instancedmesh: {maxInstances: 1000, instances: []}}},"
        "{type: Entity, entity: 8, name: Cubes, components: {"
        " transform: {position: [0, 0, 0]}, action: {target: 8},"
        " particlesystem: {version: 2, renderer: {renderMode: Mesh, mesh: cube}},"
        " mesh: {submeshes: [{material: {shader: library:unlit}}]}, instancedmesh: {instances: []}}}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_TRUE(result.migrated());
    const YAML::Node sparks = root["entities"][0]["components"];
    ASSERT_TRUE(sparks["particlesystem"]);
    EXPECT_FALSE(sparks["particlesystem"]["version"]);
    EXPECT_FALSE(sparks["mesh"]) << "the quad: its material is the Renderer's now";
    EXPECT_FALSE(sparks["instancedmesh"]);
    EXPECT_TRUE(sparks["action"]) << "the controller stays";
    const ParticleSystemComponent c = Stream::decodeParticleSystemComponent(sparks["particlesystem"]);
    EXPECT_FALSE(c.playOnAwake);
    EXPECT_FLOAT_EQ(c.playbackSpeed, 2.0f);
    EXPECT_FLOAT_EQ(c.inlineEffect.main.startSpeed.constant, 4.0f);
    EXPECT_FLOAT_EQ(c.inlineEffect.emission.rateOverTime.constant, 60.0f);
    EXPECT_EQ(c.inlineEffect.renderer.blend, ParticleBlend::Additive);
    ASSERT_EQ(c.inlineEffect.renderer.shaderProperties.size(), 1u);
    EXPECT_EQ(c.inlineEffect.renderer.shaderProperties[0].value, Vector4(1.0f, 0.5f, 0.25f, 1.0f));

    // Mesh mode draws the GameObject's own mesh: kept, disabled
    const YAML::Node cubes = root["entities"][1]["components"];
    ASSERT_TRUE(cubes["mesh"]);
    EXPECT_FALSE(cubes["mesh"]["enabled"].as<bool>());
    EXPECT_FALSE(cubes["instancedmesh"]);
    const ParticleEffectDocument d = effectOf(cubes["particlesystem"]);
    ASSERT_EQ(d.renderer.meshes.size(), 1u);
    EXPECT_EQ(d.renderer.meshes[0].type, MeshSourceType::GameObject);

    bool logged = false;
    for (const std::string& line : result.log.lines) logged = logged || line.find("Sparks (origin/main's version 2)") != std::string::npos;
    EXPECT_TRUE(logged);
}

TEST(UpstreamParticles, PointsTargetAndSpriteFrames){
    // drawn by a Points GameObject: its texture and frames (Sprites mode picks them by id)
    YAML::Node root = YAML::Load(
        "{entities: ["
        "{type: Entity, entity: 1, name: Snow, components: {transform: {position: [0, 0, 0]}, action: {target: 2},"
        " particlesystem: {version: 2, textureSheetAnimation: {enabled: true, mode: Sprites, sprites: [1, 0, 1]}}}},"
        "{type: Entity, entity: 2, name: SnowPoints, components: {transform: {position: [0, 0, 0]},"
        " points: {points: [], texture: {textureType: 2D, source: path, path: textures/flake.png},"
        "  framesRect: [{name: a, rect: [0, 0, 0.5, 1]}, {name: b, rect: [0.5, 0, 0.5, 1]}]}}}]}");
    StreamMigrations::migrate(DocumentKind::Scene, root);
    const YAML::Node points = root["entities"][1]["components"];
    ASSERT_TRUE(points["particlesystem"]) << "on its target";
    EXPECT_FALSE(root["entities"][0]["components"]["particlesystem"]);
    EXPECT_FALSE(points["points"]) << "its empty Points go";
    const ParticleEffectDocument d = effectOf(points["particlesystem"]);
    EXPECT_EQ(d.renderer.texture, "textures/flake.png");
    EXPECT_EQ(d.renderer.untexturedShape, UntexturedShape::Square);
    EXPECT_EQ(d.textureSheetAnimation.mode, TextureSheetMode::Sprites);
    ASSERT_EQ(d.textureSheetAnimation.sprites.size(), 3u);
    EXPECT_FLOAT_EQ(d.textureSheetAnimation.sprites[0].x, 0.5f);
    EXPECT_FLOAT_EQ(d.textureSheetAnimation.sprites[1].x, 0.0f);
}

TEST(UpstreamParticles, PrefabInstanceOverridesConvert){
    // an instance overriding module fields: its values the whole component, the property names
    // S20's (the modules are one document)
    YAML::Node root = YAML::Load(
        "{entities: [{type: Entity, entity: 30, name: FireInstance, components: {transform: {position: [0, 0, 0]}},"
        " prefabInstance: {prefabPath: prefabs/Fire.prefab, overrides: [{registryEntity: 5,"
        "  properties: {particlesystem: [main.startSpeed, emission.rateOverTime, main.playOnAwake]},"
        "  components: {particlesystem: {version: 2, main: {startSpeed: 7, playOnAwake: false}, emission: {rateOverTime: 33}}}}]}}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    const YAML::Node entry = root["entities"][0]["prefabInstance"]["overrides"][0];
    const YAML::Node names = entry["properties"]["particlesystem"];
    ASSERT_TRUE(names.IsSequence());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0].as<std::string>(), "inlineEffect");
    EXPECT_EQ(names[1].as<std::string>(), "playOnAwake");
    const ParticleSystemComponent c = Stream::decodeParticleSystemComponent(entry["components"]["particlesystem"]);
    EXPECT_FLOAT_EQ(c.inlineEffect.main.startSpeed.constant, 7.0f);
    EXPECT_FLOAT_EQ(c.inlineEffect.emission.rateOverTime.constant, 33.0f);
    EXPECT_FALSE(c.playOnAwake);
    bool logged = false;
    for (const std::string& line : result.log.lines) logged = logged || line.find("Prefab instance override") != std::string::npos;
    EXPECT_TRUE(logged);
}
