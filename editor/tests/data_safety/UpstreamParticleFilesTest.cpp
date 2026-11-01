// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A scene and a prefab saved by origin/main's editor (047eccf9b: create_particles, set_properties,
// create_prefab, instantiate_prefab through its MCP server; fixtures/migrations/upstream-v2) load
// into S20 without losing their particle systems (docs/specs/S20-particles.md, Upstream
// absorption). Before the absorption the scene 2 -> 3 migration read a `version: 2` system as an
// old one and wrote an effect of defaults: every module value was gone. Each value set there is
// checked here, with its conversion (degrees, sRGB, range order, shape turn, render alignment),
// the quad material in the Renderer, the targets' components, a prefab instance's override, and
// the Line and Trail Renderers' curves read the same way (S27).

#include "support/EditorTestSupport.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "StreamMigrations.h"
#include "component/LineRendererComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/TrailRendererComponent.h"
#include "stream/UpstreamParticles.h"
#include "util/Color.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
using namespace jasperengine::particles;

namespace {

    constexpr float kDeg = 57.29577951308232f;

    YAML::Node migratedScene(){
        YAML::Node root = YAML::Load(readText(fixture("migrations/upstream-v2/Upstream.scene")));
        const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
        EXPECT_EQ(result.versionRead, 0) << "origin/main writes no format version";
        EXPECT_TRUE(result.migrated());
        return root;
    }

    YAML::Node entityNamed(const YAML::Node& entities, const std::string& name){
        for (const YAML::Node& e : entities){
            if (e["name"] && e["name"].as<std::string>() == name) return e;
        }
        return YAML::Node();
    }

    ParticleSystemComponent systemOf(const YAML::Node& entity){
        EXPECT_TRUE(entity.IsMap());
        const YAML::Node node = entity["components"]["particlesystem"];
        EXPECT_TRUE(node.IsMap());
        EXPECT_FALSE(node["version"]) << "S20's form";
        EXPECT_TRUE(node["inlineEffect"]);
        return Stream::decodeParticleSystemComponent(node);
    }

    float srgb(float linear){
        return Color::linearTosRGB(Vector3(linear, linear, linear)).x;
    }

}

TEST(UpstreamParticleFiles, TheFullSystemKeepsEveryValue){
    const YAML::Node root = migratedScene();
    const YAML::Node full = entityNamed(root["entities"], "Full");
    const ParticleSystemComponent c = systemOf(full);
    const ParticleEffectDocument& d = c.inlineEffect;

    // main
    EXPECT_FLOAT_EQ(d.duration, 3.0f);
    EXPECT_EQ(d.startDelay.mode, CurveMode::TwoConstants);
    EXPECT_FLOAT_EQ(d.startDelay.constant, 0.1f);
    EXPECT_FLOAT_EQ(d.startDelay.constantMax, 0.4f);
    EXPECT_FLOAT_EQ(d.main.startLifetime.constant, 1.0f);
    EXPECT_FLOAT_EQ(d.main.startLifetime.constantMax, 2.0f);
    EXPECT_EQ(d.main.startSpeed.mode, CurveMode::Curve);
    EXPECT_FLOAT_EQ(d.main.startSpeed.multiplier, 3.0f);
    EXPECT_FLOAT_EQ(d.main.startSpeed.curve.evaluate(0.0f), 0.5f);
    EXPECT_EQ(d.main.startSize.mode, CurveMode::TwoCurves);
    EXPECT_FLOAT_EQ(d.main.startSize.multiplier, 0.5f);
    EXPECT_FLOAT_EQ(d.main.startSize.curve.evaluate(1.0f), 0.4f) << "the min curve first";
    EXPECT_FLOAT_EQ(d.main.startSize.curveMax.evaluate(1.0f), 2.0f);
    EXPECT_NEAR(d.main.startRotation.constant, 0.5f * kDeg, 1e-3f) << "radians there, degrees here";
    EXPECT_NEAR(d.main.startRotation.constantMax, 1.5f * kDeg, 1e-3f);
    EXPECT_FLOAT_EQ(d.main.flipRotation, 0.25f);
    EXPECT_EQ(d.main.startColor.mode, GradientValueMode::TwoColors);
    EXPECT_NEAR(d.main.startColor.color.y, 128.0f / 255.0f, 1e-6f) << "#FF8000 is sRGB";
    EXPECT_NEAR(d.main.startColor.colorMax.z, 1.0f, 1e-6f);
    EXPECT_NEAR(d.main.startColor.colorMax.x, srgb(0.2f), 1e-5f) << "an array is linear";
    EXPECT_NEAR(d.main.startColor.colorMax.w, 0.5f, 1e-6f);
    EXPECT_EQ(d.main.gravityModifier.mode, CurveMode::Curve);
    EXPECT_FLOAT_EQ(d.main.gravityModifier.multiplier, 2.0f);
    EXPECT_EQ(d.main.gravityModifier.driver, CurveDriver::EmitterTime);
    EXPECT_EQ(d.simulationSpace, SimulationSpace::World);
    EXPECT_FLOAT_EQ(d.simulationSpeed, 1.5f);
    EXPECT_EQ(d.scalingMode, ScalingMode::Hierarchy);
    EXPECT_EQ(d.maxParticles, 500u);
    EXPECT_EQ(d.stopAction, StopAction::Disable);
    EXPECT_EQ(c.seed, 1234u);
    EXPECT_FALSE(c.playOnAwake == false) << "Play On Awake as origin/main's default";

    // emission
    EXPECT_FLOAT_EQ(d.emission.rateOverTime.constant, 25.0f);
    EXPECT_FLOAT_EQ(d.emission.rateOverDistance.constant, 2.0f);
    ASSERT_EQ(d.emission.bursts.size(), 2u);
    EXPECT_FLOAT_EQ(d.emission.bursts[0].count.constant, 10.0f);
    EXPECT_FLOAT_EQ(d.emission.bursts[1].time, 0.5f);
    EXPECT_FLOAT_EQ(d.emission.bursts[1].count.constant, 3.0f);
    EXPECT_FLOAT_EQ(d.emission.bursts[1].count.constantMax, 6.0f);
    EXPECT_EQ(d.emission.bursts[1].cycles, 3);
    EXPECT_FLOAT_EQ(d.emission.bursts[1].interval, 0.2f);
    EXPECT_FLOAT_EQ(d.emission.bursts[1].probability, 0.75f);

    // shape
    EXPECT_EQ(d.shape.shape, ShapeType::Box);
    EXPECT_EQ(d.shape.emitFrom, ShapeEmitFrom::Shell);
    EXPECT_EQ(d.shape.arcMode, ShapeMultiMode::Loop);
    EXPECT_FLOAT_EQ(d.shape.arcSpread, 0.25f);
    EXPECT_FLOAT_EQ(d.shape.arcSpeed.constant, 2.0f);
    EXPECT_EQ(d.shape.boxThickness, Vector3(0.1f, 0.2f, 0.3f));
    EXPECT_EQ(d.shape.position, Vector3(0.5f, 0.0f, 0.0f));
    const Vector3 turn = upstreamparticles::shapeRotation(Vector3(30.0f, 45.0f, 60.0f));
    EXPECT_NEAR(d.shape.rotation.x, turn.x, 1e-3f);
    EXPECT_NEAR(d.shape.rotation.y, turn.y, 1e-3f);
    EXPECT_NEAR(d.shape.rotation.z, turn.z, 1e-3f);
    EXPECT_EQ(d.shape.scale, Vector3(2.0f, 1.0f, 3.0f));
    EXPECT_TRUE(d.shape.alignToDirection);
    EXPECT_FLOAT_EQ(d.shape.randomizeDirection, 0.2f);
    EXPECT_FLOAT_EQ(d.shape.spherizeDirection, 0.1f);
    EXPECT_FLOAT_EQ(d.shape.randomizePosition, 0.05f);

    // the other modules
    EXPECT_TRUE(d.velocityOverLifetime.present && d.velocityOverLifetime.enabled);
    EXPECT_FLOAT_EQ(d.velocityOverLifetime.x.constant, 1.0f);
    EXPECT_FLOAT_EQ(d.velocityOverLifetime.y.constant, -1.0f);
    EXPECT_FLOAT_EQ(d.velocityOverLifetime.y.constantMax, 1.0f);
    EXPECT_NEAR(d.velocityOverLifetime.orbitalY.constant, 90.0f, 0.01f);
    EXPECT_FLOAT_EQ(d.velocityOverLifetime.offsetX.constant, 0.5f);
    EXPECT_FLOAT_EQ(d.velocityOverLifetime.radial.constant, 0.25f);
    EXPECT_FLOAT_EQ(d.velocityOverLifetime.speedModifier.curve.evaluate(1.0f), 0.5f);
    EXPECT_EQ(d.velocityOverLifetime.space, ValueSpace::World);
    EXPECT_TRUE(d.limitVelocity.present && d.limitVelocity.enabled);
    EXPECT_FLOAT_EQ(d.limitVelocity.limit.constant, 3.0f);
    EXPECT_FLOAT_EQ(d.limitVelocity.dampen, 0.5f);
    EXPECT_FLOAT_EQ(d.limitVelocity.drag.constant, 0.1f);
    EXPECT_TRUE(d.limitVelocity.multiplyByVelocity) << "origin/main's default";
    EXPECT_FLOAT_EQ(d.main.inheritVelocity.constant, 0.5f);
    EXPECT_EQ(d.main.inheritVelocityMode, InheritVelocityMode::Current);
    EXPECT_FLOAT_EQ(d.forceOverLifetime.y.constant, -2.0f);
    EXPECT_TRUE(d.forceOverLifetime.randomizePerTick);
    EXPECT_EQ(d.colorOverLifetime.color.mode, GradientValueMode::Gradient);
    ASSERT_EQ(d.colorOverLifetime.color.gradient.colorKeys.size(), 3u);
    EXPECT_EQ(d.colorOverLifetime.color.gradient.colorKeys[1].color, Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(d.colorOverLifetime.color.gradient.evaluateAlpha(1.0f), 0.0f);
    EXPECT_TRUE(d.colorBySpeed.present);
    EXPECT_FALSE(d.colorBySpeed.enabled) << "its values kept, the module off as it was";
    EXPECT_EQ(d.colorBySpeed.speedRange, Vector2(0.0f, 5.0f));
    EXPECT_FLOAT_EQ(d.sizeOverLifetime.size.curve.evaluate(0.3f), 1.0f);
    EXPECT_NEAR(d.rotationOverLifetime.angularVelocity.constant, 180.0f, 0.01f);
    EXPECT_FLOAT_EQ(d.noise.strength.constant, 0.5f);
    EXPECT_FLOAT_EQ(d.noise.frequency, 1.5f);
    EXPECT_FLOAT_EQ(d.noise.scrollSpeed.constant, 0.3f);
    EXPECT_EQ(d.noise.octaves, 3);
    EXPECT_FLOAT_EQ(d.noise.rotationAmount.constant, 0.2f);
    EXPECT_EQ(d.textureSheetAnimation.tilesX, 4);
    EXPECT_EQ(d.textureSheetAnimation.tilesY, 2);
    EXPECT_EQ(d.textureSheetAnimation.timeMode, TextureSheetTimeMode::FPS);
    EXPECT_FLOAT_EQ(d.textureSheetAnimation.fps, 12.0f);
    EXPECT_FLOAT_EQ(d.textureSheetAnimation.cycles, 2.0f);
    EXPECT_FLOAT_EQ(d.textureSheetAnimation.flipU, 0.5f);

    // the renderer, with the quad's material
    const RendererModule& r = d.renderer;
    EXPECT_EQ(r.renderMode, RenderMode::FacingCameraPosition);
    EXPECT_EQ(r.sortMode, SortMode::ByDistance);
    EXPECT_FLOAT_EQ(r.sortingFudge, 2.0f);
    EXPECT_FLOAT_EQ(r.minScreenSize, 0.01f);
    EXPECT_FLOAT_EQ(r.maxScreenSize, 0.8f);
    EXPECT_EQ(r.pivot, Vector3(0.0f, 0.5f, 0.0f));
    EXPECT_EQ(r.flip, Vector3(0.5f, 0.0f, 0.0f));
    EXPECT_EQ(r.texture, "textures/spark.png");
    EXPECT_EQ(r.blend, ParticleBlend::Additive);
    EXPECT_FALSE(r.depthWrite);
    EXPECT_EQ(r.textureTiling, Vector4(2.0f, 2.0f, 0.5f, 0.0f));
    EXPECT_EQ(r.shaderFeatures, std::vector<std::string>({"SOFT"}));
    bool tint = false, softness = false;
    for (const ShaderProperty& p : r.shaderProperties){
        tint = tint || (p.name == "tint" && p.value == Vector4(0.5f, 0.25f, 1.0f, 1.0f));
        softness = softness || (p.name == "softness" && p.value.x == 0.75f);
    }
    EXPECT_TRUE(tint);
    EXPECT_TRUE(softness);
    EXPECT_FALSE(r.receiveShadows);

    // the quad and its instances are gone, the action stays its controller
    const YAML::Node components = full["components"];
    EXPECT_FALSE(components["mesh"]);
    EXPECT_FALSE(components["instancedmesh"]);
    EXPECT_TRUE(components["action"]);
    EXPECT_TRUE(components["transform"]);
}

TEST(UpstreamParticleFiles, MeshPointsAndTheOtherSystems){
    const YAML::Node root = migratedScene();
    // Mesh mode: the GameObject's own cube, kept and disabled, aligned to the velocity
    const YAML::Node cubes = entityNamed(root["entities"], "Cubes");
    const ParticleEffectDocument cd = systemOf(cubes).inlineEffect;
    EXPECT_EQ(cd.renderer.renderMode, RenderMode::Mesh);
    ASSERT_EQ(cd.renderer.meshes.size(), 1u);
    EXPECT_EQ(cd.renderer.meshes[0].type, MeshSourceType::GameObject);
    EXPECT_EQ(cd.renderer.meshAlignment, MeshAlignment::Velocity);
    ASSERT_TRUE(cubes["components"]["mesh"]);
    EXPECT_FALSE(cubes["components"]["mesh"]["enabled"].as<bool>());
    EXPECT_FALSE(cubes["components"]["instancedmesh"]);
    EXPECT_TRUE(cd.main.startRotation3D);
    EXPECT_NEAR(cd.main.startRotationX.constant, 0.5f * kDeg, 1e-3f);
    EXPECT_TRUE(cd.main.startSize3D);
    EXPECT_FLOAT_EQ(cd.main.startSizeY.constant, 2.0f);
    EXPECT_TRUE(cd.rotationBySpeed.separateAxes);
    EXPECT_NEAR(cd.rotationBySpeed.angularVelocityX.constant, kDeg, 1e-3f);
    EXPECT_NEAR(cd.rotationBySpeed.angularVelocityY.constant, 2.0f * kDeg, 1e-3f);
    EXPECT_EQ(cd.rotationBySpeed.speedRange, Vector2(0.0f, 10.0f));

    // a Points system: its points go (empty), billboards drawing squares without a texture
    const YAML::Node pts = entityNamed(root["entities"], "Pts");
    const ParticleEffectDocument pd = systemOf(pts).inlineEffect;
    EXPECT_FALSE(pts["components"]["points"]);
    EXPECT_EQ(pd.renderer.renderMode, RenderMode::Billboard);
    EXPECT_EQ(pd.renderer.untexturedShape, UntexturedShape::Square);
    EXPECT_FLOAT_EQ(pd.emission.rateOverTime.constant, 40.0f);
    EXPECT_EQ(pd.shape.shape, ShapeType::Box);
    EXPECT_EQ(pd.shape.position, Vector3(0.0f, 8.0f, 0.0f));
    EXPECT_TRUE(pd.noise.present && pd.noise.enabled);

    // 2D gravity, the Rigidbody's velocity, unscaled time, a stretched billboard
    const ParticleEffectDocument kd = systemOf(entityNamed(root["entities"], "Counter")).inlineEffect;
    EXPECT_EQ(kd.main.gravitySource, GravitySource::Physics2D);
    EXPECT_EQ(kd.emitterVelocityMode, EmitterVelocityMode::Rigidbody);
    EXPECT_TRUE(kd.useUnscaledTime);
    EXPECT_EQ(kd.renderer.renderMode, RenderMode::StretchedBillboard);
    EXPECT_FLOAT_EQ(kd.renderer.speedScale, 0.5f);
    EXPECT_FLOAT_EQ(kd.renderer.lengthScale, 3.0f);
    EXPECT_FLOAT_EQ(kd.renderer.cameraScale, 0.1f);
    EXPECT_FALSE(kd.shape.enabled);
    EXPECT_FLOAT_EQ(kd.emission.rateOverTime.constant, 20.0f);
}

TEST(UpstreamParticleFiles, PrefabAndItsInstance){
    // the prefab file (origin/main's format 3)
    YAML::Node prefab = YAML::Load(readText(fixture("migrations/upstream-v2/Fire.prefab")));
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, prefab);
    EXPECT_EQ(result.versionRead, 3);
    EXPECT_EQ(prefab["version"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Prefab));
    const ParticleEffectDocument pd = systemOf(prefab["members"][0]).inlineEffect;
    EXPECT_EQ(pd.renderer.texture, "textures/spark.png");
    EXPECT_FALSE(prefab["members"][0]["components"]["mesh"]);

    // the instance in the scene: its own values and the names of what it overrides
    const YAML::Node root = migratedScene();
    const YAML::Node instance = entityNamed(root["entities"], "FireInstance");
    const ParticleSystemComponent c = systemOf(instance);
    EXPECT_FLOAT_EQ(c.inlineEffect.main.startSpeed.constant, 7.0f);
    EXPECT_FLOAT_EQ(c.inlineEffect.emission.rateOverTime.constant, 33.0f);
    const YAML::Node names = instance["prefabInstance"]["overrides"][0]["properties"]["particlesystem"];
    ASSERT_TRUE(names.IsSequence());
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0].as<std::string>(), "inlineEffect");
    EXPECT_FALSE(instance["components"]["instancedmesh"]);
}

TEST(UpstreamParticleFiles, LinesAndTrailsKeepTheirCurves){
    const YAML::Node root = migratedScene();
    const LineRendererComponent line = Stream::decodeLineRendererComponent(entityNamed(root["entities"], "Laser")["components"]["linerenderer"]);
    ASSERT_EQ(line.widthCurve.keys.size(), 3u);
    EXPECT_EQ(line.widthCurve.preWrapMode, CurveWrapMode::PingPong);
    EXPECT_EQ(line.widthCurve.postWrapMode, CurveWrapMode::Loop);
    EXPECT_TRUE(std::isinf(line.widthCurve.keys[1].outTangent));
    EXPECT_EQ(line.colorGradient.mode, GradientMode::Fixed);
    EXPECT_NEAR(line.colorGradient.evaluate(0.5f).z, 2.0f, 1e-3f) << "an HDR linear key";
    EXPECT_NEAR(line.colorGradient.evaluateAlpha(1.0f), 0.25f, 1e-6f);
    const TrailRendererComponent trail = Stream::decodeTrailRendererComponent(entityNamed(root["entities"], "Trail")["components"]["trailrenderer"]);
    ASSERT_EQ(trail.widthCurve.keys.size(), 2u);
    EXPECT_FLOAT_EQ(trail.widthCurve.evaluate(1.0f), 0.0f);
    EXPECT_NEAR(trail.colorGradient.colorKeys[0].color.y, 1.0f, 1e-6f);
    EXPECT_NEAR(trail.colorGradient.evaluateAlpha(1.0f), 0.0f, 1e-6f);
}

TEST(UpstreamParticleFiles, TheSceneLoads){
    // the editor's loader on the file itself: every system is S20's, and a second save reads
    // back the same
    DecodedScene decoded = decodeSceneFile(fixture("migrations/upstream-v2/Upstream.scene"));
    ASSERT_TRUE(decoded.scene && decoded.scene->scene);
    Scene* scene = decoded.scene->scene;
    int systems = 0;
    for (Entity e : decoded.scene->entities){
        if (const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(e)){
            systems++;
            EXPECT_GT(ps->inlineEffect.maxParticles, 0u);
        }
    }
    EXPECT_EQ(systems, 6) << "Full, Cubes, Pts, Counter, FirePrefab, FireInstance";
    const std::string first = encodeSceneText(decoded);
    DecodedScene again = decodeSceneText(first, "Upstream.scene");
    EXPECT_EQ(encodeSceneText(again), first);
}
