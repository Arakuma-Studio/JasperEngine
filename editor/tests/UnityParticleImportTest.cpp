// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// unity/UnityParticleImport (particles spec 6.10, 9) on the synthetic Unity project of
// fixtures/unity-particles (hand-written to Unity's YAML forms; no game or Asset Store file):
// every mapped value of a system with most modules, the axis conversion (a cone rotated
// (-90, 0, 0) in Unity emits along the engine's +Y; no shape: along +Z), gamma to linear colours,
// materials (a custom shader by its Blend state, the built-in ones, _TintColor, _InvFade, tiling)
// and textures (Alpha8 expanded, PSD reported), a scene's prefab instance with modifications, a
// removed component and a `stripped` placeholder with its own id, the report codes, numbered
// names, dry runs.

#include <gtest/gtest.h>

#include "Catalog.h"
#include "Stream.h"
#include "particles/ParticleSimulation.h"
#include "unity/UnityParticleImport.h"
#include "util/Color.h"

#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    const fs::path UNITY = fs::path(JASPER_EDITOR_TEST_FIXTURES) / "unity-particles" / "Project";

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

    std::string readText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
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

    ParticleSystemComponent systemOf(const YAML::Node& member) {
        return Stream::decodeParticleSystemComponent(component(member, ComponentType::ParticleSystemComponent));
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

    // the engine's sRGB -> linear (what Unity's gamma colours become)
    float linear(float gamma) {
        return Color::sRGBToLinear(Vector3(gamma, gamma, gamma)).x;
    }

    // the mean start velocity direction of `count` particles emitted by an imported system
    Vector3 meanDirection(ParticleSystemComponent ps, int count = 200) {
        ps.main.gravityModifier = MinMaxCurve(0.0f);
        ParticleEmitterFrame frame;
        ParticleEnvironment env;
        particles::emit(ps, count, frame, env);
        Vector3 sum(0, 0, 0);
        for (size_t i = 0; i < ps.state.count(); i++) sum = sum + ps.state.velocity[i].normalized();
        return ps.state.count() ? sum / (float)ps.state.count() : sum;
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

    const ParticleSystemComponent ps = systemOf(flame);
    EXPECT_EQ(ps.version, 2);
    EXPECT_FLOAT_EQ(ps.main.duration, 2.0f);
    EXPECT_FLOAT_EQ(ps.main.simulationSpeed, 1.5f);
    EXPECT_TRUE(ps.main.looping);
    EXPECT_TRUE(ps.main.prewarm);
    EXPECT_FALSE(ps.main.autoRandomSeed);
    EXPECT_EQ(ps.main.randomSeed, 1234u);
    EXPECT_EQ(ps.main.simulationSpace, ParticleSystemSimulationSpace::World);
    EXPECT_EQ(ps.main.scalingMode, ParticleSystemScalingMode::Hierarchy);
    EXPECT_EQ(ps.main.maxParticles, 500u);
    // CFX_AutoDestructShuriken: destroyed when it finishes
    EXPECT_EQ(ps.main.stopAction, ParticleSystemStopAction::Destroy);

    EXPECT_EQ(ps.main.startLifetime.mode, ParticleSystemCurveMode::TwoConstants);
    EXPECT_FLOAT_EQ(ps.main.startLifetime.constantMin, 0.5f);
    EXPECT_FLOAT_EQ(ps.main.startLifetime.constant, 1.0f);
    EXPECT_FLOAT_EQ(ps.main.startSpeed.constant, 3.0f);
    EXPECT_EQ(ps.main.startSize.mode, ParticleSystemCurveMode::Curve);
    EXPECT_FLOAT_EQ(ps.main.startSize.constant, 2.0f);
    EXPECT_NEAR(ps.main.startSize.curve.evaluate(0.0f), 0.5f, 1e-5f);
    // the roll's sign changes (left handed view)
    EXPECT_FLOAT_EQ(ps.main.startRotation.constant, -1.0f);
    EXPECT_FLOAT_EQ(ps.main.startRotation.constantMin, 1.0f);
    EXPECT_FLOAT_EQ(ps.main.flipRotation, 0.5f);
    EXPECT_FLOAT_EQ(ps.main.gravityModifier.constant, 0.2f);

    // start colour: two colours, gamma to linear
    EXPECT_EQ(ps.main.startColor.mode, ParticleSystemGradientMode::TwoColors);
    EXPECT_NEAR(ps.main.startColor.color.x, linear(0.5f), 1e-4f);
    EXPECT_NEAR(ps.main.startColor.colorMin.x, 1.0f, 1e-5f);
    EXPECT_NEAR(ps.main.startColor.colorMin.y, 0.0f, 1e-5f);

    // emission and its burst
    EXPECT_FLOAT_EQ(ps.emission.rateOverTime.constant, 20.0f);
    ASSERT_EQ(ps.emission.bursts.size(), 1u);
    EXPECT_FLOAT_EQ(ps.emission.bursts[0].time, 0.1f);
    EXPECT_FLOAT_EQ(ps.emission.bursts[0].count.constant, 5.0f);
    EXPECT_EQ(ps.emission.bursts[0].cycles, 3);
    EXPECT_FLOAT_EQ(ps.emission.bursts[0].interval, 0.5f);
    EXPECT_FLOAT_EQ(ps.emission.bursts[0].probability, 0.5f);

    // shape: cone, its position mirrored
    EXPECT_EQ(ps.shape.shapeType, ParticleSystemShapeType::Cone);
    EXPECT_FLOAT_EQ(ps.shape.angle, 15.0f);
    EXPECT_FLOAT_EQ(ps.shape.radius, 0.25f);
    EXPECT_FLOAT_EQ(ps.shape.position.x, -0.5f);

    // colour over lifetime: three keys, linear colours, times from 0..65535
    ASSERT_TRUE(ps.colorOverLifetime.enabled);
    const Gradient& gradient = ps.colorOverLifetime.color.gradient;
    ASSERT_EQ(gradient.colorKeys.size(), 3u);
    EXPECT_NEAR(gradient.colorKeys[1].time, 32768.0f / 65535.0f, 1e-5f);
    EXPECT_NEAR(gradient.colorKeys[1].color.y, linear(0.5f), 1e-4f);
    EXPECT_NEAR(gradient.alphaKeys[1].alpha, 0.5f, 1e-5f);

    ASSERT_TRUE(ps.sizeOverLifetime.enabled);
    EXPECT_NEAR(ps.sizeOverLifetime.size.curve.evaluate(0.0f), 0.2f, 1e-5f);
    // velocity: x mirrored; orbital y negated
    ASSERT_TRUE(ps.velocityOverLifetime.enabled);
    EXPECT_FLOAT_EQ(ps.velocityOverLifetime.x.constantMin, -1.0f);
    EXPECT_FLOAT_EQ(ps.velocityOverLifetime.x.constant, -2.0f);
    EXPECT_FLOAT_EQ(ps.velocityOverLifetime.orbitalY.constant, -0.5f);
    EXPECT_EQ(ps.velocityOverLifetime.space, ParticleSystemSimulationSpace::Local);
    ASSERT_TRUE(ps.forceOverLifetime.enabled);
    EXPECT_FLOAT_EQ(ps.forceOverLifetime.y.constant, -5.0f);
    EXPECT_EQ(ps.forceOverLifetime.space, ParticleSystemSimulationSpace::World);
    ASSERT_TRUE(ps.limitVelocityOverLifetime.enabled);
    EXPECT_FLOAT_EQ(ps.limitVelocityOverLifetime.limit.constant, 8.0f);
    EXPECT_NEAR(ps.limitVelocityOverLifetime.limit.curve.evaluate(1.0f), 0.25f, 1e-5f);
    EXPECT_FLOAT_EQ(ps.limitVelocityOverLifetime.dampen, 0.1f);
    ASSERT_TRUE(ps.noise.enabled);
    EXPECT_EQ(ps.noise.octaves, 2);

    // texture sheet: 2x2 at 25 FPS, the start frame a share of the 4 frames
    ASSERT_TRUE(ps.textureSheetAnimation.enabled);
    EXPECT_EQ(ps.textureSheetAnimation.numTilesX, 2);
    EXPECT_EQ(ps.textureSheetAnimation.numTilesY, 2);
    EXPECT_EQ(ps.textureSheetAnimation.timeMode, ParticleSystemAnimationTimeMode::FPS);
    EXPECT_FLOAT_EQ(ps.textureSheetAnimation.fps, 25.0f);
    EXPECT_FLOAT_EQ(ps.textureSheetAnimation.startFrame.constant, 2.0f);

    // renderer
    EXPECT_EQ(ps.renderer.renderMode, ParticleSystemRenderMode::VerticalBillboard);
    EXPECT_EQ(ps.renderer.sortMode, ParticleSystemSortMode::Distance);
    EXPECT_FLOAT_EQ(ps.renderer.sortingFudge, 10.0f);
    EXPECT_EQ(ps.renderer.sortPriority, 2);

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
    const ParticleSystemComponent ps = systemOf(smoke);
    EXPECT_EQ(ps.main.simulationSpace, ParticleSystemSimulationSpace::Local);
    EXPECT_EQ(ps.shape.shapeType, ParticleSystemShapeType::Sphere);
    EXPECT_EQ(ps.renderer.renderMode, ParticleSystemRenderMode::Mesh);
    EXPECT_EQ(ps.renderer.mesh, "cube");
    EXPECT_EQ(ps.renderer.alignment, ParticleSystemRenderSpace::Local);
    EXPECT_EQ(ps.colorOverLifetime.color.mode, ParticleSystemGradientMode::TwoGradients);
    EXPECT_NEAR(ps.colorOverLifetime.color.gradientMin.colorKeys.back().color.x, linear(0.5f), 1e-4f);
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

    // the custom shader "Custom/Fire Additive (Soft)" (Blend SrcAlpha One): additive, soft, its tint
    const fs::path fireFile = project.root / "assets/fx/materials/FireAdd.material";
    ASSERT_TRUE(fs::is_regular_file(fireFile));
    const Material fire = Stream::decodeMaterial(YAML::LoadFile(fireFile.string()));
    EXPECT_EQ(fire.shader, "library:unlit");
    EXPECT_EQ(fire.blendMode, MaterialBlendMode::ADDITIVE);
    EXPECT_NE(std::find(fire.shaderFeatures.begin(), fire.shaderFeatures.end(), "SOFT"), fire.shaderFeatures.end());
    std::string baseTexture;
    for (const auto& texture : fire.shaderTextures) {
        if (texture.first == "u_baseTexture") baseTexture = texture.second.getPath();
    }
    EXPECT_EQ(baseTexture, "assets/fx/textures/flame.png");

    // the flame mesh links the material file
    const MeshComponent mesh = Stream::decodeMeshComponent(component(findMember(prefab["members"], "Flame"), ComponentType::MeshComponent));
    ASSERT_GT(mesh.submeshes.size(), 0u);
    EXPECT_EQ(mesh.submeshes[0].material.name, "assets/fx/materials/FireAdd.material");
    EXPECT_FALSE(mesh.castShadows);

    // the Alpha8 texture: white with the source's alpha
    int w = 0, h = 0, n = 0;
    unsigned char* pixels = stbi_load((project.root / "assets/fx/textures/flame.png").string().c_str(), &w, &h, &n, 4);
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(w, 8);
    EXPECT_EQ(pixels[0], 255);
    EXPECT_EQ(pixels[4 * 1 + 3], 32);
    stbi_image_free(pixels);

    // the smoke: Mobile/Particles/Alpha Blended with the default particle texture
    const Material smoke = Stream::decodeMaterial(YAML::LoadFile((project.root / "assets/fx/materials/Smoke.material").string()));
    EXPECT_EQ(smoke.blendMode, MaterialBlendMode::ALPHA);
    EXPECT_NE(std::find(smoke.shaderFeatures.begin(), smoke.shaderFeatures.end(), "PARTICLE_TEXTURE"), smoke.shaderFeatures.end());

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
    const ParticleSystemComponent none = systemOf(findMember(members, "NoShape"));
    EXPECT_TRUE(none.shape.enabled);
    const Vector3 forward = meanDirection(none);
    EXPECT_NEAR(forward.z, 1.0f, 1e-3f) << forward.x << " " << forward.y << " " << forward.z;
    // a hemisphere faces Unity's +Z
    EXPECT_GT(meanDirection(systemOf(findMember(members, "Hemi")), 2000).z, 0.3f);
    // an edge emits along its +Y in both
    const Vector3 edge = meanDirection(systemOf(findMember(members, "EdgeUp")));
    EXPECT_NEAR(edge.y, 1.0f, 1e-3f) << edge.x << " " << edge.y << " " << edge.z;

    // box: scale and thickness (x, z, y)
    const ParticleSystemComponent box = systemOf(findMember(members, "Box"));
    EXPECT_EQ(box.shape.shapeType, ParticleSystemShapeType::Box);
    EXPECT_FLOAT_EQ(box.shape.scale.y, 3.0f);
    EXPECT_FLOAT_EQ(box.shape.scale.z, 2.0f);
    EXPECT_FLOAT_EQ(box.shape.boxThickness.y, 0.3f);
    // donut: radius mode Loop and its spread
    const ParticleSystemComponent donut = systemOf(findMember(members, "Donut"));
    EXPECT_EQ(donut.shape.shapeType, ParticleSystemShapeType::Donut);
    EXPECT_FLOAT_EQ(donut.shape.donutRadius, 0.3f);
    EXPECT_EQ(donut.shape.radiusMode, ParticleSystemShapeMultiModeValue::Loop);
    EXPECT_FLOAT_EQ(donut.shape.radiusSpread, 0.25f);
    // hemisphere: shell (thickness 0), arc ping-pong, aligned to direction
    const ParticleSystemComponent hemi = systemOf(findMember(members, "Hemi"));
    EXPECT_FLOAT_EQ(hemi.shape.radiusThickness, 0.0f);
    EXPECT_EQ(hemi.shape.arcMode, ParticleSystemShapeMultiModeValue::PingPong);
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
        const float deg = 3.14159265f / 180.0f;
        return particles::eulerRadians(Vector3(e[0] * deg, e[1] * deg, e[2] * deg)) * v;
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
    // Attached hangs from the instance's root through the stripped placeholder (its own id)
    EXPECT_EQ(childNames(flame), std::vector<std::string>({"Smoke", "Attached"}));
    const ParticleSystemComponent ps = systemOf(flame);
    EXPECT_FLOAT_EQ(ps.main.startSize.constant, 4.0f);
    EXPECT_NEAR(ps.main.startSize.curve.evaluate(0.0f), 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(ps.emission.rateOverTime.constant, 50.0f);
    EXPECT_NEAR(ps.sizeOverLifetime.size.curve.evaluate(0.0f), 0.25f, 1e-5f);
    EXPECT_FALSE(ps.main.prewarm);
    // the auto destruct script was removed in the scene
    EXPECT_EQ(ps.main.stopAction, ParticleSystemStopAction::None);

    const ParticleSystemComponent attached = systemOf(findMember(members, "Attached"));
    EXPECT_EQ(attached.shape.shapeType, ParticleSystemShapeType::Edge);
    EXPECT_EQ(attached.renderer.renderMode, ParticleSystemRenderMode::Stretch);
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
    const ParticleSystemComponent confettiPs = systemOf(findMember(YAML::LoadFile((project.root / result.prefabs[1]).string())["members"], "Confetti"));
    EXPECT_FALSE(confettiPs.main.playOnAwake);
    EXPECT_FALSE(confettiPs.main.looping);
    ASSERT_EQ(confettiPs.emission.bursts.size(), 1u);
    EXPECT_FLOAT_EQ(confettiPs.emission.bursts[0].count.constant, 10.0f);
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
