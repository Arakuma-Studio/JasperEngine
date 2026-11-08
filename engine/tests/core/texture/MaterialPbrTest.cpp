// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Material::pbr, the extended lobes held out of line (docs/specs/S11-materials-lighting.md,
// section 6.1), and the variant a material's lobes select (getPbrVariant). CPU only.

#include "texture/Material.h"
#include "texture/MaterialPbr.h"
#include "component/TerrainComponent.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // Material as it was before the extended lobes, field for field: what SizeBudget measures
    // the growth against on every compiler
    struct PreLobesMaterial{
        Vector4 baseColorFactor;
        float metallicFactor;
        float roughnessFactor;
        float alphaCutoff;
        uint8_t _pad_28[4];
        Vector3 emissiveFactor;
        float foliageTransmission;
        // upstream's Stochastic Tiling (before S11 on the upstream sync)
        float stochasticCellSize;
        float stochasticBlendSharpness;
        float stochasticContrast;
        float stochasticRotation;
        Texture baseColorTexture;
        Texture emissiveTexture;
        Texture metallicRoughnessTexture;
        Texture occlusionTexture;
        Texture normalTexture;
        MaterialAlphaMode alphaMode;
        MaterialLighting lighting;
        bool stochasticTiling;
        int baseColorTexCoord;
        int metallicRoughnessTexCoord;
        int occlusionTexCoord;
        int emissiveTexCoord;
        int normalTexCoord;
        AssetRef name;
        ShaderRef shader;
        ShaderUniformValues shaderParams;
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        std::vector<std::string> shaderFeatures;
        // upstream's shader globals (before S11's lobes on SYNC#4)
        ShaderGlobalBindings shaderGlobalBindings;
        MaterialBlendMode blendMode;
        MaterialCullMode cullMode;
        MaterialDepthWrite depthWrite;
        MaterialDepthTest depthTest;
        float depthOffsetFactor;
        float depthOffsetUnits;
        int sortPriority;
        MaterialStencil stencil;
        int colorWrite;
        MaterialExtraPass extraPass;
    };

    bool hasDefine(const PbrVariant& variant, const std::string& define){
        return std::find(variant.defines.begin(), variant.defines.end(), define) != variant.defines.end();
    }

    Texture namedTexture(const std::string& path){
        Texture texture;
        texture.setPath(path);
        return texture;
    }

    // every lobe enabled, every map set
    Material allLobesMaterial(){
        Material material;
        MaterialPbrExtensions& pbr = material.pbr.edit();
        pbr.clearcoat.enabled = true;
        pbr.clearcoat.map = namedTexture("cc.png");
        pbr.clearcoat.normalMap = namedTexture("ccn.png");
        pbr.sheen.enabled = true;
        pbr.sheen.map = namedTexture("sheen.png");
        pbr.anisotropy.enabled = true;
        pbr.anisotropy.map = namedTexture("aniso.png");
        pbr.iridescence.enabled = true;
        pbr.iridescence.map = namedTexture("irid.png");
        pbr.transmission.enabled = true;
        pbr.transmission.map = namedTexture("trans.png");
        pbr.volume.enabled = true;
        pbr.specular.enabled = true;
        pbr.specular.map = namedTexture("spec.png");
        return material;
    }

}

TEST(MaterialPbr, SizeBudget){
    // the pointer, emissiveStrength and their padding: no more than 16 bytes in each of the 16
    // materials inline in a MeshComponent
    EXPECT_LE(sizeof(Material), sizeof(PreLobesMaterial) + 16);
    EXPECT_EQ(sizeof(OutOfLineValue<MaterialPbrExtensions>), sizeof(void*));
}

TEST(MaterialPbr, OutOfLineSemantics){
    Material material;
    EXPECT_FALSE(material.pbr.isSet());
    // an unset value reads the defaults without allocating
    EXPECT_FALSE(material.pbr->clearcoat.enabled);
    EXPECT_EQ(material.pbr->specular.ior, 1.5f);
    EXPECT_FALSE(material.pbr.isSet());
    EXPECT_TRUE(material.pbr == MaterialPbrExtensions());

    // edit allocates once, then keeps its storage
    MaterialPbrExtensions& edited = material.pbr.edit();
    EXPECT_TRUE(material.pbr.isSet());
    edited.clearcoat.enabled = true;
    edited.clearcoat.factor = 0.75f;
    EXPECT_EQ(&material.pbr.edit(), &edited);

    // a copy is deep and compares by value
    Material copy = material;
    EXPECT_TRUE(copy.pbr.isSet());
    EXPECT_NE(&copy.pbr.get(), &material.pbr.get());
    EXPECT_TRUE(copy == material);
    copy.pbr.edit().clearcoat.factor = 0.5f;
    EXPECT_FALSE(copy == material);
    EXPECT_EQ(material.pbr->clearcoat.factor, 0.75f);

    // a copy of a value at its defaults stays unset, and still equals an unset one
    Material allocatedDefault;
    allocatedDefault.pbr.edit();
    Material copyOfDefault = allocatedDefault;
    EXPECT_FALSE(copyOfDefault.pbr.isSet());
    EXPECT_TRUE(copyOfDefault == Material());
    EXPECT_TRUE(allocatedDefault == Material());

    // assigning a value keeps the storage (a Lua reference to it stays valid)
    MaterialPbrExtensions values;
    values.sheen.enabled = true;
    material.pbr = values;
    EXPECT_EQ(&material.pbr.get(), &edited);
    EXPECT_TRUE(material.pbr->sheen.enabled);
    EXPECT_FALSE(material.pbr->clearcoat.enabled);

    // emissiveStrength is part of the value
    Material bright;
    bright.emissiveStrength = 4.0f;
    EXPECT_FALSE(bright == Material());
}

TEST(MaterialPbr, DefaultHasNoReference){
    // a default material selects no define: the reference stays "" (customId 0, the variant it
    // always drew with), on every tier and lighting
    for (PbrTier tier : {PbrTier::FULL, PbrTier::REDUCED}){
        for (MaterialLighting lighting : {MaterialLighting::PIXEL, MaterialLighting::MIXED, MaterialLighting::VERTEX}){
            EXPECT_TRUE(MaterialPbr::getPbrVariant(Material(), lighting, false, tier).empty());
        }
    }
    // factors and maps without the enabled flag select nothing either
    Material disabled;
    disabled.pbr.edit().clearcoat.factor = 1.0f;
    disabled.pbr.edit().clearcoat.map = namedTexture("cc.png");
    disabled.pbr.edit().specular.ior = 1.33f;
    EXPECT_TRUE(MaterialPbr::getPbrVariant(disabled, MaterialLighting::PIXEL, false, PbrTier::FULL).empty());
}

TEST(MaterialPbr, FieldTableCoversEveryLobe){
    for (const PbrLobeInfo& lobe : MaterialPbr::getLobes()){
        const PbrField* enabled = MaterialPbr::findField(std::string(lobe.name) + ".enabled");
        ASSERT_NE(enabled, nullptr) << lobe.name;
        MaterialPbrExtensions pbr;
        *(bool*)enabled->ref(pbr) = true;
        EXPECT_TRUE(MaterialPbr::isLobeEnabled(pbr, lobe.lobe)) << lobe.name;
    }
    EXPECT_EQ(MaterialPbr::getFieldLabel(*MaterialPbr::findField("clearcoat.roughness")), "Clear Coat Roughness");
    EXPECT_EQ(MaterialPbr::findLuaField("thicknessFactor"), MaterialPbr::findField("volume.thickness"));
    EXPECT_EQ(MaterialPbr::findField("clearcoat.nothing"), nullptr);
    EXPECT_EQ(MaterialPbr::findField("nothing.factor"), nullptr);
}

TEST(MaterialPbr, ClampToRanges){
    MaterialPbrExtensions pbr;
    pbr.clearcoat.factor = 2.0f;
    pbr.sheen.roughness = -1.0f;
    pbr.specular.ior = 0.5f;
    pbr.iridescence.thicknessNm = Vector2(NAN, 300.0f);
    pbr.transmission.mapTexCoord = 3;
    EXPECT_TRUE(MaterialPbr::clamp(pbr));
    EXPECT_EQ(pbr.clearcoat.factor, 1.0f);
    EXPECT_EQ(pbr.sheen.roughness, 0.0f);
    EXPECT_EQ(pbr.specular.ior, 1.0f);
    EXPECT_EQ(pbr.iridescence.thicknessNm.x, 100.0f); // NaN takes the default
    EXPECT_EQ(pbr.transmission.mapTexCoord, 1);

    // in range: untouched, glTF's IOR 0 and the infinite attenuation distance included
    MaterialPbrExtensions valid;
    valid.specular.ior = 0.0f;
    valid.volume.attenuationDistance = INFINITY;
    EXPECT_FALSE(MaterialPbr::clamp(valid));
    EXPECT_EQ(valid.specular.ior, 0.0f);
}

TEST(PbrVariant, BudgetDropsByPriority){
    const PbrVariant variant = MaterialPbr::getPbrVariant(allLobesMaterial(), MaterialLighting::PIXEL, false, PbrTier::FULL);
    // clearcoat > specular > transmission > sheen > anisotropy > iridescence > clearcoat normal
    ASSERT_EQ(variant.maps.size(), 3u);
    EXPECT_EQ(variant.maps[0], PbrMap::CLEARCOAT);
    EXPECT_EQ(variant.maps[1], PbrMap::SPECULAR);
    EXPECT_EQ(variant.maps[2], PbrMap::TRANSMISSION);
    EXPECT_EQ(variant.droppedMaps, (std::vector<std::string>{"sheen", "anisotropy", "iridescence", "clearcoatNormal"}));
    EXPECT_TRUE(variant.strippedMaps.empty());

    EXPECT_TRUE(hasDefine(variant, "PBR_CLEARCOAT_MAP"));
    EXPECT_TRUE(hasDefine(variant, "PBR_SPECULAR_MAP"));
    EXPECT_TRUE(hasDefine(variant, "PBR_TRANSMISSION_MAP"));
    EXPECT_FALSE(hasDefine(variant, "PBR_SHEEN_MAP"));
    EXPECT_FALSE(hasDefine(variant, "PBR_CLEARCOAT_NORMAL_MAP"));
    // the lobes themselves all stay
    for (const char* define : {"PBR_CLEARCOAT", "PBR_SHEEN", "PBR_ANISOTROPY", "PBR_IRIDESCENCE", "PBR_TRANSMISSION", "PBR_VOLUME", "PBR_SPECULAR"})
        EXPECT_TRUE(hasDefine(variant, define)) << define;
    // sorted, as a reference's suffix is
    EXPECT_TRUE(std::is_sorted(variant.defines.begin(), variant.defines.end()));
}

TEST(PbrVariant, VertexLightingStripsLobes){
    Material material = allLobesMaterial();
    material.pbr.edit().hex.enabled = true;
    const PbrVariant vertex = MaterialPbr::getPbrVariant(material, MaterialLighting::VERTEX, false, PbrTier::FULL);
    // hex tiling is texturing, no lobe: it stays
    EXPECT_EQ(vertex.defines, (std::vector<std::string>{"PBR_HEX_TILING"}));
    EXPECT_TRUE(vertex.maps.empty());
    EXPECT_TRUE(vertex.droppedMaps.empty());
    EXPECT_EQ(MaterialPbr::getPbrVariant(material, MaterialLighting::NONE, false, PbrTier::FULL).defines,
              (std::vector<std::string>{"PBR_HEX_TILING"}));
    // mixed lighting has pixel lights: the lobes stay
    EXPECT_TRUE(hasDefine(MaterialPbr::getPbrVariant(material, MaterialLighting::MIXED, false, PbrTier::FULL), "PBR_CLEARCOAT"));
    // a Terrain draws its layers: nothing of the material's
    EXPECT_TRUE(MaterialPbr::getPbrVariant(material, MaterialLighting::PIXEL, true, PbrTier::FULL).empty());
}

TEST(PbrVariant, VolumeNeedsTransmission){
    Material material;
    material.pbr.edit().volume.enabled = true;
    EXPECT_TRUE(MaterialPbr::getPbrVariant(material, MaterialLighting::PIXEL, false, PbrTier::FULL).empty());
    material.pbr.edit().transmission.enabled = true;
    EXPECT_EQ(MaterialPbr::getPbrVariant(material, MaterialLighting::PIXEL, false, PbrTier::FULL).defines,
              (std::vector<std::string>{"PBR_TRANSMISSION", "PBR_VOLUME"}));
}

TEST(PbrVariant, MobileReducedTier){
    const PbrVariant reduced = MaterialPbr::getPbrVariant(allLobesMaterial(), MaterialLighting::PIXEL, false, PbrTier::REDUCED);
    EXPECT_FALSE(hasDefine(reduced, "PBR_IRIDESCENCE"));
    EXPECT_FALSE(hasDefine(reduced, "PBR_IRIDESCENCE_MAP"));
    EXPECT_FALSE(hasDefine(reduced, "PBR_ANISOTROPY_MAP"));
    EXPECT_FALSE(hasDefine(reduced, "PBR_CLEARCOAT_NORMAL_MAP"));
    // the anisotropy lobe keeps its strength and rotation
    EXPECT_TRUE(hasDefine(reduced, "PBR_ANISOTROPY"));
    EXPECT_TRUE(hasDefine(reduced, "PBR_CLEARCOAT"));
    // the budget then drops what is left past three, stripped maps not counted
    EXPECT_EQ(reduced.maps, (std::vector<PbrMap>{PbrMap::CLEARCOAT, PbrMap::SPECULAR, PbrMap::TRANSMISSION}));
    EXPECT_EQ(reduced.droppedMaps, (std::vector<std::string>{"sheen"}));
    EXPECT_EQ(reduced.strippedMaps, (std::vector<std::string>{"iridescence lobe", "anisotropy", "clearcoatNormal"}));

    // the one rule for the runtime and the exporter: desktop is always FULL
    EXPECT_EQ(MaterialPbr::resolveTier(false, PbrTier::REDUCED), PbrTier::FULL);
    EXPECT_EQ(MaterialPbr::resolveTier(true, PbrTier::REDUCED), PbrTier::REDUCED);
    EXPECT_EQ(MaterialPbr::resolveTier(true, PbrTier::FULL), PbrTier::FULL);
}

TEST(PbrVariant, BasenameIsShort){
    const PbrVariant variant = MaterialPbr::getPbrVariant(allLobesMaterial(), MaterialLighting::PIXEL, false, PbrTier::FULL);
    std::vector<std::string> every = variant.defines;
    for (const char* define : {"PBR_HEX_TILING", "TERRAIN_HEX_TILING", "PBR_SHEEN_MAP", "PBR_ANISOTROPY_MAP",
                               "PBR_IRIDESCENCE_MAP", "PBR_CLEARCOAT_NORMAL_MAP"})
        every.push_back(define);
    const std::string basename = MaterialPbr::getVariantBasename(every);
    EXPECT_EQ(basename, "lit_CcCmCnShSmAnAmIrImTrTmVoSpSqHxTh");
    EXPECT_LE(basename.size(), 40u);
    EXPECT_EQ(MaterialPbr::getVariantBasename({"PBR_CLEARCOAT"}), "lit_Cc");
}

// A lobe's map samples the mesh's UVs as the core maps do (includes/pbr_ext_slots.glsl has no slot
// for it without them), whichever lobe and whatever else the material has; a lobe's map the variant
// leaves out (an unlit variant) asks for none (BF17)
TEST(PbrVariant, ALobesMapAsksForTheMeshsUVs){
    const uint64_t uv1 = 1u << 1, uv2 = 1u << 2;
    MeshShaderEnvironment lit;
    lit.lights = true;
    MeshShaderEnvironment unlit;
    Texture map;
    map.setId("bf17-lobe-map");

    for (int m = 0; m < (int)PbrMap::COUNT; m++){
        const PbrMap kind = (PbrMap)m;
        Material material;
        MaterialPbrExtensions& pbr = material.pbr.edit();
        // the map's own lobe, so a lobe added later (S57's Hair) is covered too
        MaterialPbr::lobeEnabled(pbr, MaterialPbr::getMapLobe(kind)) = true;
        MaterialPbr::getMap(pbr, kind) = map;

        SubmeshShaderInputs inputs;
        inputs.normal = true;
        inputs.receiveLights = true;
        inputs.material = &material;
        inputs.texCoord1 = true;
        const char* name = MaterialPbr::getMapName(kind);
        EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(lit, inputs).properties & (uv1 | uv2), uv1) << name;
        inputs.texCoord2 = true;
        EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(lit, inputs).properties & (uv1 | uv2), uv1 | uv2) << name;
        EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(unlit, inputs).properties & (uv1 | uv2), 0u) << name;
        // a mesh without UVs has none to give
        inputs.texCoord1 = inputs.texCoord2 = false;
        EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(lit, inputs).properties & (uv1 | uv2), 0u) << name;
    }

    // a lobe without a map keeps the program it always was
    Material factors;
    factors.pbr.edit().sheen.enabled = true;
    SubmeshShaderInputs inputs;
    inputs.normal = true;
    inputs.receiveLights = true;
    inputs.material = &factors;
    inputs.texCoord1 = true;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(lit, inputs).properties & (uv1 | uv2), 0u);
}

// A core map other than the base colour asks for the UVs under whatever lights the surface: a scene
// light, a sky or a reflection probe. A mesh the environment does not light (its receiveIBL) draws
// none of them, as a mesh that takes no lights.
TEST(PbrVariant, ACoreMapAsksForTheMeshsUVsUnderImageBasedLighting){
    const uint64_t uv1 = 1u << 1;
    MeshShaderEnvironment sky;
    sky.ibl = true;
    MeshShaderEnvironment probes;
    probes.reflectionProbes = true;
    const auto properties = [](const MeshShaderEnvironment& environment, bool SubmeshShaderInputs::*map, bool receiveIBL, bool receiveLights){
        SubmeshShaderInputs inputs;
        inputs.normal = true;
        inputs.texCoord1 = true;
        inputs.receiveLights = receiveLights;
        inputs.receiveIBL = receiveIBL;
        inputs.*map = true;
        return RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
    };
    for (bool SubmeshShaderInputs::*map : {&SubmeshShaderInputs::metallicRoughnessTexture, &SubmeshShaderInputs::normalTexture,
                                           &SubmeshShaderInputs::occlusionTexture, &SubmeshShaderInputs::emissiveTexture}){
        EXPECT_NE(properties(sky, map, true, true) & uv1, 0u);
        EXPECT_NE(properties(probes, map, true, true) & uv1, 0u);
        EXPECT_EQ(properties(sky, map, false, true) & uv1, 0u);
        EXPECT_EQ(properties(sky, map, true, false) & uv1, 0u);
    }
}

// A hex tiled Terrain's variant carries the Stochastic Tiling path only when a layer takes it
// (TERRAIN_HEX_STOCHASTIC): without one the path is compiled out of the hex variant, which
// otherwise holds the lattices' registers for nothing (docs/perf/gpu-campaign.md, S11 perf3)
TEST(PbrVariant, TerrainHexLeavesStochasticOut){
    MeshShaderEnvironment environment;
    environment.lights = true;
    SubmeshShaderInputs inputs;
    inputs.normal = true;
    inputs.baseColorTexture = true;
    inputs.terrain = true;
    inputs.terrainBlendMaps = true;
    inputs.terrainSurface = true;
    inputs.terrainHex = true;
    const auto has = [](const std::vector<std::string>& defines, const char* define){
        return std::find(defines.begin(), defines.end(), define) != defines.end();
    };
    const SubmeshShaderVariant hex = RenderSystem::getSubmeshShaderVariant(environment, inputs);
    EXPECT_TRUE(has(hex.litDefines, MaterialPbr::TERRAIN_HEX_DEFINE));
    EXPECT_FALSE(has(hex.litDefines, MaterialPbr::TERRAIN_HEX_STOCHASTIC_DEFINE));

    inputs.terrainHexStochastic = true;
    const SubmeshShaderVariant both = RenderSystem::getSubmeshShaderVariant(environment, inputs);
    EXPECT_TRUE(has(both.litDefines, MaterialPbr::TERRAIN_HEX_DEFINE));
    EXPECT_TRUE(has(both.litDefines, MaterialPbr::TERRAIN_HEX_STOCHASTIC_DEFINE));
    EXPECT_EQ(MaterialPbr::getVariantBasename(both.litDefines), "lit_ThTs");
    std::vector<std::string> parsed;
    ASSERT_TRUE(MaterialPbr::parseVariantBasename("lit_ThTs", parsed));
    EXPECT_EQ(parsed, both.litDefines);
    EXPECT_TRUE(MaterialPbr::isBuiltinLitDefine(MaterialPbr::TERRAIN_HEX_STOCHASTIC_DEFINE));

    // without a hex layer the define never comes (the plain surface path keeps Stochastic Tiling)
    inputs.terrainHex = false;
    EXPECT_FALSE(has(RenderSystem::getSubmeshShaderVariant(environment, inputs).litDefines, MaterialPbr::TERRAIN_HEX_STOCHASTIC_DEFINE));

    // what the render system asks the Terrain
    TerrainComponent terrain;
    terrain.surfaceLayers.resize(2);
    terrain.surfaceLayers[0].hexTiling = true;
    EXPECT_TRUE(hasTerrainHexLayers(terrain));
    EXPECT_FALSE(hasTerrainStochasticLayers(terrain));
    terrain.surfaceLayers[1].stochasticTiling = true;
    EXPECT_TRUE(hasTerrainStochasticLayers(terrain));
}

TEST(PbrVariant, UniformBlockLayout){
    MaterialPbrExtensions pbr;
    pbr.clearcoat.factor = 0.5f;
    pbr.anisotropy.rotation = 0.0f;
    pbr.volume.attenuationDistance = INFINITY;
    pbr.specular.factor = 0.25f;
    float block[MaterialPbr::UNIFORM_FLOATS];
    MaterialPbr::getUniformBlock(pbr, block);
    EXPECT_EQ(block[0], 0.5f);   // clearcoat.x
    EXPECT_EQ(block[9], 1.0f);   // anisotropy.y = cos(rotation)
    EXPECT_EQ(block[18], 0.0f);  // transmission.z: infinity as 0 (absorbs nothing)
    EXPECT_EQ(block[27], 0.25f); // specular.w
    EXPECT_EQ(block[28], 1.5f);  // iorHex.x without the Specular & IOR lobe
    pbr.specular.enabled = true;
    pbr.specular.ior = 1.33f;
    MaterialPbr::getUniformBlock(pbr, block);
    EXPECT_EQ(block[28], 1.33f);

    // the lobe's dielectric f0, made once a draw in the vec4s' free floats (includes/pbr_ext.glsl)
    const float ratio = (1.33f - 1.0f) / (1.33f + 1.0f);
    EXPECT_EQ(block[23], ratio * ratio);                        // attenuation.w: the IOR's term
    pbr.specular.color = Vector3(1.0f, 0.5f, 100.0f);
    MaterialPbr::getUniformBlock(pbr, block);
    EXPECT_EQ(block[3], ratio * ratio * 0.25f);                 // clearcoat.w: f0.r
    EXPECT_EQ(block[11], (ratio * ratio * 0.5f) * 0.25f);       // anisotropy.w: f0.g
    EXPECT_EQ(block[19], 0.25f);                                // transmission.w: f0.b, at most the factor
    // an IOR under 1 (glTF's 0): a full mirror
    pbr.specular.ior = 0.0f;
    MaterialPbr::getUniformBlock(pbr, block);
    EXPECT_EQ(block[23], 1.0f);
    EXPECT_EQ(block[3], 0.25f);
}

// the lit variants a project draws (get_material_variant's projectLitVariants): two submeshes with
// the same lobes whose variants differ past bit 31 (the LOD cross-fade's 'Lfd', bit 40) are two
TEST(MaterialPbr, CountLitVariantsSeesAll64Bits){
    const uint64_t lit = (1ull << 1) | (1ull << 3);
    EXPECT_EQ(MaterialPbr::countLitVariants({{1u, lit}, {1u, lit | (1ull << 40)}, {1u, lit}, {0u, lit}}), 2u);
}
