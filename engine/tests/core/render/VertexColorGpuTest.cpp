// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Vertex colours drawn (docs/specs/V1-vertex-colors.md AC5, AC11), one suite of goldens on the
// headless device:
//
// vertex_color_view_rgb, _r, _a: the Scene view's Draw Mode Vertex Color (R102, D8) on a lit,
// tinted sphere whose vertex colours are its position (RGB, 0.5 + 0.5 * the normal) and its
// height (A): the raw colours, unlit, whatever the material; R as grey; A as grey over the
// screen-space checker. A box beside it, whose colours are the primitives' white, draws white.
//
// vertex_color_lit_tint: the built-in Lit's default Vertex Color Use (Tint) on the same sphere. Its
// program is the one Lit always drew (ShaderZeroCostTest holds the preprocessed source of every
// variant to its hash from before the section), so this image is what an old project draws (AC11).
//
// vertex_color_lit_masks: Masks on the sphere, R replacing the roughness and G masking the emission
// (R70): the glossy +X side, the glowing +Y top.
//
// vertex_blend_five_layers: library:vertex_blend at five layers on a grid painted base, R, G, B, A
// across it (R71): flat albedo layers with heights in alpha, so their edges follow the heights.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "pool/TextureDataPool.h"
#include "render/ViewDrawMode.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/MaterialPbr.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace VertexColorGpuTest_cpp {

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_gpu_test {
    using namespace jasperengine::test;

    bool vcGpuIsGoldenBackend(){
        return std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
    }

    // glcore against its own goldens: exact; another backend: the cross-backend bound
    CompareOptions vcGpuExactOnGolden(){
        CompareOptions options;
        if (vcGpuIsGoldenBackend()){
            options.pixelThreshold = 0;
            options.maxBadFraction = 0.0;
            options.maxMeanDiff = 0.0;
        }else{
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    // a 4 x 4 texture of one texel value (RGBA8, as stored: sRGB colour)
    Texture flatTexture(const std::string& id, std::array<uint8_t, 4> texel){
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

    // a UV sphere of radius 1: position, normal, uv and its RGBA vertex colour
    void buildSphere(Scene& scene, Mesh& sphere){
        constexpr int RINGS = 24;
        constexpr int SEGMENTS = 32;
        MeshComponent& mesh = sphere.getComponent<MeshComponent>();
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        mesh.buffer.addAttribute(AttributeType::COLOR, 4);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* texcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        Attribute* color = mesh.buffer.getAttribute(AttributeType::COLOR);
        for (int r = 0; r <= RINGS; r++){
            const float v = (float)r / RINGS;
            const float theta = v * 3.14159265f;
            for (int s = 0; s <= SEGMENTS; s++){
                const float u = (float)s / SEGMENTS;
                const float phi = u * 2.0f * 3.14159265f;
                const Vector3 n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
                mesh.buffer.addVector3(position, n);
                mesh.buffer.addVector2(texcoord, Vector2(u, v));
                mesh.buffer.addVector3(normal, n);
                mesh.buffer.addVector4(color, Vector4(0.5f + 0.5f * n.x, 0.5f + 0.5f * n.y, 0.5f + 0.5f * n.z, 1.0f - v));
            }
        }
        std::vector<uint16_t> indices;
        const int row = SEGMENTS + 1;
        for (int r = 0; r < RINGS; r++){
            for (int s = 0; s < SEGMENTS; s++){
                const uint16_t a = (uint16_t)(r * row + s);
                const uint16_t b = (uint16_t)((r + 1) * row + s);
                const uint16_t c = (uint16_t)((r + 1) * row + s + 1);
                const uint16_t d = (uint16_t)(r * row + s + 1);
                // counter-clockwise seen from outside
                indices.insert(indices.end(), {a, d, b, d, c, b});
            }
        }
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)&indices[0], sizeof(uint16_t));
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    }

    // a 4 x 2 grid facing up, its vertex colour painting base, R, G, B and A left to right
    void buildPaintedGrid(Scene& scene, Mesh& grid){
        constexpr int COLUMNS = 40;
        constexpr int ROWS = 16;
        constexpr float WIDTH = 4.0f;
        constexpr float DEPTH = 1.6f;
        MeshComponent& mesh = grid.getComponent<MeshComponent>();
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        mesh.buffer.addAttribute(AttributeType::COLOR, 4);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* texcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        Attribute* color = mesh.buffer.getAttribute(AttributeType::COLOR);
        for (int j = 0; j <= ROWS; j++){
            for (int i = 0; i <= COLUMNS; i++){
                const float x = (float)i / COLUMNS;
                const float z = (float)j / ROWS;
                mesh.buffer.addVector3(position, Vector3((x - 0.5f) * WIDTH, 0.0f, (z - 0.5f) * DEPTH));
                mesh.buffer.addVector2(texcoord, Vector2(x * 4.0f, z * 1.6f));
                mesh.buffer.addVector3(normal, Vector3(0.0f, 1.0f, 0.0f));
                // five bands; within one, the channel ramps up and down, so the band edges blend
                const float band = x * 5.0f;
                auto hat = [band](float centre){ return std::max(0.0f, 1.0f - std::fabs(band - centre) * 1.2f); };
                mesh.buffer.addVector4(color, Vector4(hat(1.5f), hat(2.5f), hat(3.5f), hat(4.5f)));
            }
        }
        std::vector<uint16_t> indices;
        const int row = COLUMNS + 1;
        for (int j = 0; j < ROWS; j++){
            for (int i = 0; i < COLUMNS; i++){
                const uint16_t a = (uint16_t)(j * row + i);
                const uint16_t b = (uint16_t)(j * row + i + 1);
                const uint16_t c = (uint16_t)((j + 1) * row + i + 1);
                const uint16_t d = (uint16_t)((j + 1) * row + i);
                // front faces up
                indices.insert(indices.end(), {a, d, c, a, c, b});
            }
        }
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)&indices[0], sizeof(uint16_t));
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    }

    // the Vertex Color section's defines a loaded submesh draws with (the scene's Lit Quality adds its own)
    std::vector<std::string> vertexColorDefines(const Submesh& submesh){
        std::vector<std::string> defines;
        for (const std::string& define : MaterialPbr::getDefinesFromBits(submesh.litDefineBits)){
            if (MaterialPbr::isVertexColorDefine(define))
                defines.push_back(define);
        }
        return defines;
    }

    class VertexColorGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};

        void SetUp() override{
            GpuEnvironment::resetView();
            scene.setBackgroundColor(0.12f, 0.14f, 0.18f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.6f, 1.4f, 4.2f);
            camera.setTarget(0.6f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.5f, -1.0f, -0.6f);
            sun.setIntensity(2.0f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            scene.getSystem<RenderSystem>()->setDrawMode(ViewDrawMode::SHADED);
            Engine::removeScene(&scene);
        }

        RenderSystem& render(){
            return *scene.getSystem<RenderSystem>();
        }

        // loaded, and in the Vertex Color Draw Mode no view variant still being made
        bool settle(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                const auto system = current->getSystem<RenderSystem>();
                return system->isAllLoaded() && !system->isVertexColorViewPending();
            });
        }

        void expectGolden(const std::string& name){
            const CompareResult result = compareTargetWithGolden(name, vcGpuExactOnGolden());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }
    };

TEST_F(VertexColorGpu, DrawModeShowsTheRawColours){
    JASPER_REQUIRE_GPU();
    Mesh sphere(&scene);
    buildSphere(scene, sphere);
    // what the view leaves out: a tint, lights, a glossy emissive surface
    Material material;
    material.baseColorFactor = Vector4(0.2f, 0.9f, 0.3f, 1.0f);
    material.emissiveFactor = Vector3(0.5f, 0.0f, 0.0f);
    material.roughnessFactor = 0.2f;
    sphere.setMaterial(material);
    Primitive box(&scene);
    box.createBox(0.5f, 0.5f, 0.5f);
    box.setPosition(1.9f, 0.0f, 0.0f);
    box.setColor(0.1f, 0.2f, 0.8f);

    render().setDrawMode(ViewDrawMode::VERTEX_COLOR, ViewVertexColorChannel::RGB);
    ASSERT_TRUE(settle()) << "the scene never finished loading";
    expectGolden("vertex_color_view_rgb");

    render().setDrawMode(ViewDrawMode::VERTEX_COLOR, ViewVertexColorChannel::R);
    ASSERT_TRUE(settle());
    expectGolden("vertex_color_view_r");

    render().setDrawMode(ViewDrawMode::VERTEX_COLOR, ViewVertexColorChannel::A);
    ASSERT_TRUE(settle());
    expectGolden("vertex_color_view_a");

    // leaving the mode lets its variants go, and the scene draws shaded again
    render().setDrawMode(ViewDrawMode::SHADED);
    ASSERT_TRUE(settle());
    GpuEnvironment::drawFrames(6);
    EXPECT_FALSE(render().isVertexColorViewPending());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(VertexColorGpu, LitTintIsWhatLitAlwaysDrew){
    JASPER_REQUIRE_GPU();
    Mesh sphere(&scene);
    buildSphere(scene, sphere);
    sphere.setMaterial(Material());
    ASSERT_TRUE(settle()) << "the scene never finished loading";
    const Submesh& submesh = scene.getComponent<MeshComponent>(sphere.getEntity()).submeshes[0];
    EXPECT_TRUE(vertexColorDefines(submesh).empty()) << "Tint adds no define to the built-in Lit";
    EXPECT_EQ(submesh.pbrVertexColorOffset, -1);
    expectGolden("vertex_color_lit_tint");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(VertexColorGpu, LitMasksRoughnessAndEmission){
    JASPER_REQUIRE_GPU();
    Mesh sphere(&scene);
    buildSphere(scene, sphere);
    Material material;
    // the sun from the upper left, so its highlight falls on the glossy side
    sun.setDirection(0.7f, -0.6f, -0.6f);
    sun.setIntensity(4.0f);
    material.baseColorFactor = Vector4(0.25f, 0.25f, 0.3f, 1.0f);
    material.metallicFactor = 0.0f;
    material.emissiveFactor = Vector3(0.35f, 0.12f, 0.02f);
    MaterialPbrExtensions::VertexColor& vertexColor = material.pbr.edit().vertexColor;
    vertexColor.use = VertexColorUse::MASKS;
    // R (0.5 + 0.5 x) replaces the roughness: glossy on the left
    vertexColor.r = VertexColorTarget::ROUGHNESS_REPLACE;
    // G (0.5 + 0.5 y) masks the emission: it glows towards the top
    vertexColor.g = VertexColorTarget::EMISSION;
    sphere.setMaterial(material);
    ASSERT_TRUE(settle()) << "the scene never finished loading";
    const Submesh& submesh = scene.getComponent<MeshComponent>(sphere.getEntity()).submeshes[0];
    EXPECT_EQ(vertexColorDefines(submesh), std::vector<std::string>({"LIT_VC_EMISSION", "LIT_VC_ROUGHNESS_SET"}));
    EXPECT_NE(submesh.customShaderId, 0u) << "the masks' variant was not built (the plain one stands in)";
    EXPECT_GE(submesh.pbrVertexColorOffset, 0) << "the masks reach the block";
    expectGolden("vertex_color_lit_masks");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(VertexColorGpu, VertexBlendFiveLayers){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 3.2f, 2.4f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    Mesh grid(&scene);
    buildPaintedGrid(scene, grid);
    Material material;
    material.shader = "library:vertex_blend";
    material.shaderFeatures = {"LAYERS_FIVE"};
    material.lighting = MaterialLighting::PIXEL;
    // flat layers of distinct colours; their heights (alpha) rise from the base up
    const std::array<std::pair<const char*, std::array<uint8_t, 4>>, 5> layers = {{
        {"u_baseAlbedo", {120, 110, 100, 40}},
        {"u_rAlbedo", {200, 60, 40, 120}},
        {"u_gAlbedo", {60, 170, 60, 160}},
        {"u_bAlbedo", {50, 80, 200, 200}},
        {"u_aAlbedo", {230, 220, 90, 255}},
    }};
    for (const auto& [uniform, texel] : layers)
        material.shaderTextures.push_back({uniform, flatTexture(std::string("vc-blend-") + uniform, texel)});
    grid.setMaterial(material);
    ASSERT_TRUE(settle()) << "the scene never finished loading";
    EXPECT_NE(scene.getComponent<MeshComponent>(grid.getEntity()).submeshes[0].customShaderId, 0u);
    expectGolden("vertex_blend_five_layers");
    EXPECT_NO_ENGINE_ERRORS();
}

} // namespace vertex_color_gpu_test

}  // namespace VertexColorGpuTest_cpp
