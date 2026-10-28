// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:water's spline features on the headless device (docs/specs/S18-splines.md D9, AC15).
// The shader clock is pinned (Engine::setPinnedShaderTime), so the ripples drift to the same
// place every run.
//
// water_features_off: a flat plane of a typical material (BUILTIN_TEXTURES, lit per pixel, the
// default values, no scene-depth feature) over a ground plane. Its golden was captured on glcore
// from the shader before FLOW and MESH_DEPTH, and glcore must still match it exactly: with both
// features off their code is preprocessed away and the uniform block only grows at its end, so
// the variant computes the same values in the same order. Another backend is held to the
// cross-backend bound against the same glcore image.

#include "Engine.h"
#include "Scene.h"
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

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the shader time of every golden here (seconds)
    constexpr double PINNED_TIME = 7.25;

    bool isGoldenBackend(){
        return std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
    }

    // glcore against its own goldens: exact; another backend: the cross-backend bound
    CompareOptions exactOnGolden(){
        CompareOptions options;
        if (isGoldenBackend()){
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

    // a library:water material as a scene has it: the built-in ripples and foam noise, lit per
    // pixel, every value at its default
    Material waterMaterial(const std::vector<std::string>& features){
        Material material;
        material.name = "Water";
        material.shader = "library:water";
        material.shaderFeatures = features;
        material.lighting = MaterialLighting::PIXEL;
        return material;
    }

    class WaterFeaturesGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};

        void SetUp() override{
            Engine::setPinnedShaderTime(PINNED_TIME);
            scene.setBackgroundColor(0.12f, 0.14f, 0.18f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.4f, -1.0f, -0.5f);
            sun.setIntensity(1.5f);
            sun.setShadows(false);
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

        void expectGolden(const std::string& name){
            const CompareResult result = compareTargetWithGolden(name, exactOnGolden());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }
    };

}

TEST_F(WaterFeaturesGpu, FeaturesOffMatchesTheShaderBeforeThem){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 3.0f, 7.0f);
    camera.setTarget(0.0f, 0.0f, -2.0f);
    camera.activate();

    // a sandy bed under part of the water, so its alpha shows over both it and the background
    Primitive ground(&scene);
    ground.createPlane(8.0f, 8.0f);
    ground.setPosition(-2.0f, -0.4f, -1.0f);
    ground.setColor(0.55f, 0.45f, 0.3f);

    Primitive water(&scene);
    water.createPlane(24.0f, 24.0f);
    water.setMaterial(waterMaterial({"BUILTIN_TEXTURES"}));

    ASSERT_TRUE(render()) << "the scene never finished loading";
    expectGolden("water_features_off");
    EXPECT_NO_ENGINE_ERRORS();
}
