// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S14 R2 and R3: what a texture's references say it is.

#include <gtest/gtest.h>

#include "Catalog.h"
#include "texture/TextureTypeInference.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    DepEdge inferenceEdge(const std::string& field, DepEdgeKind kind = DepEdgeKind::SceneComponent){
        DepEdge edge;
        edge.referrer = Guid::parse("0123456789abcdef0123456789abcdef");
        edge.kind = kind;
        edge.field = field;
        edge.targetPath = "assets/t.png";
        return edge;
    }

    std::vector<TextureSlot> inferenceSlots(const std::string& field, const TextureTypeInference::EdgeContext& context = {},
                                            DepEdgeKind kind = DepEdgeKind::SceneComponent){
        return TextureTypeInference::classify(inferenceEdge(field, kind), context);
    }

    bool inferenceHas(const std::vector<TextureSlot>& slots, TextureSlot slot){
        return std::find(slots.begin(), slots.end(), slot) != slots.end();
    }

}

TEST(TextureTypeInference, MaterialSlots){
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:4/MeshComponent.submeshes[0].material.normalTexture"), TextureSlot::Normal));
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:4/MeshComponent.submeshes[1].material.occlusionTexture"), TextureSlot::Occlusion));
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:4/MeshComponent.submeshes[0].material.metallicRoughnessTexture"), TextureSlot::LinearData));
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:4/MeshComponent.submeshes[0].material.baseColorTexture"), TextureSlot::Color));
    // a .material file
    EXPECT_TRUE(inferenceHas(inferenceSlots("material/normalTexture", {}, DepEdgeKind::MaterialTexture), TextureSlot::Normal));
}

TEST(TextureTypeInference, ComponentSlots){
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:2/UIComponent.texture"), TextureSlot::UI));
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:2/PointsComponent.texture"), TextureSlot::Sprite));
    // R3's CPU readers
    const std::vector<TextureSlot> height = inferenceSlots("entity:2/TerrainComponent.heightMap");
    EXPECT_TRUE(inferenceHas(height, TextureSlot::CpuReadable));
    EXPECT_TRUE(inferenceHas(height, TextureSlot::LinearData));
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:2/TerrainComponent.foliageLayers[3].densityMap"), TextureSlot::CpuReadable));
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:2/LightComponent.spotMask"), TextureSlot::CpuReadable));
    EXPECT_TRUE(inferenceHas(inferenceSlots("settings/postProcessing.lutTexture", {}, DepEdgeKind::SceneSettings), TextureSlot::CpuReadable));
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:2/TerrainComponent.blendMaps[0]"), TextureSlot::LinearData));
    // terrain layers are read from their source file
    const std::vector<TextureSlot> layer = inferenceSlots("entity:2/TerrainComponent.surfaceLayers[0].normalTexture");
    EXPECT_TRUE(inferenceHas(layer, TextureSlot::Normal));
    EXPECT_TRUE(inferenceHas(layer, TextureSlot::SourceFile));
    // HTML images are read from their file; a glTF's external images load cooked (MeshSystem)
    const std::vector<TextureSlot> html = inferenceSlots("text:12", {}, DepEdgeKind::UIImage);
    EXPECT_TRUE(inferenceHas(html, TextureSlot::UI));
    EXPECT_TRUE(inferenceHas(html, TextureSlot::SourceFile));
    TextureTypeInference::EdgeContext model;
    model.modelSlots = [](const DepEdge&){ return std::vector<TextureSlot>{TextureSlot::Normal}; };
    const std::vector<TextureSlot> gltf = inferenceSlots("text:3", model, DepEdgeKind::ModelUri);
    EXPECT_TRUE(inferenceHas(gltf, TextureSlot::Normal));
    EXPECT_FALSE(inferenceHas(gltf, TextureSlot::SourceFile));
    // no material samples it: colour
    const std::vector<TextureSlot> unused = inferenceSlots("text:3", {}, DepEdgeKind::ModelUri);
    EXPECT_EQ(unused, std::vector<TextureSlot>{TextureSlot::Color});
}

TEST(TextureTypeInference, SpritesFromTheirEntity){
    TextureTypeInference::EdgeContext context;
    const std::string sprite = Catalog::getComponentName(ComponentType::SpriteComponent, true);
    context.componentsOf = [sprite](const Guid&, uint64_t entity){
        return entity == 7 ? std::set<std::string>{"Transform", "MeshComponent", sprite} : std::set<std::string>{"MeshComponent"};
    };
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:7/MeshComponent.submeshes[0].material.baseColorTexture", context), TextureSlot::Sprite));
    EXPECT_FALSE(inferenceHas(inferenceSlots("entity:8/MeshComponent.submeshes[0].material.baseColorTexture", context), TextureSlot::Sprite));
}

// S50: a .tileset's image imports as a sprite sheet does (no mips; point filtering in Pixel Art
// Mode); another file's "Other" edge stays a colour texture
TEST(TextureTypeInference, TilesetSourcesAreSprites){
    TextureTypeInference::EdgeContext context;
    context.referrerPath = [](const Guid&){ return std::string("assets/maps/overworld.tileset"); };
    const std::vector<TextureSlot> tileset = inferenceSlots("sources[0]", context, DepEdgeKind::Other);
    EXPECT_TRUE(inferenceHas(tileset, TextureSlot::Sprite));
    EXPECT_FALSE(inferenceHas(tileset, TextureSlot::Color));
    context.referrerPath = [](const Guid&){ return std::string("assets/fx/spark.particles"); };
    EXPECT_FALSE(inferenceHas(inferenceSlots("renderer.texture", context, DepEdgeKind::Other), TextureSlot::Sprite));
}

TEST(TextureTypeInference, ShaderManifestTextures){
    TextureTypeInference::EdgeContext context;
    context.shaderTextureKind = [](const DepEdge&, const std::string& uniform, bool& cube){
        cube = uniform == "u_env";
        return uniform == "u_detailNormal" ? TextureSlot::Normal : TextureSlot::Color;
    };
    EXPECT_TRUE(inferenceHas(inferenceSlots("entity:1/MeshComponent.submeshes[0].material.shaderTextures{u_detailNormal}", context), TextureSlot::Normal));
    const std::vector<TextureSlot> env = inferenceSlots("entity:1/SkyboxComponent.shaderTextures{u_env}", context);
    EXPECT_TRUE(inferenceHas(env, TextureSlot::Cube));
}

TEST(TextureTypeInference, TypeFromUses){
    using U = TextureReferenceUse;
    TextureInference r = TextureTypeInference::fromUses({}, "textures/rock.png");
    EXPECT_EQ(r.type, TextureImportType::Default);
    EXPECT_EQ(r.reason, "no references");

    r = TextureTypeInference::fromUses({U{TextureSlot::Color, "a"}, U{TextureSlot::Normal, "b"}}, "n.png");
    EXPECT_EQ(r.type, TextureImportType::Normal);
    EXPECT_TRUE(r.linear);
    EXPECT_NE(r.reason.find("b"), std::string::npos);

    EXPECT_EQ(TextureTypeInference::fromUses({U{TextureSlot::Sprite, "s"}}, "s.png").type, TextureImportType::Sprite);
    EXPECT_EQ(TextureTypeInference::fromUses({U{TextureSlot::UI, "u"}}, "u.png").type, TextureImportType::UI);
    EXPECT_EQ(TextureTypeInference::fromUses({U{TextureSlot::Occlusion, "o"}}, "o.png").type, TextureImportType::SingleChannel);
    EXPECT_EQ(TextureTypeInference::fromUses({U{TextureSlot::Normal, "n"}}, "sky.HDR").type, TextureImportType::HDR);
    EXPECT_EQ(TextureTypeInference::fromUses({}, "env.exr").type, TextureImportType::HDR);

    r = TextureTypeInference::fromUses({U{TextureSlot::LinearData, "m"}}, "m.png");
    EXPECT_EQ(r.type, TextureImportType::Default);
    EXPECT_TRUE(r.linear);

    r = TextureTypeInference::fromUses({U{TextureSlot::LinearData, "h"}, U{TextureSlot::CpuReadable, "h"}}, "height.png");
    EXPECT_TRUE(r.cpuReadable);
    EXPECT_EQ(r.cpuReason, "h");

    r = TextureTypeInference::fromUses({U{TextureSlot::Color, "sky"}, U{TextureSlot::Cube, "sky"}}, "sky.png");
    EXPECT_EQ(r.shape, TextureShape::Cube);

    // read from the source by every reference: nothing to cook
    r = TextureTypeInference::fromUses({U{TextureSlot::UI, "page.html"}, U{TextureSlot::SourceFile, "page.html"}}, "logo.png");
    EXPECT_TRUE(r.sourceOnly);
    EXPECT_TRUE(r.readsSource);
    r = TextureTypeInference::fromUses({U{TextureSlot::UI, "page.html"}, U{TextureSlot::SourceFile, "page.html"}, U{TextureSlot::Color, "mesh"}},
                                       "logo.png");
    EXPECT_FALSE(r.sourceOnly);
    EXPECT_TRUE(r.readsSource);
}

TEST(TextureTypeInference, ConflictsAreWarnings){
    using U = TextureReferenceUse;
    const TextureInference uses = TextureTypeInference::fromUses({U{TextureSlot::Color, "albedo slot"}, U{TextureSlot::Normal, "normal slot"}}, "x.png");
    std::vector<std::string> conflicts = TextureTypeInference::conflicts(TextureImportType::Normal, uses);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_NE(conflicts[0].find("colour slot"), std::string::npos);
    EXPECT_NE(conflicts[0].find("albedo slot"), std::string::npos);
    conflicts = TextureTypeInference::conflicts(TextureImportType::Default, uses);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_NE(conflicts[0].find("normal slot"), std::string::npos);
    EXPECT_TRUE(TextureTypeInference::conflicts(TextureImportType::Default,
                                                TextureTypeInference::fromUses({U{TextureSlot::Color, "a"}}, "a.png")).empty());
}
