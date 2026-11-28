// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R1, R5-R7, R132: the Vertex Colors component in a cooked scene (the
// runtime fields the editor's tables generate, read back by the player's loader) and in a clone.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/VertexColorsComponent.h"
#include "registry/EntityCloner.h"
#include "serialization/RuntimeFields.h"
#include "vertexcolor/VertexColorTypes.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace component_VertexColorsComponentTest_cpp {

using namespace jasperengine;

namespace vertex_colors_component_test {

    VertexColorsComponent authored() {
        VertexColorsComponent colors;
        colors.enabled = false;
        colors.data = AssetRef("paint/Rock.vcolors");
        colors.format = vertexcolor::Format::RGBA16F;
        colors.initialValue = vertexcolor::InitialValue::Custom;
        colors.customInitial = Vector4(0.25f, 0.5f, 2.0f, 1.0f);
        colors.channelNames = {"Moss", "", "Wetness", "Snow"};
        return colors;
    }

}

using namespace vertex_colors_component_test;

TEST(VertexColorsCooked, RecordKeepsTheAuthoredFieldsAndNoRuntimeState) {
    const cook::ComponentTable* table = cook::findComponentTableByName("VertexColorsComponent");
    ASSERT_NE(table, nullptr) << "no runtime field table: RuntimeFields.gen.inc was not regenerated";
    EXPECT_STREQ(table->key, "vertexcolors");

    VertexColorsComponent written = authored();
    // runtime state a cook must not carry
    written.loaded = std::make_shared<vertexcolor::Data>();
    written.loadedPath = "paint/Rock.vcolors";
    written.revision = 7;
    written.boundRevision = 7;
    written.needUpdate = false;

    Scene scene;
    const Entity entity = scene.createEntity();
    cook::StringTable strings;
    cook::Writer writer(strings, [](Entity e) { return static_cast<uint32_t>(e); });
    cook::WriteContext writeContext;
    writeContext.registry = &scene;
    writeContext.entity = entity;
    cook::writeComponent(writer, *table, &written, writeContext);

    cook::Reader reader(std::span<const uint8_t>(writer.data().data(), writer.data().size()), strings.strings(),
                        [](uint32_t id) { return static_cast<Entity>(id); });
    cook::LoadContext loadContext;
    loadContext.registry = &scene;
    loadContext.scene = &scene;
    loadContext.entity = entity;
    ASSERT_TRUE(cook::readComponent(reader, loadContext));
    EXPECT_FALSE(reader.failed()) << reader.error();

    const VertexColorsComponent* read = scene.findComponent<VertexColorsComponent>(entity);
    ASSERT_NE(read, nullptr);
    EXPECT_FALSE(read->enabled);
    EXPECT_EQ(read->data.path, "paint/Rock.vcolors");
    EXPECT_EQ(read->format, vertexcolor::Format::RGBA16F);
    EXPECT_EQ(read->initialValue, vertexcolor::InitialValue::Custom);
    EXPECT_EQ(read->customInitial, Vector4(0.25f, 0.5f, 2.0f, 1.0f));
    EXPECT_EQ(read->channelNames, (std::array<std::string, 4>{"Moss", "", "Wetness", "Snow"}));
    EXPECT_EQ(read->loaded, nullptr);
    EXPECT_TRUE(read->loadedPath.empty());
    EXPECT_EQ(read->revision, 0u);
    EXPECT_TRUE(read->needUpdate);
}

TEST(VertexColorsCooked, DefaultsReadBackAsDefaults) {
    const cook::ComponentTable* table = cook::findComponentTableByName("VertexColorsComponent");
    ASSERT_NE(table, nullptr);
    Scene scene;
    const Entity entity = scene.createEntity();
    const VertexColorsComponent written;
    cook::StringTable strings;
    cook::Writer writer(strings, [](Entity e) { return static_cast<uint32_t>(e); });
    cook::WriteContext writeContext;
    writeContext.registry = &scene;
    writeContext.entity = entity;
    cook::writeComponent(writer, *table, &written, writeContext);
    cook::Reader reader(std::span<const uint8_t>(writer.data().data(), writer.data().size()), strings.strings(),
                        [](uint32_t id) { return static_cast<Entity>(id); });
    cook::LoadContext loadContext;
    loadContext.registry = &scene;
    loadContext.scene = &scene;
    loadContext.entity = entity;
    ASSERT_TRUE(cook::readComponent(reader, loadContext));
    const VertexColorsComponent* read = scene.findComponent<VertexColorsComponent>(entity);
    ASSERT_NE(read, nullptr);
    EXPECT_TRUE(read->enabled);
    EXPECT_TRUE(read->data.path.empty());
    EXPECT_EQ(read->format, vertexcolor::Format::RGBA8);
    EXPECT_EQ(read->initialValue, vertexcolor::InitialValue::Asset);
    EXPECT_EQ(read->customInitial, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    for (const std::string& name : read->channelNames) EXPECT_TRUE(name.empty());
}

// R132: a copy shares the .vcolors asset (and its decoded values) until an edit; what it paints in
// a game and what it bound on the GPU are its own
TEST(VertexColorsClone, CopySharesTheAssetNotTheRuntimeLayers) {
    Scene scene;
    const Entity original = scene.createEntity();
    scene.addComponent<Transform>(original, {});
    scene.addComponent<MeshComponent>(original, {});
    VertexColorsComponent colors = authored();
    colors.loaded = std::make_shared<vertexcolor::Data>();
    colors.loadedPath = "paint/Rock.vcolors";
    colors.revision = 3;
    colors.boundRevision = 3;
    colors.needUpdate = false;
    colors.cpuGeometryLogged = true;
    colors.runtimeLayers[vertexcolor::TargetKey{0, 0, -1}] = nullptr;
    scene.addComponent<VertexColorsComponent>(original, colors);

    const std::vector<Entity> copies = EntityCloner::clone(&scene, EntityCloner::collectHierarchy(&scene, original), &scene);
    ASSERT_EQ(copies.size(), 1u);
    const VertexColorsComponent* copy = scene.findComponent<VertexColorsComponent>(copies[0]);
    ASSERT_NE(copy, nullptr) << "the clone left the component behind";
    const VertexColorsComponent& source = scene.getComponent<VertexColorsComponent>(original);
    EXPECT_FALSE(copy->enabled);
    EXPECT_EQ(copy->data.path, "paint/Rock.vcolors");
    EXPECT_EQ(copy->format, vertexcolor::Format::RGBA16F);
    EXPECT_EQ(copy->initialValue, vertexcolor::InitialValue::Custom);
    EXPECT_EQ(copy->customInitial, Vector4(0.25f, 0.5f, 2.0f, 1.0f));
    EXPECT_EQ(copy->channelNames[2], "Wetness");
    EXPECT_EQ(copy->loaded, source.loaded) << "the decoded values are shared until either copy is edited";
    EXPECT_EQ(copy->revision, 3u);
    EXPECT_EQ(copy->boundRevision, 0u);
    EXPECT_TRUE(copy->needUpdate);
    EXPECT_FALSE(copy->cpuGeometryLogged);
    EXPECT_TRUE(copy->runtimeLayers.empty());
    EXPECT_EQ(source.runtimeLayers.size(), 1u) << "the original keeps its gameplay paint";
}

}  // namespace component_VertexColorsComponentTest_cpp
