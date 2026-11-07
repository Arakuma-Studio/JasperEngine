// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:water lit per pixel under a shadow (docs/superpowers/specs/2026-11-04-water-reflections-design.md
// F6, F7), on the headless device with the shader clock pinned:
//   - the shadow lookup takes the surface's normal without the ripples, so where the flat water
//     is fully lit the rippled water is too (the ripples do not move the shadow's edge about);
//   - the caustics on the bed go out where the surface over it is in shadow.

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr double SHADING_PINNED_TIME = 5.75;

    std::vector<uint8_t> readShadingTarget(int& width, int& height){
        Framebuffer& target = GpuEnvironment::target();
        width = (int)target.getWidth();
        height = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), width, height, rgba);
        return rgba;
    }

    // the mean luminance (0..255) of a 15 x 15 block around where a world point is drawn
    double shadingLuminanceAt(Scene& scene, const std::vector<uint8_t>& rgba, int width, int height, const Vector3& world){
        const CameraComponent& camera = scene.getComponent<CameraComponent>(scene.getCamera());
        const Vector4 clip = camera.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
        const int cx = (int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * (float)width);
        const int cy = (int)std::lround((0.5f - clip.y / clip.w * 0.5f) * (float)height);
        double sum = 0.0;
        int count = 0;
        for (int y = cy - 7; y <= cy + 7; y++){
            for (int x = cx - 7; x <= cx + 7; x++){
                if (x < 0 || y < 0 || x >= width || y >= height)
                    continue;
                const size_t i = ((size_t)y * width + x) * 4;
                sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
                count++;
            }
        }
        return count ? sum / count : 0.0;
    }

    Material litWater(const std::vector<std::string>& features, float normalStrength){
        Material material;
        material.name = "Water";
        material.shader = "library:water";
        material.shaderFeatures = features;
        material.lighting = MaterialLighting::PIXEL;
        ShaderUniforms::set(material.shaderParams, "normalStrength", Vector4(normalStrength, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "foamAmount", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        return material;
    }

    class WaterShadingGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};

        void SetUp() override{
            // the view the goldens and pixel positions were made at
            GpuEnvironment::resetView();
            Engine::setPinnedShaderTime(SHADING_PINNED_TIME);
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            sun.setType(LightType::DIRECTIONAL);
            sun.setIntensity(2.0f);
            sun.setShadows(true);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::setPinnedShaderTime(-1.0);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }
    };

}

// F6: shadow factor < 1 on the rippled water only where the flat water has it too
TEST_F(WaterShadingGpu, ShadowEdgeFollowsSmoothSurface){
    JASPER_REQUIRE_GPU();
    sun.setDirection(-0.3f, -1.0f, -0.2f);
    // a large normal offset, so where the lookup is offset along shows
    sun.setShadowNormalBias(4.0f);
    camera.setPosition(0.0f, 7.0f, 6.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive caster(&scene);
    caster.createBox(2.0f, 0.2f, 2.0f);
    caster.setPosition(0.0f, 1.5f, 0.0f);
    caster.setColor(0.8f, 0.2f, 0.2f);

    Primitive water(&scene);
    water.createPlane(16.0f, 16.0f);

    // the four images: flat or rippled, receiving the shadow or not
    struct Shot{ std::vector<uint8_t> rgba; };
    auto shoot = [&](float normalStrength, bool receive){
        water.setMaterial(litWater({"BUILTIN_TEXTURES"}, normalStrength));
        water.setReceiveShadows(receive);
        EXPECT_TRUE(render()) << "the scene never finished loading";
        int width = 0, height = 0;
        return Shot{readShadingTarget(width, height)};
    };
    const Shot flatShadowed = shoot(0.0f, true);
    const Shot flatLit = shoot(0.0f, false);
    const Shot rippledShadowed = shoot(1.5f, true);
    const Shot rippledLit = shoot(1.5f, false);

    // a pixel is in some shadow when receiving it darkens it
    auto shadowed = [](const Shot& with, const Shot& without, size_t i){
        int most = 0;
        for (int c = 0; c < 3; c++)
            most = std::max(most, (int)without.rgba[i + c] - (int)with.rgba[i + c]);
        return most > 3;
    };
    size_t flatLitPixels = 0;
    size_t movedPixels = 0;
    size_t flatShadowPixels = 0;
    for (size_t i = 0; i + 3 < flatLit.rgba.size(); i += 4){
        if (shadowed(flatShadowed, flatLit, i)){
            flatShadowPixels++;
            continue;
        }
        flatLitPixels++;
        if (shadowed(rippledShadowed, rippledLit, i))
            movedPixels++;
    }
    std::printf("[gpu] shadow on %s: %zu flat shadowed, %zu flat lit, %zu of them shadowed once rippled\n",
                HeadlessContext::getBackendName(), flatShadowPixels, flatLitPixels, movedPixels);
    ASSERT_GT(flatShadowPixels, 500u) << "the box casts no shadow on the water";
    // (the same lookup: only a handful of pixels, where the variants' arithmetic rounds apart)
    EXPECT_LE(movedPixels, 5u) << "the ripples move the shadow lookup";
    EXPECT_NO_ENGINE_ERRORS();
}

// F7: caustics brighten the bed under the lit surface, not under the surface in shadow
TEST_F(WaterShadingGpu, CausticsDarkInSurfaceShadow){
    JASPER_REQUIRE_GPU();
    sun.setDirection(0.0f, -1.0f, 0.0f);
    camera.setPosition(0.0f, 6.0f, 8.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive bed(&scene);
    bed.createPlane(30.0f, 30.0f);
    bed.setPosition(0.0f, -1.0f, 0.0f);
    bed.setColor(0.6f, 0.55f, 0.4f);

    // a plate high over the water: its shadow falls straight down on (-2..2, -2..2), around the
    // surface point the camera looks through at the middle of the view
    Primitive caster(&scene);
    caster.createBox(4.0f, 0.1f, 4.0f);
    caster.setPosition(0.0f, 3.0f, 0.0f);

    Primitive water(&scene);
    water.createPlane(20.0f, 20.0f);

    auto shoot = [&](bool caustics, std::vector<uint8_t>& rgba, int& width, int& height){
        Material material = litWater(caustics ? std::vector<std::string>{"REFRACTION", "CAUSTICS"} : std::vector<std::string>{"REFRACTION"}, 0.0f);
        ShaderUniforms::set(material.shaderParams, "shallowColor", Vector4(0.2f, 0.6f, 0.7f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "deepColor", Vector4(0.2f, 0.6f, 0.7f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "causticsStrength", Vector4(2.0f, 0.0f, 0.0f, 0.0f));
        water.setMaterial(material);
        EXPECT_TRUE(render()) << "the scene never finished loading";
        rgba = readShadingTarget(width, height);
    };
    std::vector<uint8_t> off, on;
    int width = 0, height = 0;
    shoot(false, off, width, height);
    shoot(true, on, width, height);

    // surface points: the bed seen through them is lit in both, the surface only at the second
    const Vector3 inShadow(0.0f, 0.0f, 0.0f);
    const Vector3 inSun(3.2f, 0.0f, 0.0f);
    const double shadowRatio = shadingLuminanceAt(scene, on, width, height, inShadow) /
                               std::max(shadingLuminanceAt(scene, off, width, height, inShadow), 1.0);
    const double sunRatio = shadingLuminanceAt(scene, on, width, height, inSun) /
                            std::max(shadingLuminanceAt(scene, off, width, height, inSun), 1.0);
    std::printf("[gpu] caustics on %s: %.3f in the surface's shadow, %.3f in the sun\n",
                HeadlessContext::getBackendName(), shadowRatio, sunRatio);
    EXPECT_GT(sunRatio, 1.05) << "no caustics in the sun at all: the test proves nothing";
    if (HasFailure()){
        writePng(getOutputDirectory() + "/water_caustics_off.png", off, width, height);
        writePng(getOutputDirectory() + "/water_caustics_on.png", on, width, height);
    }
    EXPECT_LT(shadowRatio, 1.02) << "caustics brighten the bed under the shadowed surface";
    EXPECT_NO_ENGINE_ERRORS();
}
