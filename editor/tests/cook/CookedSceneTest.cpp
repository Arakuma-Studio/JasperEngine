// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The cooked scenes of a prebuilt player (docs/specs/S30-shipping.md AC4): every component's
// saved values (S08 tables) cooked by the editor (SceneCooker) and decoded by the runtime loader
// (CookedSceneLoader) come back equal, compared through the S08 encode of both. Random values
// for the table components; the AllComponents fixture's values for every component, codec ones
// (mesh, tilemap, terrain, particles, UI document, ...) included. RuntimeFields.Generated fails
// when the committed RuntimeFields.gen.inc is not what the editor's tables emit.

#include "support/EditorTestSupport.h"
#include "reflect/ReflectTestSupport.h"

#include "Catalog.h"
#include "component/ActionComponent.h"
#include "component/MeshComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SplineComponent.h"
#include "component/TweenComponent.h"
#include "cook/RuntimeFieldsEmitter.h"
#include "cook/SceneCooker.h"
#include "reflect/Reflect.h"
#include "serialization/CookedScene.h"
#include "serialization/SceneSettingsCodec.h"
#include "serialization/codecs/CustomCodecs.h"
#include "util/ProjectUtils.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace cooked_scene_test {

    // Keys of a component's encode that a cooked scene does not carry over as they are, each
    // for a reason that holds in the Factory (Compile) path too
    const std::map<std::string, std::set<std::string>>& derivedKeys() {
        static const std::map<std::string, std::set<std::string>> keys = {
            // a build starts every action and audio source stopped; a saved Running, Paused or
            // Playing state is replayed through its triggers (the generated code of the Compile
            // path does the same): checkPlaybackState checks that instead
            {"action", {"state"}},
            {"audiosource", {"state"}},
        };
        return keys;
    }

    YAML::Node withoutDerived(const std::string& component, YAML::Node node) {
        auto it = derivedKeys().find(component);
        if (it == derivedKeys().end() || !node.IsMap()) return node;
        YAML::Node out(YAML::NodeType::Map);
        for (auto item = node.begin(); item != node.end(); ++item)
            if (!it->second.count(item->first.Scalar())) out[item->first] = item->second;
        return out;
    }

    std::string encodeOf(const reflect::ComponentDesc& desc, Scene* scene, Entity entity) {
        // a document's encode, as a .scene holds it (S29: the transform's world values are the
        // engine's to compute, in a cooked scene too)
        reflect::DocumentEncodeScope document;
        void* comp = desc.find(scene, entity);
        if (!comp) return "<missing>";
        reflect::EncodeContext ctx;
        ctx.registry = scene;
        ctx.entity = entity;
        ctx.signature = scene->getSignature(entity);
        YAML::Emitter out;
        out << withoutDerived(desc.key, reflect::encode(desc, comp, ctx));
        return out.c_str();
    }

    DecodedScene emptyScene() {
        return decodeSceneText("formatVersion: 1\nid: 4242\nname: Cooked\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\n"
                               "mainCamera: 0\nentities: []\n",
                               "Cooked.scene");
    }

    // cooks `entities` of `from` and loads the bytes into a fresh scene
    bool roundTrip(Scene* from, const std::vector<Entity>& entities, DecodedScene& to, std::string& error) {
        const std::vector<uint8_t> bytes = editor::cook::SceneCooker::cookScene(from, 4242, entities, NULL_ENTITY, SceneResources{});
        to = emptyScene();
        return CookedSceneLoader::loadScene(to.scene->scene, std::span<const uint8_t>(bytes.data(), bytes.size()), &error);
    }

    // what a saved playback state became after the load (ComponentHooks.cpp afterAction /
    // afterAudioSource): stopped, with the triggers that start (and pause) it
    void checkPlaybackState(const reflect::ComponentDesc& desc, Scene* source, Scene* loaded, Entity entity) {
        if (desc.type == ComponentType::ActionComponent) {
            const ActionComponent& before = source->getComponent<ActionComponent>(entity);
            const ActionComponent& after = loaded->getComponent<ActionComponent>(entity);
            EXPECT_EQ(after.state, ActionComponent().state);
            EXPECT_EQ(after.startTrigger, before.state != ActionState::Stopped) << "action state " << static_cast<int>(before.state);
            EXPECT_EQ(after.pauseTrigger, before.state == ActionState::Paused) << "action state " << static_cast<int>(before.state);
        } else if (desc.type == ComponentType::AudioSourceComponent) {
            const AudioSourceComponent& before = source->getComponent<AudioSourceComponent>(entity);
            const AudioSourceComponent& after = loaded->getComponent<AudioSourceComponent>(entity);
            EXPECT_EQ(after.state, AudioSourceState::Stopped);
            EXPECT_EQ(after.startTrigger, before.state == AudioSourceState::Playing) << "audio state " << static_cast<int>(before.state);
        }
    }

    // the component types whose tables the random values fill (a codec component's state is more
    // than its table: the fixture's values cover those)
    bool randomizable(const reflect::ComponentDesc& desc) {
        return desc.codec == nullptr && desc.type != ComponentType::Transform;
    }

}

using namespace cooked_scene_test;

TEST(CookedScene, RoundTripEveryComponent) {
    constexpr int SAMPLES = 4;
    OwnedFramebuffers owned;
    size_t checked = 0;
    for (const reflect::ComponentDesc* desc : reflect::all()) {
        if (!randomizable(*desc)) continue;
        for (int sample = 0; sample < SAMPLES; sample++) {
            DecodedScene source = emptyScene();
            Scene* scene = source.scene->scene;
            const Entity entity = scene->createEntity();
            ASSERT_NE(entity, NULL_ENTITY);
            ProjectUtils::addEntityComponent(scene, entity, desc->type, source.scene->entities, YAML::Node());
            void* comp = desc->find(scene, entity);
            ASSERT_NE(comp, nullptr) << desc->cppName;
            RandomValues random(9001u * static_cast<uint32_t>(desc->type) + static_cast<uint32_t>(sample), true);
            random.withoutReferences();
            random.fillStruct(desc->body, comp);
            // what a scene load leaves (the component a cook sees): the invariants its decode keeps
            // (a spline's unit up axis and non-negative radii)
            if (desc->postDecode) desc->postDecode(comp, reflect::DecodeContext{.registry = scene, .entity = entity});

            DecodedScene loaded;
            std::string error;
            ASSERT_TRUE(roundTrip(scene, {entity}, loaded, error)) << desc->cppName << " sample " << sample << ": " << error;
            ASSERT_TRUE(loaded.scene->scene->isEntityCreated(entity)) << desc->cppName;
            EXPECT_EQ(encodeOf(*desc, loaded.scene->scene, entity), encodeOf(*desc, scene, entity))
                << desc->cppName << " sample " << sample;
            checkPlaybackState(*desc, scene, loaded.scene->scene, entity);
            checked++;
        }
    }
    EXPECT_GT(checked, 0u);
}

TEST(CookedScene, RoundTripAllComponentsFixture) {
    const fs::path fixture = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "reflection-parity" / "AllComponents.scene";
    DecodedScene source = decodeSceneFile(fixture);
    Scene* scene = source.scene->scene;
    DecodedScene loaded;
    std::string error;
    ASSERT_TRUE(roundTrip(scene, source.scene->entities, loaded, error)) << error;
    std::set<ComponentType> covered;
    for (Entity entity : source.scene->entities) {
        ASSERT_TRUE(loaded.scene->scene->isEntityCreated(entity)) << "entity " << entity;
        EXPECT_EQ(loaded.scene->scene->getEntityName(entity), scene->getEntityName(entity));
        for (ComponentType type : Catalog::findComponents(scene, entity)) {
            const reflect::ComponentDesc* desc = reflect::find(type);
            ASSERT_NE(desc, nullptr);
            EXPECT_EQ(encodeOf(*desc, loaded.scene->scene, entity), encodeOf(*desc, scene, entity)) << desc->cppName;
            covered.insert(type);
        }
    }
    EXPECT_EQ(covered.size(), static_cast<size_t>(LAST_COMPONENT_TYPE) + 1) << "the fixture misses a component type";
}

// a spline's curve is read through SplineData's editable accessors (S18 cppPath fields): the
// knots and settings arrive, and the load bumps the version so the cache and consumers rebuild
TEST(CookedScene, SplineCurveThroughAccessors) {
    DecodedScene source = emptyScene();
    Scene* scene = source.scene->scene;
    const Entity entity = scene->createEntity();
    ProjectUtils::addEntityComponent(scene, entity, ComponentType::SplineComponent, source.scene->entities, YAML::Node());
    SplineComponent& spline = scene->getComponent<SplineComponent>(entity);
    spline.spline.editableType() = spline::SplineType::CubicBezier;
    spline.spline.editableClosed() = true;
    spline.spline.editableUpAxis() = Vector3(0.0f, 0.0f, 1.0f);
    spline.spline.editableKnots() = {{Vector3(0.0f, 0.0f, 0.0f)}, {Vector3(10.0f, 1.0f, 5.0f), Vector3(-2.0f, 0.0f, 0.0f), Vector3(2.0f, 0.0f, 0.0f), 4.5f, 12.0f},
                                     {Vector3(3.0f, 0.0f, -4.0f)}};
    spline.spline.markDirty();

    DecodedScene loaded;
    std::string error;
    ASSERT_TRUE(roundTrip(scene, {entity}, loaded, error)) << error;
    const SplineComponent& after = loaded.scene->scene->getComponent<SplineComponent>(entity);
    EXPECT_EQ(after.spline.type(), spline::SplineType::CubicBezier);
    EXPECT_TRUE(after.spline.closed());
    ASSERT_EQ(after.spline.knots().size(), 3u);
    EXPECT_EQ(after.spline.knots()[1].position, Vector3(10.0f, 1.0f, 5.0f));
    EXPECT_EQ(after.spline.knots()[1].tangentOut, Vector3(2.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(after.spline.knots()[1].radius, 4.5f);
    EXPECT_FLOAT_EQ(after.spline.knots()[1].rollDeg, 12.0f);
    EXPECT_EQ(after.spline.upAxis(), Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_GT(after.spline.version(), SplineComponent().spline.version()) << "the load must mark the curve dirty";
}

// The default sky of a new 3D scene is six images the editor holds in memory (the Compile path's
// generated code embeds them); a prebuilt player only reads files, so its cooked Skybox names the
// files the build ships (SceneCooker::defaultSkyFiles), or the player has a cube texture with no
// face ("Texture is missing texture for face 0" and no sky: the upgrade test's prebuilt player of
// an origin/main 3D scene)
TEST(CookedScene, DefaultSkyShipsItsFaces) {
    DecodedScene source = emptyScene();
    Scene* scene = source.scene->scene;
    const Entity entity = scene->createEntity();
    ASSERT_NE(entity, NULL_ENTITY);
    ProjectUtils::addEntityComponent(scene, entity, ComponentType::SkyboxComponent, source.scene->entities, YAML::Node());
    ProjectUtils::setDefaultSkyTexture(scene->getComponent<SkyboxComponent>(entity).texture);

    DecodedScene loaded;
    std::string error;
    ASSERT_TRUE(roundTrip(scene, {entity}, loaded, error)) << error;
    const Texture& texture = loaded.scene->scene->getComponent<SkyboxComponent>(entity).texture;
    ASSERT_EQ(texture.getNumFaces(), 6u);
    std::set<std::string> shipped;
    for (const editor::cook::CookedFileBytes& file : editor::cook::SceneCooker::defaultSkyFiles()) {
        ASSERT_GT(file.bytes.size(), 8u) << file.relativePath;
        EXPECT_EQ(std::string(file.bytes.begin(), file.bytes.begin() + 4), "\x89PNG") << file.relativePath;
        shipped.insert(file.relativePath);
    }
    for (size_t f = 0; f < 6; f++) {
        const std::string path = texture.getPath(f);
        ASSERT_FALSE(path.empty()) << "face " << f << " names no file";
        EXPECT_TRUE(shipped.count("assets/" + path)) << path << " is not shipped";
    }
}

// A body's colliders keep what their scene codec saves beside the reflected fields (the hooks of
// ComponentHooks.cpp): a 2D polygon's vertex count (a player's vertex array holds its maximum, so
// with no count a polygon fell back to the box of pointA and pointB: the upgrade test's prebuilt
// player let its balls fall through the ground), a 2D collider's collision filter, and a 3D
// body's allowed degrees of freedom and a mesh collider's indices
TEST(CookedScene, RigidBodiesKeepTheirColliders) {
    const char* text = R"(formatVersion: 1
id: 4242
name: Bodies
scene:
  backgroundColor: [0, 0, 0, 1]
sceneType: scene_3d
mainCamera: 0
entities:
  - type: Entity
    entity: 1001
    name: Ground
    components:
      transform:
        position: [30, 20, 0]
      rigidbody2d:
        enabled: true
        type: static
        numColliders: 2
        colliders:
          - type: polygon
            pointA: [450, 20]
            pointB: [0, 0]
            friction: 0.4
            contactEvents: true
            categoryBits: 6
            maskBits: 3
            groupIndex: -2
            vertices: [[0, 0], [900, 0], [900, 40], [0, 40]]
          - type: chain
            loop: true
            categoryBits: 8
            maskBits: 65533
            groupIndex: 5
            vertices: [[0, 0], [10, 0], [10, 10], [0, 10], [-5, 5]]
  - type: Entity
    entity: 1002
    name: Rock
    components:
      transform:
        position: [0, 1, 0]
      rigidbody:
        enabled: true
        type: dynamic
        allowedDOFs:
          translationX: true
          translationY: true
          translationZ: false
          rotationX: false
          rotationY: true
          rotationZ: false
        numColliders: 1
        colliders:
          - type: mesh
            source: raw_mesh
            vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]]
            indices: [0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2]
)";
    DecodedScene source = decodeSceneText(text, "Bodies.scene");
    Scene* scene = source.scene->scene;
    ASSERT_EQ(source.scene->entities.size(), 2u);
    DecodedScene loaded;
    std::string error;
    ASSERT_TRUE(roundTrip(scene, source.scene->entities, loaded, error)) << error;
    for (ComponentType type : {ComponentType::Rigidbody2DComponent, ComponentType::RigidbodyComponent}) {
        const reflect::ComponentDesc* desc = reflect::find(type);
        ASSERT_NE(desc, nullptr);
        for (Entity entity : source.scene->entities) {
            if (!desc->find(scene, entity)) continue;
            EXPECT_EQ(encodeOf(*desc, loaded.scene->scene, entity), encodeOf(*desc, scene, entity)) << desc->cppName;
        }
    }
    const Collider2D& polygon = loaded.scene->scene->getComponent<Rigidbody2DComponent>(source.scene->entities[0]).colliders[0];
    EXPECT_EQ(polygon.points.size(), 4u);
    EXPECT_EQ(polygon.groupIndex, -2);
}

// S27: a Tween eased along its curve keeps the curve whole in a cooked scene (keys, tangent
// modes, wraps), and a curve cooked before the modes (keys only) reads as clamp, free tangents
TEST(CookedScene, TweenCurveKeepsModesAndWraps) {
    DecodedScene source = emptyScene();
    Scene* scene = source.scene->scene;
    const Entity entity = scene->createEntity();
    ProjectUtils::addEntityComponent(scene, entity, ComponentType::TweenComponent, source.scene->entities, YAML::Node());
    TweenComponent& tween = scene->getComponent<TweenComponent>(entity);
    tween.function = EaseType::CURVE;
    tween.curve = Curve({CurveKey{0.0f, 0.0f, 0.0f, 0.0f}, CurveKey{0.5f, 1.0f, 0.0f, INFINITY}, CurveKey{1.0f, 0.25f, 2.0f, 2.0f}});
    tween.curve.keys[0].setBothModes(CurveTangentMode::ClampedAuto);
    tween.curve.keys[1].setInMode(CurveTangentMode::Linear);
    tween.curve.keys[1].setOutMode(CurveTangentMode::Constant);
    tween.curve.keys[1].setBroken(true);
    tween.curve.preWrapMode = CurveWrapMode::PingPong;
    tween.curve.postWrapMode = CurveWrapMode::LoopWithOffset;

    DecodedScene loaded;
    std::string error;
    ASSERT_TRUE(roundTrip(scene, {entity}, loaded, error)) << error;
    const TweenComponent& after = loaded.scene->scene->getComponent<TweenComponent>(entity);
    EXPECT_EQ(after.function.getType(), EaseType::CURVE);
    EXPECT_EQ(after.curve, tween.curve);

    // the keys alone, as a cook before S27 wrote them
    jasperengine::cook::StringTable strings;
    jasperengine::cook::Writer w(strings, [](Entity e) { return static_cast<uint32_t>(e); });
    jasperengine::cook::Codec<std::vector<CurveKey>>::write(w, tween.curve.keys);
    const std::vector<std::string> names;
    jasperengine::cook::Reader r(std::span<const uint8_t>(w.data().data(), w.data().size()), names, [](uint32_t id) { return static_cast<Entity>(id); });
    Curve old;
    jasperengine::cook::Codec<Curve>::read(r, old);
    ASSERT_EQ(old.keys.size(), 3u);
    EXPECT_EQ(old.keys[1].value, 1.0f);
    EXPECT_EQ(old.keys[1].mode, 0u);
    EXPECT_EQ(old.postWrapMode, CurveWrapMode::Clamp);
}

// S43: a material's Palette Swap bindings survive a cook (a prebuilt player draws the swap)
TEST(CookedScene, MaterialKeepsPaletteBindings) {
    DecodedScene source = emptyScene();
    Scene* scene = source.scene->scene;
    const Entity entity = scene->createEntity();
    ProjectUtils::addEntityComponent(scene, entity, ComponentType::MeshComponent, source.scene->entities, YAML::Node());
    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    mesh.numSubmeshes = 1;
    pixelart::PaletteBinding binding;
    binding.source = AssetRef("Palettes/hero.palette", Guid::parse("0123456789abcdef0123456789abcdef"));
    binding.targets.push_back(pixelart::PaletteTarget::parse("Palettes/enemy.palette#Night"));
    binding.targets.push_back(pixelart::PaletteTarget::parse("Palettes/gray.palette"));
    binding.useVariants = false;
    mesh.submeshes[0].material.shaderPalettes.edit().push_back({"u_palette", binding});
    mesh.submeshes[0].material.shaderFeatures = {"PALETTE_SWAP"};

    DecodedScene loaded;
    std::string error;
    ASSERT_TRUE(roundTrip(scene, {entity}, loaded, error)) << error;
    const MeshComponent& after = loaded.scene->scene->getComponent<MeshComponent>(entity);
    ASSERT_GE(after.numSubmeshes, 1u);
    const pixelart::ShaderPalettes& palettes = after.submeshes[0].material.shaderPalettes.get();
    ASSERT_EQ(palettes.size(), 1u);
    EXPECT_EQ(palettes[0].first, "u_palette");
    EXPECT_EQ(palettes[0].second, binding);
    EXPECT_EQ(palettes[0].second.source.guid.str(), "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(after.submeshes[0].material, mesh.submeshes[0].material);
}

// S57: the scene's capsule shadows survive a cook (format 1.1), and S54's LOD cross-fade after
// them (1.2); a 1.0 record (without either) and a 1.1 record (without the cross-fade) leave what
// they lack at its defaults
TEST(CookedScene, SceneSettingsKeepCapsuleShadows) {
    DecodedScene source = emptyScene();
    Scene* scene = source.scene->scene;
    scene->setCapsuleShadowsEnabled(true);
    scene->setCapsuleShadowIntensity(0.6f);
    scene->setCapsuleShadowSoftness(1.25f);
    scene->setMeshLodCrossFade(false);
    scene->setMeshLodCrossFadeDuration(1.5f);

    jasperengine::cook::StringTable strings;
    jasperengine::cook::Writer w(strings, [](Entity e) { return static_cast<uint32_t>(e); });
    jasperengine::cook::writeSceneSettings(w, *scene);
    const std::vector<uint8_t>& bytes = w.data();
    auto entityOf = [](uint32_t id) { return static_cast<Entity>(id); };

    DecodedScene loaded = emptyScene();
    jasperengine::cook::Reader r(std::span<const uint8_t>(bytes.data(), bytes.size()), strings.strings(), entityOf);
    jasperengine::cook::readSceneSettings(r, *loaded.scene->scene);
    ASSERT_FALSE(r.failed()) << r.error();
    EXPECT_TRUE(loaded.scene->scene->isCapsuleShadowsEnabled());
    EXPECT_FLOAT_EQ(loaded.scene->scene->getCapsuleShadowIntensity(), 0.6f);
    EXPECT_FLOAT_EQ(loaded.scene->scene->getCapsuleShadowSoftness(), 1.25f);
    EXPECT_FALSE(loaded.scene->scene->isMeshLodCrossFade());
    EXPECT_FLOAT_EQ(loaded.scene->scene->getMeshLodCrossFadeDuration(), 1.5f);

    // the sizes of each format's appended values: a bool and two floats (1.1), a bool and a float (1.2)
    jasperengine::cook::StringTable none;
    jasperengine::cook::Writer v11(none, [](Entity e) { return static_cast<uint32_t>(e); });
    v11.boolean(true);
    v11.f32(0.0f);
    v11.f32(0.0f);
    jasperengine::cook::Writer v12(none, [](Entity e) { return static_cast<uint32_t>(e); });
    v12.boolean(true);
    v12.f32(0.0f);
    ASSERT_GT(bytes.size(), v11.data().size() + v12.data().size());

    // the 1.1 record: the same bytes without the cross-fade
    DecodedScene older = emptyScene();
    jasperengine::cook::Reader r11(std::span<const uint8_t>(bytes.data(), bytes.size() - v12.data().size()), strings.strings(), entityOf);
    jasperengine::cook::readSceneSettings(r11, *older.scene->scene);
    ASSERT_FALSE(r11.failed()) << r11.error();
    EXPECT_TRUE(older.scene->scene->isCapsuleShadowsEnabled());
    EXPECT_FLOAT_EQ(older.scene->scene->getCapsuleShadowSoftness(), 1.25f);
    EXPECT_TRUE(older.scene->scene->isMeshLodCrossFade());
    EXPECT_FLOAT_EQ(older.scene->scene->getMeshLodCrossFadeDuration(), 0.5f);

    // the 1.0 record: without either
    DecodedScene old = emptyScene();
    jasperengine::cook::Reader r10(std::span<const uint8_t>(bytes.data(), bytes.size() - v11.data().size() - v12.data().size()),
                                   strings.strings(), entityOf);
    jasperengine::cook::readSceneSettings(r10, *old.scene->scene);
    ASSERT_FALSE(r10.failed()) << r10.error();
    EXPECT_FALSE(old.scene->scene->isCapsuleShadowsEnabled());
    EXPECT_FLOAT_EQ(old.scene->scene->getCapsuleShadowIntensity(), 1.0f);
    EXPECT_FLOAT_EQ(old.scene->scene->getCapsuleShadowSoftness(), 0.25f);
    EXPECT_TRUE(old.scene->scene->isMeshLodCrossFade());
    EXPECT_FLOAT_EQ(old.scene->scene->getMeshLodCrossFadeDuration(), 0.5f);
}

TEST(RuntimeFields, Generated) {
    std::vector<std::string> errors;
    const std::string emitted = editor::cook::emitRuntimeFields(errors);
    for (const std::string& error : errors) {
        ADD_FAILURE() << error;
    }
    const fs::path committed = fs::path(JASPER_TEST_REPO_DIR) / "engine" / "core" / "serialization" / "RuntimeFields.gen.inc";
    std::ifstream in(committed, std::ios::binary);
    ASSERT_TRUE(in) << committed;
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_TRUE(text.str() == emitted) << "engine/core/serialization/RuntimeFields.gen.inc is stale: regenerate it with "
                                          "jasperengine-editor reflect --emit-runtime-fields engine/core/serialization/RuntimeFields.gen.inc";
}
