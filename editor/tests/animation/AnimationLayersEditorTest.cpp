// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor plumbing of Animation Player layers and Avatar Masks (upstream anim layers spec 6.3,
// 9, 10; docs/specs/S21-animation.md "Upstream absorption"): the scene codec of layers and masks
// with their GUIDs (an old scene stays byte-identical; upstream's controller key is the
// migration's), the reflected properties (S08: Catalog, prefab overrides per field, the C++ export
// with their layer, L8), set_properties values and refusals, the S21 mask and controller documents
// (create, commit, undo, outside edits, users), the references a rename or an outside move updates,
// and the scene resources.

#include <gtest/gtest.h>

#include <algorithm>

#include "serialization/CookedScene.h"
#include "component/Transform.h"

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Catalog.h"
#include "Engine.h"
#include "Platform.h"
#include "Project.h"
#include "Scene.h"
#include "Stream.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorAssetUsers.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimationClipDocument.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PlayerLayerActions.h"
#include "command/type/RenameFileCmd.h"
#include "mcp/McpValues.h"
#include "component/AnimatorComponent.h"
#include "pool/AnimatorAssetPool.h"
#include "project/ProjectPrefabInternal.h"

#include "support/PosixCompat.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::editor::mcp::Json;
using jasperengine::editor::mcp::McpValues;
namespace fs = std::filesystem;

namespace {

    const char* const MASK_GUID = "11112222333344445555666677778888";

    AnimationPlayerClip animationLayersClipEntry(const std::string& clip, int layer = 0) {
        AnimationPlayerClip entry;
        entry.clip = clip;
        entry.layer = layer;
        return entry;
    }

    AnimationPlayerLayer animationLayersLayerOf(const std::string& name, float weight = 1.0f, AnimationLayerBlend blend = AnimationLayerBlend::Override,
                                 const std::string& animationLayersMask = "", const std::string& maskGuid = "") {
        AnimationPlayerLayer layer;
        layer.name = name;
        layer.weight = weight;
        layer.blend = blend;
        layer.mask = animationLayersMask;
        layer.maskGuid = maskGuid;
        return layer;
    }

    AnimationPlayerComponent layeredPlayer() {
        AnimationPlayerComponent player;
        player.clips = {animationLayersClipEntry("anim/walk.anim"), animationLayersClipEntry("anim/hold.anim", 1), animationLayersClipEntry("anim/nod.anim", 2)};
        player.layers = {animationLayersLayerOf("Base Layer"), animationLayersLayerOf("Holding", 0.0f, AnimationLayerBlend::Override, "anim/arm.mask", MASK_GUID),
                         animationLayersLayerOf("Hit", 0.5f, AnimationLayerBlend::Additive)};
        return player;
    }

    Entity animationLayersPlayerEntity(Scene& scene, const AnimationPlayerComponent& player) {
        Entity entity = scene.createUserEntity();
        scene.addComponent<Transform>(entity, {});
        scene.addComponent<AnimationPlayerComponent>(entity, player);
        return entity;
    }

}

// ---- Stream --------------------------------------------------------------------------------

TEST(AnimationLayersEditor, StreamRoundTripOfLayersAndMasks) {
    const AnimationPlayerComponent player = layeredPlayer();
    const YAML::Node node = Stream::encodeAnimationPlayerComponent(player);
    EXPECT_FALSE(node["clips"][0]["layer"]) << "layer 0 is not written";
    EXPECT_EQ(node["clips"][1]["layer"].as<int>(), 1);
    ASSERT_TRUE(node["layers"].IsSequence());
    ASSERT_EQ(node["layers"].size(), 3u);
    EXPECT_EQ(node["layers"][0]["name"].as<std::string>(), "Base Layer");
    EXPECT_FALSE(node["layers"][0]["weight"]) << "weight 1 is not written";
    EXPECT_FALSE(node["layers"][0]["blend"]) << "override is not written";
    EXPECT_EQ(node["layers"][1]["weight"].as<float>(), 0.0f);
    EXPECT_EQ(node["layers"][1]["mask"].as<std::string>(), "anim/arm.mask");
    EXPECT_EQ(node["layers"][1]["maskGuid"].as<std::string>(), MASK_GUID);
    EXPECT_EQ(node["layers"][2]["blend"].as<std::string>(), "additive");
    EXPECT_FALSE(node["controller"]) << "a controller plays on an Animator";

    const AnimationPlayerComponent decoded = Stream::decodeAnimationPlayerComponent(node);
    EXPECT_EQ(decoded.clips, player.clips);
    EXPECT_EQ(decoded.layers, player.layers);
    EXPECT_EQ(YAML::Dump(Stream::encodeAnimationPlayerComponent(decoded)), YAML::Dump(node));

    // upstream's controller key in a current file is not read (the scene migration moved the
    // older files' to an Animator): said once
    YAML::Node withController = YAML::Clone(node);
    withController["controller"] = "anim/granjero.controller";
    const AnimationPlayerComponent ignored = Stream::decodeAnimationPlayerComponent(withController);
    EXPECT_EQ(ignored.layers, player.layers);

    // decoding a player without the keys over a layered one clears them (a missing key is the default)
    AnimationPlayerComponent plain;
    plain.clips = {animationLayersClipEntry("anim/walk.anim")};
    AnimationPlayerComponent over = Stream::decodeAnimationPlayerComponent(Stream::encodeAnimationPlayerComponent(plain), &player);
    EXPECT_TRUE(over.layers.empty());
    EXPECT_TRUE(over.needsLoad);

    // lenient: a bad blend is override, a weight is clamped, a scalar is a name
    const AnimationPlayerComponent lenient = Stream::decodeAnimationPlayerComponent(YAML::Load(
        "clips: [{clip: anim/a.anim, layer: 1}, {clip: anim/b.anim, layer: -3}]\n"
        "layers: [Base Layer, {name: Upper, weight: 7, blend: sideways}]\n"));
    ASSERT_EQ(lenient.layers.size(), 2u);
    EXPECT_EQ(lenient.layers[0].name, "Base Layer");
    EXPECT_EQ(lenient.layers[1].weight, 1.0f);
    EXPECT_EQ(lenient.layers[1].blend, AnimationLayerBlend::Override);
    EXPECT_EQ(lenient.clips[0].layer, 1);
    EXPECT_EQ(lenient.clips[1].layer, 0);
}

TEST(AnimationLayersEditor, OldScenesStayByteIdentical) {
    // a player written before layers: the same text back, and nothing new in it
    const char* const old =
        "enabled: true\n"
        "clips:\n"
        "  - clip: anim/walk.anim\n"
        "    guid: 0123456789abcdef0123456789abcdef\n"
        "    name: Walk\n"
        "    wrap: loop\n"
        "defaultClip: Walk\n"
        "playAutomatically: true\n"
        "speed: 1\n"
        "defaultFadeTime: 0.25";
    const YAML::Node node = YAML::Load(old);
    const AnimationPlayerComponent decoded = Stream::decodeAnimationPlayerComponent(node);
    EXPECT_TRUE(decoded.layers.empty());
    EXPECT_EQ(decoded.clips[0].layer, 0);
    EXPECT_EQ(YAML::Dump(Stream::encodeAnimationPlayerComponent(decoded)), YAML::Dump(node));
    // an explicit single default Base Layer is the default too
    AnimationPlayerComponent explicitBase = decoded;
    explicitBase.layers = {animationLayersLayerOf(anim::BASE_LAYER_NAME)};
    EXPECT_EQ(YAML::Dump(Stream::encodeAnimationPlayerComponent(explicitBase)), YAML::Dump(node));
}

// docs/specs/mesh-space-rotation.md R44, AC17: meshSpaceRotation is written only when true, so a
// player without it (the default layering too) saves as before
TEST(AnimationLayersEditor, MeshSpaceRotationRoundTripsAndIsWrittenOnlyWhenSet) {
    AnimationPlayerComponent player = layeredPlayer();
    const std::string without = YAML::Dump(Stream::encodeAnimationPlayerComponent(player));
    EXPECT_EQ(without.find("meshSpaceRotation"), std::string::npos) << without;
    player.layers[1].meshSpaceRotation = true;
    const YAML::Node node = Stream::encodeAnimationPlayerComponent(player);
    ASSERT_TRUE(node["layers"][1]["meshSpaceRotation"]) << YAML::Dump(node);
    EXPECT_EQ(node["layers"][1]["meshSpaceRotation"].Scalar(), "true");
    EXPECT_FALSE(node["layers"][0]["meshSpaceRotation"]);
    EXPECT_FALSE(node["layers"][2]["meshSpaceRotation"]);
    const AnimationPlayerComponent decoded = Stream::decodeAnimationPlayerComponent(node);
    EXPECT_TRUE(decoded.layers[1].meshSpaceRotation);
    EXPECT_FALSE(decoded.layers[2].meshSpaceRotation);
    EXPECT_EQ(decoded.layers, player.layers);
    EXPECT_EQ(YAML::Dump(Stream::encodeAnimationPlayerComponent(decoded)), YAML::Dump(node));
    // part of a layer's value (operator==: prefab diffs, Catalog copies)
    EXPECT_NE(decoded.layers[1], layeredPlayer().layers[1]);

    // the default layering stays unwritten; a base layer holding the flag keeps it (ignored there)
    AnimationPlayerComponent plain;
    plain.clips = {animationLayersClipEntry("anim/walk.anim")};
    plain.layers = {animationLayersLayerOf(anim::BASE_LAYER_NAME)};
    EXPECT_FALSE(Stream::encodeAnimationPlayerComponent(plain)["layers"]);
    plain.layers[0].meshSpaceRotation = true;
    const YAML::Node kept = Stream::encodeAnimationPlayerComponent(plain);
    ASSERT_TRUE(kept["layers"]);
    EXPECT_TRUE(Stream::decodeAnimationPlayerComponent(kept).layers[0].meshSpaceRotation);

    // lenient: anything but a boolean is off
    const AnimationPlayerComponent lenient = Stream::decodeAnimationPlayerComponent(YAML::Load(
        "clips: [{clip: anim/a.anim}]\n"
        "layers: [Base Layer, {name: Upper, meshSpaceRotation: sideways}, {name: Lower, meshSpaceRotation: false}]\n"));
    ASSERT_EQ(lenient.layers.size(), 3u);
    EXPECT_FALSE(lenient.layers[1].meshSpaceRotation);
    EXPECT_FALSE(lenient.layers[2].meshSpaceRotation);
}

// ---- Catalog -------------------------------------------------------------------------------

TEST(AnimationLayersEditor, CatalogProperties) {
    Scene scene;
    const Entity entity = animationLayersPlayerEntity(scene, layeredPlayer());
    const ComponentType type = ComponentType::AnimationPlayerComponent;
    AnimationPlayerComponent& player = scene.getComponent<AnimationPlayerComponent>(entity);

    const PropertyData layers = Catalog::findProperty(&scene, entity, type, "layers");
    EXPECT_EQ(layers.type, Catalog::findProperty(&scene, entity, type, "clips").type) << "a whole list, like the clips";
    EXPECT_EQ(layers.ref, &player.layers);
    EXPECT_TRUE(layers.updateFlags & UpdateFlags_AnimationPlayer_Reload);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "layers[1].name").ref, &player.layers[1].name);
    const PropertyData weight = Catalog::findProperty(&scene, entity, type, "layers[2].weight");
    EXPECT_EQ(weight.type, PropertyType::Float);
    EXPECT_EQ(weight.ref, &player.layers[2].weight);
    const PropertyData blend = Catalog::findProperty(&scene, entity, type, "layers[2].blend");
    EXPECT_EQ(blend.type, PropertyType::Custom) << "a one-byte enum, never an int";
    EXPECT_EQ(blend.ref, &player.layers[2].blend);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "layers[1].mask").ref, &player.layers[1].mask);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "layers[1].maskGuid").ref, &player.layers[1].maskGuid);
    const PropertyData meshSpace = Catalog::findProperty(&scene, entity, type, "layers[1].meshSpaceRotation");
    EXPECT_EQ(meshSpace.type, PropertyType::Bool);
    EXPECT_EQ(meshSpace.ref, &player.layers[1].meshSpaceRotation);
    EXPECT_TRUE(meshSpace.updateFlags & UpdateFlags_AnimationPlayer_Reload);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "layers[3].name").ref);
    const PropertyData layer = Catalog::findProperty(&scene, entity, type, "clips[1].layer");
    EXPECT_EQ(layer.type, PropertyType::Int);
    EXPECT_EQ(layer.ref, &player.clips[1].layer);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "controller").ref) << "an Animator's property";

    const std::map<std::string, PropertyData> templates = Catalog::getProperties(type, nullptr);
    for (const char* name : {"layers", "layers[].name", "layers[].weight", "layers[].blend", "layers[].mask", "layers[].maskGuid",
                             "layers[].meshSpaceRotation", "clips[].layer"}) {
        EXPECT_TRUE(templates.count(name)) << name;
    }

    // copies (a prefab revert): the list, and a blend
    const Entity target = animationLayersPlayerEntity(scene, AnimationPlayerComponent());
    Catalog::copyPropertyValue(&scene, entity, &scene, target, type, "layers");
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(target).layers, layeredPlayer().layers);
    scene.getComponent<AnimationPlayerComponent>(entity).layers[1].blend = AnimationLayerBlend::Additive;
    Catalog::copyPropertyValue(&scene, entity, &scene, target, type, "layers[1].blend");
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(target).layers[1].blend, AnimationLayerBlend::Additive);
}

// ---- prefab overrides ----------------------------------------------------------------------

TEST(AnimationLayersEditor, PrefabOverrideOfALayerWeight) {
    const ComponentType type = ComponentType::AnimationPlayerComponent;
    Scene asset;
    const AnimationPlayerComponent assetPlayer = layeredPlayer();
    const Entity assetRoot = animationLayersPlayerEntity(asset, assetPlayer);

    Scene instance;
    AnimationPlayerComponent heavier = assetPlayer;
    heavier.layers[1].weight = 0.75f;
    const Entity heavierRoot = animationLayersPlayerEntity(instance, heavier);
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, heavierRoot, type),
              (std::vector<std::string>{"layers", "layers[1].weight"}));

    AnimationPlayerComponent moved = assetPlayer;
    moved.clips[2].layer = 1;
    const Entity movedRoot = animationLayersPlayerEntity(instance, moved);
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, movedRoot, type),
              (std::vector<std::string>{"clips", "clips[2].layer"}));

    // applied as the instance loads: the prefab's player with the instance's weight only
    prefabinternal::ScratchEntity values;
    values.takeComponents(&instance, heavierRoot, Catalog::componentTypeToSignature(&instance, type));
    Scene loaded;
    AnimationPlayerComponent changedAsset = assetPlayer;
    changedAsset.layers[2].weight = 0.25f;     // the prefab changed since: its other values come through
    const Entity loadedRoot = animationLayersPlayerEntity(loaded, changedAsset);
    std::set<std::string> names{"layers[1].weight"};
    prefabinternal::copyProperties(values, &loaded, loadedRoot, type, names, "test");
    EXPECT_FLOAT_EQ(loaded.getComponent<AnimationPlayerComponent>(loadedRoot).layers[1].weight, 0.75f);
    EXPECT_FLOAT_EQ(loaded.getComponent<AnimationPlayerComponent>(loadedRoot).layers[2].weight, 0.25f);
}

// ---- layer edits ---------------------------------------------------------------------------

TEST(AnimationLayersEditor, LayerEditsMoveTheirEntries) {
    const AnimationPlayerComponent player = layeredPlayer();
    PlayerLayerActions::Result result;
    std::string error;
    // Hit (2) above Holding (1): the entries follow their layers
    ASSERT_TRUE(PlayerLayerActions::computeMove(player, 2, 1, result, &error)) << error;
    ASSERT_EQ(result.layers.size(), 3u);
    EXPECT_EQ(result.layers[1].name, "Hit");
    EXPECT_EQ(result.layers[2].name, "Holding");
    EXPECT_EQ(result.clips[1].layer, 2) << "hold follows Holding";
    EXPECT_EQ(result.clips[2].layer, 1) << "nod follows Hit";
    EXPECT_FALSE(PlayerLayerActions::computeMove(player, 0, 1, result, &error)) << "the base layer stays first";
    EXPECT_FALSE(PlayerLayerActions::computeMove(player, 1, 0, result, &error));

    ASSERT_TRUE(PlayerLayerActions::computeRemove(player, 1, result, &error));
    EXPECT_EQ(result.clips[1].layer, 0) << "its entries move to the base layer";
    EXPECT_EQ(result.clips[2].layer, 1) << "the layers above move down";
    EXPECT_FALSE(PlayerLayerActions::computeRemove(player, 0, result, &error));
    // an entry on a layer the player does not have (it plays on the base) is not moved onto one
    AnimationPlayerComponent stray = player;
    stray.clips[2].layer = 3;
    ASSERT_TRUE(PlayerLayerActions::computeRemove(stray, 1, result, &error));
    EXPECT_EQ(result.clips[2].layer, 3) << "out of range before, out of range after";

    EXPECT_FALSE(PlayerLayerActions::computeRename(player, 1, "Hit", result, &error)) << "names are unique";
    EXPECT_FALSE(PlayerLayerActions::computeAdd(player, animationLayersLayerOf("Holding"), result, &error));
    ASSERT_TRUE(PlayerLayerActions::computeAdd(AnimationPlayerComponent(), animationLayersLayerOf("Upper"), result, &error));
    EXPECT_EQ(result.index, 1);
    ASSERT_EQ(result.layers.size(), 2u);
    EXPECT_EQ(result.layers[0].name, anim::BASE_LAYER_NAME) << "the implicit base layer is made explicit";

    // removing the last added layer brings the default back (stored empty)
    AnimationPlayerComponent two;
    two.layers = result.layers;
    ASSERT_TRUE(PlayerLayerActions::computeRemove(two, 1, result, &error));
    EXPECT_TRUE(result.layers.empty());
}

// ---- with a project ------------------------------------------------------------------------

namespace {

    struct AnimationLayersHeadless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    AnimationLayersHeadless& animationLayersHeadless() {
        static AnimationLayersHeadless h = []() {
            AnimationLayersHeadless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-layers-editor-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: layersproject\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the test binary's TestSystem stays the engine's System (a second
            // Engine::systemInit would break its log capture): the project's paths go in
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* animationLayersProject() { return animationLayersHeadless().project.get(); }
    fs::path animationLayersAssets() { return animationLayersProject()->getAssetsPath(); }

    void animationLayersWriteAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string animationLayersReadAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    struct PlayerScene {
        SceneProject* sceneProject = nullptr;
        Entity entity = NULL_ENTITY;
        AnimationPlayerComponent& player() const { return sceneProject->scene->getComponent<AnimationPlayerComponent>(entity); }
    };

    PlayerScene playerScene(const AnimationPlayerComponent& player) {
        PlayerScene out;
        Project* p = animationLayersProject();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Layers", SceneType::SCENE_3D);
        out.sceneProject = p->getScene(p->getSelectedSceneId());
        Scene* scene = out.sceneProject->scene;
        out.entity = scene->createUserEntity();
        scene->setEntityName(out.entity, "Granjero");
        scene->addComponent<Transform>(out.entity, {});
        scene->addComponent<AnimationPlayerComponent>(out.entity, player);
        out.sceneProject->entities.push_back(out.entity);
        return out;
    }

    bool animationLayersApply(const PlayerScene& s, const Json& values, std::string& error) {
        std::vector<McpValues::Change> changes;
        if (!McpValues::parseChanges(values, changes, error)) return false;
        MultiPropertyCmd multi;
        if (!McpValues::appendChangeCommands(animationLayersProject(), s.sceneProject, {s.entity}, changes, multi, error)) return false;
        return multi.execute();
    }

    Json animationLayersRead(const PlayerScene& s, const std::string& name) {
        McpValues::ReadContext ctx;
        ctx.project = animationLayersProject();
        ctx.scene = s.sceneProject;
        ctx.entity = s.entity;
        ctx.component = ComponentType::AnimationPlayerComponent;
        const PropertyData property = Catalog::findProperty(s.sceneProject->scene, s.entity, ComponentType::AnimationPlayerComponent, name);
        return McpValues::readProperty(ctx, name, property, false);
    }

    std::string maskText(const std::string& guid) {
        anim::AvatarMaskDef data;
        data.guid = guid;
        data.defaultWeight = 0.0f;
        data.paths.push_back({"Arm", 1.0f, true});
        return anim::writeMask(data);
    }

    const char* const CONTROLLER_TEXT = R"({"format": "jasperengine.controller", "version": 1,
      "layers": [{"name": "Base Layer", "states": [{"name": "Walk", "clip": "anim/walk.anim"}, {"name": "Hit", "clip": "models/hero.gltf#Hit"}]},
                 {"name": "Holding", "mask": "anim/arm.mask", "states": []}]})";

}

TEST(AnimationLayersEditor, SetPropertiesValuesAndRefusals) {
    ASSERT_TRUE(animationLayersHeadless().ready);
    animationLayersWriteAll(animationLayersAssets() / "anim/arm.mask", maskText(MASK_GUID));
    animationLayersWriteAll(animationLayersAssets() / "anim/granjero.controller", CONTROLLER_TEXT);
    animationLayersWriteAll(animationLayersAssets() / "anim/walk.anim", "{\"format\": \"jasperengine.anim\", \"version\": 1, \"wrap\": \"loop\", \"tracks\": []}");
    AnimationPlayerComponent initial;
    initial.clips = {animationLayersClipEntry("anim/walk.anim")};
    PlayerScene s = playerScene(initial);
    std::string error;

    // the whole list, project-relative masks; the GUID follows the file
    ASSERT_TRUE(animationLayersApply(s, Json{{"animationplayer", {{"layers", Json::array({Json{{"name", "Base Layer"}},
        Json{{"name", "Holding"}, {"weight", 0.5}, {"mask", "assets/anim/arm.mask"}}})}}}}, error)) << error;
    ASSERT_EQ(s.player().layers.size(), 2u);
    EXPECT_EQ(s.player().layers[1].mask, "anim/arm.mask");
    EXPECT_EQ(s.player().layers[1].maskGuid, MASK_GUID);
    EXPECT_FLOAT_EQ(s.player().layers[1].weight, 0.5f);
    const Json layers = animationLayersRead(s, "layers");
    ASSERT_TRUE(layers.is_array()) << layers.dump();
    EXPECT_EQ(layers[1]["mask"], "assets/anim/arm.mask");
    EXPECT_EQ(layers[1]["blend"], "override");

    // one field; an entry's layer
    ASSERT_TRUE(animationLayersApply(s, Json{{"animationplayer", {{"layers[1].blend", "additive"}, {"clips[0].layer", 1}}}}, error)) << error;
    EXPECT_EQ(s.player().layers[1].blend, AnimationLayerBlend::Additive);
    EXPECT_EQ(s.player().clips[0].layer, 1);
    EXPECT_EQ(animationLayersRead(s, "layers[1].blend"), "additive");
    // upstream's player controller: the Animator's now
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"controller", "assets/anim/granjero.controller"}}}}, error));

    // Mesh Space Rotation (mesh-space-rotation spec R39): one field, the whole list, and an edit of
    // another field keeps it
    ASSERT_TRUE(animationLayersApply(s, Json{{"animationplayer", {{"layers[1].blend", "override"}, {"layers[1].meshSpaceRotation", true}}}}, error)) << error;
    EXPECT_TRUE(s.player().layers[1].meshSpaceRotation);
    EXPECT_EQ(animationLayersRead(s, "layers[1].meshSpaceRotation"), true);
    EXPECT_EQ(animationLayersRead(s, "layers")[1]["meshSpaceRotation"], true);
    EXPECT_FALSE(animationLayersRead(s, "layers")[0].contains("meshSpaceRotation")) << "written only when on";
    ASSERT_TRUE(animationLayersApply(s, Json{{"animationplayer", {{"clips", Json::array({Json{{"clip", "assets/anim/walk.anim"}, {"layer", 1}}})}}}}, error))
        << error;
    EXPECT_TRUE(s.player().layers[1].meshSpaceRotation) << "a clips edit re-checks the encoded layers";
    ASSERT_TRUE(animationLayersApply(s, Json{{"animationplayer", {{"layers", Json::array({Json{{"name", "Base Layer"}},
        Json{{"name", "Holding"}, {"mask", "assets/anim/arm.mask"}, {"meshSpaceRotation", false}}})}}}}, error)) << error;
    EXPECT_FALSE(s.player().layers[1].meshSpaceRotation);
    ASSERT_TRUE(animationLayersApply(s, Json{{"animationplayer", {{"layers", Json::array({Json{{"name", "Base Layer"}},
        Json{{"name", "Holding"}, {"mask", "assets/anim/arm.mask"}, {"meshSpaceRotation", true}}})}}}}, error)) << error;
    EXPECT_TRUE(s.player().layers[1].meshSpaceRotation);
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"layers", Json::array({Json{{"name", "Base Layer"}},
        Json{{"name", "Holding"}, {"meshSpaceRotation", "yes"}}})}}}}, error));
    EXPECT_NE(error.find("meshSpaceRotation must be true or false"), std::string::npos) << error;
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"layers[1].meshSpaceRotation", "yes"}}}}, error));
    EXPECT_NE(error.find("true or false"), std::string::npos) << error;

    // refusals, with the reason
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"layers[0].blend", "additive"}}}}, error));
    EXPECT_NE(error.find("base layer"), std::string::npos) << error;
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"layers", Json::array({Json{{"name", "Base Layer"}, {"weight", 0.5}}})}}}}, error));
    EXPECT_NE(error.find("always has weight 1"), std::string::npos) << error;
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"layers", Json::array({Json{{"name", "A"}}, Json{{"name", "A"}}})}}}}, error));
    EXPECT_NE(error.find("names are unique"), std::string::npos) << error;
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"layers[1].mask", "assets/anim/nope.mask"}}}}, error));
    EXPECT_NE(error.find("does not exist"), std::string::npos) << error;
    animationLayersWriteAll(animationLayersAssets() / "anim/bad.mask", "{\"format\": \"jasperengine.mask\",,}");
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"layers[1].mask", "assets/anim/bad.mask"}}}}, error));
    EXPECT_NE(error.find("does not load"), std::string::npos) << error;
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"layers[1].maskGuid", MASK_GUID}}}}, error));
    EXPECT_NE(error.find("read-only"), std::string::npos) << error;
    EXPECT_FALSE(animationLayersApply(s, Json{{"animationplayer", {{"clips", Json::array({Json{{"clip", "assets/anim/walk.anim"}, {"layer", 5}}})}}}}, error));
    EXPECT_NE(error.find("add the layer first"), std::string::npos) << error;

    // the base layer of a default player: its whole list holds the field (kept, and ignored there)
    AnimationPlayerComponent single;
    single.clips = {animationLayersClipEntry("anim/walk.anim")};
    PlayerScene plain = playerScene(single);
    ASSERT_TRUE(animationLayersApply(plain, Json{{"animationplayer", {{"layers[0].meshSpaceRotation", true}}}}, error)) << error;
    ASSERT_EQ(plain.player().layers.size(), 1u);
    EXPECT_TRUE(plain.player().layers[0].meshSpaceRotation);
}

TEST(AnimationLayersEditor, ALayerReorderIsOneUndoStep) {
    ASSERT_TRUE(animationLayersHeadless().ready);
    PlayerScene s = playerScene(layeredPlayer());
    const AnimationPlayerComponent before = s.player();
    std::string error;
    ASSERT_TRUE(PlayerLayerActions::moveLayer(animationLayersProject(), s.sceneProject->id, s.entity, 2, 1, &error)) << error;
    EXPECT_EQ(s.player().layers[1].name, "Hit");
    EXPECT_EQ(s.player().clips[2].layer, 1);
    CommandHistory* history = CommandHandle::get(s.sceneProject->id);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(s.player().layers, before.layers) << "one step: the layers and the entries together";
    EXPECT_EQ(s.player().clips, before.clips);
    ASSERT_TRUE(history->redo());
    EXPECT_EQ(s.player().layers[2].name, "Holding");
    EXPECT_EQ(s.player().clips[1].layer, 2);
    ASSERT_TRUE(PlayerLayerActions::renameLayer(animationLayersProject(), s.sceneProject->id, s.entity, 2, "Carry", &error)) << error;
    EXPECT_EQ(s.player().layers[2].name, "Carry");
    EXPECT_EQ(s.player().clips[1].layer, 2) << "L7: a rename changes nothing else";
}

TEST(AnimationLayersEditor, MaskAndControllerDocuments) {
    ASSERT_TRUE(animationLayersHeadless().ready);
    fs::remove(animationLayersAssets() / "anim/new.mask");
    std::string error;
    std::shared_ptr<AnimatorAssetDocument> animationLayersMask =
        AnimatorAssetDocument::create(animationLayersProject(), "assets/anim/new.mask", AnimatorAssetDocument::Kind::Mask, "Create Avatar Mask", &error);
    ASSERT_TRUE(animationLayersMask) << error;
    EXPECT_TRUE(anim::isClipGuid(animationLayersMask->mask().guid)) << "a new file gets a GUID";
    const std::string guid = animationLayersMask->mask().guid;
    EXPECT_EQ(animationLayersMask->mask().defaultWeight, 1.0f) << "a new mask passes everything";
    EXPECT_FALSE(AnimatorAssetDocument::create(animationLayersProject(), "assets/anim/new.mask", AnimatorAssetDocument::Kind::Mask, "", &error)) << "it exists";
    EXPECT_EQ(AnimatorAssetDocument::open(animationLayersProject(), "anim/new.mask", &error), animationLayersMask) << "one document per path (asset-relative form too)";
    anim::AvatarMaskDef initial = animationLayersMask->mask();
    initial.paths.push_back({"Arm", 1.0f, true});
    ASSERT_TRUE(animationLayersMask->commit(initial, "Edit Mask", "", &error)) << error;
    animationLayersProject()->refreshAssetDatabase(true);
    EXPECT_EQ(animationLayersProject()->getAssetDatabase().getGuid(animationLayersAssets() / "anim/new.mask"), guid) << "the asset database tracks it";

    // a player using it reloads it on every revision (the pool's generation)
    AnimationPlayerComponent user;
    user.layers = {animationLayersLayerOf("Base Layer"), animationLayersLayerOf("Upper", 1.0f, AnimationLayerBlend::Override, "anim/new.mask", guid)};
    PlayerScene s = playerScene(user);
    AnimatorAssetPool::mask("anim/new.mask");
    const uint64_t generation = AnimatorAssetPool::getGeneration("anim/new.mask");
    anim::AvatarMaskDef edited = animationLayersMask->mask();
    edited.defaultWeight = 0.25f;
    edited.guid = "ffffffffffffffffffffffffffffffff";
    const uint64_t revision = animationLayersMask->revision();
    ASSERT_TRUE(animationLayersMask->commit(edited, "Edit Mask", "", &error)) << error;
    EXPECT_GT(animationLayersMask->revision(), revision);
    EXPECT_EQ(animationLayersMask->mask().defaultWeight, 0.25f);
    EXPECT_EQ(animationLayersMask->mask().guid, guid) << "a commit never changes the GUID";
    AnimatorAssetPool::mask("anim/new.mask");
    EXPECT_NE(AnimatorAssetPool::getGeneration("anim/new.mask"), generation) << "the players' layers load it again";
    EXPECT_EQ(animationLayersReadAll(animationLayersAssets() / "anim/new.mask"), anim::writeMask(animationLayersMask->mask()));
    EXPECT_NE(animationLayersReadAll(animationLayersAssets() / "anim/new.mask").find("\"version\": 2"), std::string::npos);

    // undo and redo reach the document
    ASSERT_TRUE(animationLayersProject()->getProjectCommandHistory()->undo());
    EXPECT_EQ(animationLayersMask->mask().defaultWeight, 1.0f);
    ASSERT_TRUE(animationLayersProject()->getProjectCommandHistory()->redo());
    EXPECT_EQ(animationLayersMask->mask().defaultWeight, 0.25f);

    // an outside edit reloads (the animation documents' broadcast); a broken one keeps the last
    // good data with the error
    edited.defaultWeight = 0.5f;
    animationLayersWriteAll(animationLayersAssets() / "anim/new.mask", anim::writeMask(edited));
    AnimationClipDocument::filesChanged({animationLayersAssets() / "anim/new.mask"}, animationLayersProject());
    EXPECT_EQ(animationLayersMask->mask().defaultWeight, 0.5f);
    animationLayersWriteAll(animationLayersAssets() / "anim/new.mask", "{\"format\": \"jasperengine.mask\",\n \"version\": 1,,}");
    AnimationClipDocument::filesChanged({animationLayersAssets() / "anim/new.mask"}, animationLayersProject());
    EXPECT_EQ(animationLayersMask->mask().defaultWeight, 0.5f);
    EXPECT_NE(animationLayersMask->loadError().find("2:"), std::string::npos) << animationLayersMask->loadError();
    animationLayersWriteAll(animationLayersAssets() / "anim/new.mask", anim::writeMask(edited));
    AnimationClipDocument::checkDiskChanges(animationLayersProject());
    EXPECT_TRUE(animationLayersMask->loadError().empty()) << "fixed on disk: read again";

    const std::vector<AnimatorAssetUser> users = findMaskUsers(animationLayersProject(), "assets/anim/new.mask");
    ASSERT_EQ(users.size(), 1u);
    EXPECT_EQ(users[0].entity, s.entity);
    EXPECT_EQ(users[0].layer, 1);
    EXPECT_EQ(findMaskUsers(animationLayersProject(), guid).size(), 1u) << "by GUID too";

    // a controller document: create, the users of a controller (Animators)
    fs::remove(animationLayersAssets() / "anim/new.controller");
    std::shared_ptr<AnimatorAssetDocument> controller =
        AnimatorAssetDocument::create(animationLayersProject(), "assets/anim/new.controller", AnimatorAssetDocument::Kind::Controller, "", &error);
    ASSERT_TRUE(controller) << error;
    anim::ControllerDef withState = controller->controller();
    anim::StateDef idle;
    idle.name = "Idle";
    withState.layers[0].machine.states.push_back(idle);
    ASSERT_TRUE(controller->commit(withState, "Add State", "", &error)) << error;
    EXPECT_EQ(controller->controller().layers[0].machine.states.size(), 1u);
    ASSERT_TRUE(animationLayersProject()->getProjectCommandHistory()->undo());
    EXPECT_TRUE(controller->controller().layers[0].machine.states.empty());
    AnimatorComponent animator;
    animator.controller = AssetRef("anim/new.controller", Guid::parse(controller->controller().guid));
    s.sceneProject->scene->addComponent<AnimatorComponent>(s.entity, animator);
    EXPECT_EQ(findControllerUsers(animationLayersProject(), "assets/anim/new.controller").size(), 1u);
    EXPECT_FALSE(AnimatorAssetDocument::open(animationLayersProject(), "assets/anim/new.txt", &error)) << "not an Animator asset";
    animationLayersWriteAll(animationLayersAssets() / "anim/unity.mask", "%YAML 1.1\n--- !u!319 &31900000\nAvatarMask:\n");
    EXPECT_FALSE(AnimatorAssetDocument::open(animationLayersProject(), "assets/anim/unity.mask", &error));
    EXPECT_NE(error.find("Unity .mask"), std::string::npos) << error;

    // an upstream version 1 mask: read converted, written as version 2 on the next commit
    animationLayersWriteAll(animationLayersAssets() / "anim/upstream.mask", R"({"format": "jasperengine.mask", "version": 1, "default": 0,
      "humanoid": {"right_arm": 1}, "transforms": [{"path": "Arm", "weight": 0.5}]})");
    std::shared_ptr<AnimatorAssetDocument> upstream = AnimatorAssetDocument::open(animationLayersProject(), "assets/anim/upstream.mask", &error);
    ASSERT_TRUE(upstream) << error;
    EXPECT_TRUE(upstream->converted());
    EXPECT_EQ(upstream->dialect(), anim::AnimatorFileDialect::UpstreamVersion1);
    EXPECT_EQ(upstream->mask().partWeight(AvatarBodyPart::RightHand), 1.0f);
    ASSERT_TRUE(upstream->commit(upstream->mask(), "Save Mask", "", &error)) << error;
    EXPECT_FALSE(upstream->converted());
    const anim::MaskParseResult saved = anim::parseMask(animationLayersReadAll(animationLayersAssets() / "anim/upstream.mask"));
    ASSERT_TRUE(saved.ok);
    EXPECT_EQ(saved.dialect, anim::AnimatorFileDialect::Current);
    ASSERT_EQ(saved.data.paths.size(), 1u);
    EXPECT_TRUE(saved.data.paths[0].children);
    EXPECT_FLOAT_EQ(saved.data.paths[0].weight, 0.5f);
}

TEST(AnimationLayersEditor, ReferencesFollowRenamesAndScenesPreloadThem) {
    ASSERT_TRUE(animationLayersHeadless().ready);
    animationLayersWriteAll(animationLayersAssets() / "anim/arm.mask", maskText(MASK_GUID));
    animationLayersWriteAll(animationLayersAssets() / "anim/granjero.controller", CONTROLLER_TEXT);
    animationLayersProject()->refreshAssetDatabase(true);
    const std::string controllerGuid = animationLayersProject()->getAssetDatabase().getGuid(animationLayersAssets() / "anim/granjero.controller");
    ASSERT_TRUE(anim::isClipGuid(controllerGuid)) << "the database gives a JSON asset without one its GUID";
    EXPECT_EQ(anim::readClipGuid(animationLayersReadAll(animationLayersAssets() / "anim/granjero.controller")), controllerGuid) << "written into the file";

    AnimationPlayerComponent user;
    user.layers = {animationLayersLayerOf("Base Layer"), animationLayersLayerOf("Upper", 1.0f, AnimationLayerBlend::Override, "anim/arm.mask")};
    PlayerScene s = playerScene(user);
    AnimatorComponent animator;
    animator.controller = AssetRef("anim/granjero.controller");
    s.sceneProject->scene->addComponent<AnimatorComponent>(s.entity, animator);
    // a reference without a GUID gets it filled in (silently: where it points did not change)
    animationLayersProject()->resolveRegistryClipRefs(s.sceneProject->scene);
    EXPECT_EQ(s.player().layers[1].maskGuid, MASK_GUID);

    // the scene's resources: the masks, the controller and the clips (and models) it names
    ASSERT_TRUE(animationLayersProject()->saveSceneToPath(s.sceneProject->id, "Layers.scene"));
    const fs::path cooked = animationLayersProject()->getCookedCacheDir() / "scenes" / (std::to_string(s.sceneProject->id) + ".jscn");
    const std::string bytes = animationLayersReadAll(cooked);
    ASSERT_FALSE(bytes.empty()) << cooked;
    const SceneResources resources = CookedSceneLoader::sceneResources(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    auto has = [](const std::vector<std::string>& list, const char* path) { return std::find(list.begin(), list.end(), path) != list.end(); };
    EXPECT_TRUE(has(resources.models, "models/hero.gltf"));
    EXPECT_TRUE(has(resources.clips, "anim/walk.anim"));
    EXPECT_TRUE(has(resources.clips, "models/hero.gltf#Hit"));
    EXPECT_TRUE(has(resources.masks, "anim/arm.mask"));
    EXPECT_TRUE(has(resources.controllers, "anim/granjero.controller"));

    // the mask renamed in the editor: the player's path and the controller's layer follow
    RenameFileCmd rename(animationLayersProject(), "arm.mask", "arm2.mask", (animationLayersAssets() / "anim").string());
    ASSERT_TRUE(rename.execute());
    EXPECT_EQ(s.player().layers[1].mask, "anim/arm2.mask");
    anim::ControllerParseResult renamed = anim::parseController(animationLayersReadAll(animationLayersAssets() / "anim/granjero.controller"));
    ASSERT_TRUE(renamed.ok);
    EXPECT_EQ(renamed.data.layers[1].mask.path, "anim/arm2.mask");
    rename.undo();
    EXPECT_EQ(s.player().layers[1].mask, "anim/arm.mask");
    EXPECT_EQ(animationLayersReadAll(animationLayersAssets() / "anim/granjero.controller").find("arm2"), std::string::npos);
}

// A clip and a mask moved outside the editor: the controllers naming them by GUID follow (the
// GUID first, anim layers spec 6.3), and an open controller document reloads; upstream's
// version 1 controller is read converted and written as version 2
TEST(AnimationLayersEditor, ControllersFollowClipsAndMasksMovedOutsideTheEditor) {
    ASSERT_TRUE(animationLayersHeadless().ready);
    const char* const clipGuid = "c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1";
    const char* const maskGuid = "d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1";
    animationLayersWriteAll(animationLayersAssets() / "relink/arm.mask", maskText(maskGuid));
    animationLayersWriteAll(animationLayersAssets() / "relink/run.anim", std::string("{\"format\": \"jasperengine.anim\", \"version\": 1, \"guid\": \"") + clipGuid +
                                           "\", \"wrap\": \"loop\", \"tracks\": []}");
    animationLayersWriteAll(animationLayersAssets() / "relink/hero.controller", std::string(R"({"format": "jasperengine.controller", "version": 1,
      "layers": [{"name": "Base Layer", "states": [{"name": "Run", "clip": "relink/run.anim", "clip_guid": ")") + clipGuid + R"("},
          {"name": "Mix", "blend_tree": {"parameter": "Speed", "children": [{"clip": "relink/run.anim", "clip_guid": ")" + clipGuid +
          R"(", "threshold": 0}]}}]},
        {"name": "Holding", "mask": "relink/arm.mask", "mask_guid": ")" + maskGuid + R"(", "states": []}],
      "parameters": [{"name": "Speed", "type": "float"}]})");
    animationLayersProject()->refreshAssetDatabase(true);
    std::string error;
    std::shared_ptr<AnimatorAssetDocument> document = AnimatorAssetDocument::open(animationLayersProject(), "assets/relink/hero.controller", &error);
    ASSERT_TRUE(document) << error;

    fs::create_directories(animationLayersAssets() / "relink/moved");
    fs::rename(animationLayersAssets() / "relink/arm.mask", animationLayersAssets() / "relink/moved/arm.mask");
    fs::rename(animationLayersAssets() / "relink/run.anim", animationLayersAssets() / "relink/moved/run.anim");
    animationLayersProject()->refreshAssetDatabase(true);

    anim::ControllerParseResult parsed = anim::parseController(animationLayersReadAll(animationLayersAssets() / "relink/hero.controller"));
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    EXPECT_EQ(parsed.dialect, anim::AnimatorFileDialect::Current) << "rewritten as version 2";
    const anim::StateMachineDef& machine = parsed.data.layers[0].machine;
    EXPECT_EQ(machine.states[0].motion.clip.path, "relink/moved/run.anim") << "a state's clip";
    EXPECT_EQ(machine.states[1].motion.tree[0].children[0].motion.clip.path, "relink/moved/run.anim") << "a blend-tree child";
    EXPECT_EQ(parsed.data.layers[1].mask.path, "relink/moved/arm.mask") << "a layer's mask";
    EXPECT_EQ(parsed.data.layers[1].mask.guid, maskGuid) << "the GUIDs stay";
    EXPECT_EQ(document->controller().layers[1].mask.path, "relink/moved/arm.mask") << "the open document reloaded";
}
