// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Textures without an alpha channel on additive particles (docs/specs/S94-rendering-anim-gaps.md
// F3, AC3; the port's R29): one white glow as RGBA (its disc in alpha), RGB and greyscale (the
// disc in the colour, no alpha), each on one additive particle over black. Each draws white at
// its centre: every colour channel, not only red. The textures load as a project's do: the
// Texture Importer's settings of a texture nobody configured (type Default, sRGB, not Read/Write),
// which the editor applies and an exported game is cooked with. The textures are
// tools/particles/make_test_textures.py's; golden engine/tests/core/golden/particles_additive_formats.png.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/ParticleSystem.h"
#include "pool/TextureDataPool.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace ParticleRenderGpuTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    CompareOptions additiveTolerance(){
        CompareOptions options;
        const bool goldenBackend = std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
        options.pixelThreshold = 16;
        options.maxBadFraction = goldenBackend ? 0.005 : 0.03;
        options.maxMeanDiff = goldenBackend ? 1.0 : 3.0;
        return options;
    }

    // one still white particle of size 2 at the system, additive, with `texture`
    std::string oneAdditive(const std::string& texture){
        return R"({"version": 1, "duration": 5, "looping": false, "seed": 1, "maxParticles": 4, "modules": [
            {"type": "main", "startLifetime": 100, "startSpeed": 0, "startSize": 2, "startColor": [1, 1, 1, 1]},
            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
            {"type": "shape", "enabled": false},
            {"type": "renderer", "texture": ")" + texture + R"(", "blend": "additive"}]})";
    }

}

TEST(ParticleRenderGpu, AdditiveRgbAndGreyTexturesDraw){
    JASPER_REQUIRE_GPU();
    Scene scene;
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
    Camera camera(&scene);
    // 8 world units across the target: the particles at x -2.5, 0 and 2.5
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-4.0f, 4.0f, -2.25f, 2.25f, 0.1f, 100.0f);
    camera.setPosition(0.0f, 0.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();
    Engine::setScene(&scene);

    const char* textures[] = {"particles/textures/glow_rgba.png", "particles/textures/glow_rgb.png", "particles/textures/glow_grey.png"};
    std::vector<std::unique_ptr<ParticleSystem>> systems;
    // the scene leaves the engine when the test does, an ASSERT's return too
    struct Leave{
        Scene* scene;
        std::vector<std::unique_ptr<ParticleSystem>>& systems;
        ~Leave(){ systems.clear(); Engine::removeScene(scene); TextureDataPool::setProcessingProvider(nullptr); }
    } leave{&scene, systems};
    // the importer's defaults for these three (TextureCookQueue::processingFor in the editor)
    TextureDataPool::setProcessingProvider([](const std::string& path, TextureProcessing& out){
        if (path.find("particles/textures/glow_") == std::string::npos)
            return false;
        out = TextureProcessing();
        return true;
    });

    for (int i = 0; i < 3; i++){
        systems.push_back(std::make_unique<ParticleSystem>(&scene));
        ParticleSystem& ps = *systems.back();
        ps.setPosition(-2.5f + 2.5f * (float)i, 0.0f, 0.0f);
        ps.setPlayOnAwake(false);
        ASSERT_TRUE(ps.setInlineEffect(oneAdditive(textures[i])));
    }

    Scene* current = &scene;
    // the effects bind on the first frame; then each is seeked past its burst and held (the
    // headless frames advance no game time)
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
    for (auto& ps : systems){
        ps->seek(0.5f);
        ps->pause();
    }
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));

    Framebuffer& target = GpuEnvironment::target();
    const int width = (int)target.getWidth();
    const int height = (int)target.getHeight();
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));

    for (int i = 0; i < 3; i++){
        const int x = (int)((0.5f + (-2.5f + 2.5f * (float)i) / 8.0f) * (float)width);
        const int y = height / 2;
        const uint8_t* px = &rgba[((size_t)y * (size_t)width + (size_t)x) * 4];
        for (int c = 0; c < 3; c++){
            EXPECT_GT(px[c], 127) << textures[i] << ": channel " << c << " at its centre (" << (int)px[0] << ", " << (int)px[1] << ", " << (int)px[2] << ")";
        }
    }
    EXPECT_GOLDEN_OPTIONS("particles_additive_formats", additiveTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

}  // namespace ParticleRenderGpuTest_cpp
