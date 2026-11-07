// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:water's PLANAR_REFLECTION on the headless device (docs/superpowers/specs/
// 2026-11-04-water-reflections-design.md 6.4): the camera's reflection pass mirrors the scene in
// the water, one pass per water height (nearest first up to Max Planes), none with the scene's
// resolution Off (the environment reflection instead), for perspective and orthographic cameras.
// The water is a black mirror (opaque, its reflection unbent), so the pixels it draws are its
// reflection times Schlick's fresnel.

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr double REFLECTION_PINNED_TIME = 2.0;

    struct ReflectionPixel{ int r = 0, g = 0, b = 0; };

    // the mean colour of a 7 x 7 block around where a world point is drawn
    ReflectionPixel reflectionPixelAt(Scene& scene, const Vector3& world){
        Framebuffer& target = GpuEnvironment::target();
        const int width = (int)target.getWidth();
        const int height = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), width, height, rgba);
        const CameraComponent& camera = scene.getComponent<CameraComponent>(scene.getCamera());
        const Vector4 clip = camera.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
        const int cx = (int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * (float)width);
        const int cy = (int)std::lround((0.5f - clip.y / clip.w * 0.5f) * (float)height);
        ReflectionPixel sum;
        int count = 0;
        for (int y = cy - 3; y <= cy + 3; y++){
            for (int x = cx - 3; x <= cx + 3; x++){
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

    // a black, opaque mirror: REFLECTION with PLANAR_REFLECTION, white reflection colour, no foam
    Material mirrorWater(){
        Material material;
        material.name = "Mirror water";
        material.shader = "library:water";
        material.shaderFeatures = {"REFLECTION", "PLANAR_REFLECTION"};
        ShaderUniforms::set(material.shaderParams, "shallowColor", Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        ShaderUniforms::set(material.shaderParams, "deepColor", Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        ShaderUniforms::set(material.shaderParams, "foamAmount", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "reflectionColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        ShaderUniforms::set(material.shaderParams, "reflectionDistortion", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        return material;
    }

    void makeRedCube(Primitive& cube){
        cube.createBox(1.0f, 1.0f, 1.0f);
        Material material;
        material.name = "Red";
        material.shader = "library:unlit";
        ShaderUniforms::set(material.shaderParams, "baseColor", Vector4(1.0f, 0.0f, 0.0f, 1.0f));
        cube.setMaterial(material);
    }

    class WaterReflectionGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            Engine::setPinnedShaderTime(REFLECTION_PINNED_TIME);
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
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

        RenderSystem::PlanarReflectionStats stats(){
            return scene.getSystem<RenderSystem>()->getPlanarReflectionStats();
        }
    };

    void printReflectionPixel(const char* what, const ReflectionPixel& pixel){
        std::printf("[gpu] %s: (%d, %d, %d) on %s\n", what, pixel.r, pixel.g, pixel.b, HeadlessContext::getBackendName());
    }

}

// the red cube above the water shows in it at its mirrored place; Off draws the environment
TEST_F(WaterReflectionGpu, PlanarReflectionMirrorsTheScene){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 0.6f, 8.0f);
    camera.setTarget(0.0f, -0.2f, 0.0f);
    camera.activate();

    Primitive water(&scene);
    water.createPlane(30.0f, 30.0f);
    water.setMaterial(mirrorWater());

    Primitive cube(&scene);
    makeRedCube(cube);
    cube.setPosition(0.0f, 1.0f, 0.0f);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const ReflectionPixel mirrored = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("the cube mirrored", mirrored);
    EXPECT_GT(mirrored.r, 60) << "no reflection of the cube in the water";
    EXPECT_GT(mirrored.r, 2 * std::max(mirrored.g, mirrored.b) + 10);
    const RenderSystem::PlanarReflectionStats drawn = stats();
    EXPECT_EQ(drawn.planes, 1u);
    EXPECT_EQ(drawn.passes, 1u);
    EXPECT_EQ(drawn.fallbacks, 0u);
    // (compareTargetWithGolden writes the actual image on a mismatch)
    const CompareResult golden = compareTargetWithGolden("water_planar_reflection", hardwareTolerance({24, 0.01, 2.0}));
    EXPECT_TRUE(golden.passed) << golden.message;

    // Off: no pass, the environment (without a sky, the reflection colour alone): the pixels
    // REFLECTION alone draws
    scene.setPlanarReflectionResolution(PlanarReflectionResolution::OFF);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const ReflectionPixel off = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("Off", off);
    EXPECT_EQ(stats().passes, 0u);
    Material reflectionAlone = mirrorWater();
    reflectionAlone.shaderFeatures = {"REFLECTION"};
    water.setMaterial(reflectionAlone);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const ReflectionPixel alone = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("REFLECTION alone", alone);
    EXPECT_LE(std::abs(off.r - alone.r), 2) << "Off does not draw what REFLECTION alone draws";
    EXPECT_LE(std::abs(off.g - alone.g), 2);
    EXPECT_LE(std::abs(off.b - alone.b), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

// one pass per height: two tiles at one level share it; past Max Planes the farthest height
// falls back
TEST_F(WaterReflectionGpu, FallbackWhenOffOrCapped){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 3.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive left(&scene);
    left.createPlane(4.0f, 4.0f);
    left.setPosition(-2.5f, 0.0f, 0.0f);
    left.setMaterial(mirrorWater());
    Primitive right(&scene);
    right.createPlane(4.0f, 4.0f);
    right.setPosition(2.5f, 0.0f, 0.0f);
    right.setMaterial(mirrorWater());

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_EQ(stats().planes, 1u) << "two tiles at one level are one height";
    EXPECT_EQ(stats().passes, 1u);

    // two more heights, a cap of two: the farthest from the eye shows the environment
    Primitive lower(&scene);
    lower.createPlane(4.0f, 4.0f);
    lower.setPosition(0.0f, -1.0f, 4.0f);
    lower.setMaterial(mirrorWater());
    Primitive lowest(&scene);
    lowest.createPlane(4.0f, 4.0f);
    lowest.setPosition(0.0f, -2.0f, -4.0f);
    lowest.setMaterial(mirrorWater());
    scene.setMaxPlanarReflections(2);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_EQ(stats().planes, 3u);
    EXPECT_EQ(stats().passes, 2u);
    EXPECT_EQ(stats().fallbacks, 1u);
    // the farthest height shows the environment (grey, the reflection colour alone); a reflected
    // one the empty, black scene
    const ReflectionPixel fallback = reflectionPixelAt(scene, Vector3(0.0f, -2.0f, -4.0f));
    const ReflectionPixel reflected = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 4.0f));
    printReflectionPixel("the capped height", fallback);
    printReflectionPixel("a reflected height", reflected);
    EXPECT_GT(fallback.r, reflected.r + 20) << "the capped height does not show the environment";
    EXPECT_LT(std::abs(fallback.r - fallback.b), 8);

    // seen from below a surface: no pass for it
    camera.setPosition(0.0f, -1.5f, 10.0f);
    camera.setTarget(0.0f, -1.5f, 0.0f);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_EQ(stats().planes, 1u) << "only the lowest water is under the eye";
    EXPECT_NO_ENGINE_ERRORS();
}

// an orthographic camera reflects the same way
TEST_F(WaterReflectionGpu, OrthographicViewer){
    JASPER_REQUIRE_GPU();
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f);
    camera.setPosition(0.0f, 2.0f, 8.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive water(&scene);
    water.createPlane(30.0f, 30.0f);
    water.setMaterial(mirrorWater());

    Primitive cube(&scene);
    makeRedCube(cube);
    cube.setPosition(0.0f, 1.0f, 0.0f);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const ReflectionPixel mirrored = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("the cube mirrored (orthographic)", mirrored);
    EXPECT_EQ(stats().passes, 1u);
    EXPECT_GT(mirrored.r, 2 * std::max(mirrored.g, mirrored.b) + 10) << "no reflection of the cube for an orthographic camera";
    EXPECT_NO_ENGINE_ERRORS();
}
