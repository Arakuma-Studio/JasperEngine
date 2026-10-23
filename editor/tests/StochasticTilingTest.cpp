// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Stochastic Tiling of the built-in Lit material: the variant bits (MESH 5, DEPTH 14, GBUFFER
// 10, 'Sto'), where RenderSystem::getSubmeshShaderVariant sets them, the stale MESH bit 5 ('Pcf',
// retired July 2026) of keys files hold from before (editor::Stream), and the material's
// scene encoding.

#include <gtest/gtest.h>

#include "Catalog.h"
#include "Stream.h"
#include "component/MeshComponent.h"
#include "pool/ShaderPool.h"
#include "registry/EntityRegistry.h"
#include "subsystem/MeshSystem.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"

#include "yaml-cpp/yaml.h"

#include <ostream>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using editor::Stream;
using editor::PropertyData;
using editor::PropertyType;
using editor::ComponentType;

namespace jasperengine {
    void PrintTo(const ShaderKey& key, std::ostream* os);
}

namespace {

    const uint64_t meshSto = shaderPropertyBit(5);
    const uint64_t depthSto = shaderPropertyBit(14);
    const uint64_t gbufferSto = shaderPropertyBit(10);

    std::string emit(const YAML::Node& node) {
        YAML::Emitter out;
        out << node;
        return out.c_str();
    }

    // a textured mesh with UVs and normals, its material asking for Stochastic Tiling
    SubmeshShaderInputs texturedInputs() {
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.baseColorTexture = true;
        inputs.stochasticTiling = true;
        return inputs;
    }

    MeshShaderEnvironment litEnvironment() {
        MeshShaderEnvironment environment;
        environment.lights = true;
        environment.shadows = true;
        environment.ssr = true;
        return environment;
    }

}

// ---- (a) the names and bits

TEST(StochasticTilingKey, BitsAreNamedSto) {
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 5, true), "Sto");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 5, false), "Stochastic Tiling");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::DEPTH, 14, true), "Sto");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::DEPTH, 14, false), "Stochastic Tiling");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 10, true), "Sto");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 10, false), "Stochastic Tiling");
    EXPECT_EQ(ShaderPool::getShaderPropertyCount(ShaderType::DEPTH), 15);
    EXPECT_EQ(ShaderPool::getShaderPropertyCount(ShaderType::GBUFFER), 11);
    EXPECT_NE(ShaderPool::getValidPropertyMask(ShaderType::MESH) & meshSto, 0u);
    EXPECT_NE(ShaderPool::getValidPropertyMask(ShaderType::DEPTH) & depthSto, 0u);
    EXPECT_NE(ShaderPool::getValidPropertyMask(ShaderType::GBUFFER) & gbufferSto, 0u);
}

TEST(StochasticTilingKey, NoOtherNameClashesWithSto) {
    // shader names are the short names concatenated in bit order and read back greedily
    // (ShaderPool::getShaderCliSpec): no other name of the type may be a prefix of "Sto" or
    // start with it
    for (ShaderType type : {ShaderType::MESH, ShaderType::DEPTH, ShaderType::GBUFFER}) {
        const int stoBit = type == ShaderType::MESH ? 5 : (type == ShaderType::DEPTH ? 14 : 10);
        for (int bit = 0; bit < ShaderPool::getShaderPropertyCount(type); bit++) {
            if (bit == stoBit) continue;
            const std::string name = ShaderPool::getShaderPropertyName(type, bit, true);
            EXPECT_NE(name.rfind("Sto", 0), 0u) << name;
            EXPECT_NE(std::string("Sto").rfind(name, 0), 0u) << name;
            EXPECT_NE(ShaderPool::getShaderPropertyName(type, bit, false), "Stochastic Tiling");
        }
    }
}

TEST(StochasticTilingKey, NamesAndSpecsRoundTrip) {
    struct Case { ShaderType type; uint64_t properties; const char* name; };
    const Case cases[] = {
        {ShaderType::MESH, meshSto, "mesh_Sto"},
        {ShaderType::MESH, shaderPropertyBit(1) | shaderPropertyBit(3) | shaderPropertyBit(4) | meshSto | shaderPropertyBit(6), "mesh_Uv1PucShwStoNor"},
        {ShaderType::MESH, shaderPropertyBit(0) | shaderPropertyBit(1) | meshSto | shaderPropertyBit(24), "mesh_UltUv1StoAms"},
        {ShaderType::DEPTH, shaderPropertyBit(0) | shaderPropertyBit(7) | depthSto, "depth_TexAmsSto"},
        {ShaderType::DEPTH, shaderPropertyBit(0) | shaderPropertyBit(13) | depthSto, "depth_TexTxrSto"},
        {ShaderType::GBUFFER, shaderPropertyBit(0) | shaderPropertyBit(8) | gbufferSto, "gbuffer_BctMrtSto"},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(ShaderPool::getShaderStr(c.type, c.properties), c.name);
        ShaderKey parsed;
        ASSERT_TRUE(ShaderPool::parseShaderStr(c.name, parsed)) << c.name;
        EXPECT_EQ(parsed, ShaderPool::getShaderKey(c.type, c.properties)) << c.name;
    }
    ShaderKey key;
    ASSERT_TRUE(ShaderPool::parseShaderSpec("mesh:Uv1,Sto,Nor", key));
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), shaderPropertyBit(1) | meshSto | shaderPropertyBit(6));
    ASSERT_TRUE(ShaderPool::parseShaderSpec("mesh:Stochastic Tiling", key));
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), meshSto);
    ASSERT_TRUE(ShaderPool::parseShaderSpec("depth:Tex,Ams,Sto", key));
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), shaderPropertyBit(0) | shaderPropertyBit(7) | depthSto);
    ASSERT_TRUE(ShaderPool::parseShaderSpec("gbuffer:Bct,Sto", key));
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), shaderPropertyBit(0) | gbufferSto);
}

TEST(StochasticTilingKey, PropertyFunctionsSetTheBits) {
    EXPECT_EQ(ShaderPool::getMeshProperties(false, true, false, false, false, false, false, false, false, false, false, false,
                                            false, false, false, false, false, false, false, false, false, false, false, false,
                                            false, false, false, false, 8, true), shaderPropertyBit(1) | meshSto);
    EXPECT_EQ(ShaderPool::getDepthMeshProperties(true, false, false, false, false, false, false, true, false, false, false, false,
                                                 false, false, true), shaderPropertyBit(0) | shaderPropertyBit(7) | depthSto);
    EXPECT_EQ(ShaderPool::getGBufferMeshProperties(true, false, false, false, false, false, false, false, false, false, true),
              shaderPropertyBit(0) | gbufferSto);
    // off: the keys every variant had
    EXPECT_EQ(ShaderPool::getMeshProperties(false, true, false, false, false, false, false, false, false, false, false, false,
                                            false, false, false, false, false, false, false, false), shaderPropertyBit(1));
}

// ---- (b) where the variant takes it (RenderSystem::getSubmeshShaderVariant, also the export collector's)

TEST(StochasticTilingVariant, LitColourDepthAndGBuffer) {
    SubmeshShaderInputs inputs = texturedInputs();
    inputs.metallicRoughnessTexture = true;
    inputs.alphaMode = MaterialAlphaMode::MASK;
    const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(litEnvironment(), inputs);
    EXPECT_NE(variant.properties & meshSto, 0u);
    ASSERT_TRUE(variant.depth);
    // the alpha clip reads the base colour as the colour pass does
    EXPECT_NE(variant.depthProperties & shaderPropertyBit(0), 0u);
    EXPECT_NE(variant.depthProperties & depthSto, 0u);
    ASSERT_TRUE(variant.gbuffer);
    EXPECT_NE(variant.gbufferProperties & gbufferSto, 0u);

    // off: none of them, and the keys it had
    inputs.stochasticTiling = false;
    const SubmeshShaderVariant off = RenderSystem::getSubmeshShaderVariant(litEnvironment(), inputs);
    EXPECT_EQ(off.properties, variant.properties & ~meshSto);
    EXPECT_EQ(off.depthProperties, variant.depthProperties & ~depthSto);
    EXPECT_EQ(off.gbufferProperties, variant.gbufferProperties & ~gbufferSto);
}

TEST(StochasticTilingVariant, OpaqueDepthHasNoTextureToClip) {
    const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(litEnvironment(), texturedInputs());
    EXPECT_NE(variant.properties & meshSto, 0u);
    ASSERT_TRUE(variant.depth);
    EXPECT_EQ(variant.depthProperties & depthSto, 0u);
}

TEST(StochasticTilingVariant, UnlitVertexLightsInstancingSkinning) {
    // unlit (no lights): the base colour
    MeshShaderEnvironment unlit;
    EXPECT_NE(RenderSystem::getSubmeshShaderVariant(unlit, texturedInputs()).properties & meshSto, 0u);
    // vertex lights, instancing and skinning keep it
    SubmeshShaderInputs inputs = texturedInputs();
    inputs.lighting = MaterialLighting::VERTEX;
    inputs.instanced = true;
    inputs.skinning = true;
    const uint64_t properties = RenderSystem::getSubmeshShaderVariant(litEnvironment(), inputs).properties;
    EXPECT_NE(properties & meshSto, 0u);
    EXPECT_NE(properties & shaderPropertyBit(28), 0u);
    // 2D lights read the normal map: an unlit sprite-less mesh with only a normal map takes it
    MeshShaderEnvironment lights2D;
    lights2D.lights2D = true;
    SubmeshShaderInputs normalOnly;
    normalOnly.texCoord1 = true;
    normalOnly.normal = true;
    normalOnly.tangent = true;
    normalOnly.normalTexture = true;
    normalOnly.stochasticTiling = true;
    EXPECT_NE(RenderSystem::getSubmeshShaderVariant(lights2D, normalOnly).properties & meshSto, 0u);
    // without 2D lights an unlit variant reads no normal map: nothing to tile
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(unlit, normalOnly).properties & meshSto, 0u);
}

TEST(StochasticTilingVariant, LeftOffWhereItDoesNotApply) {
    const MeshShaderEnvironment environment = litEnvironment();
    // no texture at all
    SubmeshShaderInputs inputs = texturedInputs();
    inputs.baseColorTexture = false;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment, inputs).properties & meshSto, 0u);
    // no UVs
    inputs = texturedInputs();
    inputs.texCoord1 = false;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment, inputs).properties & meshSto, 0u);
    // a texture rect (sprites, tilemaps): the offset taps would read the neighbouring frames
    inputs = texturedInputs();
    inputs.textureRect = true;
    inputs.alphaMode = MaterialAlphaMode::MASK;
    SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
    EXPECT_EQ(variant.properties & meshSto, 0u);
    EXPECT_EQ(variant.depthProperties & depthSto, 0u);
    EXPECT_EQ(variant.gbufferProperties & gbufferSto, 0u);
    // a terrain: its layers have their own
    inputs = texturedInputs();
    inputs.terrain = true;
    inputs.terrainBlendMaps = true;
    variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
    EXPECT_EQ(variant.properties & meshSto, 0u);
    EXPECT_EQ(variant.gbufferProperties & gbufferSto, 0u);
    // a shader with a manifest (a library shader: its STOCHASTIC feature)
    inputs = texturedInputs();
    inputs.manifest = ShaderManifests::get("library:simple_lit");
    if (inputs.manifest) {
        variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
        EXPECT_EQ(variant.properties & meshSto, 0u);
        EXPECT_EQ(variant.gbufferProperties & gbufferSto, 0u);
    }
}

// ---- (b2) forks of Lit: only one that names the values (LIT_STOCHASTIC_PARAMS) takes it, in
// every pass, so the depth clip and the G-buffer never differ from the colour it draws

TEST(StochasticTilingFork, DefineIsTheSignal) {
    EXPECT_TRUE(ShaderManifest::definesLitStochasticParams("#define LIT_STOCHASTIC_PARAMS pbrParams.stochastic\n"));
    EXPECT_TRUE(ShaderManifest::definesLitStochasticParams(
        "uniform u_fs_pbrParams { vec4 stochastic; } pbrParams;\n#ifdef USE_STOCHASTIC_TILING\n    #  define   LIT_STOCHASTIC_PARAMS pbrParams.stochastic // values\n#endif\n"));
    EXPECT_TRUE(ShaderManifest::definesLitStochasticParams("\t#define LIT_STOCHASTIC_PARAMS(x) x\r\n"));
    // in comments, another name, or only used
    EXPECT_FALSE(ShaderManifest::definesLitStochasticParams("//   #define LIT_STOCHASTIC_PARAMS pbrParams.stochastic\n"));
    EXPECT_FALSE(ShaderManifest::definesLitStochasticParams("/* old:\n#define LIT_STOCHASTIC_PARAMS pbrParams.stochastic\n*/\nvoid main(){}\n"));
    EXPECT_FALSE(ShaderManifest::definesLitStochasticParams("#define LIT_STOCHASTIC_PARAMS_OLD 1\n"));
    EXPECT_FALSE(ShaderManifest::definesLitStochasticParams("#defineLIT_STOCHASTIC_PARAMS 1\n"));
    EXPECT_FALSE(ShaderManifest::definesLitStochasticParams("#ifdef LIT_STOCHASTIC_PARAMS\n#endif\n"));
    EXPECT_FALSE(ShaderManifest::definesLitStochasticParams(""));
}

TEST(StochasticTilingFork, NoPassTakesItWhenTheColourShaderDoesNot) {
    SubmeshShaderInputs inputs = texturedInputs();
    inputs.metallicRoughnessTexture = true;
    inputs.alphaMode = MaterialAlphaMode::MASK;
    const SubmeshShaderVariant supported = RenderSystem::getSubmeshShaderVariant(litEnvironment(), inputs);
    ASSERT_NE(supported.properties & meshSto, 0u);
    ASSERT_NE(supported.depthProperties & depthSto, 0u);
    ASSERT_NE(supported.gbufferProperties & gbufferSto, 0u);

    inputs.stochasticShader = false;
    const SubmeshShaderVariant old = RenderSystem::getSubmeshShaderVariant(litEnvironment(), inputs);
    EXPECT_EQ(old.properties, supported.properties & ~meshSto);
    ASSERT_TRUE(old.depth);
    EXPECT_EQ(old.depthProperties, supported.depthProperties & ~depthSto);
    ASSERT_TRUE(old.gbuffer);
    EXPECT_EQ(old.gbufferProperties, supported.gbufferProperties & ~gbufferSto);
    // nor without lights, nor on the 2D light path
    MeshShaderEnvironment lights2D;
    lights2D.lights2D = true;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(MeshShaderEnvironment(), inputs).properties & meshSto, 0u);
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(lights2D, inputs).properties & meshSto, 0u);
}

TEST(StochasticTilingFork, InputsReadTheForkSource) {
    // the forks as the project (editor) or the asset root (game) holds them
    const std::string oldFrag =
        "#version 450\n"
        "uniform u_fs_pbrParams {\n    vec4 baseColorFactor;\n} pbrParams;\n"
        "// Lit's Stochastic Tiling: #define LIT_STOCHASTIC_PARAMS is not here\n"
        "#include \"includes/pbr.glsl\"\nvoid main(){}\n";
    const std::string newFrag =
        "#version 450\n"
        "uniform u_fs_pbrParams {\n    vec4 baseColorFactor;\n    #ifdef USE_STOCHASTIC_TILING\n        vec4 stochastic;\n    #endif\n} pbrParams;\n"
        "#ifdef USE_STOCHASTIC_TILING\n    #define LIT_STOCHASTIC_PARAMS pbrParams.stochastic\n#endif\n"
        "#include \"includes/pbr.glsl\"\nvoid main(){}\n";
    ShaderManifests::setForkReader([&](const std::string& path, std::string& content) {
        if (path == "shaders/OldLit.frag") { content = oldFrag; return true; }
        if (path == "shaders/NewLit.frag") { content = newFrag; return true; }
        if (path == "shaders/OldLit.vert" || path == "shaders/NewLit.vert") { content = "#version 450\nvoid main(){}\n"; return true; }
        return false;
    });

    EXPECT_TRUE(RenderSystem::supportsStochasticTiling(""));
    EXPECT_FALSE(RenderSystem::supportsStochasticTiling("shaders/OldLit"));
    EXPECT_TRUE(RenderSystem::supportsStochasticTiling("shaders/NewLit"));
    EXPECT_TRUE(RenderSystem::supportsStochasticTiling("shaders/NewLit#A"));
    // (a fork whose files cannot be read does not either: ShaderManifests::get logs it, which needs the
    // platform System these tests run without)
    EXPECT_FALSE(RenderSystem::supportsStochasticTiling("library:simple_lit"));

    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    Submesh& submesh = mesh.submeshes[0];
    submesh.hasTexCoord1 = true;
    submesh.hasNormal = true;
    submesh.material.baseColorTexture = Texture("leaves.png");
    submesh.material.alphaMode = MaterialAlphaMode::MASK;
    submesh.material.stochasticTiling = true;

    struct Case { const char* meshShader; const char* materialShader; const char* sceneShader; bool takesIt; };
    const Case cases[] = {
        {"", "", "", true},                          // the built-in Lit
        {"shaders/OldLit", "", "", false},           // the mesh's fork, made before
        {"shaders/NewLit", "", "", true},            // one that names the values
        {"", "shaders/OldLit", "", false},           // the material's
        {"shaders/NewLit", "shaders/OldLit", "", false},
        {"", "", "shaders/OldLit", false},           // the scene's default mesh shader
        {"", "", "shaders/NewLit", true},
    };
    for (const Case& c : cases) {
        mesh.customShader = c.meshShader;
        submesh.material.shader = c.materialShader;
        const SubmeshShaderInputs inputs = RenderSystem::getSubmeshShaderInputs(mesh, submesh, nullptr, nullptr, false, c.sceneShader);
        EXPECT_EQ(inputs.stochasticShader, c.takesIt) << c.meshShader << " | " << c.materialShader << " | " << c.sceneShader;
        const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(litEnvironment(), inputs);
        // all three agree
        EXPECT_EQ((variant.properties & meshSto) != 0, c.takesIt) << c.meshShader << " | " << c.materialShader;
        EXPECT_EQ((variant.depthProperties & depthSto) != 0, c.takesIt) << c.meshShader << " | " << c.materialShader;
        EXPECT_EQ((variant.gbufferProperties & gbufferSto) != 0, c.takesIt) << c.meshShader << " | " << c.materialShader;
    }

    // an extra pass drawing with the old fork does not take it either
    MaterialPass pass;
    pass.shader = "shaders/OldLit";
    mesh.customShader.clear();
    submesh.material.shader.clear();
    const SubmeshShaderInputs inputs = RenderSystem::getSubmeshShaderInputs(mesh, submesh, nullptr, nullptr, false, "");
    EXPECT_FALSE(RenderSystem::getExtraPassShaderInputs(inputs, pass).stochasticShader);
    pass.shader = "shaders/NewLit";
    EXPECT_TRUE(RenderSystem::getExtraPassShaderInputs(inputs, pass).stochasticShader);

    // back to the game's assets reader (forgets these forks)
    ShaderManifests::setForkReader(nullptr);
}

// ---- (c) keys in files: the stale MESH bit 5

TEST(StochasticTilingStaleKeys, OldFilesLoseBitFive) {
    // what an editor of before October 2026 wrote: no keyVersion. Bit 5 there is the retired 'Pcf'
    const std::vector<ShaderKey> decoded = Stream::decodeShaderKeys(YAML::Load(
        "- type: mesh\n"
        "  properties: 97\n"            // 0x61: Ult, Pcf, Nor
        "- type: mesh\n"
        "  properties: 32\n"            // Pcf alone
        "- type: depth\n"
        "  properties: 33\n"            // DEPTH bit 5 is Terrain: kept
        "- type: gbuffer\n"
        "  properties: 1056\n"));       // GBUFFER 5 (Mtg) and 10 (Sto): kept
    ASSERT_EQ(decoded.size(), 4u);
    EXPECT_EQ(decoded[0], ShaderPool::getShaderKey(ShaderType::MESH, 0x41));
    EXPECT_EQ(decoded[1], ShaderPool::getShaderKey(ShaderType::MESH, 0));
    EXPECT_EQ(decoded[2], ShaderPool::getShaderKey(ShaderType::DEPTH, 33));
    EXPECT_EQ(decoded[3], ShaderPool::getShaderKey(ShaderType::GBUFFER, 1056));

    // the legacy packed form too (always from before)
    ShaderKey key;
    ASSERT_TRUE(Stream::decodeShaderKey(YAML::Load("0x200000061"), key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::MESH, 0x41));
}

TEST(StochasticTilingStaleKeys, StochasticKeysCarryTheirVersion) {
    const ShaderKey sto = ShaderPool::getShaderKey(ShaderType::MESH, 0x61);
    EXPECT_EQ(emit(Stream::encodeShaderKey(sto)), "type: mesh\nproperties: 97\nkeyVersion: 2");
    ShaderKey key;
    ASSERT_TRUE(Stream::decodeShaderKey(YAML::Load(emit(Stream::encodeShaderKey(sto))), key));
    EXPECT_EQ(key, sto);

    // a fork's
    const uint16_t forkId = ShaderPool::registerCustomShader("shaders/stoFork");
    const ShaderKey forked = ShaderPool::getShaderKey(ShaderType::MESH, meshSto | 0x2, forkId);
    EXPECT_EQ(emit(Stream::encodeShaderKey(forked)), "type: mesh\nproperties: 34\ncustomShader: shaders/stoFork\nkeyVersion: 2");
    ASSERT_TRUE(Stream::decodeShaderKey(YAML::Load(emit(Stream::encodeShaderKey(forked))), key));
    EXPECT_EQ(key, forked);

    // only MESH keys with bit 5 are marked: the others are written as before
    EXPECT_EQ(emit(Stream::encodeShaderKey(ShaderPool::getShaderKey(ShaderType::MESH, 0x41))), "type: mesh\nproperties: 65");
    EXPECT_EQ(emit(Stream::encodeShaderKey(ShaderPool::getShaderKey(ShaderType::DEPTH, 33 | depthSto))), "type: depth\nproperties: 16417");
    EXPECT_EQ(emit(Stream::encodeShaderKey(ShaderPool::getShaderKey(ShaderType::GBUFFER, 1 | gbufferSto))), "type: gbuffer\nproperties: 1025");
}

TEST(StochasticTilingStaleKeys, ListsRoundTripAndOldListsReSaveWithoutTheStaleBit) {
    const std::set<ShaderKey> keys = {
        ShaderPool::getShaderKey(ShaderType::MESH, 0x41),
        ShaderPool::getShaderKey(ShaderType::MESH, 0x61),
        ShaderPool::getShaderKey(ShaderType::DEPTH, shaderPropertyBit(0) | shaderPropertyBit(7) | depthSto),
        ShaderPool::getShaderKey(ShaderType::GBUFFER, 1 | gbufferSto)};
    const std::string text = emit(Stream::encodeShaderKeys(keys));
    EXPECT_EQ(text,
              "- type: mesh\n  properties: 65\n"
              "- type: mesh\n  properties: 97\n  keyVersion: 2\n"
              "- type: depth\n  properties: 16513\n"
              "- type: gbuffer\n  properties: 1025");
    const std::vector<ShaderKey> decoded = Stream::decodeShaderKeys(YAML::Load(text));
    EXPECT_EQ(std::set<ShaderKey>(decoded.begin(), decoded.end()), keys);

    // an old scene's list with a stale bit: read without it, so the next save drops it (and the
    // key it duplicates is one entry)
    const std::vector<ShaderKey> old = Stream::decodeShaderKeys(YAML::Load(
        "- type: mesh\n  properties: 65\n- type: mesh\n  properties: 97"));
    const std::set<ShaderKey> oldKeys(old.begin(), old.end());
    EXPECT_EQ(emit(Stream::encodeShaderKeys(oldKeys)), "- type: mesh\n  properties: 65");
}

// ---- (d) the material in scenes, prefabs and .material files

TEST(StochasticTilingMaterial, DefaultsAreNotWritten) {
    const Material material = Material();
    const YAML::Node node = Stream::encodeMaterial(material);
    for (const char* key : {"stochasticTiling", "stochasticCellSize", "stochasticBlendSharpness", "stochasticContrast", "stochasticRotation"}) {
        EXPECT_FALSE(node[key].IsDefined()) << key;
    }
    // and a node without them reads the defaults, whatever it is decoded over
    Material over;
    over.stochasticTiling = true;
    over.stochasticCellSize = 2.0f;
    over.stochasticRotation = 0.5f;
    const Material decoded = Stream::decodeMaterial(node, &over);
    EXPECT_FALSE(decoded.stochasticTiling);
    EXPECT_EQ(decoded.stochasticCellSize, 1.0f);
    EXPECT_EQ(decoded.stochasticBlendSharpness, 0.4f);
    EXPECT_EQ(decoded.stochasticContrast, 0.45f);
    EXPECT_EQ(decoded.stochasticRotation, 0.0f);
}

TEST(StochasticTilingMaterial, ValuesRoundTrip) {
    Material material;
    material.stochasticTiling = true;
    material.stochasticCellSize = 0.5f;
    material.stochasticBlendSharpness = 0.7f;
    material.stochasticContrast = 0.2f;
    material.stochasticRotation = 0.25f;
    const YAML::Node node = Stream::encodeMaterial(material);
    EXPECT_EQ(node["stochasticTiling"].as<bool>(), true);
    const Material decoded = Stream::decodeMaterial(YAML::Load(emit(node)));
    EXPECT_EQ(decoded, material);

    // a value set while it is off is kept too
    Material off;
    off.stochasticContrast = 0.9f;
    const YAML::Node offNode = Stream::encodeMaterial(off);
    EXPECT_FALSE(offNode["stochasticTiling"].IsDefined());
    EXPECT_EQ(Stream::decodeMaterial(YAML::Load(emit(offNode))), off);
}

TEST(StochasticTilingMaterial, ShaderPrefixLayout) {
    // u_fs_pbrParams is a prefix of Material: the four values are floats 12-15 (the vec4 the
    // 'Sto' variants declare last)
    Material material;
    material.stochasticCellSize = 2.0f;
    material.stochasticBlendSharpness = 0.3f;
    material.stochasticContrast = 0.6f;
    material.stochasticRotation = 0.1f;
    const float* prefix = reinterpret_cast<const float*>(&material);
    EXPECT_EQ(prefix[0], material.baseColorFactor.x);
    EXPECT_EQ(prefix[4], material.metallicFactor);
    EXPECT_EQ(prefix[6], material.alphaCutoff);
    EXPECT_EQ(prefix[11], material.foliageTransmission);
    EXPECT_EQ(prefix[12], 2.0f);
    EXPECT_EQ(prefix[13], 0.3f);
    EXPECT_EQ(prefix[14], 0.6f);
    EXPECT_EQ(prefix[15], 0.1f);
}

// ---- (e) editing: Catalog paths, model overrides, what a change reloads

TEST(StochasticTilingMaterial, CatalogPathsAndOverrides) {
    EntityRegistry registry;
    const Entity entity = registry.createEntity();
    MeshComponent mesh;
    mesh.numSubmeshes = 2;
    registry.addComponent<MeshComponent>(entity, mesh);

    const struct { const char* field; PropertyType type; uint64_t flags; } fields[] = {
        {"stochasticTiling", PropertyType::Bool, editor::UpdateFlags_Mesh_Reload},
        {"stochasticCellSize", PropertyType::Float, editor::UpdateFlags_None},
        {"stochasticBlendSharpness", PropertyType::Float, editor::UpdateFlags_None},
        {"stochasticContrast", PropertyType::Float, editor::UpdateFlags_None},
        {"stochasticRotation", PropertyType::Float, editor::UpdateFlags_None}};
    MeshComponent& stored = registry.getComponent<MeshComponent>(entity);
    for (const auto& field : fields) {
        const std::string path = std::string("submeshes[1].material.") + field.field;
        const PropertyData data = editor::Catalog::findProperty(&registry, entity, ComponentType::MeshComponent, path);
        ASSERT_NE(data.ref, nullptr) << path;
        EXPECT_EQ(data.type, field.type) << path;
        EXPECT_EQ(data.updateFlags, field.flags) << path;
        // a model reload keeps the edit: the one override bit of the five
        uint32_t overrideFields = 0;
        uint32_t* mask = editor::Catalog::getSubmeshOverrideMask(&registry, entity, ComponentType::MeshComponent, path, overrideFields);
        ASSERT_NE(mask, nullptr) << path;
        EXPECT_EQ(mask, &stored.submeshes[1].overrideFields) << path;
        EXPECT_EQ(overrideFields, (uint32_t)SubmeshOverride_StochasticTiling) << path;
    }
    EXPECT_EQ(editor::Catalog::findProperty(&registry, entity, ComponentType::MeshComponent, "submeshes[1].material.stochasticTilings").ref, nullptr);
    EXPECT_NE(SubmeshOverride_Material & SubmeshOverride_StochasticTiling, 0u);
    EXPECT_EQ(SubmeshOverride_ShaderLibrary & SubmeshOverride_StochasticTiling, 0u);
}

TEST(StochasticTilingMaterial, ModelOverridesRestoreAllFive) {
    Submesh saved;
    saved.material.stochasticTiling = true;
    saved.material.stochasticCellSize = 2.5f;
    saved.material.stochasticBlendSharpness = 0.1f;
    saved.material.stochasticContrast = 0.9f;
    saved.material.stochasticRotation = 0.75f;
    saved.material.roughnessFactor = 0.2f;
    saved.overrideFields = SubmeshOverride_StochasticTiling;
    Submesh loaded;  // as the model file gives it
    MeshSystem::applySubmeshOverride(saved, loaded);
    EXPECT_TRUE(loaded.material.stochasticTiling);
    EXPECT_EQ(loaded.material.stochasticCellSize, 2.5f);
    EXPECT_EQ(loaded.material.stochasticBlendSharpness, 0.1f);
    EXPECT_EQ(loaded.material.stochasticContrast, 0.9f);
    EXPECT_EQ(loaded.material.stochasticRotation, 0.75f);
    EXPECT_EQ(loaded.material.roughnessFactor, 1.0f) << "only the flagged fields";

    Submesh untouched;
    saved.overrideFields = SubmeshOverride_RoughnessFactor;
    MeshSystem::applySubmeshOverride(saved, untouched);
    EXPECT_FALSE(untouched.material.stochasticTiling);
    EXPECT_EQ(untouched.material.stochasticCellSize, 1.0f);
}

TEST(StochasticTilingMaterial, OnlyTheSwitchReloads) {
    Material from;
    Material to = from;
    to.stochasticTiling = true;
    EXPECT_NE(editor::Catalog::getMaterialUpdateFlags(from, to) & editor::UpdateFlags_Mesh_Reload, 0u);
    to = from;
    to.stochasticCellSize = 3.0f;
    to.stochasticRotation = 0.5f;
    EXPECT_EQ(editor::Catalog::getMaterialUpdateFlags(from, to) & editor::UpdateFlags_Mesh_Reload, 0u);
}
