// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S52 AC12 (R38-R40): 2D sorting and Sprite Batch in the scene and project formats. No version
// bump: every key is optional with a default that draws what a scene drew before. The renderers'
// Sorting Layer and Order in Layer, the Sprite Sort Point and both components round-trip; a scene
// saved before S52 loads with the defaults and re-saves byte for byte; a renderer naming a Sorting
// Layer the project lacks draws on Default with one load-report line per scene; project.yaml and
// the prebuilt player's game.json carry the layer table and the Transparency Sort settings.

#include "support/EditorTestSupport.h"

#include "App.h"
#include "LoadReport.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/SortingGroupComponent.h"
#include "component/SpriteBatchComponent.h"
#include "component/SpriteComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/Transform.h"
#include "player/PlayerConfig.h"
#include "registry/SortingLayers.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    struct SortingLayersReset {
        ~SortingLayersReset() { SortingLayers::reset(); }
    };

    // a 2D scene of two sprites, the second naming `layer`
    std::string spriteScene(int layer){
        return "formatVersion: 8\n"
               "id: 5252\n"
               "name: Sorting\n"
               "scene: {}\n"
               "sceneType: scene_2d\n"
               "entities:\n"
               "  - type: Entity\n"
               "    entity: 1001\n"
               "    name: A\n"
               "    components:\n"
               "      transform:\n"
               "        position: [0, 0, 0]\n"
               "      mesh:\n"
               "        sortingLayer: " + std::to_string(layer) + "\n"
               "        sortingOrder: 3\n"
               "      sprite:\n"
               "        width: 16\n"
               "        height: 16\n"
               "  - type: Entity\n"
               "    entity: 1002\n"
               "    name: B\n"
               "    components:\n"
               "      transform:\n"
               "        position: [0, 0, 0]\n"
               "      sortinggroup:\n"
               "        sortingLayer: " + std::to_string(layer) + "\n";
    }

}

TEST(StreamSorting, RendererFieldsRoundTrip){
    MeshComponent mesh;
    YAML::Node node = Stream::encodeMeshComponent(mesh);
    EXPECT_FALSE(node["sortingLayer"].IsDefined());
    EXPECT_FALSE(node["sortingOrder"].IsDefined());
    mesh.sortingLayer = 4;
    mesh.sortingOrder = -12;
    node = Stream::encodeMeshComponent(mesh);
    const MeshComponent meshBack = Stream::decodeMeshComponent(YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(meshBack.sortingLayer, 4);
    EXPECT_EQ(meshBack.sortingOrder, -12);

    SpriteComponent sprite;
    EXPECT_FALSE(Stream::encodeSpriteComponent(sprite)["spriteSortPoint"].IsDefined());
    sprite.spriteSortPoint = SpriteSortPoint::PIVOT;
    EXPECT_EQ(Stream::decodeSpriteComponent(YAML::Load(YAML::Dump(Stream::encodeSpriteComponent(sprite)))).spriteSortPoint, SpriteSortPoint::PIVOT);

    PointsComponent points;
    points.sortingLayer = 2;
    points.sortingOrder = 7;
    const PointsComponent pointsBack = Stream::decodePointsComponent(YAML::Load(YAML::Dump(Stream::encodePointsComponent(points))));
    EXPECT_EQ(pointsBack.sortingLayer, 2);
    EXPECT_EQ(pointsBack.sortingOrder, 7);

    LineRendererComponent lines;
    lines.sortingLayer = 3;
    lines.sortingOrder = 32767;
    const LineRendererComponent linesBack = Stream::decodeLineRendererComponent(YAML::Load(YAML::Dump(Stream::encodeLineRendererComponent(lines))));
    EXPECT_EQ(linesBack.sortingLayer, 3);
    EXPECT_EQ(linesBack.sortingOrder, 32767);

    TrailRendererComponent trail;
    trail.sortingLayer = 5;
    trail.sortingOrder = -32768;
    const TrailRendererComponent trailBack = Stream::decodeTrailRendererComponent(YAML::Load(YAML::Dump(Stream::encodeTrailRendererComponent(trail))));
    EXPECT_EQ(trailBack.sortingLayer, 5);
    EXPECT_EQ(trailBack.sortingOrder, -32768);

    ParticleSystemComponent particles;
    particles.sortingLayer = 6;
    particles.sortingOrder = 9;
    const ParticleSystemComponent particlesBack =
        Stream::decodeParticleSystemComponent(YAML::Load(YAML::Dump(Stream::encodeParticleSystemComponent(particles))));
    EXPECT_EQ(particlesBack.sortingLayer, 6);
    EXPECT_EQ(particlesBack.sortingOrder, 9);

    // an Order in Layer past the 16 bits of the sort key reads clamped; absent keys read 0 over
    // a decoded component too
    const MeshComponent clamped = Stream::decodeMeshComponent(YAML::Load("sortingOrder: 90000"));
    EXPECT_EQ(clamped.sortingOrder, 32767);
    MeshComponent old;
    old.sortingLayer = 8;
    old.sortingOrder = 8;
    const MeshComponent cleared = Stream::decodeMeshComponent(YAML::Load("castShadows: true"), &old);
    EXPECT_EQ(cleared.sortingLayer, 0);
    EXPECT_EQ(cleared.sortingOrder, 0);
}

TEST(StreamSorting, ComponentsRoundTrip){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<MeshComponent>(entity, {});
    scene.addComponent<SpriteComponent>(entity, {});
    SortingGroupComponent group;
    group.sortingLayer = 3;
    group.sortingOrder = -4;
    group.enabled = false;
    scene.addComponent<SortingGroupComponent>(entity, group);
    SpriteBatchComponent batch;
    batch.mode = SpriteBatchMode::CLEAR_EACH_FRAME;
    batch.space = SpriteBatchSpace::WORLD;
    batch.capacity = 4096;
    batch.sortInstances = true;
    batch.count = 99; // runtime: never written
    scene.addComponent<SpriteBatchComponent>(entity, batch);

    const YAML::Node components = Stream::encodeComponents(entity, &scene, scene.getSignature(entity));
    ASSERT_TRUE(components["sortinggroup"].IsDefined());
    ASSERT_TRUE(components["spritebatch"].IsDefined());
    EXPECT_FALSE(components["spritebatch"]["count"].IsDefined());

    Scene other;
    const Entity copy = other.createUserEntity();
    Stream::decodeComponents(copy, NULL_ENTITY, &other, YAML::Load(YAML::Dump(components)));
    ASSERT_TRUE(other.findComponent<SortingGroupComponent>(copy));
    ASSERT_TRUE(other.findComponent<SpriteBatchComponent>(copy));
    const SortingGroupComponent& groupBack = other.getComponent<SortingGroupComponent>(copy);
    EXPECT_EQ(groupBack.sortingLayer, 3);
    EXPECT_EQ(groupBack.sortingOrder, -4);
    EXPECT_FALSE(groupBack.enabled);
    const SpriteBatchComponent& batchBack = other.getComponent<SpriteBatchComponent>(copy);
    EXPECT_EQ(batchBack.mode, SpriteBatchMode::CLEAR_EACH_FRAME);
    EXPECT_EQ(batchBack.space, SpriteBatchSpace::WORLD);
    EXPECT_EQ(batchBack.capacity, 4096u);
    EXPECT_TRUE(batchBack.sortInstances);
    EXPECT_EQ(batchBack.count, 0u);
    EXPECT_FALSE(batchBack.state);

    // defaults write only the component's key (OmitIfDefault)
    Scene plain;
    const Entity plainEntity = plain.createUserEntity();
    plain.addComponent<Transform>(plainEntity, {});
    plain.addComponent<SortingGroupComponent>(plainEntity, {});
    const YAML::Node plainComponents = Stream::encodeComponents(plainEntity, &plain, plain.getSignature(plainEntity));
    ASSERT_TRUE(plainComponents["sortinggroup"].IsDefined());
    EXPECT_FALSE(plainComponents["sortinggroup"]["sortingLayer"].IsDefined());
    EXPECT_FALSE(plainComponents["sortinggroup"]["sortingOrder"].IsDefined());
}

// R38: a scene saved before S52 (a committed 2D scene, scenes/shader-library-export-2d.scene, in
// the editor's current format: sprites, points, lines) loads with the defaults, draws in today's
// order (no field away from its default) and re-saves byte for byte: the new keys write nothing
// at their defaults
TEST(StreamSorting, PreS52SceneLoadsWithDefaultsAndResavesAsItWas){
    const fs::path path = fixture("sorting/pre-s52.scene");
    const std::string source = readText(path);
    ASSERT_FALSE(source.empty()) << path;
    for (const char* key : {"sortingLayer", "sortingOrder", "spriteSortPoint", "sortinggroup", "spritebatch"})
        ASSERT_EQ(source.find(key), std::string::npos) << key << ": the fixture must be from before S52";

    DecodedScene decoded = decodeSceneFile(path);
    ASSERT_TRUE(decoded.scene && decoded.scene->scene);
    Scene* scene = decoded.scene->scene;
    size_t renderers = 0;
    auto checkDefaults = [&renderers](auto array){
        for (size_t i = 0; i < array->size(); i++){
            EXPECT_EQ(array->getComponentFromIndex(i).sortingLayer, 0);
            EXPECT_EQ(array->getComponentFromIndex(i).sortingOrder, 0);
            renderers++;
        }
    };
    checkDefaults(scene->getComponentArray<MeshComponent>());
    checkDefaults(scene->getComponentArray<PointsComponent>());
    checkDefaults(scene->getComponentArray<LineRendererComponent>());
    EXPECT_GT(renderers, 3u);
    auto sprites = scene->getComponentArray<SpriteComponent>();
    EXPECT_GT(sprites->size(), 0u);
    for (size_t i = 0; i < sprites->size(); i++)
        EXPECT_EQ(sprites->getComponentFromIndex(i).spriteSortPoint, SpriteSortPoint::CENTER);
    EXPECT_EQ(scene->getComponentArray<SortingGroupComponent>()->size(), 0u);
    // nothing to report: every layer is Default
    for (const LoadIssue& issue : decoded.report.issues)
        EXPECT_NE(issue.field, "sortingLayer") << issue.describe();

    EXPECT_EQ(encodeSceneText(decoded), source);
}

// R39: an unknown layer id draws on Default (the id is kept) with one line for the scene
TEST(StreamSorting, UnknownLayerDrawsOnDefaultWithOneReportLine){
    SortingLayersReset reset;
    SortingLayers::setLayers({{0, "Default"}, {1, "Foreground"}});
    DecodedScene decoded = decodeSceneText(spriteScene(77), "unknown.scene");
    ASSERT_TRUE(decoded.scene && decoded.scene->scene);
    size_t lines = 0;
    for (const LoadIssue& issue : decoded.report.issues){
        if (issue.field != "sortingLayer")
            continue;
        lines++;
        EXPECT_EQ(issue.kind, LoadIssueKind::Fallback);
        EXPECT_EQ(issue.fate, LoadIssueFate::Info);
        EXPECT_NE(issue.message.find("2 renderer"), std::string::npos) << issue.message;
    }
    EXPECT_EQ(lines, 1u);
    EXPECT_EQ(SortingLayers::getLayerValue(77), SortingLayers::getLayerValue(0));
    // the id stays as authored
    EXPECT_NE(encodeSceneText(decoded).find("sortingLayer: 77"), std::string::npos);

    // a known layer reports nothing
    DecodedScene known = decodeSceneText(spriteScene(1), "known.scene");
    for (const LoadIssue& issue : known.report.issues)
        EXPECT_NE(issue.field, "sortingLayer") << issue.describe();
}

// R39: project.yaml keeps the table in draw order with its ids, and the Transparency Sort settings;
// the defaults write nothing
TEST(StreamSorting, ProjectSettingsRoundTrip){
    SortingLayersReset reset;
    Project project;
    Stream::decodeProject(&project, YAML::Load("name: Sorting\n"), Workspace());
    const YAML::Node plain = Stream::encodeProject(&project);
    EXPECT_FALSE(plain["sortingLayers"].IsDefined());
    EXPECT_FALSE(plain["transparencySortMode"].IsDefined());
    EXPECT_FALSE(plain["transparencySortAxis"].IsDefined());

    project.setSortingLayers({{3, "Background"}, {0, "Default"}, {7, "Foreground"}});
    project.setTransparencySort(TransparencySortMode::CUSTOM_AXIS, Vector3(0.0f, 1.0f, -0.5f));
    const std::string saved = YAML::Dump(Stream::encodeProject(&project));
    SortingLayers::reset();
    Project reloaded;
    Stream::decodeProject(&reloaded, YAML::Load(saved), Workspace());
    const std::vector<SortingLayerInfo> layers = reloaded.getSortingLayers();
    ASSERT_EQ(layers.size(), 3u);
    EXPECT_EQ(layers[0].id, 3);
    EXPECT_EQ(layers[0].name, "Background");
    EXPECT_EQ(layers[1].id, 0);
    EXPECT_EQ(layers[2].id, 7);
    EXPECT_EQ(SortingLayers::getLayerValue(7), 2);
    EXPECT_EQ(SortingLayers::getLayerValue(0), 1);
    EXPECT_EQ(reloaded.getTransparencySortMode(), TransparencySortMode::CUSTOM_AXIS);
    EXPECT_FLOAT_EQ(reloaded.getTransparencySortAxis().z, -0.5f);
    EXPECT_EQ(YAML::Dump(Stream::encodeProject(&reloaded)), saved);

    // a project without the keys is Default alone again
    Stream::decodeProject(&reloaded, YAML::Load("name: Sorting\n"), Workspace());
    EXPECT_EQ(SortingLayers::getLayers().size(), 1u);
    EXPECT_EQ(reloaded.getTransparencySortMode(), TransparencySortMode::DEFAULT);
}

// R1: Default always exists and keeps its name; renaming and reordering keep the ids, so no scene
// changes; names given as a list keep the ids of the names already in the table
TEST(StreamSorting, LayersKeepTheirIds){
    SortingLayersReset reset;
    SortingLayers::setLayers({{2, "Ground"}, {5, "Sky"}});
    ASSERT_EQ(SortingLayers::getLayers().size(), 3u);
    EXPECT_EQ(SortingLayers::getLayers()[0].name, "Default");
    EXPECT_EQ(SortingLayers::getLayerValue(5), 2);
    // a renamed Default, a repeated name or id and a negative id are refused
    SortingLayers::setLayers({{0, "Base"}, {2, "Ground"}, {2, "Again"}, {4, "Ground"}, {-1, "Neg"}});
    ASSERT_EQ(SortingLayers::getLayers().size(), 2u);
    EXPECT_EQ(SortingLayers::idToName(0), "Default");
    SortingLayers::setLayers({{2, "Ground"}, {5, "Sky"}});
    const std::vector<SortingLayerInfo> reordered = SortingLayers::fromNames({"Sky", "Default", "Ground", "Clouds"});
    ASSERT_EQ(reordered.size(), 4u);
    EXPECT_EQ(reordered[0].id, 5);
    EXPECT_EQ(reordered[1].id, 0);
    EXPECT_EQ(reordered[2].id, 2);
    EXPECT_EQ(reordered[3].id, 6); // the next free id
    SortingLayers::setLayers(reordered);
    EXPECT_EQ(SortingLayers::getLayerValue(5), 0);
    EXPECT_EQ(SortingLayers::getLayerValue(6), 3);
    EXPECT_EQ(SortingLayers::nameToId("Clouds"), 6);
    EXPECT_EQ(SortingLayers::nameToId("Nope"), -1);
    EXPECT_EQ(SortingLayers::idToName(99), "");
}

// R1: a removed layer's id is never given to a later layer (its renderers draw on Default, not
// on the newcomer), project.yaml keeps the next id once it is past the listed ones, and an entry
// naming its id keeps the layer through a rename
TEST(StreamSorting, RemovedIdsAreNeverGivenAgain){
    SortingLayersReset reset;
    Project project;
    Project reloaded;
    SortingLayers::reset();
    SortingLayers::setLayers(SortingLayers::fromNames({"Default", "Back", "Front"}));
    const int32_t front = SortingLayers::nameToId("Front");
    ASSERT_GT(front, 0);
    SortingLayers::setLayers(SortingLayers::fromNames({"Default", "Back"}));
    EXPECT_FALSE(SortingLayers::isValid(front)) << "a renderer still naming it draws on Default";

    // saved and loaded: the next id survives a reopen (it is past the listed ids now)
    const int32_t next = SortingLayers::getNextId();
    EXPECT_GT(next, front);
    const YAML::Node saved = Stream::encodeProject(&project);
    ASSERT_TRUE(saved["sortingLayerNextId"].IsDefined()) << YAML::Dump(saved);
    SortingLayers::reset();
    Stream::decodeProject(&reloaded, saved, Workspace());
    EXPECT_EQ(SortingLayers::getNextId(), next);

    // a new layer takes a new id, not the removed one
    SortingLayers::setLayers(SortingLayers::fromNames({"Default", "Back", "Clouds"}));
    EXPECT_NE(SortingLayers::nameToId("Clouds"), front);
    EXPECT_GE(SortingLayers::nameToId("Clouds"), next);

    // a rename by id keeps the layer; the old name listed again is a new layer
    const int32_t back = SortingLayers::nameToId("Back");
    std::vector<SortingLayerInfo> resolved;
    std::string error;
    ASSERT_TRUE(SortingLayers::resolve({{0, "Default"}, {back, "Far"}, {-1, "Back"}}, resolved, error)) << error;
    ASSERT_EQ(resolved.size(), 3u);
    EXPECT_EQ(resolved[1].id, back);
    EXPECT_EQ(resolved[1].name, "Far");
    EXPECT_NE(resolved[2].id, back);
    EXPECT_FALSE(SortingLayers::resolve({{0, "Default"}, {4242, "Nope"}}, resolved, error));
    EXPECT_NE(error.find("4242"), std::string::npos) << error;
}

// R40: the prebuilt player's game.json carries the table and the sort settings
TEST(StreamSorting, PlayerConfigCarriesTheSorting){
    player::PlayerConfig config;
    config.sortingLayers = {{0, "Default"}, {4, "FX"}};
    config.transparencySortMode = TransparencySortMode::CUSTOM_AXIS;
    config.transparencySortAxis = Vector3(1.0f, 2.0f, 3.0f);
    const nlohmann::json json = config.toJson();
    player::PlayerConfig back;
    std::string error;
    ASSERT_TRUE(player::PlayerConfig::fromJson(json, back, error)) << error;
    ASSERT_EQ(back.sortingLayers.size(), 2u);
    EXPECT_EQ(back.sortingLayers[1].id, 4);
    EXPECT_EQ(back.sortingLayers[1].name, "FX");
    EXPECT_EQ(back.transparencySortMode, TransparencySortMode::CUSTOM_AXIS);
    EXPECT_FLOAT_EQ(back.transparencySortAxis.y, 2.0f);
    // the defaults write no key
    const std::string plain = player::PlayerConfig().toJson().dump();
    EXPECT_EQ(plain.find("sortingLayers"), std::string::npos);
    EXPECT_EQ(plain.find("transparencySort"), std::string::npos);
}

// S52's shader sources (library:sprite @batchable, 'Sfx' 'Sfr' 'Spv', the instanced snap's mode)
// are cached under v61 or later: a cache of an older version holds programs without them
TEST(StreamSorting, ShaderCacheVersionCoversThePixelArtBatching){
    const std::string name = editor::App::getUserShaderCacheDir().filename().string();
    ASSERT_EQ(name.rfind('v', 0), 0u) << name;
    EXPECT_GE(std::stoi(name.substr(1)), 61) << name;
}
