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
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
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

    // puts the frame clock and the time scale back when the test leaves, an ASSERT's return too
    struct ParticleClockRestore{
        ~ParticleClockRestore(){
            Engine::setTimeScale(1.0f);
            Engine::setFixedDeltatime(0.0);
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

TEST_F(ParticlesGpu, DeathSubEmitterChildrenDraw){
    JASPER_REQUIRE_GPU();
    // BF23 F1: the parents die at 0.5 s, so at 1.5 s all that is left on screen is the children
    // their deaths spawned, each drawn with the child effect's own Renderer (green)
    particles::ParticleEffectDocument child;
    std::vector<particles::Diagnostic> diagnostics;
    ASSERT_TRUE(particles::ParticleEffectCodec::read(R"({"version": 1, "duration": 5, "looping": false, "maxParticles": 200, "modules": [
        {"type": "main", "startLifetime": 5, "startSpeed": 0.5, "startSize": 0.5, "startColor": [0.1, 1, 0.2, 1], "gravityModifier": 0},
        {"type": "emission", "rateOverTime": 0},
        {"type": "shape", "shape": "sphere", "radius": 0.1},
        {"type": "renderer", "renderMode": "billboard", "untexturedShape": "square"}]})", child, diagnostics));
    particles::ParticleEffectLibrary::set("fx/bf23-death-child.particles", child);
    ParticleSystem& ps = system(Vector3(0.0f, 1.0f, 0.0f), R"({"version": 1, "duration": 5, "looping": false, "seed": 17, "maxParticles": 50,
        "subEmitters": [{"trigger": "death", "effect": "fx/bf23-death-child.particles", "emitCount": 6}],
        "modules": [
            {"type": "main", "startLifetime": 0.5, "startSpeed": 2, "startSize": 0.2, "startColor": [1, 0.1, 0.1, 1], "gravityModifier": 0},
            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 4}]},
            {"type": "shape", "shape": "sphere", "radius": 0.2},
            {"type": "subEmitters"},
            {"type": "renderer", "renderMode": "billboard"}]})");
    ASSERT_TRUE(render(1.5));
    EXPECT_NO_ENGINE_ERRORS();

    const particles::ParticleSystemState* state = scene.getSystem<ParticleSubsystem>()->getState(ps.getEntity());
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->runtime.count(), 0u) << "the parents are dead by now";
    ASSERT_EQ(state->runtime.childCount(), 1u);
    ASSERT_GT(state->runtime.child(0)->count(), 0u) << "the children are alive in the simulation";

    Framebuffer& target = GpuEnvironment::target();
    const int width = (int)target.getWidth(), height = (int)target.getHeight();
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
    int green = 0;
    for (size_t i = 0; i < rgba.size(); i += 4){
        if (rgba[i + 1] > 120 && rgba[i] < rgba[i + 1] / 2) green++;
    }
    EXPECT_GT(green, 100) << "the children are drawn (" << green << " green pixels)";
    particles::ParticleEffectLibrary::clear();
}

TEST_F(ParticlesGpu, SubEmittersDrawWithTheirOwnRenderers){
    JASPER_REQUIRE_GPU();
    // AC3's golden: each child has its Renderer. Left: a Death child, untextured squares. Middle:
    // a parent that draws nothing, Birth children as lit-less cubes. Right: two levels, the
    // second additive sparks with trails. A last system, without sub-emitters, makes nothing new
    auto define = [](const char* path, const std::string& json){
        particles::ParticleEffectDocument document;
        std::vector<particles::Diagnostic> diagnostics;
        ASSERT_TRUE(particles::ParticleEffectCodec::read(json, document, diagnostics)) << particles::ParticleEffectCodec::format(diagnostics);
        particles::ParticleEffectLibrary::set(path, document);
    };
    const std::string kid = R"({"version": 1, "duration": 5, "looping": false, "seed": 5, "maxParticles": 200, "modules": [
        {"type": "main", "startLifetime": 5, "startSpeed": 0.6, "startSize": 0.3, "startColor": [%COLOR%], "gravityModifier": 0},
        {"type": "emission", "rateOverTime": 0},
        {"type": "shape", "shape": "sphere", "radius": 0.1}%EXTRA%,
        {"type": "renderer", %RENDERER%}]})";
    auto effect = [&kid](const std::string& color, const std::string& extra, const std::string& renderer){
        std::string json = kid;
        json.replace(json.find("%COLOR%"), 7, color);
        json.replace(json.find("%EXTRA%"), 7, extra);
        json.replace(json.find("%RENDERER%"), 10, renderer);
        return json;
    };
    define("fx/bf23-squares.particles", effect("0.1, 1, 0.2, 1", "", R"("renderMode": "billboard", "untexturedShape": "square")"));
    define("fx/bf23-cubes.particles", effect("0.2, 0.5, 1, 1", "", R"("renderMode": "mesh", "meshes": [{"type": "primitive", "primitive": "cube"}])"));
    define("fx/bf23-sparks.particles", effect("1, 0.8, 0.3, 1", R"(, {"type": "trails", "mode": "perParticle", "lifetime": 0.4})",
                                              R"("renderMode": "billboard", "texture": "particles/textures/spark.png", "blend": "additive")"));
    define("fx/bf23-middle.particles", R"({"version": 1, "duration": 5, "looping": false, "seed": 6, "maxParticles": 50,
        "subEmitters": [{"trigger": "death", "effect": "fx/bf23-sparks.particles", "emitCount": 3}],
        "modules": [{"type": "main", "startLifetime": 0.3, "startSpeed": 1, "startSize": 0.2, "startColor": [1, 0.2, 0.8, 1], "gravityModifier": 0},
                    {"type": "emission", "rateOverTime": 0},
                    {"type": "subEmitters"},
                    {"type": "renderer", "renderMode": "billboard"}]})");
    auto parent = [](const std::string& trigger, const std::string& child, float lifetime, const std::string& renderer){
        return R"({"version": 1, "duration": 5, "looping": false, "seed": 17, "maxParticles": 50,
            "subEmitters": [{"trigger": ")" + trigger + R"(", "effect": ")" + child + R"(", "emitCount": 4}],
            "modules": [
                {"type": "main", "startLifetime": )" + std::to_string(lifetime) + R"(, "startSpeed": 1.5, "startSize": 0.2, "startColor": [1, 0.1, 0.1, 1], "gravityModifier": 0},
                {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 4}]},
                {"type": "shape", "shape": "sphere", "radius": 0.2},
                {"type": "subEmitters"},
                {"type": "renderer", )" + renderer + R"(}]})";
    };
    system(Vector3(-3.5f, 1.0f, 0.0f), parent("death", "fx/bf23-squares.particles", 0.5f, R"("renderMode": "billboard")"));
    system(Vector3(0.0f, 1.0f, 0.0f), parent("birth", "fx/bf23-cubes.particles", 5.0f, R"("renderMode": "none")"));
    system(Vector3(3.5f, 1.0f, 0.0f), parent("death", "fx/bf23-middle.particles", 0.3f, R"("renderMode": "billboard")"));
    ParticleSystem& plain = system(Vector3(0.0f, 3.2f, 0.0f), burst(R"("renderMode": "billboard")", "", 6, 0.3f));
    ASSERT_TRUE(render(1.25));
    EXPECT_GOLDEN_OPTIONS("particles_sub_emitters", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();

    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    const particles::ParticleSystemState* last = subsystem.getState(systems[2]->getEntity());
    ASSERT_NE(last, nullptr);
    ASSERT_EQ(last->render.children.size(), 1u);
    ASSERT_NE(last->render.children[0], nullptr);
    ASSERT_EQ(last->render.children[0]->children.size(), 1u);
    EXPECT_TRUE(last->render.children[0]->children[0]->loaded);
    const particles::ParticleSystemState* none = subsystem.getState(plain.getEntity());
    ASSERT_NE(none, nullptr);
    EXPECT_TRUE(none->render.children.empty()) << "no sub-emitters: nothing allocated for them";
    particles::ParticleEffectLibrary::clear();
}

TEST_F(ParticlesGpu, ChildrenKeepTheirSystemInView){
    JASPER_REQUIRE_GPU();
    // children that flew far from their dead parents: the system's bounds hold them, so a camera
    // that only sees them marks it visible and draws them (its culling would pause it otherwise)
    particles::ParticleEffectDocument child;
    std::vector<particles::Diagnostic> diagnostics;
    ASSERT_TRUE(particles::ParticleEffectCodec::read(R"({"version": 1, "duration": 5, "looping": false, "maxParticles": 50, "modules": [
        {"type": "main", "startLifetime": 20, "startSpeed": 0, "startSize": 1, "startColor": [0.1, 1, 0.2, 1], "gravityModifier": 0},
        {"type": "emission", "rateOverTime": 0},
        {"type": "renderer", "renderMode": "billboard", "untexturedShape": "square"}]})", child, diagnostics));
    particles::ParticleEffectLibrary::set("fx/bf23-far-child.particles", child);
    ParticleSystem& ps = system(Vector3(-200.0f, 1.0f, 0.0f), R"({"version": 1, "duration": 5, "looping": false, "seed": 9, "maxParticles": 50,
        "simulationSpace": "world",
        "subEmitters": [{"trigger": "death", "effect": "fx/bf23-far-child.particles", "emitCount": 1}],
        "modules": [
            {"type": "main", "startLifetime": 1, "startSpeed": 0, "startSize": 0.1, "gravityModifier": 0},
            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
            {"type": "shape", "enabled": false},
            {"type": "startVelocity", "min": [200, 0, 0], "max": [200, 0, 0]},
            {"type": "subEmitters"},
            {"type": "renderer", "renderMode": "billboard"}]})");
    ASSERT_TRUE(render(1.5));
    EXPECT_NO_ENGINE_ERRORS();
    const particles::ParticleSystemState* state = scene.getSystem<ParticleSubsystem>()->getState(ps.getEntity());
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->runtime.count(), 0u);
    ASSERT_EQ(state->runtime.child(0)->count(), 1u);
    ASSERT_TRUE(state->hasBounds);
    EXPECT_GT(state->boundsMax[0], -50.0f) << "the box follows the children, not the emitter";
    EXPECT_GT(state->drawnViews, 0u);
    particles::ParticleEffectLibrary::clear();
}

namespace {

    // the pixel at the middle of the target (world y = 1 in front of the camera)
    bool centrePixel(uint8_t out[4]){
        Framebuffer& target = GpuEnvironment::target();
        const int width = (int)target.getWidth(), height = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        if (!readTexture(target.getRender().getColorTexture(), width, height, rgba))
            return false;
        std::memcpy(out, &rgba[((size_t)(height / 2) * width + width / 2) * 4], 4);
        return true;
    }

    void defineEffect(const char* path, const std::string& json){
        particles::ParticleEffectDocument document;
        std::vector<particles::Diagnostic> diagnostics;
        ASSERT_TRUE(particles::ParticleEffectCodec::read(json, document, diagnostics)) << particles::ParticleEffectCodec::format(diagnostics);
        particles::ParticleEffectLibrary::set(path, document);
    }

    // one big opaque square at the emitter, or `speedZ` along z
    std::string square(const std::string& color, const std::string& renderer = "", float speedZ = 0.0f){
        const std::string velocity = std::to_string(speedZ);
        return R"({"version": 1, "duration": 5, "looping": false, "seed": 3, "maxParticles": 10, "modules": [
            {"type": "main", "startLifetime": 100, "startSpeed": 0, "startSize": 3, "startColor": [)" + color + R"(], "gravityModifier": 0},
            {"type": "emission", "rateOverTime": 0},
            {"type": "shape", "enabled": false},
            {"type": "startVelocity", "min": [0, 0, )" + velocity + R"(], "max": [0, 0, )" + velocity + R"(]},
            {"type": "renderer", "renderMode": "billboard", "untexturedShape": "square")" + renderer + R"(}]})";
    }

    // one particle at the emitter, drawing as `renderer`, that spawns one child of `effect` at birth
    std::string parentOf(const std::string& effect, const std::string& renderer){
        return R"({"version": 1, "duration": 5, "looping": false, "seed": 3, "maxParticles": 10,
            "subEmitters": [{"trigger": "birth", "effect": ")" + effect + R"(", "emitCount": 1}],
            "modules": [
                {"type": "main", "startLifetime": 100, "startSpeed": 0, "startSize": 3, "startColor": [0, 1, 0, 1], "gravityModifier": 0},
                {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
                {"type": "shape", "enabled": false},
                {"type": "subEmitters"},
                {"type": "renderer", )" + renderer + R"(}]})";
    }

}

TEST_F(ParticlesGpu, AChildsSortPriorityPutsItOverAnotherSystem){
    JASPER_REQUIRE_GPU();
    // farther than the green system, and yet last: Sort Priority comes before distance
    defineEffect("fx/bf23-priority.particles", square("1, 0, 0, 1", R"(, "sortPriority": 10)"));
    system(Vector3(0.0f, 1.0f, -2.0f), parentOf("fx/bf23-priority.particles", R"("renderMode": "none")"));
    system(Vector3(0.0f, 1.0f, 0.0f), R"({"version": 1, "duration": 5, "looping": false, "seed": 3, "maxParticles": 10, "modules": [
        {"type": "main", "startLifetime": 100, "startSpeed": 0, "startSize": 3, "startColor": [0, 1, 0, 1], "gravityModifier": 0},
        {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
        {"type": "shape", "enabled": false},
        {"type": "renderer", "renderMode": "billboard", "untexturedShape": "square"}]})");
    ASSERT_TRUE(render(0.5));
    EXPECT_NO_ENGINE_ERRORS();
    uint8_t pixel[4];
    ASSERT_TRUE(centrePixel(pixel));
    EXPECT_GT(pixel[0], 150) << "the child is on top: " << (int)pixel[0] << "," << (int)pixel[1];
    EXPECT_LT(pixel[1], 80);
    particles::ParticleEffectLibrary::clear();
}

TEST_F(ParticlesGpu, AChildBehindItsParentDrawsFirst){
    JASPER_REQUIRE_GPU();
    // blended without depth writes, the farther has to draw first: here the child, flying away
    defineEffect("fx/bf23-behind.particles", square("1, 0, 0, 1", "", -3.0f));
    system(Vector3(0.0f, 1.0f, 0.0f), parentOf("fx/bf23-behind.particles", R"("renderMode": "billboard", "untexturedShape": "square")"));
    ASSERT_TRUE(render(1.0));
    EXPECT_NO_ENGINE_ERRORS();
    uint8_t pixel[4];
    ASSERT_TRUE(centrePixel(pixel));
    EXPECT_GT(pixel[1], 150) << "the parent is on top: " << (int)pixel[0] << "," << (int)pixel[1];
    EXPECT_LT(pixel[0], 80);
    particles::ParticleEffectLibrary::clear();
}

TEST_F(ParticlesGpu, EditingAChildsFileShowsOnScreen){
    JASPER_REQUIRE_GPU();
    defineEffect("fx/bf23-edit.particles", square("1, 0, 0, 1"));
    system(Vector3(0.0f, 1.0f, 0.0f), parentOf("fx/bf23-edit.particles", R"("renderMode": "none")"));
    ASSERT_TRUE(render(0.5));
    uint8_t pixel[4];
    ASSERT_TRUE(centrePixel(pixel));
    ASSERT_GT(pixel[0], 150);
    ASSERT_LT(pixel[2], 80);
    defineEffect("fx/bf23-edit.particles", square("0, 0, 1, 1"));
    ASSERT_TRUE(render(0.5)) << "seeks the system again, as capture_particles does";
    EXPECT_NO_ENGINE_ERRORS();
    ASSERT_TRUE(centrePixel(pixel));
    EXPECT_GT(pixel[2], 150) << "the edit shows: " << (int)pixel[0] << "," << (int)pixel[2];
    EXPECT_LT(pixel[0], 80);
    particles::ParticleEffectLibrary::clear();
}

TEST_F(ParticlesGpu, ASystemPlaysAgainAfterTheViewComesBack){
    JASPER_REQUIRE_GPU();
    // Android's pause under low memory, or a lost context: the scenes destroy their systems and
    // keep their components
    systems.push_back(std::make_unique<ParticleSystem>(&scene));
    ParticleSystem& ps = *systems.back();
    ps.setPosition(Vector3(0.0f, 1.0f, 0.0f));
    ASSERT_TRUE(ps.setInlineEffect(R"({"version": 1, "duration": 2, "seed": 3, "maxParticles": 400, "modules": [
        {"type": "main", "startLifetime": 3, "startSpeed": 0.3, "startSize": 0.6, "startColor": [1, 0.75, 0.4, 1]},
        {"type": "emission", "rateOverTime": 400},
        {"type": "renderer", "renderMode": "billboard", "blend": "additive"}]})"));
    Scene* current = &scene;
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
    ASSERT_TRUE(GpuEnvironment::runFrames([&ps](){ return ps.getParticleCount() >= 50; }, 20000)) << ps.getParticleCount();

    Engine::systemViewDestroyed();
    Engine::systemViewLoaded();
    Engine::systemViewChanged();
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
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
    EXPECT_GT(lit, 100) << "its particles are drawn again";
}

// ---------------------------------------------------------------- S91 (docs/specs/S91-effect-authoring.md)

namespace {

    // upright coloured slabs behind the particles, lit: the scene a distortion bends
    void stripes(Scene& scene, std::vector<std::unique_ptr<Primitive>>& primitives){
        for (int i = 0; i < 12; i++){
            primitives.push_back(std::make_unique<Primitive>(&scene));
            Primitive& p = *primitives.back();
            p.createBox(0.5f, 6.0f, 0.2f);
            p.setPosition(-2.75f + 0.5f * (float)i, 1.0f, -2.0f);
            if (i % 2) p.setColor(0.9f, 0.85f, 0.2f);
            else p.setColor(0.15f, 0.3f, 0.9f);
        }
    }

}

TEST_F(ParticlesGpu, DistortionBendsTheSceneBehind){
    JASPER_REQUIRE_GPU();
    // AC7: two big distorting particles over the stripes (left a plain alpha particle for
    // comparison): the stripes are bent through the distortion map, tinted where the particle is
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(0.0f, -0.4f, -1.0f);
    sun.setIntensity(2.0f);
    sun.setShadows(false);
    stripes(scene, primitives);
    const std::string distortion = R"("renderMode": "billboard", "texture": "particles/textures/spark.png", "blend": "alpha",
        "shader": "library:particles", "shaderFeatures": ["DISTORTION"],
        "shaderProperties": [{"name": "distortionStrength", "value": [0.08, 0, 0, 0]}],
        "shaderTextures": [{"name": "u_distortionTexture", "path": "particles/textures/distortion.png"}])";
    auto one = [](const std::string& color, const std::string& renderer){
        return R"({"version": 1, "duration": 5, "looping": false, "seed": 3, "maxParticles": 4, "modules": [
            {"type": "main", "startLifetime": 100, "startSpeed": 0, "startSize": 2.6, "startColor": [)" + color + R"(], "gravityModifier": 0},
            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
            {"type": "shape", "enabled": false},
            {"type": "renderer", )" + renderer + R"(}]})";
    };
    system(Vector3(-2.2f, 1.0f, 0.0f), one("1, 1, 1, 1", R"("renderMode": "billboard", "texture": "particles/textures/spark.png", "blend": "alpha")"));
    system(Vector3(0.4f, 1.0f, 0.0f), one("1, 1, 1, 1", distortion));
    system(Vector3(2.8f, 1.0f, 0.0f), one("1, 0.6, 0.6, 1", distortion));
    ASSERT_TRUE(render(0.5));
    EXPECT_GOLDEN_OPTIONS("particles_distortion", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, RingParticles){
    JASPER_REQUIRE_GPU();
    // AC7: the Ring renderer: full rings facing the camera growing over their life with their
    // colour, a half ring turning with its 3D start rotation, a disc (inner radius 0)
    auto rings = [](const std::string& renderer, const std::string& extra){
        return R"({"version": 1, "duration": 5, "looping": false, "seed": 11, "maxParticles": 10, "modules": [
            {"type": "main", "startLifetime": 2, "startSpeed": 0, "startSize": 1.2, "startColor": [0.4, 0.9, 1, 1], "gravityModifier": 0)" + extra + R"(},
            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
            {"type": "shape", "enabled": false},
            {"type": "sizeOverLifetime", "size": {"mode": "curve", "curve": [[0, 0.5], [1, 1.5]]}},
            {"type": "renderer", "renderMode": "ring", "blend": "additive", )" + renderer + R"(}]})";
    };
    system(Vector3(-2.6f, 1.0f, 0.0f), rings(R"("meshAlignment": "view", "ringOuterRadius": 0.5, "ringInnerRadius": 0.4, "ringSegments": 48)", ""));
    system(Vector3(0.0f, 1.0f, 0.0f), rings(R"("ringOuterRadius": 0.5, "ringInnerRadius": 0.2, "ringArc": 180)",
                                            R"(, "startRotation3D": true, "startRotationX": 50, "startRotation": 30)"));
    system(Vector3(2.6f, 1.0f, 0.0f), rings(R"("meshAlignment": "view", "ringInnerRadius": 0, "ringSegments": 6)", ""));
    ASSERT_TRUE(render(1.0));
    EXPECT_GOLDEN_OPTIONS("particles_ring", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(ParticlesGpu, FollowParentChildrenStayOnTheirParents){
    JASPER_REQUIRE_GPU();
    // AC7: red parents flying up, each with a green Birth child born beside it: on the left the
    // children stay where they were born, on the right they follow their parents
    defineEffect("fx/s91-follow-child.particles", R"({"version": 1, "duration": 5, "looping": false, "maxParticles": 50, "modules": [
        {"type": "main", "startLifetime": 100, "startSpeed": 0, "startSize": 0.35, "startColor": [0.1, 1, 0.2, 1], "gravityModifier": 0},
        {"type": "emission", "rateOverTime": 0},
        {"type": "shape", "shape": "box", "scale": [0, 0, 0], "position": [0.45, 0, 0]},
        {"type": "renderer", "renderMode": "billboard", "untexturedShape": "square"}]})");
    auto parent = [](bool follow){
        return R"({"version": 1, "duration": 5, "looping": false, "seed": 9, "maxParticles": 10, "simulationSpace": "world",
            "subEmitters": [{"trigger": "birth", "effect": "fx/s91-follow-child.particles", "emitCount": 1, "followParent": )" +
            std::string(follow ? "true" : "false") + R"(}],
            "modules": [
                {"type": "main", "startLifetime": 100, "startSpeed": 0, "startSize": 0.35, "startColor": [1, 0.1, 0.1, 1], "gravityModifier": 0},
                {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 3}]},
                {"type": "shape", "shape": "edge", "radius": 0.8},
                {"type": "startVelocity", "min": [0, 1.2, 0], "max": [0, 1.2, 0]},
                {"type": "subEmitters"},
                {"type": "renderer", "renderMode": "billboard", "untexturedShape": "square"}]})";
    };
    system(Vector3(-2.0f, 0.0f, 0.0f), parent(false));
    system(Vector3(2.0f, 0.0f, 0.0f), parent(true));
    ASSERT_TRUE(render(1.5));
    EXPECT_GOLDEN_OPTIONS("particles_follow", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
    particles::ParticleEffectLibrary::clear();
}

TEST_F(ParticlesGpu, ADistortingChildReadsTheTransparentsDrawnBeforeIt){
    JASPER_REQUIRE_GPU();
    // F6: the queue's scene colour readers are its items, a sub-emitter child its own: a distorting
    // child under a plain root, drawn after a plain system behind it, takes the scene colour again
    // so that it bends what was drawn before it
    defineEffect("fx/f6-distorting-child.particles", square("1, 1, 1, 1", R"(, "blend": "alpha", "shader": "library:particles", "shaderFeatures": ["DISTORTION"],
        "shaderProperties": [{"name": "distortionStrength", "value": [0.08, 0, 0, 0]}],
        "shaderTextures": [{"name": "u_distortionTexture", "path": "particles/textures/distortion.png"}])"));
    system(Vector3(0.0f, 1.0f, -3.0f), R"({"version": 1, "duration": 5, "looping": false, "seed": 3, "maxParticles": 4, "modules": [
        {"type": "main", "startLifetime": 100, "startSpeed": 0, "startSize": 3, "startColor": [0.2, 0.4, 1, 0.5], "gravityModifier": 0},
        {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
        {"type": "shape", "enabled": false},
        {"type": "renderer", "renderMode": "billboard", "blend": "alpha", "untexturedShape": "square"}]})");
    system(Vector3(0.0f, 1.0f, 0.0f), parentOf("fx/f6-distorting-child.particles", R"("renderMode": "billboard", "blend": "alpha", "untexturedShape": "square")"));
    ASSERT_TRUE(render(0.5));
    EXPECT_NO_ENGINE_ERRORS();
    const RenderSystem::SceneInputDetails inputs = scene.getSystem<RenderSystem>()->getSceneInputDetails();
    EXPECT_GE(inputs.colorCopies, 2u) << "the copy after the opaque queue and the child's";
    EXPECT_EQ(inputs.colorRecopies, 1u) << "the child reads the scene colour, its root does not";
    particles::ParticleEffectLibrary::clear();
}

TEST_F(ParticlesGpu, ATreeSortsOncePerView){
    JASPER_REQUIRE_GPU();
    // get_particle_stats' sorted_views: a parent and its child each sorting are one view
    defineEffect("fx/s91-sorted-child.particles", square("0, 0, 1, 0.5", R"(, "blend": "alpha", "sortMode": "byDistance")"));
    ParticleSystem& ps = system(Vector3(0.0f, 1.0f, 0.0f), parentOf("fx/s91-sorted-child.particles",
                                                                    R"("renderMode": "billboard", "blend": "alpha", "sortMode": "byDistance")"));
    ASSERT_TRUE(render(0.5));
    EXPECT_NO_ENGINE_ERRORS();
    const ParticleSystemStats stats = scene.getSystem<ParticleSubsystem>()->getStats(ps.getEntity());
    EXPECT_EQ(stats.drawnViews, 2u) << "the parent and the child drew";
    EXPECT_EQ(stats.sortedViews, 1u) << "one camera";
    particles::ParticleEffectLibrary::clear();
}

// a particle shader's custom block holds the scene clock (sceneTime), as every custom block does,
// not the frame clock: with a fixed 0.1 s frame at time scale 0.5 the frame clock runs 0.1 s a
// frame and the scene clock 0.05 s, or not at all while the scene is paused
TEST_F(ParticlesGpu, CustomBlockReadsTheSceneClock){
    JASPER_REQUIRE_GPU();
    ParticleClockRestore clocks;
    Engine::setPinnedShaderTime(-1.0);
    Engine::setFixedDeltatime(0.1);
    Engine::setTimeScale(0.5f);
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
    // the red of a clock value: its fraction of 255, as the shader writes it
    auto redOf = [](double seconds){ return (int)std::lround((seconds - std::floor(seconds)) * 255.0); };

    ASSERT_TRUE(render(1.0));

    // paused at 0.25 s: the same red every frame while the frame clock runs on
    scene.setSceneTimePaused(true);
    scene.setSceneTime(0.25);
    GpuEnvironment::drawFrames(1);
    const double frameClockStart = Engine::getShaderTimeSeconds();
    for (int frame = 0; frame < 4; frame++){
        GpuEnvironment::drawFrames(1);
        const int red = clockRed();
        std::printf("[gpu] paused: sceneTime red %d (scene %.3f s, frame clock %.3f s)\n", red, scene.getSceneTime(), Engine::getShaderTimeSeconds());
        EXPECT_NEAR(red, 64, 1) << "frame " << frame << ": the particle's custom block does not hold the paused scene clock";
    }
    EXPECT_GT(Engine::getShaderTimeSeconds() - frameClockStart, 0.25) << "the frame clock did not run (the test tells the clocks apart by it)";

    // running at time scale 0.5: the scene clock's 0.05 s a frame, not the frame clock's 0.1 s
    scene.setSceneTimePaused(false);
    scene.setSceneTime(0.1);
    const double sceneStart = scene.getSceneTime();
    const double frameStart = Engine::getShaderTimeSeconds();
    for (int frame = 0; frame < 4; frame++){
        GpuEnvironment::drawFrames(1);
        const int red = clockRed();
        std::printf("[gpu] running: sceneTime red %d (scene %.3f s, frame clock %.3f s)\n", red, scene.getSceneTime(), Engine::getShaderTimeSeconds());
        EXPECT_NEAR(red, redOf(scene.getSceneTime()), 1) << "frame " << frame << ": the particle's custom block does not follow the scene clock";
    }
    const double frameRan = Engine::getShaderTimeSeconds() - frameStart;
    std::printf("[gpu] running: the scene clock ran %.3f s, the frame clock %.3f s\n", scene.getSceneTime() - sceneStart, frameRan);
    EXPECT_GT(frameRan, 0.25) << "the frame clock did not run (the test tells the clocks apart by it)";
    EXPECT_GT(scene.getSceneTime() - sceneStart, 0.1) << "the scene clock did not run";
    EXPECT_NO_ENGINE_ERRORS();
}
