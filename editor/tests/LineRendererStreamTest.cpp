// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Line and Trail Renderers in files and code (lines and trails spec 6.7, 9): a Line Renderer
// written before widths decodes to the hairline (width 0, Segments) and is written back byte for
// byte; every new field round-trips and is written only away from its default; the Trail
// Renderer's round trip (runtime points never written); the Catalog surface (properties,
// update flags, clamping, copies, names); Factory's C++ for both (the literal forms of
// curves and gradients); the variant properties the render system loads.

#include <gtest/gtest.h>

#include "Catalog.h"
#include "Factory.h"
#include "Scene.h"
#include "Stream.h"
#include "component/LineRendererComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/Transform.h"
#include "mcp/McpValues.h"
#include "object/LineRenderer.h"
#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"
#include "util/Color.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include "support/PosixCompat.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // a Line Renderer as main wrote it (debug segments, a fork's parameters)
    const char* const OLD_LINES =
        "enabled: true\n"
        "maxLines: 100\n"
        "lines:\n"
        "  - pointA: [0, 0, 0]\n"
        "    colorA: [1, 0, 0, 1]\n"
        "    pointB: [1, 2, 3]\n"
        "    colorB: [0, 1, 0, 1]\n"
        "  - pointA: [1, 2, 3]\n"
        "    colorA: [0, 1, 0, 1]\n"
        "    pointB: [4, 5, 6]\n"
        "    colorB: [0, 0, 1, 0.5]\n"
        "customShader: library:line_flipbook\n"
        "shaderUniforms:\n"
        "  tint: [1, 0.5, 0, 1]\n";

    // a curve and gradient as upstream's (lines and trails) tests wrote them: keys, wraps;
    // colour keys from linear colours (S27's keys are sRGB-encoded)
    Curve curveOf(std::vector<CurveKey> keys, CurveWrapMode pre = CurveWrapMode::Clamp, CurveWrapMode post = CurveWrapMode::Clamp){
        Curve curve(std::move(keys));
        curve.preWrapMode = pre;
        curve.postWrapMode = post;
        return curve;
    }

    GradientColorKey linearKey(float time, const Vector3& linear){
        return GradientColorKey{time, Color::linearTosRGB(linear)};
    }

    Gradient gradientOf(std::vector<GradientColorKey> colors, std::vector<GradientAlphaKey> alphas, GradientMode mode = GradientMode::Blend){
        Gradient gradient;
        gradient.colorKeys = std::move(colors);
        gradient.alphaKeys = std::move(alphas);
        gradient.mode = mode;
        return gradient;
    }

    // from one linear RGBA colour to another (upstream's Gradient::twoColors)
    Gradient twoColorsLinear(const Vector4& from, const Vector4& to){
        return Gradient::twoColors(Color::linearTosRGB(Vector3(from.x, from.y, from.z)), Color::linearTosRGB(Vector3(to.x, to.y, to.z)), from.w, to.w);
    }

    std::string dump(const YAML::Node& node){
        YAML::Emitter out;
        out << node;
        return out.c_str();
    }

    LineRendererComponent authoredLines(){
        LineRendererComponent lines;
        lines.mode = LineRendererMode::Polyline;
        lines.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0)};
        lines.loop = true;
        lines.useWorldSpace = true;
        lines.widthMultiplier = 0.35f;
        lines.widthCurve = curveOf({CurveKey{0.0f, 1.0f, 0.0f, -2.0f}, CurveKey{1.0f, 0.25f, std::numeric_limits<float>::infinity(), 0.0f}},
                                   CurveWrapMode::Clamp, CurveWrapMode::Loop);
        lines.widthUnit = LineWidthUnit::Pixels;
        lines.colorGradient = gradientOf({linearKey(0.0f, Vector3(1, 0, 0)), linearKey(0.6f, Vector3(0.2f, 0.4f, 0.8f))},
                                         {GradientAlphaKey{0.0f, 1.0f}, GradientAlphaKey{1.0f, 0.0f}}, GradientMode::Fixed);
        lines.numCornerVertices = 4;
        lines.numCapVertices = 6;
        lines.alignment = LineAlignment::TransformZ;
        lines.textureMode = LineTextureMode::Tile;
        lines.textureScale = Vector2(2.0f, 0.5f);
        lines.sortPriority = -3;
        return lines;
    }

}

TEST(LineRendererStreamTest, OldFileDecodesToTheHairlineAndWritesTheSameText){
    const YAML::Node old = YAML::Load(OLD_LINES);
    const LineRendererComponent lines = Stream::decodeLineRendererComponent(old);
    EXPECT_FLOAT_EQ(lines.widthMultiplier, 0.0f);
    EXPECT_EQ(lines.mode, LineRendererMode::Segments);
    EXPECT_TRUE(lines.positions.empty());
    EXPECT_FALSE(lines.loop);
    EXPECT_FALSE(lines.useWorldSpace);
    EXPECT_EQ(lines.widthCurve, Curve::constant(1.0f));
    EXPECT_EQ(lines.colorGradient, LineRendererComponent().colorGradient);
    EXPECT_EQ(lines.lines.size(), 2u);

    const std::string first = dump(Stream::encodeLineRendererComponent(lines));
    const std::string second = dump(Stream::encodeLineRendererComponent(Stream::decodeLineRendererComponent(YAML::Load(first))));
    EXPECT_EQ(first, second);
    // no key of the new fields: a scene saved again is the same file
    for (const char* key : {"mode", "positions", "loop", "useWorldSpace", "widthMultiplier", "widthCurve", "widthUnit", "colorGradient",
                            "numCornerVertices", "numCapVertices", "alignment", "textureMode", "textureScale", "sortPriority"}){
        EXPECT_EQ(first.find(std::string(key) + ":"), std::string::npos) << key;
    }
    // the hairline variant is the one main loaded
    Scene scene;
    EXPECT_EQ(RenderSystem::getLinesVariantProperties(&scene, lines), ShaderPool::getLinesProperties(false, true));
    EXPECT_EQ(ShaderPool::getShaderKey(ShaderType::LINES, RenderSystem::getLinesVariantProperties(&scene, lines), 0),
              ShaderPool::getShaderKey(ShaderType::LINES, (uint64_t)0x2, 0));
}

TEST(LineRendererStreamTest, EveryFieldRoundTrips){
    const LineRendererComponent authored = authoredLines();
    const YAML::Node node = Stream::encodeLineRendererComponent(authored);
    for (const char* key : {"mode", "positions", "loop", "useWorldSpace", "widthMultiplier", "widthCurve", "widthUnit", "colorGradient",
                            "numCornerVertices", "numCapVertices", "alignment", "textureMode", "textureScale", "sortPriority"}){
        EXPECT_TRUE(node[key]) << key;
    }
    EXPECT_EQ(node["mode"].as<std::string>(), "polyline");
    EXPECT_EQ(node["textureMode"].as<std::string>(), "tile");
    EXPECT_EQ(node["alignment"].as<std::string>(), "transform_z");
    EXPECT_EQ(node["widthUnit"].as<std::string>(), "pixels");

    const LineRendererComponent decoded = Stream::decodeLineRendererComponent(YAML::Load(dump(node)));
    EXPECT_EQ(decoded.mode, authored.mode);
    EXPECT_EQ(decoded.positions, authored.positions);
    EXPECT_EQ(decoded.loop, authored.loop);
    EXPECT_EQ(decoded.useWorldSpace, authored.useWorldSpace);
    EXPECT_FLOAT_EQ(decoded.widthMultiplier, authored.widthMultiplier);
    EXPECT_EQ(decoded.widthCurve, authored.widthCurve);
    EXPECT_EQ(decoded.widthUnit, authored.widthUnit);
    ASSERT_EQ(decoded.colorGradient.colorKeys.size(), 2u);
    EXPECT_EQ(decoded.colorGradient.mode, GradientMode::Fixed);
    for (size_t i = 0; i < 2; i++){
        EXPECT_NEAR(decoded.colorGradient.colorKeys[i].color.x, authored.colorGradient.colorKeys[i].color.x, 1e-6f);
        EXPECT_NEAR(decoded.colorGradient.colorKeys[i].color.z, authored.colorGradient.colorKeys[i].color.z, 1e-6f);
    }
    EXPECT_EQ(decoded.colorGradient.alphaKeys, authored.colorGradient.alphaKeys);
    EXPECT_EQ(decoded.numCornerVertices, 4u);
    EXPECT_EQ(decoded.numCapVertices, 6u);
    EXPECT_EQ(decoded.alignment, authored.alignment);
    EXPECT_EQ(decoded.textureMode, authored.textureMode);
    EXPECT_EQ(decoded.textureScale, authored.textureScale);
    EXPECT_EQ(decoded.sortPriority, -3);
    EXPECT_TRUE(decoded.needUpdateRibbon);
    EXPECT_EQ(dump(Stream::encodeLineRendererComponent(decoded)), dump(node));

    // decoding over a thick line without the keys gives the defaults (a prefab revert)
    const LineRendererComponent reverted = Stream::decodeLineRendererComponent(YAML::Load(OLD_LINES), &authored);
    EXPECT_FLOAT_EQ(reverted.widthMultiplier, 0.0f);
    EXPECT_EQ(reverted.mode, LineRendererMode::Segments);
    EXPECT_TRUE(reverted.positions.empty());

    // counts read past 90 are 90
    const LineRendererComponent clamped = Stream::decodeLineRendererComponent(YAML::Load("numCornerVertices: 500\nnumCapVertices: 91\nwidthMultiplier: -2\n"));
    EXPECT_EQ(clamped.numCornerVertices, 90u);
    EXPECT_EQ(clamped.numCapVertices, 90u);
    EXPECT_FLOAT_EQ(clamped.widthMultiplier, 0.0f);
}

TEST(LineRendererStreamTest, TrailRoundTripKeepsNoRuntime){
    TrailRendererComponent trail;
    trail.time = 0.3f;
    trail.minVertexDistance = 0.1f;
    trail.widthMultiplier = 1.0f;
    trail.widthCurve = curveOf({CurveKey{0.0f, 0.22307944f}, CurveKey{1.0f, 0.0f}});
    trail.colorGradient = twoColorsLinear(Vector4(1.0f, 0.3f, 0.0f, 1.0f), Vector4(1.0f, 0.3f, 0.0f, 0.0f));
    trail.emitting = false;
    trail.autodestruct = true;
    trail.customShader = "library:line_flipbook";
    trail.blendMode = MaterialBlendMode::ADDITIVE;
    trail.shaderGlobalBindings = {{"tint", "_PlayerColor"}};
    trail.points.push({Vector3(1, 2, 3), 4.0f});
    trail.hasLastHeadPosition = true;

    const YAML::Node node = Stream::encodeTrailRendererComponent(trail);
    for (const char* key : {"points", "lastHeadPosition", "idleTime", "ribbon"}){
        EXPECT_FALSE(node[key]) << key;
    }
    const TrailRendererComponent decoded = Stream::decodeTrailRendererComponent(YAML::Load(dump(node)));
    EXPECT_FLOAT_EQ(decoded.time, 0.3f);
    EXPECT_FLOAT_EQ(decoded.minVertexDistance, 0.1f);
    EXPECT_EQ(decoded.widthCurve, trail.widthCurve);
    EXPECT_FALSE(decoded.emitting);
    EXPECT_TRUE(decoded.autodestruct);
    EXPECT_EQ(decoded.customShader, trail.customShader);
    EXPECT_EQ(decoded.blendMode, MaterialBlendMode::ADDITIVE);
    EXPECT_EQ(decoded.shaderGlobalBindings, trail.shaderGlobalBindings);
    EXPECT_TRUE(decoded.points.empty());
    EXPECT_FALSE(decoded.hasLastHeadPosition);
    EXPECT_EQ(dump(Stream::encodeTrailRendererComponent(decoded)), dump(node));

    // a default trail is Unity's: 5 s, width 1
    const TrailRendererComponent defaults = Stream::decodeTrailRendererComponent(YAML::Load("enabled: true\n"));
    EXPECT_FLOAT_EQ(defaults.time, 5.0f);
    EXPECT_FLOAT_EQ(defaults.widthMultiplier, 1.0f);
    EXPECT_TRUE(defaults.emitting);
}

TEST(LineRendererStreamTest, TrailRegistrationAndCatalog){
    const ComponentType type = ComponentType::TrailRendererComponent;
    // appended after every older type (prefab override masks use the ordinal); the Canvas Group,
    // then later features (the Text Effect, the Video Player, the Safe Area, ...) came after it
    EXPECT_EQ(static_cast<int>(type) + 1, static_cast<int>(ComponentType::CanvasGroupComponent));
    EXPECT_GE(static_cast<int>(LastComponentType), static_cast<int>(ComponentType::TextEffectComponent));
    EXPECT_EQ(Catalog::getComponentName(type, true), "trailrenderer");
    EXPECT_EQ(Catalog::getComponentDisplayName(type), "Trail Renderer");
    EXPECT_EQ(Catalog::getComponentType("trailrenderer"), type);
    EXPECT_EQ(Catalog::getComponentType("trail"), type);
    EXPECT_TRUE(Catalog::hasEnabledProperty(type));

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<TrailRendererComponent>(entity, {});
    // it coexists with a Line Renderer (any renderer) on its GameObject
    scene.addComponent<LineRendererComponent>(entity, {});
    const std::vector<ComponentType> found = Catalog::findComponents(&scene, entity);
    EXPECT_NE(std::find(found.begin(), found.end(), type), found.end());
    EXPECT_NE(std::find(found.begin(), found.end(), ComponentType::LineRendererComponent), found.end());

    TrailRendererComponent& trail = scene.getComponent<TrailRendererComponent>(entity);
    // the width curve and colour gradient are properties of S27's Curve and Gradient
    const PropertyData curve = Catalog::findProperty(&scene, entity, type, "widthCurve");
    EXPECT_EQ(curve.type, PropertyType::Curve);
    EXPECT_EQ(curve.ref, &trail.widthCurve);
    const PropertyData gradient = Catalog::findProperty(&scene, entity, type, "colorGradient");
    EXPECT_EQ(gradient.type, PropertyType::Gradient);
    EXPECT_EQ(gradient.ref, &trail.colorGradient);
    for (const char* name : {"time", "minVertexDistance", "widthMultiplier", "widthUnit", "numCornerVertices", "numCapVertices",
                             "alignment", "textureMode", "textureScale", "emitting", "autodestruct", "sortPriority", "customShader",
                             "shaderUniforms", "shaderTextures", "shaderFeatures", "blendMode", "depthWrite", "stencil", "colorWrite", "enabled"}){
        EXPECT_TRUE(Catalog::findProperty(&scene, entity, type, name).ref) << name;
    }
    for (const char* name : {"points", "ribbon", "idleTime"}){
        EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, name).ref) << name;
    }

    // the ribbon's settings are clamped when set, and rebuild it
    trail.time = -1.0f;
    trail.numCornerVertices = 300;
    trail.needUpdateRibbon = false;
    Catalog::updateEntity(&scene, entity, UpdateFlags_Trail);
    EXPECT_FLOAT_EQ(trail.time, 0.0f);
    EXPECT_EQ(trail.numCornerVertices, 90u);
    EXPECT_TRUE(trail.needUpdateRibbon);

    // a curve copies as a value
    const Entity other = scene.createUserEntity();
    scene.addComponent<Transform>(other, {});
    scene.addComponent<TrailRendererComponent>(other, {});
    trail.widthCurve = Curve::linear(0.0f, 2.0f, 1.0f, 0.0f);
    Catalog::copyPropertyValue(&scene, entity, &scene, other, type, "widthCurve");
    EXPECT_EQ(scene.getComponent<TrailRendererComponent>(other).widthCurve, trail.widthCurve);
}

TEST(LineRendererStreamTest, LineCatalogPositionsAndFlags){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<LineRendererComponent>(entity, authoredLines());
    const ComponentType type = ComponentType::LineRendererComponent;
    LineRendererComponent& lines = scene.getComponent<LineRendererComponent>(entity);

    const PropertyData position = Catalog::findProperty(&scene, entity, type, "positions[2]");
    EXPECT_EQ(position.type, PropertyType::Vector3);
    EXPECT_EQ(position.ref, &lines.positions[2]);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "positions[3]").ref);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "positions").ref, &lines.positions);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "widthCurve").type, PropertyType::Curve);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "colorGradient").type, PropertyType::Gradient);
    EXPECT_TRUE(Catalog::findProperty(&scene, entity, type, "widthMultiplier").updateFlags & UpdateFlags_Lines);
    const std::map<std::string, PropertyData> all = Catalog::findEntityProperties(&scene, entity, type);
    for (const char* name : {"positions[0]", "positions[2]", "mode", "loop", "widthCurve", "lines", "maxLines"}){
        EXPECT_TRUE(all.count(name)) << name;
    }

    lines.numCapVertices = 1000;
    lines.widthMultiplier = -5.0f;
    lines.needUpdateRibbon = false;
    Catalog::updateEntity(&scene, entity, UpdateFlags_Lines);
    EXPECT_EQ(lines.numCapVertices, 90u);
    EXPECT_FLOAT_EQ(lines.widthMultiplier, 0.0f);
    EXPECT_TRUE(lines.needUpdateRibbon);

    const Entity other = scene.createUserEntity();
    scene.addComponent<Transform>(other, {});
    scene.addComponent<LineRendererComponent>(other, {});
    Catalog::copyPropertyValue(&scene, entity, &scene, other, type, "positions");
    EXPECT_EQ(scene.getComponent<LineRendererComponent>(other).positions, lines.positions);
}

TEST(LineRendererStreamTest, FactoryEmitsCurvesGradientsAndFields){
    // S27's literals (stream/CurveCodec): the keys with their modes, a lambda for the wraps; a
    // step tangent is INFINITY
    const Curve curve = curveOf({CurveKey{0.0f, 1.0f, 0.0f, std::numeric_limits<float>::infinity()}, CurveKey{1.0f, 0.0f}},
                                CurveWrapMode::PingPong, CurveWrapMode::Loop);
    EXPECT_EQ(Factory::formatCurve(curve),
              "[]{ Curve c = Curve({CurveKey{0.0f, 1.0f, 0.0f, INFINITY, 0}, CurveKey{1.0f, 0.0f, 0.0f, 0.0f, 0}}); "
              "c.preWrapMode = CurveWrapMode::PingPong; c.postWrapMode = CurveWrapMode::Loop; return c; }()");
    const Gradient gradient = gradientOf({GradientColorKey{0.0f, Vector3(1, 0, 0)}}, {GradientAlphaKey{0.0f, 1.0f}, GradientAlphaKey{1.0f, 0.0f}}, GradientMode::Fixed);
    EXPECT_EQ(Factory::formatGradient(gradient),
              "[]{ Gradient g; g.colorKeys = {GradientColorKey{0.0f, Vector3(1.0f, 0.0f, 0.0f)}}; "
              "g.alphaKeys = {GradientAlphaKey{0.0f, 1.0f}, GradientAlphaKey{1.0f, 0.0f}}; g.mode = GradientMode::Fixed; return g; }()");

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<LineRendererComponent>(entity, authoredLines());
    TrailRendererComponent trail;
    trail.time = 0.3f;
    trail.colorGradient = gradient;
    scene.addComponent<TrailRendererComponent>(entity, trail);

    const std::string lines = Factory::createComponent(0, &scene, entity, ComponentType::LineRendererComponent, "", "scene", "entity");
    for (const char* expected : {"linescomp.mode = LineRendererMode::Polyline;", "linescomp.positions = {Vector3(",
                                 "linescomp.loop = true;", "linescomp.useWorldSpace = true;", "linescomp.widthMultiplier = 0.350000f;",
                                 "linescomp.widthCurve = []{ Curve c = Curve({", "linescomp.widthUnit = LineWidthUnit::Pixels;",
                                 "linescomp.colorGradient = []{ Gradient g;", "linescomp.numCornerVertices = 4;", "linescomp.numCapVertices = 6;",
                                 "linescomp.alignment = LineAlignment::TransformZ;", "linescomp.textureMode = LineTextureMode::Tile;",
                                 "linescomp.textureScale = Vector2(2.000000f, 0.500000f);", "linescomp.sortPriority = -3;"}){
        EXPECT_NE(lines.find(expected), std::string::npos) << expected << "\n" << lines;
    }
    // an old Line Renderer's code is as it was
    scene.getComponent<LineRendererComponent>(entity) = LineRendererComponent();
    const std::string plain = Factory::createComponent(0, &scene, entity, ComponentType::LineRendererComponent, "", "scene", "entity");
    EXPECT_EQ(plain.find("widthMultiplier"), std::string::npos) << plain;
    EXPECT_EQ(plain.find("mode"), std::string::npos) << plain;

    const std::string trailCode = Factory::createComponent(0, &scene, entity, ComponentType::TrailRendererComponent, "", "scene", "entity");
    EXPECT_NE(trailCode.find("TrailRendererComponent trailcomp;"), std::string::npos) << trailCode;
    EXPECT_NE(trailCode.find("trailcomp.time = 0.300000f;"), std::string::npos) << trailCode;
    EXPECT_NE(trailCode.find("trailcomp.colorGradient = []{ Gradient g;"), std::string::npos) << trailCode;
    EXPECT_EQ(trailCode.find("minVertexDistance"), std::string::npos) << "defaults are not written";
}

TEST(LineRendererStreamTest, RibbonVariantProperties){
    Scene scene;
    LineRendererComponent lines;
    lines.widthMultiplier = 0.1f;
    const uint64_t ribbon = RenderSystem::getLinesVariantProperties(&scene, lines);
    EXPECT_EQ(ShaderPool::getShaderKey(ShaderType::LINES, ribbon, 0), ShaderPool::getShaderKey(ShaderType::LINES, (uint64_t)(0x2 | 0x8), 0));
    lines.widthUnit = LineWidthUnit::Pixels;
    EXPECT_EQ(RenderSystem::getLinesVariantProperties(&scene, lines), (uint64_t)(0x2 | 0x8 | 0x20));
    // a base texture on the built-in shader samples it ('Tex'), not on a library shader
    lines.widthUnit = LineWidthUnit::World;
    Texture texture("textures/beam.png");
    lines.shaderTextures = {{LINE_BASE_TEXTURE_UNIFORM, texture}};
    EXPECT_EQ(RenderSystem::getLinesVariantProperties(&scene, lines), (uint64_t)(0x2 | 0x8 | 0x10));
    lines.customShader = "library:line_flipbook";
    EXPECT_EQ(RenderSystem::getLinesVariantProperties(&scene, lines), (uint64_t)(0x2 | 0x8));
    // a hairline never has the ribbon bits
    lines.widthMultiplier = 0.0f;
    EXPECT_EQ(RenderSystem::getLinesVariantProperties(&scene, lines), (uint64_t)0x2);
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::LINES, 3, true), "Rib");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::LINES, 4, true), "Tex");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::LINES, 5, true), "Pxw");

    TrailRendererComponent trail;
    EXPECT_EQ(RenderSystem::getTrailVariantProperties(&scene, trail), (uint64_t)(0x2 | 0x8));
}

// The exported build's code for both components compiles against the engine headers (the C++
// Factory writes into a game's generated scene sources)
TEST(LineRendererStreamTest, FactoryCodeCompiles){
#if defined(JASPER_EDITOR_TEST_ENGINE_FLAGS) && defined(JASPER_EDITOR_TEST_CXX)
    std::ifstream flagsFile(JASPER_EDITOR_TEST_ENGINE_FLAGS);
    if (!flagsFile){
        GTEST_SKIP() << "no engine flags file " << JASPER_EDITOR_TEST_ENGINE_FLAGS;
    }
    std::string flags, line;
    bool defines = false;
    while (std::getline(flagsFile, line)){
        if (line.empty()) continue;
        if (line == "#defines"){ defines = true; continue; }
        flags += (defines ? " -D'" : " -isystem '") + line + "'";
    }

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<LineRendererComponent>(entity, authoredLines());
    TrailRendererComponent trail;
    trail.time = 0.3f;
    trail.widthCurve = curveOf({CurveKey{0.0f, 1.0f, 0.0f, std::numeric_limits<float>::infinity()}, CurveKey{1.0f, 0.0f}});
    trail.colorGradient = twoColorsLinear(Vector4(1, 0, 0, 1), Vector4(0, 0, 1, 0));
    trail.shaderGlobalBindings = {{"tint", "_Tint"}};
    trail.customShader = "library:line_flipbook";
    scene.addComponent<TrailRendererComponent>(entity, trail);

    std::ostringstream source;
    source << "#include \"Scene.h\"\n#include \"component/Transform.h\"\nusing namespace jasperengine;\n"
           << "void generatedLines(Scene* scene, Entity entity){\n"
           << "{\n" << Factory::createComponent(4, &scene, entity, ComponentType::LineRendererComponent, "", "scene", "entity") << "}\n"
           << "{\n" << Factory::createComponent(4, &scene, entity, ComponentType::TrailRendererComponent, "", "scene", "entity") << "}\n"
           << "}\n";
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("jasper-lines-factory-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "generated.cpp";
    std::ofstream(file) << source.str();
    const std::string command = std::string("'") + JASPER_EDITOR_TEST_CXX + "' -std=gnu++20 -fsyntax-only" + flags +
                                " '" + file.string() + "' > '" + (dir / "log.txt").string() + "' 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream log(dir / "log.txt");
    const std::string output((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    EXPECT_EQ(status, 0) << output << "\n" << source.str();
    std::filesystem::remove_all(dir);
#else
    GTEST_SKIP() << "built without the engine flags";
#endif
}

TEST(LineRendererStreamTest, SetPositionOutOfRangeChangesNothing){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    LineRendererComponent segments;
    segments.lines.push_back(LineData{Vector3(0, 0, 0), Vector4(1, 1, 1, 1), Vector3(1, 0, 0), Vector4(1, 1, 1, 1)});
    scene.addComponent<LineRendererComponent>(entity, segments);
    LineRenderer line(&scene, entity);
    EXPECT_FALSE(line.setPosition(3, Vector3(1, 2, 3)));
    EXPECT_EQ(scene.getComponent<LineRendererComponent>(entity).mode, LineRendererMode::Segments);
    line.setPositionCount(2);
    EXPECT_TRUE(line.setPosition(1, Vector3(1, 2, 3)));
    EXPECT_EQ(scene.getComponent<LineRendererComponent>(entity).mode, LineRendererMode::Polyline);
}

TEST(LineRendererStreamTest, McpLineDataPicksTheMode){
    // the whole segment list is Segments in local space, the positions list Polyline; unless the
    // same call names the mode (or the space) itself
    using mcp::McpValues;
    using mcp::Json;
    auto parse = [](const char* text){
        std::vector<McpValues::Change> changes;
        std::string error;
        EXPECT_TRUE(McpValues::parseChanges(Json::parse(text), changes, error)) << error;
        std::map<std::string, Json> byProperty;
        for (const McpValues::Change& change : changes) byProperty[change.property] = change.value;
        return byProperty;
    };
    auto lines = parse(R"({"linerenderer": {"lines": []}})");
    EXPECT_EQ(lines["mode"], "segments");
    EXPECT_EQ(lines["useWorldSpace"], false);
    auto positions = parse(R"({"line": {"positions": [[0,0,0],[1,0,0]]}})");
    EXPECT_EQ(positions["mode"], "polyline");
    EXPECT_EQ(positions.count("useWorldSpace"), 0u);
    auto explicitMode = parse(R"({"linerenderer": {"lines": [], "mode": "polyline", "useWorldSpace": true}})");
    EXPECT_EQ(explicitMode["mode"], "polyline");
    EXPECT_EQ(explicitMode["useWorldSpace"], true);
    auto element = parse(R"([{"component": "linerenderer", "property": "positions[0]", "value": [1,2,3]}])");
    EXPECT_EQ(element.count("mode"), 0u);
}

TEST(LineRendererStreamTest, MalformedCurveAndGradientFallBack){
    // upstream's form with a wrap or a mode that is not a name: the default, no exception
    const Curve curve = editor::Stream::decodeCurve(YAML::Load("{keys: [[0, 1], [1, 2]], pre: [1, 2], post: {a: 1}}"), Curve::constant(5.0f));
    EXPECT_EQ(curve.keys.size(), 2u);
    EXPECT_EQ(curve.preWrapMode, CurveWrapMode::Clamp);
    EXPECT_EQ(curve.postWrapMode, CurveWrapMode::Clamp);
    const Gradient gradient = editor::Stream::decodeGradient(YAML::Load("{mode: [x], colors: [[0, '#FF0000'], [1, '#0000FF']]}"), Gradient());
    EXPECT_EQ(gradient.mode, GradientMode::Blend);
    EXPECT_EQ(gradient.colorKeys.size(), 2u);
    // more than 8 keys in a file: loaded with the first 7 and the last
    std::string many = "{colors: [";
    for (int i = 0; i <= 10; i++) many += std::string(i ? ", " : "") + "[" + std::to_string(i / 10.0f) + ", '#FFFFFF']";
    many += "]}";
    const Gradient capped = editor::Stream::decodeGradient(YAML::Load(many), Gradient());
    ASSERT_EQ(capped.colorKeys.size(), Gradient::kMaxKeys);
    EXPECT_FLOAT_EQ(capped.colorKeys.back().time, 1.0f);
    // S27's form that does not read (a colour key of two numbers): the fallback
    const Gradient fallback = Gradient::constant(Vector3(0.5f, 0.5f, 0.5f));
    EXPECT_EQ(editor::Stream::decodeGradient(YAML::Load("{colorKeys: [[0, 1]], alphaKeys: []}"), fallback), fallback);
}

TEST(LineRendererStreamTest, UpstreamFilesReadLosslessly){
    // origin/main's lines-and-trails files: {keys, pre, post} curves with .inf steps, numbers,
    // {mode, colors, alphas} gradients with hex (sRGB) and [r, g, b] (linear) colours; each
    // reads into the same values, and the editor writes them in its own form
    const YAML::Node upstream = YAML::Load(
        "mode: polyline\n"
        "positions: [[0, 0, 0], [1, 0, 0]]\n"
        "widthMultiplier: 0.5\n"
        "widthCurve: {keys: [[0, 1], [0.5, 2, .inf, .inf], [1, 0.25, -1, -1]], pre: ping_pong, post: loop}\n"
        "colorGradient: {mode: fixed, colors: [[0, '#FF8000'], [0.5, [0.25, 0.5, 2.0]]], alphas: [[0, 1], [1, 0.25]]}\n");
    const LineRendererComponent lines = Stream::decodeLineRendererComponent(upstream);
    ASSERT_EQ(lines.widthCurve.keys.size(), 3u);
    EXPECT_EQ(lines.widthCurve.preWrapMode, CurveWrapMode::PingPong);
    EXPECT_EQ(lines.widthCurve.postWrapMode, CurveWrapMode::Loop);
    EXPECT_TRUE(std::isinf(lines.widthCurve.keys[1].outTangent));
    EXPECT_FLOAT_EQ(lines.widthCurve.keys[2].inTangent, -1.0f);
    // the step holds the key before it
    EXPECT_FLOAT_EQ(lines.widthCurve.evaluate(0.75f), 2.0f);
    EXPECT_EQ(lines.colorGradient.mode, GradientMode::Fixed);
    ASSERT_EQ(lines.colorGradient.colorKeys.size(), 2u);
    // a hex colour is the sRGB value itself; a linear one its sRGB encoding (2.0 stays above 1)
    EXPECT_NEAR(lines.colorGradient.colorKeys[0].color.y, 128.0f / 255.0f, 1e-6f);
    const Vector4 linear = lines.colorGradient.evaluate(0.5f);
    EXPECT_NEAR(linear.x, 0.25f, 1e-5f);
    EXPECT_NEAR(linear.z, 2.0f, 1e-4f);
    EXPECT_NEAR(lines.colorGradient.evaluateAlpha(1.0f), 0.25f, 1e-6f);

    // written in S27's form, and read back the same
    const YAML::Node written = Stream::encodeLineRendererComponent(lines);
    EXPECT_FALSE(written["widthCurve"]["pre"]);
    EXPECT_EQ(written["widthCurve"]["preWrapMode"].as<std::string>(), "pingPong");
    EXPECT_TRUE(written["colorGradient"]["colorKeys"]);
    EXPECT_FALSE(written["colorGradient"]["colors"]);
    const LineRendererComponent again = Stream::decodeLineRendererComponent(YAML::Load(dump(written)));
    EXPECT_EQ(again.widthCurve, lines.widthCurve);
    EXPECT_EQ(again.colorGradient, lines.colorGradient);

    // a number is a constant curve, one colour a constant gradient
    const LineRendererComponent constant = Stream::decodeLineRendererComponent(YAML::Load("widthCurve: 0.75\ncolorGradient: '#00FF0080'\n"));
    EXPECT_EQ(constant.widthCurve, Curve::constant(0.75f));
    ASSERT_EQ(constant.colorGradient.colorKeys.size(), 1u);
    EXPECT_FLOAT_EQ(constant.colorGradient.colorKeys[0].color.y, 1.0f);
    EXPECT_NEAR(constant.colorGradient.evaluateAlpha(0.0f), 128.0f / 255.0f, 1e-6f);
}
