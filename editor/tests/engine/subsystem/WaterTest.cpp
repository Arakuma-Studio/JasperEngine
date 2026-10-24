// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:water outside the shader (engine/core/object/environment/Water.h): the C++ copy of
// its waves against library/water.glsl's formula, the surfaces a scene's water meshes make
// (material values, bounds, the overlap rule), buoyancy floating a Rigidbody in a real
// physics step, the Rigidbody fields' scene encoding, the built-in ripples and noise
// textures, the manifests (library:water's new features and textures, library:underwater)
// and the GameObject menu's 3D Object > Water entry.

#include <gtest/gtest.h>

#include "Scene.h"
#include "Stream.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "object/GameObject.h"
#include "object/environment/Water.h"
#include "object/physics/Rigidbody.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "window/HierarchyWindow.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // library/water.glsl's waterWaves, in double precision
    void referenceWaves(double x, double z, double time, double dx, double dz, double height, double waveLength, double speed,
                        double& h, double& slopeX, double& slopeZ) {
        const double twoPi = 6.28318530718;
        const double length = std::max(std::sqrt(dx * dx + dz * dz), 1e-4);
        const double fx = dx / length, fz = dz / length;
        const double sx = fx * 0.6428 - fz * 0.7660, sz = fx * 0.7660 + fz * 0.6428;
        const double length1 = std::max(waveLength, 1e-3), length2 = length1 * 0.6;
        auto fract = [](double v) { return v - std::floor(v); };
        const double phase1 = ((fx * x + fz * z) / length1 - fract(time * speed / length1)) * twoPi;
        const double phase2 = ((sx * x + sz * z) / length2 - fract(time * speed / length2) + 0.37) * twoPi;
        h = height * (std::sin(phase1) + 0.5 * std::sin(phase2));
        slopeX = height * twoPi * (fx * std::cos(phase1) / length1 + sx * 0.5 * std::cos(phase2) / length2);
        slopeZ = height * twoPi * (fz * std::cos(phase1) / length1 + sz * 0.5 * std::cos(phase2) / length2);
    }

    // a water mesh: a grid at a height, drawn with library:water
    Entity makeWater(Scene& scene, const Vector3& position, float size, const std::vector<std::string>& features = {},
                     const ShaderUniformValues& params = {}) {
        const Entity entity = scene.createEntity();
        Transform transform;
        transform.position = position;
        transform.worldPosition = position;
        transform.modelMatrix = Matrix4::translateMatrix(position);
        scene.addComponent<Transform>(entity, transform);
        scene.addComponent<MeshComponent>(entity, MeshComponent());
        MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        scene.getSystem<MeshSystem>()->createGrid(mesh, size, size, 8);
        mesh.submeshes[0].material.shader = Water::SHADER;
        mesh.submeshes[0].material.shaderFeatures = features;
        mesh.submeshes[0].material.shaderParams = params;
        return entity;
    }

    ShaderUniformValues values(std::initializer_list<std::pair<const char*, float>> list) {
        ShaderUniformValues result;
        for (const auto& item : list) {
            ShaderUniforms::set(result, item.first, Vector4(item.second, 0.0f, 0.0f, 0.0f));
        }
        return result;
    }

}

TEST(Water, WavesMatchTheShaderFormula) {
    const struct { float x, z, time, dx, dz, height, length, speed; } cases[] = {
        {0.0f, 0.0f, 0.0f, 1.0f, 0.3f, 0.06f, 2.5f, 0.8f},
        {3.7f, -2.1f, 12.25f, 1.0f, 0.3f, 0.1f, 4.0f, 0.8f},
        {-15.0f, 40.0f, 100.5f, -0.2f, 1.0f, 0.5f, 7.0f, 2.0f},
        {0.5f, 0.5f, 3.0f, 0.0f, 0.0f, 0.2f, 1.0f, 1.0f},
    };
    for (const auto& c : cases) {
        double h, sx, sz;
        referenceWaves(c.x, c.z, c.time, c.dx, c.dz, c.height, c.length, c.speed, h, sx, sz);
        const Vector3 waves = Water::getWaves(c.x, c.z, c.time, Vector2(c.dx, c.dz), c.height, c.length, c.speed);
        EXPECT_NEAR(waves.x, h, 1e-4) << "height at " << c.x << ", " << c.z;
        EXPECT_NEAR(waves.y, sx, 1e-3) << "slope x at " << c.x << ", " << c.z;
        EXPECT_NEAR(waves.z, sz, 1e-3) << "slope z at " << c.x << ", " << c.z;

        // the slope is the height's derivative (the normal fits the raised surface)
        const float e = 1e-3f;
        const float hx = Water::getWaves(c.x + e, c.z, c.time, Vector2(c.dx, c.dz), c.height, c.length, c.speed).x -
                         Water::getWaves(c.x - e, c.z, c.time, Vector2(c.dx, c.dz), c.height, c.length, c.speed).x;
        EXPECT_NEAR(hx / (2.0f * e), waves.y, 2e-2);
    }
}

TEST(Water, SurfacesReadTheMaterial) {
    Scene scene;
    makeWater(scene, Vector3(0.0f, 1.5f, 0.0f), 10.0f);
    const Entity wavy = makeWater(scene, Vector3(30.0f, -2.0f, 0.0f), 4.0f, {"WAVES"},
                                  values({{"waveHeight", 0.25f}, {"waveLength", 3.0f}, {"buoyancy", 0.0f}, {"buoyancyDepth", 2.0f}}));
    // another shader: not a water
    const Entity box = scene.createEntity();
    scene.addComponent<Transform>(box, Transform());
    scene.addComponent<MeshComponent>(box, MeshComponent());
    scene.getSystem<MeshSystem>()->createBox(scene.getComponent<MeshComponent>(box), 1.0f, 1.0f, 1.0f);

    const std::vector<WaterSurface> surfaces = Water::getSurfaces(&scene);
    ASSERT_EQ(surfaces.size(), 2u);
    const WaterSurface& flat = surfaces[0];
    EXPECT_FLOAT_EQ(flat.level, 1.5f);
    EXPECT_FALSE(flat.waves);
    EXPECT_TRUE(flat.buoyancy) << "the manifest default";
    EXPECT_FLOAT_EQ(flat.buoyancyDepth, 0.0f);
    EXPECT_TRUE(flat.covers(4.9f, -4.9f));
    EXPECT_FALSE(flat.covers(5.1f, 0.0f));
    EXPECT_EQ(flat.getBottom(), -std::numeric_limits<float>::infinity());
    EXPECT_FLOAT_EQ(flat.getHeight(1.0f, 2.0f, 5.0f), 1.5f);
    EXPECT_FLOAT_EQ(flat.getNormal(1.0f, 2.0f, 5.0f).y, 1.0f);
    // the manifest's colour defaults, linear
    EXPECT_GT(flat.shallowColor.w, 0.0f);

    const WaterSurface& waves = surfaces[1];
    EXPECT_EQ(waves.entity, wavy);
    EXPECT_TRUE(waves.waves);
    EXPECT_FLOAT_EQ(waves.waveHeight, 0.25f);
    EXPECT_FLOAT_EQ(waves.waveLength, 3.0f);
    EXPECT_FALSE(waves.buoyancy);
    EXPECT_FLOAT_EQ(waves.getBottom(), -4.0f);
    const float t = 7.5f;
    const Vector3 expected = Water::getWaves(31.0f, 0.5f, t, waves.waveDirection, 0.25f, 3.0f, waves.waveSpeed);
    EXPECT_FLOAT_EQ(waves.getHeight(31.0f, 0.5f, t), -2.0f + expected.x);
    const Vector3 normal = waves.getNormal(31.0f, 0.5f, t);
    EXPECT_NEAR(normal.length(), 1.0f, 1e-5);
    EXPECT_NEAR(normal.x / normal.y, -expected.y, 1e-4);

    // a disabled mesh or an inactive GameObject is no water
    scene.getComponent<MeshComponent>(wavy).enabled = false;
    EXPECT_EQ(Water::getSurfaces(&scene).size(), 1u);
}

TEST(Water, OverlappingWatersTheHighestBottomWins) {
    std::vector<WaterSurface> surfaces(2);
    // a lake without a bottom, and a pool in it with one
    surfaces[0].level = 0.0f;
    surfaces[0].minXZ = Vector2(-100.0f, -100.0f);
    surfaces[0].maxXZ = Vector2(100.0f, 100.0f);
    surfaces[1].level = 5.0f;
    surfaces[1].buoyancyDepth = 2.0f;
    surfaces[1].minXZ = Vector2(-1.0f, -1.0f);
    surfaces[1].maxXZ = Vector2(1.0f, 1.0f);

    EXPECT_EQ(Water::findSurface(surfaces, Vector3(0.0f, 4.0f, 0.0f)), &surfaces[1]) << "in the pool";
    EXPECT_EQ(Water::findSurface(surfaces, Vector3(0.0f, 10.0f, 0.0f)), &surfaces[1]) << "over the pool";
    EXPECT_EQ(Water::findSurface(surfaces, Vector3(0.0f, 2.0f, 0.0f)), &surfaces[0]) << "under the pool's bottom: the lake";
    EXPECT_EQ(Water::findSurface(surfaces, Vector3(3.0f, 4.0f, 0.0f)), &surfaces[0]) << "beside the pool";
    EXPECT_EQ(Water::findSurface(surfaces, Vector3(300.0f, 0.0f, 0.0f)), nullptr);

    // a pool that floats nothing leaves its bodies to the lake
    surfaces[1].buoyancy = false;
    EXPECT_EQ(Water::findSurface(surfaces, Vector3(0.0f, 4.0f, 0.0f), true), &surfaces[0]);
    EXPECT_EQ(Water::findSurface(surfaces, Vector3(0.0f, 4.0f, 0.0f), false), &surfaces[1]);

    // two without a bottom: the higher surface
    surfaces[1].buoyancy = true;
    surfaces[1].buoyancyDepth = 0.0f;
    EXPECT_EQ(Water::findSurface(surfaces, Vector3(0.0f, -1.0f, 0.0f)), &surfaces[1]);
}

TEST(Water, SceneQueries) {
    Scene scene;
    makeWater(scene, Vector3(0.0f, 2.0f, 0.0f), 10.0f);
    ASSERT_TRUE(Water::getHeight(&scene, 1.0f, 1.0f).has_value());
    EXPECT_FLOAT_EQ(*Water::getHeight(&scene, 1.0f, 1.0f), 2.0f);
    EXPECT_FALSE(Water::getHeight(&scene, 20.0f, 1.0f).has_value());
    ASSERT_TRUE(Water::getNormal(&scene, 1.0f, 1.0f).has_value());
    EXPECT_FLOAT_EQ(Water::getNormal(&scene, 1.0f, 1.0f)->y, 1.0f);
    EXPECT_TRUE(Water::isUnderwater(&scene, Vector3(0.0f, 1.0f, 0.0f)));
    EXPECT_FALSE(Water::isUnderwater(&scene, Vector3(0.0f, 3.0f, 0.0f)));
    EXPECT_FALSE(Water::isUnderwater(&scene, Vector3(20.0f, 1.0f, 0.0f)));
}

#ifdef JASPERENGINE_PHYSICS_3D
namespace {

    // the height a 1 x 1 x 1 box dropped onto a water at y 0 has after `seconds`
    float floatBox(float buoyancy, float waterBuoyancy, float seconds) {
        Scene scene;
        makeWater(scene, Vector3(0.0f, 0.0f, 0.0f), 20.0f, {}, values({{"buoyancy", waterBuoyancy}}));

        GameObject object(&scene);
        object.setPosition(Vector3(0.0f, 1.0f, 0.0f));
        Transform& transform = scene.getComponent<Transform>(object.getEntity());
        transform.worldPosition = transform.position;
        Rigidbody body = object.getRigidbody();
        body.createBoxCollider(1.0f, 1.0f, 1.0f);
        body.setType(BodyType::DYNAMIC);
        body.setBuoyancy(buoyancy);

        const double step = 1.0 / 60.0;
        for (int i = 0; i < (int)(seconds / step); i++) {
            scene.fixedUpdate(step);
            // the world pose the next step syncs the body from (RenderSystem::update's job)
            Transform& moved = scene.getComponent<Transform>(object.getEntity());
            moved.worldPosition = moved.position;
            moved.worldRotation = moved.rotation;
        }
        return body.getPosition().y;
    }

}

TEST(Water, BuoyancyFloatsARigidbody) {
    const float floating = floatBox(1.5f, 1.0f, 8.0f);
    // floats with a third of it above the surface, give or take the bobbing
    EXPECT_GT(floating, -0.5f);
    EXPECT_LT(floating, 0.5f);

    // no buoyancy, or a water that floats nothing: it falls through (the water has no collider)
    EXPECT_LT(floatBox(0.0f, 1.0f, 3.0f), -10.0f);
    EXPECT_LT(floatBox(1.5f, 0.0f, 3.0f), -10.0f);
}
#endif

TEST(Water, RigidbodyFieldsRoundTrip) {
    RigidbodyComponent body;
    EXPECT_FLOAT_EQ(body.buoyancy, 0.0f);
    body.buoyancy = 1.25f;
    body.waterDrag = 0.8f;
    body.waterAngularDrag = 0.05f;
    const YAML::Node node = editor::Stream::encodeRigidbodyComponent(body);
    EXPECT_FLOAT_EQ(node["buoyancy"].as<float>(), 1.25f);
    const RigidbodyComponent decoded = editor::Stream::decodeRigidbodyComponent(node, nullptr, Vector3(1.0f, 1.0f, 1.0f));
    EXPECT_FLOAT_EQ(decoded.buoyancy, 1.25f);
    EXPECT_FLOAT_EQ(decoded.waterDrag, 0.8f);
    EXPECT_FLOAT_EQ(decoded.waterAngularDrag, 0.05f);

    // a scene from before keeps the defaults
    YAML::Node old = node;
    old.remove("buoyancy");
    old.remove("waterDrag");
    EXPECT_FLOAT_EQ(editor::Stream::decodeRigidbodyComponent(old, nullptr, Vector3(1.0f, 1.0f, 1.0f)).buoyancy, 0.0f);
    EXPECT_FLOAT_EQ(editor::Stream::decodeRigidbodyComponent(old, nullptr, Vector3(1.0f, 1.0f, 1.0f)).waterDrag, 0.5f);
}

TEST(Water, BuiltInTexturesTile) {
    const int size = 64;
    std::vector<uint32_t> ripples(size * size), noise(size * size);
    Water::makeRipplesTexture(ripples.data(), size);
    Water::makeNoiseTexture(noise.data(), size);

    auto channel = [](uint32_t pixel, int c) { return (int)((pixel >> (8 * c)) & 0xFF); };
    // the largest step between neighbours inside, against the one across the wrap
    auto steps = [&](const std::vector<uint32_t>& pixels, int c, int& inside, int& wrap) {
        inside = 0;
        wrap = 0;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size - 1; x++) {
                inside = std::max(inside, std::abs(channel(pixels[y * size + x], c) - channel(pixels[y * size + x + 1], c)));
            }
            wrap = std::max(wrap, std::abs(channel(pixels[y * size + size - 1], c) - channel(pixels[y * size], c)));
        }
    };
    int inside, wrap;
    steps(ripples, 0, inside, wrap);
    EXPECT_LE(wrap, inside + 2) << "ripples wrap seamlessly";
    steps(noise, 0, inside, wrap);
    EXPECT_LE(wrap, inside + 2) << "noise wraps seamlessly";

    int lowest = 255, highest = 0, minBlue = 255;
    for (int i = 0; i < size * size; i++) {
        lowest = std::min(lowest, channel(noise[i], 0));
        highest = std::max(highest, channel(noise[i], 0));
        minBlue = std::min(minBlue, channel(ripples[i], 2));
        EXPECT_EQ(channel(ripples[i], 3), 255);
    }
    EXPECT_EQ(lowest, 0);
    EXPECT_EQ(highest, 255);
    EXPECT_GT(minBlue, 128) << "normals point out of the surface";
}

TEST(Water, Manifests) {
    std::shared_ptr<const ShaderManifest> water = ShaderManifests::get(Water::SHADER);
    ASSERT_TRUE(water && water->present);
    for (const char* feature : {"WAVES", "BUILTIN_TEXTURES", "REFLECTION", "UNDERSIDE", "DEPTH_FOAM", "ABSORPTION", "REFRACTION", "CAUSTICS"}) {
        EXPECT_TRUE(std::any_of(water->features.begin(), water->features.end(),
                    [&](const ShaderManifestFeature& f) { return f.define == feature; })) << feature;
    }
    Vector4 value;
    ASSERT_TRUE(water->getDefault("buoyancy", value));
    EXPECT_FLOAT_EQ(value.x, 1.0f);
    // unset textures keep their old defaults (flat, grey: an existing water looks the same);
    // BUILTIN_TEXTURES picks the engine's ripples and noise instead (default[...]=)
    const ShaderManifestTexture* normal = water->findTexture("u_normalTexture");
    const ShaderManifestTexture* noise = water->findTexture("u_noiseTexture");
    ASSERT_TRUE(normal && noise);
    EXPECT_EQ(normal->getDefault({}), ShaderTextureDefault::NORMAL);
    EXPECT_EQ(noise->getDefault({"WAVES"}), ShaderTextureDefault::GRAY);
    EXPECT_EQ(normal->getDefault({"BUILTIN_TEXTURES", "WAVES"}), ShaderTextureDefault::RIPPLES);
    EXPECT_EQ(noise->getDefault({"BUILTIN_TEXTURES"}), ShaderTextureDefault::NOISE);
    EXPECT_TRUE(std::none_of(water->diagnostics.begin(), water->diagnostics.end(),
                             [](const ShaderManifestDiagnostic& d) { return d.error; }));

    std::shared_ptr<const ShaderManifest> underwater = ShaderManifests::get(Water::UNDERWATER_SHADER);
    ASSERT_TRUE(underwater && underwater->present);
    EXPECT_EQ(underwater->type, ShaderType::POSTPROCESS);
    ASSERT_TRUE(underwater->getDefault("followWater", value));
    EXPECT_FLOAT_EQ(value.x, 1.0f);
}

TEST(Water, TextureDefaultRuleParsing) {
    ShaderManifest manifest;
    manifest.parse("// @shader mesh\n// @name \"T\"\n// @feature FANCY \"Fancy\"\n"
                   "// @texture u_a normal \"A\" default[FANCY]=ripples\n"
                   "// @texture u_b gray default[!FANCY]=noise\n"
                   "// @texture u_c white default[NOPE]=noise\n"
                   "// @texture u_d white default[FANCY]=sparkles\n", "t.frag");
    manifest.checkComplete("t.frag", false);
    const ShaderManifestTexture* a = manifest.findTexture("u_a");
    const ShaderManifestTexture* b = manifest.findTexture("u_b");
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->featureDefaultDefine, "FANCY") << "the define keeps its case";
    EXPECT_EQ(a->getDefault({"FANCY"}), ShaderTextureDefault::RIPPLES);
    EXPECT_EQ(a->getDefault({}), ShaderTextureDefault::NORMAL);
    EXPECT_EQ(b->getDefault({}), ShaderTextureDefault::NOISE) << "!FANCY: while it is off";
    EXPECT_EQ(b->getDefault({"FANCY"}), ShaderTextureDefault::GRAY);
    auto reported = [&](const char* text) {
        return std::any_of(manifest.diagnostics.begin(), manifest.diagnostics.end(),
                           [&](const ShaderManifestDiagnostic& d) { return d.message.find(text) != std::string::npos; });
    };
    EXPECT_TRUE(reported("default[NOPE] names undeclared feature"));
    EXPECT_TRUE(reported("unknown @texture default 'sparkles'"));
}

TEST(Water, GameObjectMenuEntry) {
    const auto& entries = editor::HierarchyWindow::getCreateMenuEntries();
    auto it = std::find_if(entries.begin(), entries.end(), [](const editor::CreateMenuEntry& entry) {
        return entry.type == editor::EntityCreationType::WATER;
    });
    ASSERT_NE(it, entries.end());
    EXPECT_STREQ(it->submenu, "3D Object");
    EXPECT_STREQ(it->label, "Water");
    EXPECT_TRUE(editor::HierarchyWindow::isCreateEntryAvailable(*it, editor::SceneType::SCENE_3D));
    EXPECT_FALSE(editor::HierarchyWindow::isCreateEntryAvailable(*it, editor::SceneType::SCENE_2D));
}
