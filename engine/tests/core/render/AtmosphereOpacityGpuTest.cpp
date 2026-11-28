// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CPU opacity estimate (docs/specs/S101-vapor.md R18, AC10) against the GPU's analytic fog: for
// each built-in fog look, a black wall straight ahead of the main camera at 10, 50 and 200 m,
// seen through that look's medium lit by a white fog colour alone, so the pixel is the fog's
// opacity (S = colour (1 - T) over black). AtmosphereDiagnostics::estimateOpacity must match the
// read-back pixel within 0.02. The suite is AtmosphereMathGpu, not the spec's AtmosphereMathTest:
// a suite that needs a GPU ends in Gpu (docs/testing.md, Naming).

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "object/environment/Fog.h"
#include "component/FogComponent.h"
#include "render/AtmosphereDiagnostics.h"
#include "render/AtmospherePresets.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    double opacitySrgbToLinear(double encoded){
        return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
    }

    // the linear red of the 2x2 block at the target's centre, where the camera's forward ray lands
    double centreLinear(){
        Framebuffer& target = GpuEnvironment::target();
        const int width = (int)target.getWidth();
        const int height = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        if (!readTexture(target.getRender().getColorTexture(), width, height, rgba))
            return -1.0;
        double sum = 0.0;
        for (int y = height / 2 - 1; y <= height / 2; y++){
            for (int x = width / 2 - 1; x <= width / 2; x++)
                sum += opacitySrgbToLinear(rgba[((size_t)y * width + x) * 4] / 255.0);
        }
        return sum / 4.0;
    }

}

TEST(AtmosphereMathGpu, OpacityMatchesGpuAnalytic){
    JASPER_REQUIRE_GPU();
    Scene scene;
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
    // nothing lights the wall: its colour is the fog's alone
    scene.setGlobalIllumination(0.0f);
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setFarClip(1000.0f);
    camera.activate();
    Engine::setScene(&scene);

    // looking 5 degrees down, so the ray also crosses the medium's heights
    const float pitch = 5.0f * 3.14159265f / 180.0f;
    const float eye = 3.0f;
    camera.setPosition(0.0f, eye, 0.0f);
    camera.setTarget(0.0f, eye - std::tan(pitch) * 100.0f, -100.0f);

    // a vertical black wall across the view; its front face at depth z of the camera
    Primitive wall(&scene);
    wall.createBox(1200.0f, 1200.0f, 0.2f);
    wall.setColor(0.0f, 0.0f, 0.0f);

    Fog fog(&scene);
    auto render = [&scene](){
        Scene* current = &scene;
        return GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        });
    };

    // without fog the wall is black (the red control of the method: no light reaches it)
    fog.setType(FogType::LINEAR);
    fog.setLinearStartEnd(5000.0f, 6000.0f);
    wall.setPosition(0.0f, eye, -50.0f * std::cos(pitch) - 0.1f);
    ASSERT_TRUE(render());
    EXPECT_LT(centreLinear(), 0.002);

    const float distances[3] = {10.0f, 50.0f, 200.0f};
    for (const AtmospherePreset& preset : AtmospherePresets::getFogPresets()){
        FogComponent& component = fog.getComponent<FogComponent>();
        ASSERT_TRUE(AtmospherePresets::applyFogPreset(preset.id, component));
        // the preset's medium, lit by a white fog colour only
        component.colorMode = FogColorMode::COLOR;
        component.color = Vector3(1.0f, 1.0f, 1.0f);
        component.directionalInscattering = 0.0f;
        component.skyFill = false;
        component.volumetric = false;

        float estimate[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 3; i++){
            // the forward ray meets the wall distances[i] along it
            wall.setPosition(0.0f, eye, -distances[i] * std::cos(pitch) - 0.1f);
            ASSERT_TRUE(render()) << preset.id;
            const AtmosphereOpacity opacity = AtmosphereDiagnostics::estimateOpacity(scene);
            ASSERT_TRUE(opacity.valid);
            estimate[i] = i == 0 ? opacity.at10 : (i == 1 ? opacity.at50 : opacity.at200);
            const double gpu = centreLinear();
            std::printf("[atmosphere] opacity %s at %.0f m: cpu %.4f gpu %.4f\n", preset.id.c_str(), distances[i], estimate[i], gpu);
            EXPECT_NEAR(estimate[i], gpu, 0.02) << preset.id << " at " << distances[i] << " m";
        }
        // the looks differ, and the fog thickens with distance
        EXPECT_LE(estimate[0], estimate[1]) << preset.id;
        EXPECT_LE(estimate[1], estimate[2]) << preset.id;
    }
    Engine::removeScene(&scene);
    EXPECT_NO_ENGINE_ERRORS();
}
