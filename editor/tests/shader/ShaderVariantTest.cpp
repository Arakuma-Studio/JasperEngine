// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S52's variant bits (docs/specs/S52-sprite-instancing.md R25, AC10): 'Sfx', 'Sfr' and 'Spv' exist
// only for the variants that need them. A project without a Pixel Perfect Camera and without an
// instance= feature exports none of them; a snapping scene's Sprites ship their 'Pxs' + 'Spv' twin;
// a library:sprite FLASH / OUTLINE Sprite ships 'Sfx' and 'Sfr'. Each such variant compiles for
// every backend an export targets (GL 3.3, GLES3/WebGL2, Metal, D3D11, Vulkan) within its budget.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/PixelPerfectCamera.h"
#include "object/Sprite.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const uint64_t kSfx = ShaderPool::getSpriteBatchProperty(ShaderType::MESH, ShaderPool::SpriteBatchInput::FX);
    const uint64_t kSfr = ShaderPool::getSpriteBatchProperty(ShaderType::MESH, ShaderPool::SpriteBatchInput::FRAME_RECT);
    const uint64_t kSpv = ShaderPool::getSpriteBatchProperty(ShaderType::MESH, ShaderPool::SpriteBatchInput::SNAP_PIVOT);
    const uint64_t kDepthSpv = ShaderPool::getSpriteBatchProperty(ShaderType::DEPTH, ShaderPool::SpriteBatchInput::SNAP_PIVOT);

    class ShaderVariantTest : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }

        // the scene's export keys, after MeshSystem built its sprites (or, `drawn` false, as a
        // scene just loaded for an export has them)
        std::set<ShaderKey> collect(Scene& scene, bool drawn = true){
            if (drawn)
                scene.getSystem<MeshSystem>()->update(0.0);
            ShaderVariantContent content;
            content.registry = &scene;
            content.entities = scene.getEntityList();
            std::set<ShaderKey> keys;
            ShaderVariantCollector::collectScene(&scene, content, {}, keys);
            return keys;
        }

        static size_t withBits(const std::set<ShaderKey>& keys, ShaderType type, uint64_t bits){
            size_t count = 0;
            for (ShaderKey key : keys){
                if (ShaderPool::getShaderTypeFromKey(key) == type && (ShaderPool::getPropertiesFromKey(key) & bits))
                    count++;
            }
            return count;
        }

        static std::string names(const std::set<ShaderKey>& keys){
            std::string text;
            for (ShaderKey key : keys)
                text += " " + ShaderPool::getShaderStr(ShaderPool::getShaderTypeFromKey(key), ShaderPool::getPropertiesFromKey(key), 0);
            return text;
        }
    };

}

TEST_F(ShaderVariantTest, S52BitsOnlyWhenUsed){
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.activate();
    Sprite plain(&scene);
    plain.setSize(16, 16);
    Sprite tinted(&scene);
    tinted.setSize(16, 16);
    MeshComponent& mesh = tinted.getComponent<MeshComponent>();
    mesh.submeshes[0].material.shader = "library:sprite";
    mesh.submeshes[0].material.shaderFeatures = {"GRAYSCALE"};
    const std::set<ShaderKey> keys = collect(scene);
    EXPECT_EQ(withBits(keys, ShaderType::MESH, kSfx | kSfr | kSpv), 0u) << names(keys);
    EXPECT_EQ(withBits(keys, ShaderType::DEPTH, kDepthSpv), 0u) << names(keys);
    EXPECT_EQ(plain.getComponent<MeshComponent>().batchAttributes, 0u);
    EXPECT_EQ(mesh.batchAttributes, 0u) << "GRAYSCALE reads no FX slot and no frame rect";
}

TEST_F(ShaderVariantTest, S52BitsForTheFeaturesThatReadThem){
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.activate();
    Sprite flash(&scene);
    flash.setSize(16, 16);
    MeshComponent& mesh = flash.getComponent<MeshComponent>();
    mesh.submeshes[0].material.shader = "library:sprite";
    mesh.submeshes[0].material.shaderFeatures = {"FLASH", "OUTLINE"};
    const std::set<ShaderKey> keys = collect(scene);
    EXPECT_EQ(mesh.batchAttributes, MeshBatchAttribute_SpriteFx | MeshBatchAttribute_FrameRect);
    EXPECT_NE(mesh.buffer.getAttribute(AttributeType::SPRITEFX), nullptr);
    EXPECT_NE(mesh.buffer.getAttribute(AttributeType::FRAMERECT), nullptr);
    EXPECT_GT(withBits(keys, ShaderType::MESH, kSfx), 0u) << names(keys);
    EXPECT_GT(withBits(keys, ShaderType::MESH, kSfr), 0u) << names(keys);
    EXPECT_EQ(withBits(keys, ShaderType::MESH, kSpv), 0u) << "no Pixel Perfect Camera, no snap pivot:" << names(keys);
}

// the export's list does not depend on a draw: a scene loaded and never updated lists the same
// 'Sfx' / 'Sfr' keys (MeshSystem::spriteBatchAttributes from the material, not the mesh's fields)
TEST_F(ShaderVariantTest, S52BitsWithoutADraw){
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.activate();
    Sprite flash(&scene);
    flash.setSize(16, 16);
    MeshComponent& mesh = flash.getComponent<MeshComponent>();
    mesh.submeshes[0].material.shader = "library:sprite";
    mesh.submeshes[0].material.shaderFeatures = {"FLASH", "OUTLINE"};
    const std::set<ShaderKey> loaded = collect(scene, false);
    EXPECT_EQ(mesh.batchAttributes, 0u) << "nothing drew it";
    EXPECT_GT(withBits(loaded, ShaderType::MESH, kSfx), 0u) << names(loaded);
    EXPECT_GT(withBits(loaded, ShaderType::MESH, kSfr), 0u) << names(loaded);
}

TEST_F(ShaderVariantTest, SnappingScenesShipTheSnapPivotTwin){
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.activate();
    PixelPerfectCamera ppc(&scene, camera.getEntity());
    Sprite sprite(&scene);
    sprite.setSize(16, 16);
    const std::set<ShaderKey> keys = collect(scene);
    const uint64_t pixelSnap = ShaderPool::getPixelSnapProperty(ShaderType::MESH);
    size_t twins = 0;
    for (ShaderKey key : keys){
        const uint64_t p = ShaderPool::getPropertiesFromKey(key);
        if (ShaderPool::getShaderTypeFromKey(key) == ShaderType::MESH && (p & pixelSnap) && (p & kSpv))
            twins++;
    }
    EXPECT_GT(twins, 0u) << names(keys);
}

// R24: the new inputs compile everywhere: the built-in sprite with 'Pxs' + 'Spv' (colour and depth),
// library:sprite with FLASH, OUTLINE, PALETTE_SWAP and 'Sfx' + 'Sfr' + 'Pxs' + 'Spv', and their
// instanced forms (a Sprite Batch: 'Ist' + 'Sfx', i_spriteFx; 'Ist' + 'Spv', i_snapPivot)
TEST_F(ShaderVariantTest, S52VariantsCompileForEveryBackend){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    // unlit, uv1, vertex colour 4, texture rect: a sprite
    const uint64_t sprite = shaderPropertyBit(0) | shaderPropertyBit(1) | shaderPropertyBit(10) | shaderPropertyBit(11);
    const uint64_t pixelSnap = ShaderPool::getPixelSnapProperty(ShaderType::MESH);
    const uint64_t instancing = shaderPropertyBit(18);
    const std::string library = "library:sprite#FLASH,OUTLINE,PALETTE_SWAP";
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(library,
        [](const std::string&, std::string&){ return false; }, ShaderType::MESH);
    ASSERT_TRUE(sources.valid) << sources.error;
    const uint16_t libraryId = ShaderPool::registerCustomShader(library);
    struct Variant { ShaderType type; uint64_t properties; uint16_t id; const CustomShaderSources* sources; };
    const Variant variants[] = {
        {ShaderType::MESH, sprite | pixelSnap | kSpv, 0, nullptr},
        {ShaderType::DEPTH, ShaderPool::getPixelSnapProperty(ShaderType::DEPTH) | kDepthSpv, 0, nullptr},
        {ShaderType::MESH, sprite | kSfx | kSfr | pixelSnap | kSpv, libraryId, &sources},
        {ShaderType::MESH, sprite | instancing | kSfx | pixelSnap, libraryId, &sources},
        {ShaderType::MESH, sprite | instancing | pixelSnap, 0, nullptr},
        {ShaderType::MESH, sprite | instancing | pixelSnap | kSpv, 0, nullptr},
        {ShaderType::MESH, sprite | instancing | kSfx | pixelSnap | kSpv, libraryId, &sources},
    };
    for (ShaderBackend backend : ShaderPool::getShaderBackends()){
        for (const Variant& variant : variants){
            const ShaderKey key = ShaderPool::getShaderKey(variant.type, variant.properties, variant.id);
            const std::string name = ShaderPool::getShaderStr(variant.type, variant.properties, 0) + (variant.id ? " " + library : std::string()) +
                                     " (" + ShaderPool::getShaderLangStr(backend) + ")";
            ShaderData data;
            std::string error, log;
            ASSERT_TRUE(ShaderVariantCompiler::compile(key, backend, variant.sources, data, &error, &log)) << name << ": " << error << "\n" << log;
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
            if ((variant.properties & (variant.type == ShaderType::DEPTH ? kDepthSpv : kSpv)) && !(variant.properties & instancing)){
                EXPECT_GE(data.getAttrIndex(AttributeType::SNAPPIVOT), 0) << name << " reads no a_snapPivot";
            }
            if (variant.type == ShaderType::MESH && (variant.properties & kSpv) && (variant.properties & instancing)){
                EXPECT_GE(data.getAttrIndex(AttributeType::INSTANCESNAPPIVOT), 0) << name << " reads no i_snapPivot";
            }
            if ((variant.properties & kSfx) && !(variant.properties & instancing)){
                EXPECT_GE(data.getAttrIndex(AttributeType::SPRITEFX), 0) << name << " reads no a_spriteFx";
            }
            if ((variant.properties & kSfx) && (variant.properties & instancing)){
                EXPECT_GE(data.getAttrIndex(AttributeType::INSTANCESPRITEFX), 0) << name << " reads no i_spriteFx";
            }
            if (variant.properties & kSfr){
                EXPECT_GE(data.getAttrIndex(AttributeType::FRAMERECT), 0) << name << " reads no a_frameRect";
            }
        }
    }
}
