// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// unity/UnityParticleImport (docs/specs/S20-particles.md, Upstream absorption) on the synthetic
// Unity project of fixtures/unity-particles (hand-written to Unity's YAML forms; no game or
// Asset Store file): every mapped value of a system with most modules as an S20 effect (rotations
// in degrees, random ranges in their order, Unity's sRGB colours as they are), the axis
// conversion (a cone rotated (-90, 0, 0) in Unity emits along the engine's +Y; no shape: along
// +Z), the materials in the Renderer modules (a custom shader by its Blend state, the built-in
// ones, _TintColor, _InvFade, tiling; no .material file), textures (Alpha8 expanded, PSD
// reported), a scene's prefab instance with modifications, a removed component and a `stripped`
// placeholder with its own id, the report codes, numbered names, dry runs.

#include <gtest/gtest.h>
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"

#include "Catalog.h"
#include "Stream.h"
#include "particles/EmitterRuntime.h"
#include "particles/ParticleCompiler.h"
#include "unity/UnityParticleImport.h"
#include "util/Prefab.h"
#include "cli/CommandLine.h"

#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    const fs::path UNITY = fs::path(JASPER_TEST_FIXTURES_DIR) / "unity-particles" / "Project";
    constexpr float DEG = 57.29577951308232f;   // a Unity radian in S20's degrees

    // an empty engine project folder, removed at the end
    struct TempProject {
        fs::path root;
        TempProject() {
            static std::atomic<int> counter{0};
            std::random_device rd;
            root = fs::temp_directory_path() / ("jasper-unity-import-" + std::to_string(rd()) + "-" + std::to_string(counter++));
            fs::create_directories(root);
            std::ofstream(root / "project.yaml") << "name: test\n";
        }
        ~TempProject() {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };

    unity::ImportResult importFile(const TempProject& project, const fs::path& file, const std::string& object = "", bool dryRun = false) {
        unity::ImportOptions options;
        options.file = UNITY / file;
        options.engineProject = project.root;
        options.object = object;
        options.dryRun = dryRun;
        return unity::importParticles(options);
    }

    // a member of a prefab file by name (depth first)
    YAML::Node findMember(const YAML::Node& members, const std::string& name) {
        for (const YAML::Node& member : members) {
            if (member["name"] && member["name"].as<std::string>() == name) return member;
            if (member["children"]) {
                YAML::Node found = findMember(member["children"], name);
                if (found.IsMap()) return found;
            }
        }
        return YAML::Node();
    }

    std::vector<std::string> childNames(const YAML::Node& member) {
        std::vector<std::string> names;
        if (member["children"]) {
            for (const YAML::Node& child : member["children"]) names.push_back(child["name"].as<std::string>());
        }
        return names;
    }

    YAML::Node component(const YAML::Node& member, ComponentType type) {
        return member["components"][Catalog::getComponentName(type, true)];
    }

    ParticleSystemComponent componentOf(const YAML::Node& member) {
        return Stream::decodeParticleSystemComponent(component(member, ComponentType::ParticleSystemComponent));
    }

    // the system's inline effect (the importer writes no .particles asset)
    ParticleEffectDocument systemOf(const YAML::Node& member) {
        return componentOf(member).inlineEffect;
    }

    const Json* systemReport(const Json& report, const std::string& path) {
        for (const Json& system : report["systems"]) {
            if (system["path"] == path) return &system;
        }
        return nullptr;
    }

    bool hasNote(const Json& system, const std::string& code) {
        for (const Json& note : system["notes"]) {
            if (note["code"] == code) return true;
        }
        return false;
    }

    // the mean start direction of `count` particles of an imported system, born at once
    Vector3 meanDirection(ParticleEffectDocument d, int count = 200) {
        d.main.gravityModifier = MinMaxCurve(0.0f);
        d.main.startSpeed = MinMaxCurve(1.0f);
        d.simulationSpace = SimulationSpace::World;
        d.startDelay = MinMaxCurve(0.0f);
        d.prewarm = false;
        d.maxParticles = (uint32_t)count;
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        d.emission.rateOverDistance = MinMaxCurve(0.0f);
        Burst burst;
        burst.count = MinMaxCurve((float)count);
        d.emission.bursts = {burst};
        d.velocityOverLifetime.present = false;
        d.forceOverLifetime.present = false;
        d.noise.present = false;
        d.limitVelocity.present = false;
        EmitterRuntime runtime;
        std::shared_ptr<const CompiledEffect> compiled = ParticleCompiler::compile(d, CompileEnvironment{});
        EXPECT_TRUE(compiled->valid);
        runtime.bind(compiled);
        runtime.reset(1);
        runtime.play();
        EmitterInput input;
        input.gravity[1] = 0.0f;
        runtime.step(input);
        const ParticleChannels& ch = runtime.particleChannels();
        Vector3 sum(0, 0, 0);
        for (uint32_t i = 0; i < ch.count; i++) {
            sum = sum + Vector3(ch.get(ChVelX)[i], ch.get(ChVelY)[i], ch.get(ChVelZ)[i]).normalized();
        }
        return ch.count ? sum / (float)ch.count : sum;
    }

    // S20's shape turn (Euler degrees: Z, then X, then Y) of v
    Vector3 shapeTurn(const float e[3], const Vector3& v) {
        const float r = 3.14159265f / 180.0f;
        const float cx = std::cos(e[0] * r), sx = std::sin(e[0] * r), cy = std::cos(e[1] * r), sy = std::sin(e[1] * r);
        const float cz = std::cos(e[2] * r), sz = std::sin(e[2] * r);
        // Rz, then Rx, then Ry
        Vector3 a(cz * v.x - sz * v.y, sz * v.x + cz * v.y, v.z);
        Vector3 b(a.x, cx * a.y - sx * a.z, sx * a.y + cx * a.z);
        return Vector3(cy * b.x + sy * b.z, b.y, -sy * b.x + cy * b.z);
    }

}

TEST(UnityParticleImportTest, PrefabSystemValues) {
    TempProject project;
    const unity::ImportResult result = importFile(project, "Assets/Fx/Flame.prefab");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.prefabs.size(), 1u);
    EXPECT_EQ(result.prefabs[0], "assets/fx/Flame.prefab");

    const YAML::Node prefab = YAML::LoadFile((project.root / result.prefabs[0]).string());
    EXPECT_EQ(prefab["type"].as<std::string>(), "Prefab");
    EXPECT_FALSE(prefab["guid"].as<std::string>().empty());
    const YAML::Node flame = findMember(prefab["members"], "Flame");
    ASSERT_TRUE(flame.IsMap());
    EXPECT_EQ(childNames(flame), std::vector<std::string>({"Smoke"}));

    // the root sits at the origin; its rotation is kept, mirrored (90 deg about Y -> -90)
    const Transform transform = Stream::decodeTransform(component(flame, ComponentType::Transform));
    EXPECT_FLOAT_EQ(transform.position.x, 0.0f);
    EXPECT_NEAR(transform.rotation.w, 0.70710677f, 1e-5f);
    EXPECT_NEAR(transform.rotation.y, -0.70710677f, 1e-5f);

    const ParticleSystemComponent flameSystem = componentOf(flame);
    const ParticleEffectDocument ps = flameSystem.inlineEffect;
    EXPECT_TRUE(flameSystem.effect.path.empty()) << "an inline effect";
    EXPECT_FLOAT_EQ(ps.duration, 2.0f);
    EXPECT_FLOAT_EQ(ps.simulationSpeed, 1.5f);
    EXPECT_TRUE(ps.looping);
    EXPECT_TRUE(ps.prewarm);
    // Auto Random Seed off: the component's fixed seed
    EXPECT_EQ(flameSystem.seed, 1234u);
    EXPECT_EQ(ps.simulationSpace, SimulationSpace::World);
    EXPECT_EQ(ps.scalingMode, ScalingMode::Hierarchy);
    EXPECT_EQ(ps.maxParticles, 500u);
    // CFX_AutoDestructShuriken: destroyed when it finishes
    EXPECT_EQ(ps.stopAction, StopAction::Destroy);

    // Two Constants: S20's first value is Unity's min
    EXPECT_EQ(ps.main.startLifetime.mode, CurveMode::TwoConstants);
    EXPECT_FLOAT_EQ(ps.main.startLifetime.constant, 0.5f);
    EXPECT_FLOAT_EQ(ps.main.startLifetime.constantMax, 1.0f);
    EXPECT_FLOAT_EQ(ps.main.startSpeed.constant, 3.0f);
    EXPECT_EQ(ps.main.startSize.mode, CurveMode::Curve);
    EXPECT_FLOAT_EQ(ps.main.startSize.multiplier, 2.0f);
    EXPECT_NEAR(ps.main.startSize.curve.evaluate(0.0f), 0.5f, 1e-5f);
    // radians to degrees; the roll's sign changes (left handed view)
    EXPECT_NEAR(ps.main.startRotation.constant, 1.0f * DEG, 1e-3f);
    EXPECT_NEAR(ps.main.startRotation.constantMax, -1.0f * DEG, 1e-3f);
    EXPECT_FLOAT_EQ(ps.main.flipRotation, 0.5f);
    EXPECT_FLOAT_EQ(ps.main.gravityModifier.constant, 0.2f);

    // start colour: two colours, Unity's sRGB as it is
    EXPECT_EQ(ps.main.startColor.mode, GradientValueMode::TwoColors);
    EXPECT_NEAR(ps.main.startColor.colorMax.x, 0.5f, 1e-5f);
    EXPECT_NEAR(ps.main.startColor.color.x, 1.0f, 1e-5f);
    EXPECT_NEAR(ps.main.startColor.color.y, 0.0f, 1e-5f);

    // emission and its burst
    EXPECT_FLOAT_EQ(ps.emission.rateOverTime.constant, 20.0f);
    ASSERT_EQ(ps.emission.bursts.size(), 1u);
    EXPECT_FLOAT_EQ(ps.emission.bursts[0].time, 0.1f);
    EXPECT_FLOAT_EQ(ps.emission.bursts[0].count.constant, 5.0f);
    EXPECT_EQ(ps.emission.bursts[0].cycles, 3);
    EXPECT_FLOAT_EQ(ps.emission.bursts[0].interval, 0.5f);
    EXPECT_FLOAT_EQ(ps.emission.bursts[0].probability, 0.5f);

    // shape: cone (from its base), its position mirrored
    EXPECT_EQ(ps.shape.shape, ShapeType::Cone);
    EXPECT_EQ(ps.shape.emitFrom, ShapeEmitFrom::Base);
    EXPECT_FLOAT_EQ(ps.shape.angle, 15.0f);
    EXPECT_FLOAT_EQ(ps.shape.radius, 0.25f);
    EXPECT_FLOAT_EQ(ps.shape.position.x, -0.5f);

    // colour over lifetime: three keys, sRGB colours, times from 0..65535
    ASSERT_TRUE(ps.colorOverLifetime.present && ps.colorOverLifetime.enabled);
    const Gradient& gradient = ps.colorOverLifetime.color.gradient;
    ASSERT_EQ(gradient.colorKeys.size(), 3u);
    EXPECT_NEAR(gradient.colorKeys[1].time, 32768.0f / 65535.0f, 1e-5f);
    EXPECT_NEAR(gradient.colorKeys[1].color.y, 0.5f, 1e-5f);
    EXPECT_NEAR(gradient.alphaKeys[1].alpha, 0.5f, 1e-5f);

    ASSERT_TRUE(ps.sizeOverLifetime.enabled);
    EXPECT_NEAR(ps.sizeOverLifetime.size.curve.evaluate(0.0f), 0.2f, 1e-5f);
    // velocity: x mirrored (min first); orbital y negated, in degrees
    ASSERT_TRUE(ps.velocityOverLifetime.present && ps.velocityOverLifetime.enabled);
    EXPECT_FLOAT_EQ(ps.velocityOverLifetime.x.constant, -1.0f);
    EXPECT_FLOAT_EQ(ps.velocityOverLifetime.x.constantMax, -2.0f);
    EXPECT_NEAR(ps.velocityOverLifetime.orbitalY.constant, -0.5f * DEG, 1e-3f);
    EXPECT_EQ(ps.velocityOverLifetime.space, ValueSpace::Local);
    ASSERT_TRUE(ps.forceOverLifetime.enabled);
    EXPECT_FLOAT_EQ(ps.forceOverLifetime.y.constant, -5.0f);
    EXPECT_EQ(ps.forceOverLifetime.space, ValueSpace::World);
    ASSERT_TRUE(ps.limitVelocity.present && ps.limitVelocity.enabled);
    EXPECT_FLOAT_EQ(ps.limitVelocity.limit.multiplier, 8.0f);
    EXPECT_NEAR(ps.limitVelocity.limit.curve.evaluate(1.0f), 0.25f, 1e-5f);
    EXPECT_FLOAT_EQ(ps.limitVelocity.dampen, 0.1f);
    ASSERT_TRUE(ps.noise.enabled);
    EXPECT_EQ(ps.noise.octaves, 2);

    // texture sheet: 2x2 at 25 FPS, the start frame a share of the 4 frames
    ASSERT_TRUE(ps.textureSheetAnimation.enabled);
    EXPECT_EQ(ps.textureSheetAnimation.tilesX, 2);
    EXPECT_EQ(ps.textureSheetAnimation.tilesY, 2);
    EXPECT_EQ(ps.textureSheetAnimation.timeMode, TextureSheetTimeMode::FPS);
    EXPECT_FLOAT_EQ(ps.textureSheetAnimation.fps, 25.0f);
    EXPECT_FLOAT_EQ(ps.textureSheetAnimation.startFrame.constant, 2.0f);

    // renderer
    EXPECT_EQ(ps.renderer.renderMode, RenderMode::VerticalBillboard);
    EXPECT_EQ(ps.renderer.sortMode, SortMode::ByDistance);
    EXPECT_FLOAT_EQ(ps.renderer.sortingFudge, 10.0f);
    EXPECT_EQ(ps.renderer.sortPriority, 2);
    // it draws itself: no MeshComponent, no InstancedMesh, no action
    EXPECT_FALSE(component(flame, ComponentType::MeshComponent));
    EXPECT_FALSE(component(flame, ComponentType::InstancedMeshComponent));
    EXPECT_FALSE(component(flame, ComponentType::ActionComponent));
    // the prefab is in today's format (S20 components)
    EXPECT_EQ(prefab["version"].as<int>(), Prefab::FILE_VERSION);

    // the report
    const Json& report = result.report;
    const Json* root = systemReport(report, "Flame");
    ASSERT_NE(root, nullptr) << report.dump(2);
    EXPECT_EQ((*root)["engine_entity"], "assets/fx/Flame.prefab:Flame");
    EXPECT_EQ((*root)["modules"]["InitialModule"], "imported");
    EXPECT_EQ((*root)["modules"]["NoiseModule"], "approximated");
    EXPECT_EQ((*root)["modules"]["ParticleSystemRenderer"], "imported");
    EXPECT_TRUE(hasNote(*root, "noise-pattern"));
    EXPECT_TRUE(hasNote(*root, "shader-approximated"));
    EXPECT_TRUE(hasNote(*root, "auto-destruct"));
    EXPECT_FALSE(hasNote(*root, "module-unsupported"));
    EXPECT_TRUE(result.hasWarnings);
    EXPECT_TRUE(fs::is_regular_file(project.root / "assets/fx/Flame.import-report.json"));
    EXPECT_EQ(result.reportFile, "assets/fx/Flame.import-report.json");
}

TEST(UnityParticleImportTest, ChildSystemMeshAndUnsupportedModules) {
    TempProject project;
    const unity::ImportResult result = importFile(project, "Assets/Fx/Flame.prefab");
    ASSERT_TRUE(result.ok) << result.error;
    const YAML::Node prefab = YAML::LoadFile((project.root / result.prefabs[0]).string());
    const YAML::Node smoke = findMember(prefab["members"], "Smoke");
    ASSERT_TRUE(smoke.IsMap());
    const ParticleEffectDocument ps = systemOf(smoke);
    EXPECT_EQ(ps.simulationSpace, SimulationSpace::Local);
    EXPECT_EQ(ps.shape.shape, ShapeType::Sphere);
    EXPECT_EQ(ps.renderer.renderMode, RenderMode::Mesh);
    ASSERT_EQ(ps.renderer.meshes.size(), 1u);
    EXPECT_EQ(ps.renderer.meshes[0].type, MeshSourceType::Primitive);
    EXPECT_EQ(ps.renderer.meshes[0].primitive, PrimitiveMesh::Cube);
    EXPECT_EQ(ps.renderer.meshAlignment, MeshAlignment::Local);
    // Two Gradients: S20's first is Unity's min gradient
    EXPECT_EQ(ps.colorOverLifetime.color.mode, GradientValueMode::TwoGradients);
    EXPECT_NEAR(ps.colorOverLifetime.color.gradient.colorKeys.back().color.x, 0.5f, 1e-5f);
    // the child's transform: (0, 1, 0) under the root
    const Transform transform = Stream::decodeTransform(component(smoke, ComponentType::Transform));
    EXPECT_FLOAT_EQ(transform.position.y, 1.0f);

    const Json* system = systemReport(result.report, "Flame/Smoke");
    ASSERT_NE(system, nullptr) << result.report.dump(2);
    EXPECT_EQ((*system)["modules"]["SubModule"], "unsupported");
    EXPECT_EQ((*system)["modules"]["CollisionModule"], "disabled");
    EXPECT_TRUE(hasNote(*system, "module-unsupported"));
    EXPECT_TRUE(hasNote(*system, "weighted-tangents"));
}

TEST(UnityParticleImportTest, MaterialsAndTextures) {
    TempProject project;
    const unity::ImportResult result = importFile(project, "Assets/Fx/Flame.prefab");
    ASSERT_TRUE(result.ok) << result.error;
    const YAML::Node prefab = YAML::LoadFile((project.root / result.prefabs[0]).string());

    // the custom shader "Custom/Fire Additive (Soft)" (Blend SrcAlpha One): additive, soft, its
    // texture, in the flame's Renderer module (no .material file)
    const RendererModule fire = systemOf(findMember(prefab["members"], "Flame")).renderer;
    EXPECT_EQ(fire.shader, "library:particles");
    EXPECT_EQ(fire.blend, ParticleBlend::Additive);
    EXPECT_NE(std::find(fire.shaderFeatures.begin(), fire.shaderFeatures.end(), "SOFT"), fire.shaderFeatures.end());
    bool softness = false;
    for (const ShaderProperty& p : fire.shaderProperties) softness = softness || (p.name == "softness" && p.value.x > 0.0f);
    EXPECT_TRUE(softness);
    EXPECT_EQ(fire.texture, "assets/fx/textures/flame.png");
    EXPECT_FALSE(fs::exists(project.root / "assets/fx/materials"));
    EXPECT_FALSE(fire.depthWrite);

    // the Alpha8 texture: white with the source's alpha
    int w = 0, h = 0, n = 0;
    unsigned char* pixels = stbi_load((project.root / "assets/fx/textures/flame.png").string().c_str(), &w, &h, &n, 4);
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(w, 8);
    EXPECT_EQ(pixels[0], 255);
    EXPECT_EQ(pixels[4 * 1 + 3], 32);
    stbi_image_free(pixels);

    // the smoke: Mobile/Particles/Alpha Blended with the default particle texture (S20's soft disc)
    const RendererModule smoke = systemOf(findMember(prefab["members"], "Smoke")).renderer;
    EXPECT_EQ(smoke.blend, ParticleBlend::Alpha);
    EXPECT_TRUE(smoke.texture.empty());
    EXPECT_EQ(smoke.untexturedShape, UntexturedShape::Disc);

    bool alpha8 = false;
    for (const Json& texture : result.report["textures"]) alpha8 = alpha8 || texture.value("alpha8", false);
    EXPECT_TRUE(alpha8);
}

TEST(UnityParticleImportTest, ShapeAxes) {
    TempProject project;
    const unity::ImportResult result = importFile(project, "Assets/Fx/Shapes.prefab");
    ASSERT_TRUE(result.ok) << result.error;
    const YAML::Node members = YAML::LoadFile((project.root / result.prefabs[0]).string())["members"];
    EXPECT_EQ(childNames(findMember(members, "Shapes")),
              std::vector<std::string>({"Up", "NoShape", "Box", "MeshShape", "Donut", "Hemi", "EdgeUp"}));

    // a cone along Unity's +Z rotated (-90, 0, 0) emits up: the engine's +Y
    const Vector3 up = meanDirection(systemOf(findMember(members, "Up")));
    EXPECT_NEAR(up.y, 1.0f, 1e-3f) << up.x << " " << up.y << " " << up.z;
    // no shape: Unity's +Z, the engine's +Z (S mirrors x only)
    const ParticleEffectDocument none = systemOf(findMember(members, "NoShape"));
    EXPECT_TRUE(none.shape.enabled);
    const Vector3 forward = meanDirection(none);
    EXPECT_NEAR(forward.z, 1.0f, 1e-3f) << forward.x << " " << forward.y << " " << forward.z;
    // a hemisphere faces Unity's +Z
    EXPECT_GT(meanDirection(systemOf(findMember(members, "Hemi")), 2000).z, 0.3f);
    // an edge emits along its +Y in both
    const Vector3 edge = meanDirection(systemOf(findMember(members, "EdgeUp")));
    EXPECT_NEAR(edge.y, 1.0f, 1e-3f) << edge.x << " " << edge.y << " " << edge.z;

    // box: scale and thickness (x, z, y)
    const ParticleEffectDocument box = systemOf(findMember(members, "Box"));
    EXPECT_EQ(box.shape.shape, ShapeType::Box);
    EXPECT_EQ(box.shape.emitFrom, ShapeEmitFrom::Volume);
    EXPECT_FLOAT_EQ(box.shape.scale.y, 3.0f);
    EXPECT_FLOAT_EQ(box.shape.scale.z, 2.0f);
    EXPECT_FLOAT_EQ(box.shape.boxThickness.y, 0.3f);
    // donut: radius mode Loop and its spread
    const ParticleEffectDocument donut = systemOf(findMember(members, "Donut"));
    EXPECT_EQ(donut.shape.shape, ShapeType::Donut);
    EXPECT_FLOAT_EQ(donut.shape.donutRadius, 0.3f);
    EXPECT_EQ(donut.shape.radiusMode, ShapeMultiMode::Loop);
    EXPECT_FLOAT_EQ(donut.shape.radiusSpread, 0.25f);
    // hemisphere: shell (thickness 0), arc ping-pong, aligned to direction
    const ParticleEffectDocument hemi = systemOf(findMember(members, "Hemi"));
    EXPECT_FLOAT_EQ(hemi.shape.radiusThickness, 0.0f);
    EXPECT_EQ(hemi.shape.arcMode, ShapeMultiMode::PingPong);
    EXPECT_TRUE(hemi.shape.alignToDirection);

    const Json* mesh = systemReport(result.report, "Shapes/MeshShape");
    ASSERT_NE(mesh, nullptr);
    EXPECT_TRUE(hasNote(*mesh, "shape-unsupported"));
    EXPECT_EQ((*systemReport(result.report, "Shapes/NoShape"))["modules"]["ShapeModule"], "disabled");
}

TEST(UnityParticleImportTest, ConvertShapeRotation) {
    auto apply = [](const float unity[3], bool edge, const Vector3& v) {
        float e[3];
        unity::convertShapeRotation(unity, edge, e);
        return shapeTurn(e, v);
    };
    // the engine shape's axis (+Y) is Unity's +Z: no rotation points it along the engine's +Z
    const float none[3] = {0, 0, 0};
    const Vector3 a = apply(none, false, Vector3(0, 1, 0));
    EXPECT_NEAR(a.z, 1.0f, 1e-5f);
    // (-90, 0, 0): up
    const float up[3] = {-90, 0, 0};
    EXPECT_NEAR(apply(up, false, Vector3(0, 1, 0)).y, 1.0f, 1e-5f);
    // Unity's (0, 90, 0) turns +Z to its +X: the engine's -X
    const float right[3] = {0, 90, 0};
    EXPECT_NEAR(apply(right, false, Vector3(0, 1, 0)).x, -1.0f, 1e-5f);
    // a turn about every axis: the engine's turn of the shape's other axes too
    const float any[3] = {30, 45, 60};
    const Vector3 x = apply(any, false, Vector3(1, 0, 0)), y = apply(any, false, Vector3(0, 1, 0));
    EXPECT_NEAR(x.dotProduct(y), 0.0f, 1e-5f);
    EXPECT_NEAR(x.length(), 1.0f, 1e-5f);
}

TEST(UnityParticleImportTest, SceneInstanceWithModifications) {
    TempProject project;
    const unity::ImportResult result = importFile(project, "Assets/Scenes/Level.unity");
    ASSERT_TRUE(result.ok) << result.error;
    // the topmost GameObjects with a system: the (renamed) instance and the canvas' confetti
    ASSERT_EQ(result.prefabs.size(), 2u) << result.report.dump(2);
    EXPECT_EQ(result.prefabs[0], "assets/fx/BigFlame.prefab");
    EXPECT_EQ(result.prefabs[1], "assets/fx/Confetti.prefab");

    const YAML::Node members = YAML::LoadFile((project.root / result.prefabs[0]).string())["members"];
    const YAML::Node flame = findMember(members, "BigFlame");
    ASSERT_TRUE(flame.IsMap());
    // Attached hangs from the instance's root through the stripped placeholder (its own id);
    // the instance removed its Smoke child (m_RemovedGameObjects)
    EXPECT_EQ(childNames(flame), std::vector<std::string>({"Attached"}));
    EXPECT_FALSE(findMember(members, "Smoke").IsMap());
    // m_Materials.Array.data[0] overridden by an object reference: the Smoke material (alpha
    // blended, the default particle texture) instead of the additive flame
    const ParticleEffectDocument ps = systemOf(flame);
    EXPECT_EQ(ps.renderer.blend, ParticleBlend::Alpha);
    EXPECT_TRUE(ps.renderer.texture.empty());
    EXPECT_FLOAT_EQ(ps.main.startSize.multiplier, 4.0f);
    EXPECT_NEAR(ps.main.startSize.curve.evaluate(0.0f), 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(ps.emission.rateOverTime.constant, 50.0f);
    EXPECT_NEAR(ps.sizeOverLifetime.size.curve.evaluate(0.0f), 0.25f, 1e-5f);
    EXPECT_FALSE(ps.prewarm);
    // the auto destruct script was removed in the scene
    EXPECT_EQ(ps.stopAction, StopAction::None);

    const ParticleEffectDocument attached = systemOf(findMember(members, "Attached"));
    EXPECT_EQ(attached.shape.shape, ShapeType::Edge);
    EXPECT_EQ(attached.renderer.renderMode, RenderMode::StretchedBillboard);
    EXPECT_FLOAT_EQ(attached.renderer.lengthScale, 3.0f);

    bool unknown = false;
    for (const Json& warning : result.report["warnings"]) {
        unknown = unknown || warning.get<std::string>().find("NotAModule.value") != std::string::npos;
        EXPECT_EQ(warning.get<std::string>().find("m_LocalEulerAnglesHint"), std::string::npos);
    }
    EXPECT_TRUE(unknown) << result.report.dump(2);

    // the confetti: a UI particle, its PSD texture reported
    const Json* confetti = systemReport(result.report, "Canvas/Confetti");
    ASSERT_NE(confetti, nullptr);
    EXPECT_TRUE(hasNote(*confetti, "ui-particle"));
    EXPECT_TRUE(hasNote(*confetti, "texture-format"));
    const ParticleSystemComponent confettiPs = componentOf(findMember(YAML::LoadFile((project.root / result.prefabs[1]).string())["members"], "Confetti"));
    EXPECT_FALSE(confettiPs.playOnAwake);
    EXPECT_FALSE(confettiPs.inlineEffect.looping);
    ASSERT_EQ(confettiPs.inlineEffect.emission.bursts.size(), 1u);
    EXPECT_FLOAT_EQ(confettiPs.inlineEffect.emission.bursts[0].count.constant, 10.0f);
}

TEST(UnityParticleImportTest, SelectObjectNumberedNamesAndDryRun) {
    TempProject project;
    // dry run: the report, no file
    const unity::ImportResult dry = importFile(project, "Assets/Fx/Shapes.prefab", "Shapes/Up", true);
    ASSERT_TRUE(dry.ok) << dry.error;
    EXPECT_EQ(dry.prefabs, std::vector<std::string>({"assets/fx/Up.prefab"}));
    EXPECT_FALSE(fs::exists(project.root / "assets"));
    EXPECT_TRUE(dry.written.empty());

    const unity::ImportResult first = importFile(project, "Assets/Fx/Shapes.prefab", "Up");
    ASSERT_TRUE(first.ok) << first.error;
    const unity::ImportResult second = importFile(project, "Assets/Fx/Shapes.prefab", "Up");
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(second.prefabs, std::vector<std::string>({"assets/fx/Up 1.prefab"}));
    EXPECT_TRUE(fs::is_regular_file(project.root / "assets/fx/Up 1.import-report.json"));

    const unity::ImportResult missing = importFile(project, "Assets/Fx/Shapes.prefab", "Nope");
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("Nope"), std::string::npos);

    unity::ImportOptions outside;
    outside.file = UNITY / "Assets/Fx/Flame.prefab";
    outside.engineProject = project.root;
    outside.dest = "../elsewhere";
    EXPECT_FALSE(unity::importParticles(outside).ok);
}

namespace {

    RendererModule rendererOf(const YAML::Node& members, const std::string& name) {
        const YAML::Node member = findMember(members, name);
        return member.IsMap() ? systemOf(member).renderer : RendererModule();
    }

}

TEST(UnityParticleImportTest, ShaderBlendsAndSharedAssets) {
    TempProject project;
    unity::ImportOptions options;
    options.file = UNITY / "Assets/Fx/Materials.prefab";
    options.engineProject = project.root;
    const unity::ImportResult result = unity::importParticles(options);
    ASSERT_TRUE(result.ok) << result.error;
    const YAML::Node members = YAML::LoadFile((project.root / result.prefabs[0]).string())["members"];

    // "blend one oneminussrcalpha" (any case, after a comment naming another Blend): premultiplied
    EXPECT_EQ(rendererOf(members, "Premul").blend, ParticleBlend::Premultiplied);
    // Blend [_SrcBlend] [_DstBlend] with the material's 1 / 1: additive
    EXPECT_EQ(rendererOf(members, "ByProperties").blend, ParticleBlend::Additive);
    // built-in shaders by guid: Particles/Standard Unlit with _Mode 4 additive, 202 premultiplied
    EXPECT_EQ(rendererOf(members, "StandardAdd").blend, ParticleBlend::Additive);
    EXPECT_EQ(rendererOf(members, "LegacyPremul").blend, ParticleBlend::Premultiplied);
    // a _MainTex not in the project: the soft particle, with a note
    const RendererModule noTexture = rendererOf(members, "NoTexture");
    EXPECT_TRUE(noTexture.texture.empty());
    EXPECT_EQ(noTexture.untexturedShape, UntexturedShape::Disc);
    EXPECT_TRUE(hasNote(*systemReport(result.report, "Materials/NoTexture"), "texture-missing"));

    // two Unity materials named Fire, two textures named smoke.png: two texture files
    const std::string textureA = rendererOf(members, "FireA").texture, textureB = rendererOf(members, "FireB").texture;
    EXPECT_EQ(textureA, "assets/fx/textures/smoke.png");
    EXPECT_EQ(textureB, "assets/fx/textures/smoke 1.png");
    EXPECT_TRUE(fs::is_regular_file(project.root / textureA));
    EXPECT_TRUE(fs::is_regular_file(project.root / textureB));
    // the report lists both materials (by their Unity files)
    EXPECT_GE(result.report["materials"].size(), 2u);

    // a material used twice: both systems carry its notes
    EXPECT_TRUE(hasNote(*systemReport(result.report, "Materials/Premul"), "shader-approximated"));
    EXPECT_TRUE(hasNote(*systemReport(result.report, "Materials/PremulAgain"), "shader-approximated"));

    // Unity's render mode None: simulated, not drawn
    bool hiddenTwin = false;
    for (const YAML::Node& child : findMember(members, "Materials")["children"]) {
        if (child["name"].as<std::string>() != "Twin") continue;
        hiddenTwin = hiddenTwin || !systemOf(child).renderer.enabled;
    }
    EXPECT_TRUE(hiddenTwin);
}

TEST(UnityParticleImportTest, ObjectPickingByPathThenUniqueName) {
    TempProject project;
    // an exact path wins
    const unity::ImportResult byPath = importFile(project, "Assets/Fx/Flame.prefab", "Flame/Smoke", true);
    ASSERT_TRUE(byPath.ok) << byPath.error;
    EXPECT_EQ(byPath.prefabs, std::vector<std::string>({"assets/fx/Smoke.prefab"}));
    // a name naming two GameObjects is refused with both paths
    const unity::ImportResult twins = importFile(project, "Assets/Fx/Materials.prefab", "Twin", true);
    EXPECT_FALSE(twins.ok);
    EXPECT_NE(twins.error.find("2 GameObjects"), std::string::npos) << twins.error;
    EXPECT_NE(twins.error.find("Materials/Twin"), std::string::npos) << twins.error;
}

TEST(UnityParticleImportTest, NestedPrefabInstances) {
    TempProject project;
    const unity::ImportResult result = importFile(project, "Assets/Scenes/Nested.unity");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.prefabs.size(), 1u) << result.report.dump(2);
    // Rig (no system) holds the Flame instance: that is the topmost system, renamed by Rig
    EXPECT_EQ(result.prefabs[0], "assets/fx/NestedFlame.prefab");
    const YAML::Node members = YAML::LoadFile((project.root / result.prefabs[0]).string())["members"];
    const ParticleEffectDocument ps = systemOf(findMember(members, "NestedFlame"));
    EXPECT_FLOAT_EQ(ps.main.startSpeed.constant, 9.0f) << "Rig's modification of its nested instance";
    EXPECT_FLOAT_EQ(ps.emission.rateOverTime.constant, 33.0f) << "the scene's modification of the nested object";
    EXPECT_EQ(childNames(findMember(members, "NestedFlame")), std::vector<std::string>({"Smoke"}));
}

TEST(UnityParticleImportTest, MalformedFileFailsCleanly) {
    TempProject project;
    const unity::ImportResult result = importFile(project, "Assets/Fx/Broken.prefab");
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
    // the CLI exits 1 with a message (no exception escapes)
    const std::string file = (UNITY / "Assets/Fx/Broken.prefab").string();
    const std::string projectDir = project.root.string();
    std::vector<std::string> args = {"import-unity-particles", file, "--project", projectDir};
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    EXPECT_EQ(CommandLine::runImportUnityParticlesCommand((int)args.size(), argv.data(), "jasperengine-editor"), 1);
}

TEST(UnityParticleImportTest, NeverWritesIntoTheUnityProject) {
    unity::ImportOptions options;
    options.file = UNITY / "Assets/Fx/Flame.prefab";
    options.engineProject = UNITY;
    options.dest = "ASSETS/Imported";
    options.dryRun = true;
    const unity::ImportResult result = unity::importParticles(options);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("inside the Unity project"), std::string::npos) << result.error;
}
