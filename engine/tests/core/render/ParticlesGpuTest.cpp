// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Particle systems on the headless device (docs/specs/S20-particles.md AC6, AC8): one golden per
// render mode family: the billboard alignments, stretched billboards, mesh particles, frame
// blending, six-way lighting, soft particles with the near fade, and trails and ribbons. Each
// system is seeked to a fixed time with a fixed seed and paused, so every frame draws the same
// particles. Goldens in engine/tests/core/golden/particles_*.png, made on glcore
// (JASPER_TEST_UPDATE_GOLDEN=1) and matched on the other backends within the cross-backend bound.
// The textures are tools/particles/make_test_textures.py's (fixtures/particles/textures). A
// particle shader's custom block gets the engine-written scene clock as a mesh's does.

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/ParticleSystemComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/ParticleSystem.h"
#include "object/Primitive.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the goldens are glcore's; another backend is held to the cross-backend bound
    CompareOptions goldenTolerance(){
        CompareOptions options;
        const bool goldenBackend = std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
        options.pixelThreshold = 16;
        options.maxBadFraction = goldenBackend ? 0.005 : 0.03;
        options.maxMeanDiff = goldenBackend ? 1.0 : 3.0;
        return options;
    }

    class ParticlesGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<ParticleSystem>> systems;
        std::vector<std::unique_ptr<Primitive>> primitives;

        void SetUp() override{
            scene.setBackgroundColor(0.08f, 0.09f, 0.12f, 1.0f);
            scene.setLitQuality(LitQuality::STANDARD);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 1.5f, 9.0f);
            camera.setTarget(0.0f, 1.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            systems.clear();
            Engine::removeScene(&scene);
        }

        // a system at `position` playing `json` (an inline effect), seeked to `time` and paused
        ParticleSystem& system(const Vector3& position, const std::string& json){
            systems.push_back(std::make_unique<ParticleSystem>(&scene));
            ParticleSystem& ps = *systems.back();
            ps.setPosition(position);
            EXPECT_TRUE(ps.setInlineEffect(json)) << json;
            ps.setPlayOnAwake(false);
            return ps;
        }

        Primitive& plane(const Vector3& position, float size, const Vector3& color){
            primitives.push_back(std::make_unique<Primitive>(&scene));
            Primitive& p = *primitives.back();
            p.createPlane(size, size);
            p.setPosition(position.x, position.y, position.z);
            p.setColor(color.x, color.y, color.z);
            return p;
        }

        // the effects bind on the first frame; then each one is seeked and held
        bool render(double time){
            Scene* current = &scene;
            if (!GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }))
                return false;
            for (auto& ps : systems){
                ps->seek((float)time);
                ps->pause();
            }
            return GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); });
        }
    };

    // a burst of particles flying out, seed fixed (the mode and the extra keys of `renderer`)
    std::string burst(const std::string& renderer, const std::string& extraModules = "", int count = 24, float speed = 0.8f){
        return R"({"version": 1, "duration": 5, "looping": false, "seed": 17, "maxParticles": 200, "modules": [
            {"type": "main", "startLifetime": 5, "startSpeed": )" + std::to_string(speed) + R"(, "startSize": 0.45,
             "startColor": [1, 0.75, 0.4, 1]},
            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": )" + std::to_string(count) + R"(}]},
            {"type": "shape", "shape": "sphere", "radius": 0.3})" + extraModules + R"(,
            {"type": "renderer", )" + renderer + R"(}]})";
    }

}

TEST_F(ParticlesGpu, BillboardModes){
    JASPER_REQUIRE_GPU();
    const char* modes[] = {"billboard", "facingCameraPosition", "horizontalBillboard", "verticalBillboard", "worldOriented"};
    for (int i = 0; i < 5; i++){
        system(Vector3(-4.0f + 2.0f * (float)i, 1.0f, 0.0f),
               burst(std::string(R"("renderMode": ")") + modes[i] + R"(", "texture": "particles/textures/spark.png", "blend": "additive")"));
    }
    ASSERT_TRUE(render(1.0));
    EXPECT_GOLDEN_OPTIONS("particles_billboard_modes", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, StretchedAndUntextured){
    JASPER_REQUIRE_GPU();
    // stretched along their velocity, and the soft disc / square a billboard draws without a texture
    system(Vector3(-2.5f, 1.0f, 0.0f), burst(R"("renderMode": "stretchedBillboard", "speedScale": 0.4, "lengthScale": 1.5,
        "texture": "particles/textures/spark.png", "blend": "additive")", "", 24, 3.0f));
    system(Vector3(1.0f, 1.0f, 0.0f), burst(R"("renderMode": "billboard")"));
    system(Vector3(3.5f, 1.0f, 0.0f), burst(R"("renderMode": "billboard", "untexturedShape": "square")"));
    ASSERT_TRUE(render(0.6));
    EXPECT_GOLDEN_OPTIONS("particles_stretched", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, MeshParticles){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -0.8f, -0.5f);
    sun.setIntensity(3.0f);
    sun.setShadows(false);
    system(Vector3(-1.5f, 1.0f, 0.0f), burst(R"("renderMode": "mesh", "meshes": [{"type": "primitive", "primitive": "cube"}],
        "lighting": "lit")", R"(, {"type": "rotationOverLifetime", "angularVelocity": 90})", 16));
    system(Vector3(1.5f, 1.0f, 0.0f), burst(R"("renderMode": "mesh", "meshes": [{"type": "primitive", "primitive": "sphere"},
        {"type": "primitive", "primitive": "torus"}], "meshSelection": "index", "lighting": "lit")", "", 16));
    ASSERT_TRUE(render(1.2));
    EXPECT_GOLDEN_OPTIONS("particles_mesh", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, FrameBlending){
    JASPER_REQUIRE_GPU();
    // the same sheet without and with frame blending, half-way between frames 5 and 6 (4 frames a
    // second at the age of 1.375 s): the left shows frame 5, the right the mix
    const std::string sheet = R"(, {"type": "textureSheetAnimation", "tilesX": 4, "tilesY": 4, "timeMode": "fps", "fps": 4)";
    system(Vector3(-2.0f, 1.0f, 0.0f), burst(R"("renderMode": "billboard", "texture": "particles/textures/sheet.png")", sheet + "}", 12, 0.5f));
    system(Vector3(2.0f, 1.0f, 0.0f), burst(R"("renderMode": "billboard", "texture": "particles/textures/sheet.png")",
                                            sheet + R"(, "frameBlending": true})", 12, 0.5f));
    ASSERT_TRUE(render(1.375));
    EXPECT_GOLDEN_OPTIONS("particles_frame_blend", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, SixWayLighting){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(1.0f, -0.3f, -0.2f);
    sun.setColor(1.0f, 0.8f, 0.6f);
    sun.setIntensity(4.0f);
    sun.setShadows(false);
    system(Vector3(0.0f, 1.0f, 0.0f), burst(R"("renderMode": "billboard", "lighting": "sixWay", "sixWayLayout": "rgba",
        "sixWayMapA": "particles/textures/sixway_a.png", "sixWayMapB": "particles/textures/sixway_b.png", "blend": "alpha",
        "sortMode": "byDistance")", "", 20, 0.6f));
    ASSERT_TRUE(render(1.0));
    EXPECT_GOLDEN_OPTIONS("particles_sixway", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, SoftParticlesAndNearFade){
    JASPER_REQUIRE_GPU();
    // big particles cutting a floor: soft fades where they meet it; the near fade where they reach the camera
    plane(Vector3(0.0f, 0.8f, 0.0f), 20.0f, Vector3(0.4f, 0.45f, 0.5f));
    system(Vector3(-1.5f, 0.8f, 0.0f), burst(R"("renderMode": "billboard", "texture": "particles/textures/spark.png",
        "shader": "library:particles", "shaderFeatures": ["SOFT"], "shaderProperties": [{"name": "softness", "value": [0.6, 0, 0, 0]}])",
        "", 8, 0.3f));
    system(Vector3(1.5f, 1.2f, 6.5f), burst(R"("renderMode": "billboard", "texture": "particles/textures/spark.png",
        "shader": "library:particles", "shaderFeatures": ["NEAR_FADE"], "shaderProperties": [{"name": "nearFadeStart", "value": [1.0, 0, 0, 0]}, {"name": "nearFadeEnd", "value": [3.0, 0, 0, 0]}])",
        "", 8, 0.3f));
    ASSERT_TRUE(render(1.0));
    EXPECT_GOLDEN_OPTIONS("particles_soft_nearfade", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, TrailsAndRibbons){
    JASPER_REQUIRE_GPU();
    const std::string gravity = R"(, {"type": "forceOverLifetime", "y": -1.5})";
    system(Vector3(-2.0f, 1.5f, 0.0f), burst(R"("renderMode": "billboard", "texture": "particles/textures/spark.png", "blend": "additive")",
        gravity + R"(, {"type": "trails", "mode": "perParticle", "lifetime": 0.6, "widthOverTrail": {"mode": "curve", "curve": [[0, 1], [1, 0]]}})",
        10, 2.0f));
    system(Vector3(2.0f, 1.5f, 0.0f), burst(R"("renderMode": "none", "blend": "additive")",
        gravity + R"(, {"type": "trails", "mode": "ribbon", "ribbonCount": 1, "lifetime": 1.0})", 10, 2.0f));
    ASSERT_TRUE(render(1.0));
    EXPECT_GOLDEN_OPTIONS("particles_trails", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, LongRibbonDrawsEveryParticle){
    JASPER_REQUIRE_GPU();
    // a ribbon of ~110 particles rising from y = -1 to y = 2.7: more than a per-particle trail's
    // points, so its triangles must come from the ribbon's own layout (they once covered only the
    // newest 17 particles, and WebGL refused the draw)
    system(Vector3(0.0f, -1.0f, 0.0f), R"({"version": 1, "duration": 10, "looping": true, "seed": 3, "maxParticles": 200, "modules": [
        {"type": "main", "startLifetime": 4, "startSpeed": 1, "startSize": 0.3, "startColor": [1, 0.75, 0.4, 1]},
        {"type": "emission", "rateOverTime": 30},
        {"type": "shape", "shape": "cone", "angle": 0, "radius": 0},
        {"type": "trails", "mode": "ribbon", "ribbonCount": 1, "lifetime": 1.0},
        {"type": "renderer", "renderMode": "none", "blend": "additive"}]})");
    ASSERT_TRUE(render(3.7));
    EXPECT_NO_ENGINE_ERRORS();

    Framebuffer& target = GpuEnvironment::target();
    const int width = (int)target.getWidth(), height = (int)target.getHeight();
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
    // lit pixels in the central columns, above and below the image's middle (world y = 1)
    auto litRows = [&](int fromRow, int toRow){
        int rows = 0;
        for (int y = fromRow; y < toRow; y++){
            for (int x = width / 2 - width / 20; x <= width / 2 + width / 20; x++){
                if (rgba[((size_t)y * width + x) * 4] > 120){
                    rows++;
                    break;
                }
            }
        }
        return rows;
    };
    const int upper = litRows(0, height / 2), lower = litRows(height / 2, height);
    EXPECT_GT(lower, height / 10) << "the ribbon's newest part, near the emitter";
    EXPECT_GT(upper, height / 10) << "the ribbon's oldest part, far from the emitter (" << upper << " lit rows)";
}

TEST_F(ParticlesGpu, PlayOnAwakeSystemsShowWithoutASeek){
    JASPER_REQUIRE_GPU();
    // a looping system that plays on awake in a running scene (no seek, no editor): after some
    // frames its particles are on screen (an exported game's case)
    systems.push_back(std::make_unique<ParticleSystem>(&scene));
    ParticleSystem& ps = *systems.back();
    ps.setPosition(Vector3(0.0f, 1.0f, 0.0f));
    ASSERT_TRUE(ps.setInlineEffect(R"({"version": 1, "duration": 2, "seed": 3, "maxParticles": 400, "modules": [
        {"type": "main", "startLifetime": 3, "startSpeed": 0.3, "startSize": 0.6, "startColor": [1, 0.75, 0.4, 1]},
        {"type": "emission", "rateOverTime": 400},
        {"type": "renderer", "renderMode": "billboard", "blend": "additive"}]})"));
    Scene* current = &scene;
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
    // frames at the real clock (a test's are short): until it has a crowd, then more to draw it
    ASSERT_TRUE(GpuEnvironment::runFrames([&ps](){ return ps.getParticleCount() >= 50; }, 20000)) << ps.getParticleCount();
    int frames = 0;
    ASSERT_TRUE(GpuEnvironment::runFrames([&frames](){ return ++frames >= 5; }));
    Framebuffer& target = GpuEnvironment::target();
    const int width = (int)target.getWidth(), height = (int)target.getHeight();
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
    int lit = 0;
    for (size_t i = 0; i < rgba.size(); i += 4){
        if (rgba[i] > 120) lit++;
    }
    EXPECT_GT(lit, 100) << "its particles are drawn";
}

// a particle shader's custom block holds the scene clock (sceneTime), as every custom block does:
// it holds while the scene is paused and is the scene's, not the frame clock
TEST_F(ParticlesGpu, CustomBlockReadsTheSceneClock){
    JASPER_REQUIRE_GPU();
    Engine::setPinnedShaderTime(-1.0);
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
    system(Vector3(0.0f, 1.0f, 0.0f), R"({"version": 1, "duration": 5, "looping": false, "seed": 17, "maxParticles": 4, "modules": [
        {"type": "main", "startLifetime": 5, "startSpeed": 0, "startSize": 2, "startColor": [1, 1, 1, 1]},
        {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
        {"type": "shape", "shape": "sphere", "radius": 0.01},
        {"type": "renderer", "renderMode": "billboard", "shader": "particles/scene_clock.frag"}]})");

    // the red the shader draws at the particle's centre (0..255)
    auto clockRed = [this](){
        Framebuffer& target = GpuEnvironment::target();
        const int width = (int)target.getWidth(), height = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
        const CameraComponent& view = scene.getComponent<CameraComponent>(scene.getCamera());
        const Vector4 clip = view.viewProjectionMatrix * Vector4(0.0f, 1.0f, 0.0f, 1.0f);
        const int cx = (int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * (float)width);
        const int cy = (int)std::lround((0.5f - clip.y / clip.w * 0.5f) * (float)height);
        int sum = 0, count = 0;
        for (int y = cy - 2; y <= cy + 2; y++){
            for (int x = cx - 2; x <= cx + 2; x++){
                if (x < 0 || y < 0 || x >= width || y >= height)
                    continue;
                sum += rgba[((size_t)y * width + x) * 4];
                count++;
            }
        }
        return count > 0 ? sum / count : -1;
    };

    ASSERT_TRUE(render(1.0));
    scene.setSceneTimePaused(true);
    scene.setSceneTime(0.25);
    GpuEnvironment::drawFrames(2);
    const int quarter = clockRed();
    GpuEnvironment::drawFrames(3);
    const int held = clockRed();
    scene.setSceneTime(0.75);
    GpuEnvironment::drawFrames(2);
    const int threeQuarters = clockRed();
    std::printf("[gpu] the particle shader's sceneTime as red: %d at 0.25, %d three frames later, %d at 0.75 (frame clock %.2f s)\n",
                quarter, held, threeQuarters, Engine::getShaderTime());
    EXPECT_NEAR(quarter, 64, 3) << "the particle's custom block does not hold the scene clock";
    EXPECT_NEAR(held, 64, 3) << "the scene clock moved while the scene is paused";
    EXPECT_NEAR(threeQuarters, 191, 3) << "the particle's custom block does not follow the scene clock";
    EXPECT_NO_ENGINE_ERRORS();
}
