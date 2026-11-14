// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Environment Lighting Source (docs/specs/S94-rendering-anim-gaps.md F15, AC15) on the headless
// device: Gradient lights a sphere's top with the sky colour, its side with the equator's and its
// bottom with the ground's, in the built-in Lit and through a library shader's ambientLight
// (library:simple_lit); Color draws what it drew before the gradient existed (the golden
// ambient_color_mode, written by the engine before S94); and the gradient adds no pass or draw.
// glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the view: 640 x 360 pixels over 6.4 x 3.6 units (100 a unit), the origin at its centre
    constexpr int VIEW_W = 640;
    constexpr int VIEW_H = 360;

    class AmbientGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Primitive lit{&scene};
        Primitive library{&scene};

        void SetUp() override{
            GpuEnvironment::resetView();
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            // lighting on, no light: the ambient alone
            scene.setLightState(LightState::ON);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(-3.2f, 3.2f, -1.8f, 1.8f, 0.1f, 20.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            // the built-in Lit, rough and dielectric, on the left; library:simple_lit on the right
            lit.createSphere(1.5f, 48, 24);
            lit.setPosition(-1.6f, 0.0f, 0.0f);
            Material material;
            material.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            material.metallicFactor = 0.0f;
            material.roughnessFactor = 1.0f;
            lit.setMaterial(material);
            library.createSphere(1.5f, 48, 24);
            library.setPosition(1.6f, 0.0f, 0.0f);
            library.setColor(1.0f, 1.0f, 1.0f);
            library.setCustomShader("library:simple_lit");
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        std::vector<uint8_t> frame(){
            GpuEnvironment::drawFrames(2);
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        // the pixel at world (x, y) on the z = 0 plane (rows top first)
        static const uint8_t* at(const std::vector<uint8_t>& rgba, float x, float y){
            const int px = (int)std::lround(VIEW_W * 0.5f + x * 100.0f);
            const int py = (int)std::lround(VIEW_H * 0.5f - y * 100.0f);
            return &rgba[((size_t)py * VIEW_W + (size_t)px) * 4];
        }

        uint64_t submeshProperties(const Primitive& primitive){
            return scene.getComponent<MeshComponent>(primitive.getEntity()).submeshes[0].shaderProperties;
        }
    };

    std::string describe(const uint8_t* p){
        return "(" + std::to_string(p[0]) + ", " + std::to_string(p[1]) + ", " + std::to_string(p[2]) + ")";
    }

    // sRGB encoding of a linear value, 0..255
    int encode(float linear){
        const float c = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        return (int)std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f);
    }

}

// Gradient: sky red, equator green, ground blue at intensity 1. A normal tilted 0.8 up takes 0.8
// sky and 0.2 equator, a sideways one the equator alone, one 0.8 down 0.8 ground and 0.2 equator:
// exactly through ambientLight (simple_lit: albedo times it), in proportion through the Lit (its
// environment BRDF scales every channel alike on a rough dielectric)
TEST_F(AmbientGpu, GradientTrilight){
    JASPER_REQUIRE_GPU();
    scene.setGlobalIllumination(1.0f);
    scene.setAmbientMode(AmbientMode::GRADIENT);
    scene.setAmbientGradient(Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f));
    ASSERT_TRUE(render());
    const std::vector<uint8_t> rgba = frame();
    ASSERT_EQ(rgba.size(), (size_t)VIEW_W * VIEW_H * 4);

    EXPECT_NE(submeshProperties(lit) & ShaderPool::getAmbientGradientProperty(), 0u) << "the Lit takes 'Agr'";
    EXPECT_NE(submeshProperties(library) & ShaderPool::getAmbientGradientProperty(), 0u) << "simple_lit takes 'Agr'";

    // (the sphere's radius 1.5: a point at height 0.8 * 1.5 faces 0.8 up, 0.6 towards the camera)
    const float r = 1.5f;
    for (float cx : {-1.6f, 1.6f}){
        const bool exact = cx > 0.0f;
        const uint8_t* top = at(rgba, cx, 0.8f * r);
        const uint8_t* side = at(rgba, cx + 0.8f * r, 0.0f);
        const uint8_t* bottom = at(rgba, cx, -0.8f * r);
        const std::string which = exact ? "simple_lit" : "Lit";
        // (a channel the gradient gives almost nothing reads up to 30: the sRGB curve is steep near
        // 0, and the sampled pixel's normal is a tessellated sphere's, half a pixel off)
        if (exact){
            EXPECT_NEAR(top[0], encode(0.8f), 3) << which << " top " << describe(top);
            EXPECT_NEAR(top[1], encode(0.2f), 3) << which << " top " << describe(top);
            EXPECT_LE(top[2], 30) << which << " top " << describe(top);
            EXPECT_LE(side[0], 30) << which << " side " << describe(side);
            EXPECT_NEAR(side[1], 255, 3) << which << " side " << describe(side);
            EXPECT_LE(side[2], 30) << which << " side " << describe(side);
            EXPECT_LE(bottom[0], 30) << which << " bottom " << describe(bottom);
            EXPECT_NEAR(bottom[1], encode(0.2f), 3) << which << " bottom " << describe(bottom);
            EXPECT_NEAR(bottom[2], encode(0.8f), 3) << which << " bottom " << describe(bottom);
        }else{
            EXPECT_GT(top[0], top[1] + 60) << which << " top: the sky's red leads " << describe(top);
            EXPECT_LE(top[2], 30) << which << " top " << describe(top);
            EXPECT_GT(side[1], 40) << which << " side " << describe(side);
            EXPECT_LE(side[0], 30) << which << " side: the equator alone " << describe(side);
            EXPECT_LE(side[2], 30) << which << " side " << describe(side);
            EXPECT_GT(bottom[2], bottom[1] + 60) << which << " bottom: the ground's blue leads " << describe(bottom);
            EXPECT_LE(bottom[0], 30) << which << " bottom " << describe(bottom);
        }
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// Color: the image the engine drew before the gradient existed, bit for bit (a point light over the
// Lit and simple_lit spheres, an ambient colour), and no lit variant takes 'Agr'
TEST_F(AmbientGpu, ColorModeUnchanged){
    JASPER_REQUIRE_GPU();
    scene.setGlobalIllumination(0.35f, Vector3(0.9f, 0.6f, 0.3f));
    Light light(&scene);
    light.setType(LightType::POINT);
    light.setPosition(0.0f, 2.5f, 3.0f);
    light.setRange(12.0f);
    light.setIntensity(6.0f);
    ASSERT_TRUE(render());
    GpuEnvironment::drawFrames(2);
    // bit for bit on glcore, which wrote the golden; Vulkan within the suites' usual tolerance
    CompareOptions options;
    if (Engine::isOpenGL()){
        options.pixelThreshold = 0;
        options.maxBadFraction = 0.0;
        options.maxMeanDiff = 0.0;
    }
    EXPECT_GOLDEN_OPTIONS("ambient_color_mode", options);
    EXPECT_EQ(submeshProperties(lit) & (1ull << 55), 0u) << "MESH bit 55 ('Agr') stays off in Color mode";
    EXPECT_EQ(submeshProperties(library) & (1ull << 55), 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// Gradient changes the variants, not the frame's work: the same passes and draws as Color
TEST_F(AmbientGpu, GradientAddsNoPassOrDraw){
    JASPER_REQUIRE_GPU();
    auto counts = [](uint32_t& passes, uint32_t& draws){
        GpuEnvironment::drawFrames(2);
        const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
        passes = (uint32_t)list.passes.size();
        draws = 0;
        for (const PassRecord& pass : list.passes)
            draws += pass.draws;
    };
    ASSERT_TRUE(render());
    uint32_t colorPasses = 0, colorDraws = 0;
    counts(colorPasses, colorDraws);
    scene.setAmbientMode(AmbientMode::GRADIENT);
    ASSERT_TRUE(render());
    uint32_t gradientPasses = 0, gradientDraws = 0;
    counts(gradientPasses, gradientDraws);
    EXPECT_GT(colorDraws, 0u);
    EXPECT_EQ(gradientPasses, colorPasses);
    EXPECT_EQ(gradientDraws, colorDraws);
    EXPECT_NO_ENGINE_ERRORS();
}
