// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A texture given to a loaded mesh's material gets the variants the material asks for now, in
// every pass (docs/superpowers/specs/2026-11-04-render-correctness-design.md 6.4). The update
// compared a hand-picked few bits of the colour variant: a mesh that already had UVs and gained a
// base colour or metallic-roughness texture kept a G-buffer variant without them (SSR reflected
// the factors, Doriax 17ce79969), and a masked material's shadow and depth pre-pass kept a depth
// variant without the alpha-clip texture. The new variant is built while the loaded one draws.
// glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "pool/TextureDataPool.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <functional>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    Texture variantFlatTexture(const std::string& id, std::array<uint8_t, 4> texel){
        TextureDataPool::setGenerator(id, [texel]() -> TextureData {
            const int size = 4 * 4 * 4;
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(size));
            if (!pixels)
                return TextureData();
            for (int i = 0; i < 16; i++)
                std::copy(texel.begin(), texel.end(), pixels + i * 4);
            return TextureData(4, 4, (unsigned int)size, ColorFormat::RGBA, 4, pixels);
        });
        Texture texture;
        texture.setId(id);
        return texture;
    }

    uint64_t variantBit(int index){
        return uint64_t(1) << index;
    }

    class MaterialVariantGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        Primitive plane{&scene};

        void SetUp() override{
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 2.0f, 2.5f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(0.0f, -1.0f, -0.5f);
            sun.setIntensity(3.0f);
            plane.createPlane(3.0f, 3.0f);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        // frames until the scene settles: loaded, and no variant waiting to be swapped in
        void settle(){
            Scene* current = &scene;
            const Entity entity = plane.getEntity();
            int frames = 0;
            EXPECT_TRUE(GpuEnvironment::runFrames([current, entity, &frames](){
                const auto render = current->getSystem<RenderSystem>();
                const MeshComponent& mesh = current->getComponent<MeshComponent>(entity);
                frames++;
                return render->isAllLoaded() && mesh.loaded && !mesh.needVariantReload && frames > 4;
            })) << "the scene never settled";
            GpuEnvironment::drawFrames(2);
        }

        Submesh& submesh(){
            return scene.getComponent<MeshComponent>(plane.getEntity()).submeshes[0];
        }

        // as the Inspector, MCP and Lua edit a loaded material: the texture, then needUpdateTexture
        void edit(const std::function<void(Material&)>& change){
            change(submesh().material);
            submesh().needUpdateTexture = true;
        }
    };

}

TEST_F(MaterialVariantGpu, GBufferFollowsATextureGivenToALoadedMaterial){
    JASPER_REQUIRE_GPU();
    sun.setShadows(false);
    scene.setSSREnabled(true);
    Material material;
    material.normalTexture = variantFlatTexture("rc-variant-normal", {128, 128, 255, 255});
    plane.setMaterial(material);
    settle();
    ASSERT_TRUE(submesh().gbufferShader) << "SSR on draws no G-buffer";
    ASSERT_NE(submesh().shaderProperties & variantBit(1), 0u) << "the case needs UVs before the edit ('Uv1')";
    ASSERT_EQ(submesh().gbufferShaderProperties & (variantBit(0) | variantBit(8)), 0u);

    edit([](Material& m){
        m.baseColorTexture = variantFlatTexture("rc-variant-base", {200, 60, 60, 255});
        m.metallicRoughnessTexture = variantFlatTexture("rc-variant-mr", {255, 40, 255, 255});
    });
    settle();
    EXPECT_NE(submesh().gbufferShaderProperties & variantBit(0), 0u) << "the G-buffer variant has no base colour texture ('Bct')";
    EXPECT_NE(submesh().gbufferShaderProperties & variantBit(8), 0u) << "the G-buffer variant has no metallic-roughness texture ('Mrt')";
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 0, true), "Bct");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 8, true), "Mrt");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(MaterialVariantGpu, DepthClipFollowsABaseColourGivenToAMaskMaterial){
    JASPER_REQUIRE_GPU();
    sun.setShadows(true);
    Material material;
    material.alphaMode = MaterialAlphaMode::MASK;
    material.alphaCutoff = 0.5f;
    material.normalTexture = variantFlatTexture("rc-variant-mask-normal", {128, 128, 255, 255});
    plane.setMaterial(material);
    settle();
    ASSERT_TRUE(submesh().depthShader) << "a shadow-casting light draws no depth pass";
    ASSERT_NE(submesh().shaderProperties & variantBit(1), 0u) << "the case needs UVs before the edit ('Uv1')";
    ASSERT_EQ(submesh().depthShaderProperties & variantBit(0), 0u);

    edit([](Material& m){
        m.baseColorTexture = variantFlatTexture("rc-variant-mask-base", {255, 255, 255, 0});
    });
    settle();
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::DEPTH, 0, true), "Tex");
    EXPECT_NE(submesh().depthShaderProperties & variantBit(0), 0u) << "the shadow and depth passes do not clip by the new base colour ('Tex')";
    EXPECT_NO_ENGINE_ERRORS();
}
