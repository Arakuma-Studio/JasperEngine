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
#include "cook/RuntimeFieldsEmitter.h"
#include "cook/SceneCooker.h"
#include "reflect/Reflect.h"
#include "serialization/CookedScene.h"
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
        static const std::map<std::string, std::set<std::string>> keys = {};
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
        const std::vector<uint8_t> bytes = cook::SceneCooker::cookScene(from, 4242, entities, NULL_ENTITY, SceneResources{});
        to = emptyScene();
        return CookedSceneLoader::loadScene(to.scene->scene, std::span<const uint8_t>(bytes.data(), bytes.size()), &error);
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

            DecodedScene loaded;
            std::string error;
            ASSERT_TRUE(roundTrip(scene, {entity}, loaded, error)) << desc->cppName << " sample " << sample << ": " << error;
            ASSERT_TRUE(loaded.scene->scene->isEntityCreated(entity)) << desc->cppName;
            EXPECT_EQ(encodeOf(*desc, loaded.scene->scene, entity), encodeOf(*desc, scene, entity))
                << desc->cppName << " sample " << sample;
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

TEST(RuntimeFields, Generated) {
    std::vector<std::string> errors;
    const std::string emitted = cook::emitRuntimeFields(errors);
    for (const std::string& error : errors) ADD_FAILURE() << error;
    const fs::path committed = fs::path(JASPER_TEST_REPO_DIR) / "engine" / "core" / "serialization" / "RuntimeFields.gen.inc";
    std::ifstream in(committed, std::ios::binary);
    ASSERT_TRUE(in) << committed;
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_TRUE(text.str() == emitted) << "engine/core/serialization/RuntimeFields.gen.inc is stale: regenerate it with "
                                          "jasperengine-editor reflect --emit-runtime-fields engine/core/serialization/RuntimeFields.gen.inc";
}
