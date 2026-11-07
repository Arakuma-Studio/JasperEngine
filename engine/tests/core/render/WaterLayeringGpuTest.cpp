// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene colour copy and the transparent queue around library:water on the headless device
// (docs/superpowers/specs/2026-11-04-water-reflections-design.md, F2 to F5):
//   - the copy reproduces the pass colour where the target's alpha is under 1 (it does not blend
//     what it captures);
//   - a transparent object under a refracting water is in the copy the water samples;
//   - a transparent object under a water whose transform is farther from the camera than the
//     object draws before the water (the water's rank), so the water tints it;
//   - a refracting water over another one samples a copy that holds the lower one;
//   - the UI (and the hierarchy loop's other overlays) is not in the copy: a HUD never refracts.
// The shader clock is pinned, so the ripples are where they always are.

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "object/ui/Image.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr double LAYERING_PINNED_TIME = 3.5;

    struct LayeringPixel{ int r = 0, g = 0, b = 0; };

    std::vector<uint8_t> readLayeringTarget(int& width, int& height){
        Framebuffer& target = GpuEnvironment::target();
        width = (int)target.getWidth();
        height = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), width, height, rgba);
        return rgba;
    }

    // the mean colour of a 9 x 9 block around where a world point is drawn
    LayeringPixel layeringPixelAt(Scene& scene, const Vector3& world){
        int width = 0, height = 0;
        const std::vector<uint8_t> rgba = readLayeringTarget(width, height);
        const CameraComponent& camera = scene.getComponent<CameraComponent>(scene.getCamera());
        const Vector4 clip = camera.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
        const int cx = (int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * (float)width);
        const int cy = (int)std::lround((0.5f - clip.y / clip.w * 0.5f) * (float)height);
        LayeringPixel sum;
        int count = 0;
        for (int y = cy - 4; y <= cy + 4; y++){
            for (int x = cx - 4; x <= cx + 4; x++){
                if (x < 0 || y < 0 || x >= width || y >= height)
                    continue;
                const size_t i = ((size_t)y * width + x) * 4;
                sum.r += rgba[i];
                sum.g += rgba[i + 1];
                sum.b += rgba[i + 2];
                count++;
            }
        }
        if (count > 0){
            sum.r /= count;
            sum.g /= count;
            sum.b /= count;
        }
        return sum;
    }

    // a water that is only its refraction where it is whole (colours without alpha, no foam,
    // no ripples): it draws the copy in place of what the target holds
    Material refractionOnlyWater(){
        Material material;
        material.name = "Water";
        material.shader = "library:water";
        material.shaderFeatures = {"REFRACTION"};
        ShaderUniforms::set(material.shaderParams, "shallowColor", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "deepColor", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "foamAmount", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "refractionStrength", Vector4(0.2f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "refractionDepth", Vector4(0.01f, 0.0f, 0.0f, 0.0f));
        return material;
    }

    // a plain alpha-blended water of one colour
    Material tintWater(const Vector4& color){
        Material material;
        material.name = "Water";
        material.shader = "library:water";
        ShaderUniforms::set(material.shaderParams, "shallowColor", color);
        ShaderUniforms::set(material.shaderParams, "deepColor", color);
        ShaderUniforms::set(material.shaderParams, "foamAmount", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        return material;
    }

    // a 4 x 4 green image (a TextureData made from memory points at its pixels)
    TextureData greenImage(){
        static std::vector<uint8_t> pixels;
        if (pixels.empty()){
            for (int i = 0; i < 16; i++){
                pixels.insert(pixels.end(), {0, 255, 0, 255});
            }
        }
        return TextureData(4, 4, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    }

    // an unlit, alpha-blended red quad
    void makeRedGlass(Primitive& quad, float size){
        quad.createPlane(size, size);
        Material material;
        material.name = "Red glass";
        material.shader = "library:unlit";
        material.blendMode = MaterialBlendMode::ALPHA;
        ShaderUniforms::set(material.shaderParams, "baseColor", Vector4(1.0f, 0.0f, 0.0f, 0.5f));
        quad.setMaterial(material);
    }

    class WaterLayeringGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            // the view the goldens and pixel positions were made at
            GpuEnvironment::resetView();
            Engine::setPinnedShaderTime(LAYERING_PINNED_TIME);
            scene.setBackgroundColor(0.2f, 0.4f, 0.6f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
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

    void printLayeringPixel(const char* what, const LayeringPixel& pixel){
        std::printf("[gpu] %s: (%d, %d, %d) on %s\n", what, pixel.r, pixel.g, pixel.b, HeadlessContext::getBackendName());
    }

}

// F5: over a background whose alpha is 0.5 the copy holds the background's colour, not half of it
TEST_F(WaterLayeringGpu, SceneColorCopyKeepsColourWhereTargetAlphaBelowOne){
    JASPER_REQUIRE_GPU();
    scene.setBackgroundColor(0.2f, 0.4f, 0.6f, 0.5f);
    camera.setPosition(0.0f, 6.0f, 0.01f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive water(&scene);
    water.createPlane(4.0f, 4.0f);
    water.setMaterial(refractionOnlyWater());

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const LayeringPixel through = layeringPixelAt(scene, Vector3(0.0f, 0.0f, 0.0f));
    const LayeringPixel background = layeringPixelAt(scene, Vector3(4.0f, 0.0f, 0.0f));
    printLayeringPixel("through the water", through);
    printLayeringPixel("background", background);
    EXPECT_NEAR(through.r, background.r, 3);
    EXPECT_NEAR(through.g, background.g, 3);
    EXPECT_NEAR(through.b, background.b, 3);
    EXPECT_NO_ENGINE_ERRORS();
}

// F2: a transparent quad under a refracting water (farther from the camera than the water's
// transform, so the distance sort alone drew it first) shows through it
TEST_F(WaterLayeringGpu, TransparentUnderRefractingWaterStaysVisible){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 5.0f, 3.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive floor(&scene);
    floor.createPlane(20.0f, 20.0f);
    floor.setPosition(0.0f, -1.0f, 0.0f);
    floor.setColor(0.5f, 0.5f, 0.5f);

    Primitive glass(&scene);
    makeRedGlass(glass, 2.0f);
    glass.setPosition(0.0f, -0.5f, -1.0f);

    Primitive water(&scene);
    water.createPlane(10.0f, 10.0f);
    water.setMaterial(refractionOnlyWater());

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const LayeringPixel quad = layeringPixelAt(scene, Vector3(0.0f, -0.5f, -1.0f));
    const LayeringPixel floorOnly = layeringPixelAt(scene, Vector3(3.0f, -1.0f, -1.0f));
    printLayeringPixel("glass under the water", quad);
    printLayeringPixel("floor under the water", floorOnly);
    EXPECT_GT(quad.r - floorOnly.r, 40) << "the glass under the refracting water is not in the scene colour copy";
    EXPECT_NO_ENGINE_ERRORS();
}

// F3: a transparent quad under a large lake, nearer the camera than the lake's transform: the
// lake is drawn over it (tinting it blue) instead of the quad over the lake
TEST_F(WaterLayeringGpu, TransparentUnderWaterNearerThanItsCentre){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 4.0f, 6.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive floor(&scene);
    floor.createPlane(60.0f, 60.0f);
    floor.setPosition(0.0f, -1.0f, -15.0f);
    floor.setColor(0.5f, 0.5f, 0.5f);

    Primitive glass(&scene);
    makeRedGlass(glass, 2.0f);
    glass.setPosition(0.0f, -0.5f, 0.0f);

    Primitive lake(&scene);
    lake.createPlane(40.0f, 40.0f);
    lake.setPosition(0.0f, 0.0f, -15.0f);
    lake.setMaterial(tintWater(Vector4(0.0f, 0.0f, 1.0f, 0.6f)));

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const LayeringPixel underLake = layeringPixelAt(scene, Vector3(0.0f, -0.5f, 0.0f));
    printLayeringPixel("glass under the lake", underLake);

    // the same glass with no lake: what the quad looks like untinted
    lake.setVisible(false);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const LayeringPixel alone = layeringPixelAt(scene, Vector3(0.0f, -0.5f, 0.0f));
    printLayeringPixel("glass alone", alone);
    // (drawn over the lake, the glass keeps most of its own red and gains little blue)
    EXPECT_GT(underLake.b - alone.b, 80) << "the glass under the lake was drawn over it";
    EXPECT_NO_ENGINE_ERRORS();
}

// F4: a refracting pool over a refracting lake sees the lake through it
TEST_F(WaterLayeringGpu, OverlappingRefractingWatersSeeEachOther){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 9.0f, 0.5f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive floor(&scene);
    floor.createPlane(40.0f, 40.0f);
    floor.setPosition(0.0f, -1.0f, 0.0f);
    floor.setColor(0.5f, 0.5f, 0.5f);

    // the lake: half its own green over what it refracts
    Primitive lake(&scene);
    lake.createPlane(30.0f, 30.0f);
    Material lakeMaterial = refractionOnlyWater();
    ShaderUniforms::set(lakeMaterial.shaderParams, "shallowColor", Vector4(0.0f, 0.8f, 0.0f, 0.5f));
    ShaderUniforms::set(lakeMaterial.shaderParams, "deepColor", Vector4(0.0f, 0.8f, 0.0f, 0.5f));
    lake.setMaterial(lakeMaterial);

    // the pool 2 m above it: only its refraction
    Primitive pool(&scene);
    pool.createPlane(3.0f, 3.0f);
    pool.setPosition(0.0f, 2.0f, 0.0f);
    pool.setMaterial(refractionOnlyWater());

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const LayeringPixel throughPool = layeringPixelAt(scene, Vector3(0.0f, 2.0f, 0.0f));
    const LayeringPixel lakeOnly = layeringPixelAt(scene, Vector3(5.0f, 0.0f, 0.0f));
    printLayeringPixel("through the pool", throughPool);
    printLayeringPixel("the lake beside it", lakeOnly);
    EXPECT_NEAR(throughPool.g, lakeOnly.g, 20) << "the pool's copy does not hold the lake under it";
    EXPECT_GT(throughPool.g - throughPool.r, 20);
    EXPECT_NO_ENGINE_ERRORS();
}

// the UI drawn in the camera's pass (a HUD element; here one lying under the water, where the
// water covers it) is not in the scene colour the water refracts: the copy is taken before the
// hierarchy loop's UI, and the transparent queue takes no copy again once the UI drew (a glass
// under the water elsewhere asks for one), including UI draws the batcher still holds
TEST_F(WaterLayeringGpu, UIStaysOutOfTheRefraction){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 5.0f, 3.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive floor(&scene);
    floor.createPlane(20.0f, 20.0f);
    floor.setPosition(0.0f, -1.0f, 0.0f);
    floor.setColor(0.5f, 0.5f, 0.5f);

    Primitive glass(&scene);
    makeRedGlass(glass, 1.5f);
    glass.setPosition(-2.5f, -0.5f, -1.0f);

    // a 2 x 2 green image lying face up under the surface
    Image hud(&scene);
    hud.setTexture("water-layering-hud", greenImage());
    hud.setSize(2, 2);
    hud.setRotation(-90.0f, 0.0f, 0.0f);
    hud.setPosition(0.0f, -0.5f, 0.0f);

    Primitive water(&scene);
    water.createPlane(10.0f, 10.0f);
    water.setMaterial(refractionOnlyWater());

    // where the image is drawn (the water hidden)
    water.setVisible(false);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    int width = 0, height = 0;
    const std::vector<uint8_t> drawn = readLayeringTarget(width, height);
    long sumX = 0, sumY = 0, count = 0;
    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            const uint8_t* p = &drawn[((size_t)y * width + x) * 4];
            if (p[1] > p[0] + 80 && p[1] > p[2] + 80){
                sumX += x;
                sumY += y;
                count++;
            }
        }
    }
    ASSERT_GT(count, 400) << "the image is not drawn";
    const int cx = (int)(sumX / count);
    const int cy = (int)(sumY / count);

    water.setVisible(true);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const std::vector<uint8_t> refracted = readLayeringTarget(width, height);
    LayeringPixel over;
    int samples = 0;
    for (int y = cy - 4; y <= cy + 4; y++){
        for (int x = cx - 4; x <= cx + 4; x++){
            if (x < 0 || y < 0 || x >= width || y >= height)
                continue;
            const uint8_t* p = &refracted[((size_t)y * width + x) * 4];
            over.r += p[0];
            over.g += p[1];
            over.b += p[2];
            samples++;
        }
    }
    ASSERT_GT(samples, 0);
    over.r /= samples;
    over.g /= samples;
    over.b /= samples;
    const LayeringPixel floorOnly = layeringPixelAt(scene, Vector3(3.0f, -1.0f, -1.0f));
    printLayeringPixel("the water over the image", over);
    printLayeringPixel("the water over the floor", floorOnly);
    EXPECT_LT(over.g - floorOnly.g, 20) << "the water refracts the UI";
    EXPECT_LT(over.g - over.r, 20) << "the water refracts the UI";
    EXPECT_NO_ENGINE_ERRORS();
}
