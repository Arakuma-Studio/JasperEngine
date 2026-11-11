// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S78 (docs/specs/S78-sprite-normals-improvements.md): the variants of Light Steps and of the 2D
// ambient without a Light 2D, as RenderSystem::getSubmeshShaderVariant derives them for loadMesh
// and the export's collector alike. Light Steps add 'Lst' (MESH bit 55) only to a 2D-lit variant
// and change nothing while off (R3); a tinting 2D ambient with no Light 2D gives the unlit
// variants the 2D light path, without its normal map, and leaves the lit path and a neutral
// ambient as they were (R6).

#include "Scene.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {

    const uint64_t kUnlit = shaderPropertyBit(0);
    const uint64_t kNormalMap = shaderPropertyBit(7);
    const uint64_t kLight2D = shaderPropertyBit(22);

    // a Sprite of the built-in shader with a normal map: uv1, colour, its rect, normals and tangents
    SubmeshShaderInputs spriteInputs(){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.vertexColor4 = true;
        inputs.textureRect = true;
        inputs.baseColorTexture = true;
        inputs.normalTexture = true;
        return inputs;
    }

    uint64_t propertiesOf(const MeshShaderEnvironment& environment, const SubmeshShaderInputs& inputs){
        return RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
    }

}

TEST(Light2DSteps, SceneClampsTheCount){
    Scene scene;
    EXPECT_EQ(scene.getLight2DSteps(), 0) << "off by default";
    scene.setLight2DSteps(6);
    EXPECT_EQ(scene.getLight2DSteps(), 6);
    scene.setLight2DSteps(-3);
    EXPECT_EQ(scene.getLight2DSteps(), 0);
    scene.setLight2DSteps(40);
    EXPECT_EQ(scene.getLight2DSteps(), 16);
}

TEST(Light2DSteps, PropertyIsMeshBit54NamedLst){
    EXPECT_EQ(ShaderPool::getLight2DStepsProperty(), shaderPropertyBit(55));
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 55, true), "Lst");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 55, false), "Light Steps");
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::MESH, kLight2D | shaderPropertyBit(55)), "mesh_L2dLst");
}

// R3: 'Lst' only on a variant with 'L2d'; off, every variant is the one of before
TEST(Light2DSteps, VariantTakesLstOnlyWithTheLight2DPath){
    const uint64_t lst = ShaderPool::getLight2DStepsProperty();
    const SubmeshShaderInputs sprite = spriteInputs();

    MeshShaderEnvironment lights2D;
    lights2D.lights2D = true;
    MeshShaderEnvironment stepped = lights2D;
    stepped.light2DSteps = true;
    const uint64_t smooth = propertiesOf(lights2D, sprite);
    ASSERT_NE(smooth & kLight2D, 0u);
    EXPECT_EQ(smooth & lst, 0u) << "steps off";
    EXPECT_EQ(propertiesOf(stepped, sprite), smooth | lst) << "steps on: the same variant with 'Lst'";

    // no 2D light path: steps change nothing
    MeshShaderEnvironment none;
    none.light2DSteps = true;
    EXPECT_EQ(propertiesOf(none, sprite), propertiesOf(MeshShaderEnvironment(), sprite));
    SubmeshShaderInputs unlitByChoice = sprite;
    unlitByChoice.receiveLights = false;
    EXPECT_EQ(propertiesOf(stepped, unlitByChoice) & (kLight2D | lst), 0u) << "not receiving lights";

    // a lit mesh in a 2D scene (3D lights and 2D lights): its 2D term is stepped too
    SubmeshShaderInputs lit = sprite;
    lit.textureRect = false;
    lit.vertexColor4 = false;
    MeshShaderEnvironment mixed = stepped;
    mixed.lights = true;
    const uint64_t litStepped = propertiesOf(mixed, lit);
    EXPECT_EQ(litStepped & kUnlit, 0u);
    EXPECT_NE(litStepped & kLight2D, 0u);
    EXPECT_NE(litStepped & lst, 0u);

    // library:sprite takes it as the built-in shader does
    SubmeshShaderInputs library = sprite;
    library.manifest = ShaderManifests::get("library:sprite");
    ASSERT_TRUE(library.manifest);
    library.builtinLit = false;
    EXPECT_NE(propertiesOf(stepped, library) & lst, 0u);
    EXPECT_EQ(propertiesOf(lights2D, library) & lst, 0u);
}

// a stale key's 'Lst' without 'L2d' goes where keys are read, and a build missing the stepped
// variant draws the smooth one rather than nothing
TEST(Light2DSteps, KeysKeepLstOnlyWithL2d){
    const uint64_t lst = ShaderPool::getLight2DStepsProperty();
    EXPECT_EQ(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, kUnlit | lst)),
              ShaderPool::getShaderKey(ShaderType::MESH, kUnlit));
    EXPECT_EQ(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, kUnlit | kLight2D | lst)),
              ShaderPool::getShaderKey(ShaderType::MESH, kUnlit | kLight2D | lst));
    const std::vector<uint64_t> fallbacks = ShaderPool::getFallbackProperties(ShaderType::MESH, kUnlit | kLight2D | lst);
    ASSERT_FALSE(fallbacks.empty());
    EXPECT_EQ(fallbacks.front(), kUnlit | kLight2D) << "the stepped variant gives up its steps first";
}

TEST(Light2DAmbient, TintsAwayFromWhiteOnly){
    EXPECT_FALSE(RenderSystem::isAmbient2DTinted(Vector3(1.0f, 1.0f, 1.0f), 1.0f)) << "the default";
    EXPECT_FALSE(RenderSystem::isAmbient2DTinted(Vector3(1.0f, 1.0f, 1.0f), 1.00005f)) << "within 1e-4";
    EXPECT_FALSE(RenderSystem::isAmbient2DTinted(Vector3(2.0f, 2.0f, 2.0f), 0.5f)) << "the product counts";
    EXPECT_TRUE(RenderSystem::isAmbient2DTinted(Vector3(1.0f, 1.0f, 1.0f), 0.8f));
    EXPECT_TRUE(RenderSystem::isAmbient2DTinted(Vector3(1.0f, 1.0f, 0.999f), 1.0f)) << "one channel";
    EXPECT_TRUE(RenderSystem::isAmbient2DTinted(Vector3(0.073f, 0.073f, 0.133f), 1.0f)) << "(0.3, 0.3, 0.4) sRGB";
    EXPECT_TRUE(RenderSystem::isAmbient2DTinted(Vector3(1.0f, 1.0f, 1.0f), 1.2f)) << "brighter than white tints too";
}

// R6: no Light 2D and a tinting ambient: the unlit variants take the 2D path (with no lights, so
// no normal map), the lit path keeps its variant, and a neutral ambient keeps every variant
TEST(Light2DAmbient, UnlitVariantsTakeTheLightPathWithoutLights){
    const SubmeshShaderInputs sprite = spriteInputs();
    MeshShaderEnvironment tinted;
    tinted.ambient2D = true;
    const uint64_t ambientOnly = propertiesOf(tinted, sprite);
    EXPECT_NE(ambientOnly & kUnlit, 0u);
    EXPECT_NE(ambientOnly & kLight2D, 0u) << "the ambient multiplies the sprite";
    EXPECT_EQ(ambientOnly & kNormalMap, 0u) << "no light reads the normal map";
    EXPECT_EQ(ambientOnly & ~kLight2D, propertiesOf(MeshShaderEnvironment(), sprite)) << "otherwise the variant of before";
    // its steps: on the 2D path, so 'Lst' (the ambient is not stepped, and nothing else is lit)
    MeshShaderEnvironment tintedStepped = tinted;
    tintedStepped.light2DSteps = true;
    EXPECT_EQ(propertiesOf(tintedStepped, sprite), ambientOnly | ShaderPool::getLight2DStepsProperty());

    SubmeshShaderInputs library = sprite;
    library.manifest = ShaderManifests::get("library:sprite");
    ASSERT_TRUE(library.manifest);
    library.builtinLit = false;
    EXPECT_NE(propertiesOf(tinted, library) & kLight2D, 0u) << "library:sprite";

    SubmeshShaderInputs unlitByChoice = sprite;
    unlitByChoice.receiveLights = false;
    EXPECT_EQ(propertiesOf(tinted, unlitByChoice) & kLight2D, 0u) << "not receiving lights";
}

TEST(Light2DAmbient, LitVariantsAndANeutralAmbientAreUnchanged){
    SubmeshShaderInputs lit = spriteInputs();
    lit.textureRect = false;
    lit.vertexColor4 = false;
    MeshShaderEnvironment lights;
    lights.lights = true;
    MeshShaderEnvironment tinted = lights;
    tinted.ambient2D = true;
    const uint64_t before = propertiesOf(lights, lit);
    ASSERT_EQ(before & kUnlit, 0u);
    EXPECT_EQ(propertiesOf(tinted, lit), before) << "the lit path adds 2D lights only, never the 2D ambient";

    // a neutral ambient (no ambient2D) with no Light 2D: no 'L2d' anywhere
    EXPECT_EQ(propertiesOf(MeshShaderEnvironment(), spriteInputs()) & kLight2D, 0u);
    // with a Light 2D the ambient flag changes nothing: the light path is there already
    MeshShaderEnvironment lights2D;
    lights2D.lights2D = true;
    MeshShaderEnvironment both = lights2D;
    both.ambient2D = true;
    EXPECT_EQ(propertiesOf(both, spriteInputs()), propertiesOf(lights2D, spriteInputs()));
}
