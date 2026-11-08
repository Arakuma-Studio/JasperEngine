// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The spline components in scene files (docs/specs/S18-splines.md R29, AC11): each re-encodes
// identically, values at their defaults are not written, knots are flow maps one per line, a
// scene with all four saves byte-identically after a load, its Spline Extrude's Mesh carries no
// buffers, and a scene written before S18 (format 3 or 4) loads with nothing reported or changed.

#include "Stream.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SplineAnimateComponent.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "StreamMigrations.h"
#include "reflect/Reflect.h"
#include "support/EditorTestSupport.h"

#include "LogCapture.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    template<typename T, typename Encode, typename Decode>
    void expectSplineStable(const T& component, Encode encode, Decode decode){
        const YAML::Node first = encode(component);
        const T decoded = decode(YAML::Load(YAML::Dump(first)));
        EXPECT_EQ(YAML::Dump(first), YAML::Dump(encode(decoded)));
    }

    spline::SplineKnot knotAt(float x, float z){
        spline::SplineKnot knot;
        knot.position = Vector3(x, 0.0f, z);
        return knot;
    }

}

TEST(SplineStream, ComponentsReEncodeIdentically){
    SplineComponent spline;
    spline.spline.setType(spline::SplineType::CubicBezier);
    spline::SplineKnot a = knotAt(0, 0);
    a.tangentOut = Vector3(1, 0, 0);
    a.tangentIn = Vector3(-1, 0, 0);
    a.radius = 3.5f;
    a.rollDeg = 10.0f;
    a.mode = spline::TangentMode::Aligned;
    spline.spline.setKnots({a, knotAt(10, 5), knotAt(20, 0)});
    spline.spline.setClosed(true);
    spline.terrain.region = SplineTerrainRegion::Area;
    spline.terrain.paint = true;
    spline.terrain.paintLayer = 4;
    spline.terrain.terrain = 1005;
    expectSplineStable(spline, [](const SplineComponent& c){ return reflect::encodeValue(ComponentType::SplineComponent, &c); },
                       [](const YAML::Node& n){ return reflect::decodeValue<SplineComponent>(ComponentType::SplineComponent, n); });

    SplineExtrudeComponent extrude;
    extrude.shape = SplineExtrudeShape::Custom;
    extrude.customProfile = {{Vector2(-1, 0), false, true}, {Vector2(0, 0.5f), true, false}, {Vector2(1, 0), false, true}};
    extrude.corners = SplineCornerStyle::Round;
    extrude.collision = SplineCollision::Mesh;
    extrude.flow = true;
    extrude.seaLevelFloor = true;
    extrude.seaLevel = -2.0f;
    expectSplineStable(extrude, [](const SplineExtrudeComponent& c){ return reflect::encodeValue(ComponentType::SplineExtrudeComponent, &c); },
                       [](const YAML::Node& n){ return reflect::decodeValue<SplineExtrudeComponent>(ComponentType::SplineExtrudeComponent, n); });

    SplineInstantiateComponent instantiate;
    instantiate.mode = SplineInstantiateMode::Tile;
    instantiate.items.push_back({AssetRef("models/rock.glb"), 2.0f});
    instantiate.gates.push_back({AssetRef("models/gate.glb"), 1.0f});
    instantiate.spanOverrides.push_back({1, 2, SplineSpanOverrideKind::Piece, 0});
    instantiate.scaleRange = Vector2(0.5f, 1.5f);
    instantiate.dropout = 0.25f;
    instantiate.seed = 42;
    expectSplineStable(instantiate, [](const SplineInstantiateComponent& c){ return reflect::encodeValue(ComponentType::SplineInstantiateComponent, &c); },
                       [](const YAML::Node& n){ return reflect::decodeValue<SplineInstantiateComponent>(ComponentType::SplineInstantiateComponent, n); });

    SplineAnimateComponent animate;
    animate.spline = 1001;
    animate.method = SplineAnimateMethod::Speed;
    animate.loop = SplineLoop::PingPong;
    animate.easing = SplineEasing::EaseInOut;
    animate.alignment = SplineAlignment::World;
    animate.startOffset = 0.25f;
    expectSplineStable(animate, [](const SplineAnimateComponent& c){ return reflect::encodeValue(ComponentType::SplineAnimateComponent, &c); },
                       [](const YAML::Node& n){ return reflect::decodeValue<SplineAnimateComponent>(ComponentType::SplineAnimateComponent, n); });
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(SplineStream, DefaultsAreNotWritten){
    const SplineExtrudeComponent defaultExtrude;
    const SplineInstantiateComponent defaultInstantiate;
    const SplineAnimateComponent defaultAnimate;
    const YAML::Node extrude = reflect::encodeValue(ComponentType::SplineExtrudeComponent, &defaultExtrude);
    EXPECT_EQ(extrude.size(), 1u) << YAML::Dump(extrude);
    const YAML::Node instantiate = reflect::encodeValue(ComponentType::SplineInstantiateComponent, &defaultInstantiate);
    EXPECT_EQ(instantiate.size(), 1u) << YAML::Dump(instantiate);
    const YAML::Node animate = reflect::encodeValue(ComponentType::SplineAnimateComponent, &defaultAnimate);
    EXPECT_EQ(animate.size(), 1u) << YAML::Dump(animate);

    SplineComponent spline;
    spline.spline.setKnots({knotAt(0, 0), knotAt(10, 0)});
    const YAML::Node node = reflect::encodeValue(ComponentType::SplineComponent, &spline);
    EXPECT_FALSE(node["type"]);
    EXPECT_FALSE(node["closed"]);
    EXPECT_FALSE(node["terrain"]);
    // a knot at its defaults is just its position
    EXPECT_EQ(node["knots"][1].size(), 1u);

    // and a decode of nothing is the defaults
    const SplineExtrudeComponent decoded = reflect::decodeValue<SplineExtrudeComponent>(ComponentType::SplineExtrudeComponent, YAML::Load("{enabled: true}"));
    EXPECT_EQ(decoded.shape, SplineExtrudeComponent().shape);
    EXPECT_EQ(decoded.width, SplineExtrudeComponent().width);
}

TEST(SplineStream, KnotsAreFlowMapsOnePerLine){
    SplineComponent spline;
    spline.spline.setKnots({knotAt(0, 0), knotAt(10, 0), knotAt(20, 5)});
    YAML::Node root;
    root["spline"] = reflect::encodeValue(ComponentType::SplineComponent, &spline);
    const std::string text = YAML::Dump(root);
    EXPECT_NE(text.find("- {p: [0, 0, 0]}"), std::string::npos) << text;
    EXPECT_NE(text.find("- {p: [10, 0, 0]}"), std::string::npos) << text;
    EXPECT_NE(text.find("- {p: [20, 0, 5]}"), std::string::npos) << text;
}

TEST(SplineStream, SceneSavesByteIdenticallyWithoutChunkBuffers){
    DecodedScene decoded = decodeSceneFile(fixture("splines/splines.scene"));
    ASSERT_TRUE(decoded.scene);
    EXPECT_EQ(decoded.report.dropped(), 0u);
    const std::string first = encodeSceneText(decoded);
    DecodedScene again = decodeSceneText(first, "splines.scene");
    EXPECT_EQ(encodeSceneText(again), first);
    // the Spline Extrude's Mesh is the chunks' template: no geometry is saved
    const YAML::Node root = YAML::Load(first);
    const YAML::Node mesh = root["entities"][0]["components"]["mesh"];
    ASSERT_TRUE(mesh.IsDefined());
    EXPECT_FALSE(mesh["buffer"]);
    EXPECT_FALSE(mesh["indices"]);
    EXPECT_EQ(root["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    // and the components' values came through
    EXPECT_EQ(root["entities"][1]["components"]["splineinstantiate"]["spans"][0]["weight"].as<float>(), 2.0f);
    EXPECT_EQ(root["entities"][2]["components"]["splineanimate"]["loop"].as<std::string>(), "ping_pong");
}

// SplineSystem gives the template its one submesh (the chunks' material) when it first runs: a save
// before that, an editor's first frame, writes the same file
TEST(SplineStream, TheTemplateSavesTheSameBeforeAndAfterSplineSystemRan){
    DecodedScene decoded = decodeSceneFile(fixture("splines/splines.scene"));
    ASSERT_TRUE(decoded.scene);
    Scene* scene = decoded.scene->scene;
    MeshComponent* mesh = nullptr;
    for (Entity entity : decoded.scene->entities){
        if (scene->findComponent<SplineExtrudeComponent>(entity)) mesh = scene->findComponent<MeshComponent>(entity);
    }
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->numSubmeshes, 0u);
    const std::string before = encodeSceneText(decoded);

    mesh->numSubmeshes = 1;
    EXPECT_EQ(encodeSceneText(decoded), before);
    EXPECT_EQ(YAML::Load(before)["entities"][0]["components"]["mesh"]["submeshes"].size(), 1u) << before;
}

TEST(SplineStream, APreSplineSceneLoadsUnchanged){
    // format 3 reads through the identity migrations: nothing is reported, nothing changes
    DecodedScene decoded = decodeSceneFile(fixture("splines/pre-s18.scene"));
    ASSERT_TRUE(decoded.scene);
    EXPECT_EQ(decoded.report.dropped(), 0u);
    EXPECT_EQ(decoded.report.preserved(), 0u);
    const std::string first = encodeSceneText(decoded);
    const YAML::Node saved = YAML::Load(first);
    EXPECT_EQ(saved["formatVersion"].as<int>(), 8);
    EXPECT_EQ(saved["entities"][0]["name"].as<std::string>(), "Box");
    EXPECT_EQ(saved["entities"][0]["components"]["transform"]["position"][2].as<float>(), 3.0f);
    DecodedScene again = decodeSceneText(first, "pre-s18.scene");
    EXPECT_EQ(encodeSceneText(again), first);
}
