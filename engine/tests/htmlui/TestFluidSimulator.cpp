// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GPU fluid (FluidSimulator, Pavel Dobryakov's WebGL-Fluid-Simulation on the render layer):
// byte-identical outputs for a seed and a step count (and different ones for another seed),
// splats at their place (the output's top row is the fluid's top), the clock and its substep
// cap, pause (no steps, splats still paint), pointer strokes spread over a frame's steps,
// resizes that keep the fields, the RGBA16F fallback of RG16F/R16F and the manual filtering
// fallback, unsupported devices, release, and the output Framebuffer object kept across its
// recreations; FluidSystem in a scene: the SCREEN composite over the view, the MATERIAL binding
// of a Sprite and its restoration, the paused system and the Inspector preview. Small
// resolutions keep each frame cheap on llvmpipe and lavapipe.

#include "harness/GpuEnvironment.h"
#include "harness/Readback.h"
#include "harness/SceneFixture.h"
#include "harness/TestSystem.h"
#include "TestRenderCommon.h"

#include "Engine.h"
#include "System.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "fluid/FluidSimulator.h"
#include "object/Sprite.h"
#include "object/Terrain.h"
#include "object/ui/Image.h"
#include "component/TerrainComponent.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "subsystem/FluidSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class FluidSimulatorTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            FluidSimulator::setDisabledFeaturesForTesting(0);
        }

        void TearDown() override{
            FluidSimulator::setDisabledFeaturesForTesting(0);
            Engine::setFixedDeltatime(0.0);
        }
    };

    // a cheap fluid: small grids, the whole look on
    FluidSimulationComponent smallFluid(uint32_t seed){
        FluidSimulationComponent c;
        c.simResolution = 32;
        c.dyeResolution = 64;
        c.bloomResolution = 64;
        c.bloomIterations = 8;
        c.sunraysResolution = 48;
        c.pressureIterations = 10;
        c.seed = seed;
        return c;
    }

    // only the dye shows: no shading, bloom or sunrays, no start burst
    FluidSimulationComponent plainFluid(){
        FluidSimulationComponent c = smallFluid(7);
        c.shading = false;
        c.bloom = false;
        c.sunrays = false;
        c.splatsOnStart = false;
        c.colorful = false;
        return c;
    }

    // one frame: the update between the runtime compiler's turn and the commit
    bool frame(FluidSimulator& sim, FluidSimulationComponent& c, float aspect, double dt){
        beginTestFrame();
        const bool drawn = sim.update(c, aspect, dt);
        SystemRender::commit();
        return drawn;
    }

    // frames until the simulator draws (its two variants compile, one a frame)
    ::testing::AssertionResult warmUp(FluidSimulator& sim, FluidSimulationComponent& c, float aspect = 1.0f){
        for (int i = 0; i < 600; i++){
            if (frame(sim, c, aspect, 0.0))
                return ::testing::AssertionSuccess();
            const std::string& error = sim.getError();
            if (error.find("being compiled") == std::string::npos)
                return ::testing::AssertionFailure() << "the simulator failed: " << error;
        }
        return ::testing::AssertionFailure() << "the fluid shaders never became ready: " << sim.getError();
    }

    void steps(FluidSimulator& sim, FluidSimulationComponent& c, float aspect, int count){
        beginTestFrame();
        sim.step(c, aspect, count);
        SystemRender::commit();
    }

    struct FluidImage {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;

        // the texel at fluid coordinates (0,0 bottom-left): rows are top first
        const uint8_t* at(float fx, float fy) const{
            const int x = std::clamp((int)(fx * (float)width), 0, width - 1);
            const int y = std::clamp((int)((1.0f - fy) * (float)height), 0, height - 1);
            return &rgba[((size_t)y * (size_t)width + (size_t)x) * 4];
        }
    };

    FluidImage readOutput(FluidSimulator& sim){
        FluidImage image;
        Framebuffer* output = sim.getOutputFramebuffer();
        image.width = (int)output->getWidth();
        image.height = (int)output->getHeight();
        EXPECT_TRUE(readTexture(output->getRender().getColorTexture(), image.width, image.height, image.rgba));
        return image;
    }

    size_t differingBytes(const FluidImage& a, const FluidImage& b){
        if (a.rgba.size() != b.rgba.size())
            return (size_t)-1;
        size_t n = 0;
        for (size_t i = 0; i < a.rgba.size(); i++)
            n += a.rgba[i] != b.rgba[i] ? 1 : 0;
        return n;
    }

    // the largest channel difference (INT_MAX when the sizes differ)
    int maxDifference(const FluidImage& a, const FluidImage& b){
        if (a.rgba.size() != b.rgba.size())
            return 1 << 30;
        int worst = 0;
        for (size_t i = 0; i < a.rgba.size(); i++)
            worst = std::max(worst, std::abs((int)a.rgba[i] - (int)b.rgba[i]));
        return worst;
    }

    int brightest(const FluidImage& image){
        int value = 0;
        for (size_t i = 0; i < image.rgba.size(); i += 4)
            value = std::max<int>(value, std::max<int>(image.rgba[i], std::max<int>(image.rgba[i + 1], image.rgba[i + 2])));
        return value;
    }

    FluidSplat splatAt(float x, float y, Vector3 color, Vector2 velocity = Vector2(0.0f, 0.0f)){
        FluidSplat s;
        s.position = Vector2(x, y);
        s.velocity = velocity;
        s.color = color;
        return s;
    }

}

TEST_F(FluidSimulatorTest, SupportedAndSized){
    std::string reason;
    ASSERT_TRUE(FluidSimulator::isSupported(&reason)) << reason;

    FluidSimulator sim;
    FluidSimulationComponent c = smallFluid(11);
    ASSERT_TRUE(warmUp(sim, c, 2.0f));
    const FluidStats& stats = sim.getStats();
    // Pavel's getResolution: the short side is the resolution, the long one x aspect
    EXPECT_EQ(stats.simWidth, 64);
    EXPECT_EQ(stats.simHeight, 32);
    EXPECT_EQ(stats.dyeWidth, 128);
    EXPECT_EQ(stats.dyeHeight, 64);
    EXPECT_EQ(stats.outputWidth, 128);
    EXPECT_EQ(stats.outputHeight, 64);
    EXPECT_EQ(stats.seed, 11u);
    EXPECT_GT(stats.gpuBytes, 0u);
    EXPECT_GT(stats.passesLastFrame, 0);
    EXPECT_FALSE(c.needReset);
    // the start burst (5 to 24 splats), the look on
    EXPECT_GE(stats.splats, 5u);
    EXPECT_LE(stats.splats, 24u);
    EXPECT_TRUE(stats.shadingActive);
    EXPECT_TRUE(stats.bloomActive);
    EXPECT_TRUE(stats.sunraysActive);
    EXPECT_TRUE(sim.getError().empty()) << sim.getError();
    ASSERT_NE(sim.getDyeFramebuffer(), nullptr);
    ASSERT_NE(sim.getVelocityFramebuffer(), nullptr);
    EXPECT_EQ(sim.getDyeFramebuffer()->getWidth(), 128u);
    EXPECT_EQ(sim.getVelocityFramebuffer()->getHeight(), 32u);

    // a tall fluid: the long side vertical
    ASSERT_TRUE(frame(sim, c, 0.5f, 0.0));
    EXPECT_EQ(sim.getStats().simWidth, 32);
    EXPECT_EQ(sim.getStats().simHeight, 64);
}

TEST_F(FluidSimulatorTest, SameSeedSameBytes){
    FluidSimulator a, b, other;
    FluidSimulationComponent ca = smallFluid(1234);
    FluidSimulationComponent cb = smallFluid(1234);
    FluidSimulationComponent co = smallFluid(4321);
    ASSERT_TRUE(warmUp(a, ca));
    ASSERT_TRUE(warmUp(b, cb));
    ASSERT_TRUE(warmUp(other, co));
    steps(a, ca, 1.0f, 20);
    steps(b, cb, 1.0f, 20);
    steps(other, co, 1.0f, 20);
    EXPECT_EQ(a.getStats().steps, 20u);
    EXPECT_EQ(b.getStats().steps, 20u);
    EXPECT_NEAR(a.getStats().time, 20.0 / 60.0, 1e-9);

    const FluidImage ia = readOutput(a);
    const FluidImage ib = readOutput(b);
    const FluidImage io = readOutput(other);
    ASSERT_FALSE(ia.rgba.empty());
    EXPECT_EQ(differingBytes(ia, ib), 0u) << "a seed and its steps must give the same image";
    EXPECT_GT(differingBytes(ia, io), ia.rgba.size() / 20) << "another seed gives another image";

    // something is there: the burst is bright
    EXPECT_GT(brightest(ia), 100);

    // a reset with the same seed replays the same start
    ca.needReset = true;
    cb.needReset = true;
    ASSERT_TRUE(frame(a, ca, 1.0f, 0.0));
    steps(a, ca, 1.0f, 5);
    a.reset(ca);
    steps(a, ca, 1.0f, 20);
    steps(b, cb, 1.0f, 20);
    EXPECT_EQ(differingBytes(readOutput(a), readOutput(b)), 0u);
    EXPECT_EQ(a.getStats().steps, 20u);
}

TEST_F(FluidSimulatorTest, SeedZeroPicksNewSeeds){
    FluidSimulator sim;
    FluidSimulationComponent c = smallFluid(0);
    ASSERT_TRUE(warmUp(sim, c));
    const uint32_t first = sim.getStats().seed;
    EXPECT_NE(first, 0u);
    c.needReset = true;
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.0));
    EXPECT_NE(sim.getStats().seed, 0u);
    EXPECT_NE(sim.getStats().seed, first);
}

TEST_F(FluidSimulatorTest, SplatPaintsAtItsPlace){
    FluidSimulator sim;
    FluidSimulationComponent c = plainFluid();
    ASSERT_TRUE(warmUp(sim, c));
    FluidImage empty = readOutput(sim);
    const uint8_t* black = empty.at(0.25f, 0.75f);
    EXPECT_EQ(black[0], 0);
    EXPECT_EQ(black[3], 255) << "opaque over the back colour";

    // red at the top left: queued splats apply at the next update, with no step
    c.pendingSplats.push_back(splatAt(0.25f, 0.75f, Vector3(1.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.0));
    EXPECT_TRUE(c.pendingSplats.empty());
    EXPECT_EQ(sim.getStats().splats, 1u);
    EXPECT_EQ(sim.getStats().steps, 0u);
    FluidImage image = readOutput(sim);
    const uint8_t* hit = image.at(0.25f, 0.75f);
    EXPECT_GT(hit[0], 200);
    EXPECT_LT(hit[1], 10);
    EXPECT_EQ(hit[3], 255);
    const uint8_t* away = image.at(0.75f, 0.25f);
    EXPECT_LT(away[0], 5) << "the opposite corner stays dark (orientation)";

    // the back colour under a transparent-less fluid, and a transparent one is premultiplied
    c.backColor = Vector3(0.0f, 0.0f, 1.0f);
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.0));
    image = readOutput(sim);
    EXPECT_GT(image.at(0.75f, 0.25f)[2], 250);
    c.transparent = true;
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.0));
    image = readOutput(sim);
    EXPECT_LT(image.at(0.75f, 0.25f)[3], 5);
    EXPECT_GT(image.at(0.25f, 0.75f)[3], 200);
    EXPECT_LT(image.at(0.25f, 0.75f)[2], 10);
}

TEST_F(FluidSimulatorTest, ClockAndSubstepCap){
    FluidSimulator sim;
    FluidSimulationComponent c = plainFluid();
    c.simulationRate = 60.0f;
    c.maxSubsteps = 4;
    ASSERT_TRUE(warmUp(sim, c));
    EXPECT_EQ(sim.getStats().steps, 0u);
    ASSERT_TRUE(frame(sim, c, 1.0f, 1.0 / 120.0));
    EXPECT_EQ(sim.getStats().steps, 0u);
    ASSERT_TRUE(frame(sim, c, 1.0f, 1.0 / 120.0));
    EXPECT_EQ(sim.getStats().steps, 1u);
    // a long frame: 4 steps, the rest dropped
    ASSERT_TRUE(frame(sim, c, 1.0f, 1.0));
    EXPECT_EQ(sim.getStats().steps, 5u);
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.0));
    EXPECT_EQ(sim.getStats().steps, 5u);
    // queued steps run on top of the clock's
    c.pendingSteps = 3;
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.0));
    EXPECT_EQ(sim.getStats().steps, 8u);
    EXPECT_EQ(c.pendingSteps, 0u);
}

TEST_F(FluidSimulatorTest, PauseStopsStepsSplatsStillPaint){
    FluidSimulator sim;
    FluidSimulationComponent c = plainFluid();
    c.paused = true;
    ASSERT_TRUE(warmUp(sim, c));
    ASSERT_TRUE(frame(sim, c, 1.0f, 1.0));
    EXPECT_EQ(sim.getStats().steps, 0u);

    c.pendingSplats.push_back(splatAt(0.5f, 0.5f, Vector3(0.0f, 1.0f, 0.0f)));
    ASSERT_TRUE(frame(sim, c, 1.0f, 1.0));
    EXPECT_EQ(sim.getStats().steps, 0u);
    EXPECT_EQ(sim.getStats().splats, 1u);
    EXPECT_GT(readOutput(sim).at(0.5f, 0.5f)[1], 200);

    // a drag paints at once while paused (his pause)
    sim.pointerDown(0, Vector2(0.2f, 0.2f));
    sim.pointerMove(0, Vector2(0.3f, 0.2f));
    ASSERT_TRUE(frame(sim, c, 1.0f, 1.0));
    EXPECT_EQ(sim.getStats().splats, 2u);
    EXPECT_EQ(sim.getStats().steps, 0u);
    sim.pointerUp(0);

    // a random burst also paints while paused
    c.pendingBursts.push_back(3);
    ASSERT_TRUE(frame(sim, c, 1.0f, 1.0));
    EXPECT_EQ(sim.getStats().splats, 5u);
    EXPECT_TRUE(c.pendingBursts.empty());

    // explicit steps run while paused
    steps(sim, c, 1.0f, 3);
    EXPECT_EQ(sim.getStats().steps, 3u);
}

TEST_F(FluidSimulatorTest, StrokeSpreadsOverTheFrameSteps){
    FluidSimulator sim;
    FluidSimulationComponent c = plainFluid();
    c.densityDissipation = 0.0f;
    ASSERT_TRUE(warmUp(sim, c));
    sim.pointerDown(0, Vector2(0.1f, 0.5f));
    // a move between steps waits for the next one
    sim.pointerMove(0, Vector2(0.5f, 0.5f));
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.004));
    EXPECT_EQ(sim.getStats().splats, 0u);
    sim.pointerMove(0, Vector2(0.9f, 0.5f));
    // four steps: four splats along the stroke
    ASSERT_TRUE(frame(sim, c, 1.0f, 4.0 / 60.0));
    EXPECT_EQ(sim.getStats().steps, 4u);
    EXPECT_EQ(sim.getStats().splats, 4u);
    const FluidImage image = readOutput(sim);
    int lit = 0;
    for (float x : {0.3f, 0.5f, 0.7f, 0.9f}){
        const uint8_t* p = image.at(x, 0.5f);
        lit += std::max<int>(p[0], std::max<int>(p[1], p[2])) > 20 ? 1 : 0;
    }
    EXPECT_GE(lit, 3) << "the stroke is painted along its length";
    sim.pointerUp(0);
    ASSERT_TRUE(frame(sim, c, 1.0f, 1.0 / 60.0));
    EXPECT_EQ(sim.getStats().splats, 4u);
}

TEST_F(FluidSimulatorTest, ResizeKeepsTheFields){
    FluidSimulator sim;
    FluidSimulationComponent c = plainFluid();
    c.pendingSplats.push_back(splatAt(0.5f, 0.5f, Vector3(1.0f, 1.0f, 1.0f)));
    ASSERT_TRUE(warmUp(sim, c));
    EXPECT_GT(readOutput(sim).at(0.5f, 0.5f)[0], 200);

    Framebuffer* output = sim.getOutputFramebuffer();
    Texture texture = sim.getOutputTexture();
    texture.getRender();
    const unsigned long version = output->getVersion();
    EXPECT_FALSE(texture.isFramebufferOutdated());

    // wider: new targets, the dye resampled into them
    ASSERT_TRUE(frame(sim, c, 2.0f, 0.0));
    EXPECT_EQ(sim.getOutputFramebuffer(), output) << "the output object is stable";
    EXPECT_NE(output->getVersion(), version);
    EXPECT_TRUE(texture.isFramebufferOutdated());
    EXPECT_EQ(sim.getStats().dyeWidth, 128);
    const FluidImage wide = readOutput(sim);
    EXPECT_EQ(wide.width, 128);
    EXPECT_GT(wide.at(0.5f, 0.5f)[0], 200) << "the dye survives the resize";
    EXPECT_LT(wide.at(0.05f, 0.95f)[0], 10);

    // a finer simulation grid keeps it too
    c.simResolution = 64;
    c.dyeResolution = 96;
    ASSERT_TRUE(frame(sim, c, 2.0f, 0.0));
    EXPECT_EQ(sim.getStats().simHeight, 64);
    EXPECT_EQ(sim.getStats().dyeHeight, 96);
    EXPECT_GT(readOutput(sim).at(0.5f, 0.5f)[0], 200);
}

TEST_F(FluidSimulatorTest, HalfFloatFallbackGivesTheSameImage){
    FluidSimulationComponent compact = smallFluid(99);
    FluidSimulator a;
    ASSERT_TRUE(warmUp(a, compact));
    steps(a, compact, 1.0f, 12);
    const FluidImage reference = readOutput(a);
    const size_t compactBytes = a.getStats().gpuBytes;

    // RG16F and R16F refused: every field in RGBA16F (Pavel's fallback), the same values
    FluidSimulator::setDisabledFeaturesForTesting(FLUID_TEST_NO_RG16F | FLUID_TEST_NO_R16F);
    FluidSimulationComponent wide = smallFluid(99);
    FluidSimulator b;
    ASSERT_TRUE(warmUp(b, wide));
    steps(b, wide, 1.0f, 12);
    EXPECT_FALSE(b.getStats().compactFormats);
    EXPECT_FALSE(b.getStats().manualFiltering);
    EXPECT_GE(b.getStats().gpuBytes, compactBytes);
    // half floats either way: the same values (a filter rounding apart at most)
    EXPECT_LE(maxDifference(readOutput(b), reference), 1);
    const RenderFeatures& features = SystemRender::getFeatures();
    std::printf("[fluid] RG16F render %d filter %d, R16F render %d filter %d: compact formats %d\n", (int)features.renderRG16F,
                (int)features.filterRG16F, (int)features.renderR16F, (int)features.filterR16F, (int)a.getStats().compactFormats);
    if (features.renderRG16F && features.filterRG16F && features.renderR16F) {
        EXPECT_TRUE(a.getStats().compactFormats);
    }
    EXPECT_LT(b.getStats().gpuBytes, compactBytes * 2);
}

TEST_F(FluidSimulatorTest, ManualFilteringFallback){
    // the reference: hardware filtering, the look off (manual filtering turns it off)
    FluidSimulationComponent linear = plainFluid();
    linear.splatsOnStart = true;
    FluidSimulator a;
    ASSERT_TRUE(warmUp(a, linear));
    steps(a, linear, 1.0f, 6);
    const FluidImage reference = readOutput(a);
    EXPECT_FALSE(a.getStats().manualFiltering);

    FluidSimulator::setDisabledFeaturesForTesting(FLUID_TEST_NO_LINEAR_FILTER);
    FluidSimulationComponent c = plainFluid();
    c.splatsOnStart = true;
    c.shading = true;
    c.bloom = true;
    c.sunrays = true;
    FluidSimulator sim;
    ASSERT_TRUE(warmUp(sim, c));
    steps(sim, c, 1.0f, 6);
    const FluidStats& stats = sim.getStats();
    EXPECT_TRUE(stats.manualFiltering);
    EXPECT_FALSE(stats.shadingActive);
    EXPECT_FALSE(stats.bloomActive);
    EXPECT_FALSE(stats.sunraysActive);
    EXPECT_EQ(stats.steps, 6u);
    // his bilerp by hand: the same fluid as the hardware's filter, to its precision. 24 levels on
    // llvmpipe (whose filter weights are full floats); a hardware GPU's filter quantises the
    // weights (8 bits of sub-texel position on NVIDIA), and six steps of advection carry that
    // further: 41 on an NVIDIA GTX 1080 and a GTX 1650 (Vulkan, driver 580). The limit is that
    // plus about 12 % there (24 on the goldens' renderer); a broken manual bilerp (the half-texel
    // offset dropped) is off by 255.
    const FluidImage image = readOutput(sim);
    EXPECT_GT(brightest(image), 100);
    EXPECT_LE(maxDifference(image, reference), GpuEnvironment::isReferenceRenderer() ? 24 : 46);
}

TEST_F(FluidSimulatorTest, UnsupportedDevice){
    FluidSimulator::setDisabledFeaturesForTesting(FLUID_TEST_NO_FLOAT_TARGETS);
    std::string reason;
    EXPECT_FALSE(FluidSimulator::isSupported(&reason));
    EXPECT_FALSE(reason.empty());
    FluidSimulator sim;
    FluidSimulationComponent c = smallFluid(1);
    EXPECT_FALSE(frame(sim, c, 1.0f, 0.1));
    EXPECT_FALSE(sim.getError().empty());
    EXPECT_EQ(sim.getStats().gpuBytes, 0u);
    // queued work waits
    sim.step(c, 1.0f, 2);
    EXPECT_EQ(c.pendingSteps, 2u);
}

TEST_F(FluidSimulatorTest, ReleaseFreesAndRecreates){
    FluidSimulator sim;
    FluidSimulationComponent c = smallFluid(3);
    ASSERT_TRUE(warmUp(sim, c));
    Framebuffer* output = sim.getOutputFramebuffer();
    TextureRender& dyeTexture = sim.getDyeFramebuffer()->getRender().getColorTexture();
    const sg_image dyeImage = dyeTexture.backend.get();
    const sg_image outputImage = output->getRender().getColorTexture().backend.get();
    ASSERT_EQ(sg_query_image_state(dyeImage), SG_RESOURCESTATE_VALID);
    const unsigned long version = output->getVersion();

    sim.release();
    EXPECT_EQ(sg_query_image_state(dyeImage), SG_RESOURCESTATE_INVALID);
    EXPECT_EQ(sg_query_image_state(outputImage), SG_RESOURCESTATE_INVALID);
    EXPECT_EQ(sim.getDyeFramebuffer(), nullptr);
    EXPECT_EQ(sim.getVelocityFramebuffer(), nullptr);
    EXPECT_FALSE(output->isCreated());
    EXPECT_EQ(sim.getStats().gpuBytes, 0u);

    // the next update makes them again, restarted, into the same output object
    ASSERT_TRUE(warmUp(sim, c));
    EXPECT_EQ(sim.getOutputFramebuffer(), output);
    EXPECT_TRUE(output->isCreated());
    EXPECT_GT(output->getVersion(), version);
    EXPECT_GT(sim.getStats().gpuBytes, 0u);
    EXPECT_GE(sim.getStats().splats, 5u);

    // a destroyed simulator leaves nothing behind
    const sg_image again = sim.getDyeFramebuffer()->getRender().getColorTexture().backend.get();
    {
        FluidSimulator other;
        FluidSimulationComponent oc = smallFluid(4);
        ASSERT_TRUE(warmUp(other, oc));
        const sg_image otherDye = other.getDyeFramebuffer()->getRender().getColorTexture().backend.get();
        ASSERT_EQ(sg_query_image_state(otherDye), SG_RESOURCESTATE_VALID);
        sim.release();
        EXPECT_EQ(sg_query_image_state(again), SG_RESOURCESTATE_INVALID);
        // the shared programs stay for the other one
        ASSERT_TRUE(frame(other, oc, 1.0f, 1.0 / 60.0));
    }
}


namespace {

    // frames until `done` holds (false if it never does)
    bool framesUntil(const std::function<bool()>& done, int maxFrames = 400){
        for (int i = 0; i < maxFrames; i++){
            GpuEnvironment::drawFrames(1);
            if (done())
                return true;
        }
        return false;
    }

    void ensureFluidComponent(Scene& scene){
        // (registered by the engine's registry once the GameObject wrapper lands; tests make sure)
        if (!scene.isComponentRegistered<FluidSimulationComponent>())
            scene.registerComponent<FluidSimulationComponent>();
    }

}

TEST_F(FluidSimulatorTest, SystemCompositesScreenFluids){
    TestSystem::get().clearLogs();
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    ensureFluidComponent(scene);

    // an opaque SCREEN fluid, paused (splats still paint), red in its top-left quarter
    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::SCREEN;
    c.paused = true;
    FluidSplat red = splatAt(0.25f, 0.75f, Vector3(1.0f, 0.0f, 0.0f));
    red.radius = 2.0f;
    c.pendingSplats.push_back(red);
    const Entity entity = scene.createEntity();
    scene.addComponent<FluidSimulationComponent>(entity, c);
    FluidSimulationComponent& fluid = scene.getComponent<FluidSimulationComponent>(entity);
    FluidSystem* system = scene.getSystem<FluidSystem>().get();

    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil([&](){ return fluid.simulator && fluid.simulator->getStats().splats == 1; }))
        << (fluid.simulator ? fluid.simulator->getError() : std::string("no simulator"));
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(system->hasScreenFluids());
    // the canvas's aspect (640 x 360)
    EXPECT_NEAR(system->getAspectRatio(entity), 640.0f / 360.0f, 1e-4f);
    EXPECT_EQ(fluid.simulator->getStats().dyeHeight, 64);

    Framebuffer& target = GpuEnvironment::target();
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba));
    PixelProbe probe{rgba, (int)target.getWidth()};
    const int w = (int)target.getWidth(), h = (int)target.getHeight();
    // over the view, its top row at the top, opaque (the scene's background hidden)
    EXPECT_TRUE(probe.is(w / 4, h / 4, 255, 0, 0, 255, 40));
    EXPECT_TRUE(probe.is(3 * w / 4, 3 * h / 4, 0, 0, 0, 255, 2));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);

    // a disabled fluid is not drawn: the background shows again
    fluid.enabled = false;
    GpuEnvironment::drawFrames(2);
    EXPECT_FALSE(system->hasScreenFluids());
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), w, h, rgba));
    EXPECT_TRUE(probe.isBackground(3 * w / 4, 3 * h / 4));
    scene.removeComponent<FluidSimulationComponent>(entity);
}

TEST_F(FluidSimulatorTest, SystemLendsMaterialAndPreviews){
    TestSystem::get().clearLogs();
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    ensureFluidComponent(scene);
    Sprite& sprite = fixture.add<Sprite>();
    sprite.setSize(120, 60);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(200.0f, 150.0f, 0.0f);

    // red along the fluid's top edge (fluid y up), no steps
    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::MATERIAL;
    c.paused = true;
    FluidSplat red = splatAt(0.5f, 1.0f, Vector3(1.0f, 0.0f, 0.0f));
    red.radius = 5.0f;
    c.pendingSplats.push_back(red);
    const Entity entity = sprite.getEntity();
    scene.addComponent<FluidSimulationComponent>(entity, c);
    FluidSimulationComponent& fluid = scene.getComponent<FluidSimulationComponent>(entity);
    FluidSystem* system = scene.getSystem<FluidSystem>().get();
    MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);

    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil([&](){ return system->getMaterialTexture(entity) != nullptr; }))
        << (fluid.simulator ? fluid.simulator->getError() : std::string("no simulator"));
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(system->hasMaterialFluids());
    EXPECT_EQ(system->getMaterialTexture(entity), &fluid.simulator->getOutputFramebuffer()->getRender().getColorTexture());
    // lent at draw time: the sprite's own texture is untouched (nothing saved, no override)
    EXPECT_EQ(mesh.submeshes[0].material.baseColorTexture.getFramebuffer(), nullptr);
    EXPECT_TRUE(mesh.submeshes[0].material.baseColorTexture.getId().empty());
    // the sprite's shape
    EXPECT_NEAR(system->getAspectRatio(entity), 2.0f, 1e-6f);
    EXPECT_EQ(fluid.simulator->getStats().dyeWidth, 128);
    EXPECT_GE(fluid.revision, 1u);
    EXPECT_FALSE(system->hasScreenFluids());

    // drawn: the sprite (120 x 60 around (200, 150) of the UI camera's view) shows the red fluid
    Framebuffer& target = GpuEnvironment::target();
    const int w = (int)target.getWidth(), h = (int)target.getHeight();
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), w, h, rgba));
    PixelProbe probe{rgba, w};
    // (a UI camera's sprite shows its texture's first row, the fluid's top, at its top: fluid
    // y 0.85 at y 129, 0.15 at y 171)
    const uint8_t* top = probe.at(200, 129);
    EXPECT_GT((int)top[0], 150) << "the sprite shows the fluid's top at its top";
    EXPECT_LT((int)top[1], 60);
    const uint8_t* bottom = probe.at(200, 171);
    EXPECT_LT((int)bottom[0], 60) << "and its bottom at its bottom";
    EXPECT_TRUE(probe.isBackground(400, 300));

    // a paused system (the editor's edit mode) does not step; a preview does (frames of a
    // step each: headless frames are so short that hundreds of them make no 1/60 s step)
    Engine::setFixedDeltatime(1.0 / 60.0);
    fluid.paused = false;
    system->setPaused(true);
    const uint64_t before = fluid.simulator->getStats().steps;
    GpuEnvironment::drawFrames(10);
    EXPECT_EQ(fluid.simulator->getStats().steps, before);
    system->setPreview(entity, true);
    EXPECT_TRUE(system->isPreview(entity));
    EXPECT_TRUE(framesUntil([&](){ return fluid.simulator->getStats().steps > before; }));
    system->setPreview(entity, false);
    Engine::setFixedDeltatime(0.0);

    // the editor's brush in edit mode (paused system, no preview): its own pointer id paints at once
    const uint64_t splats = fluid.simulator->getStats().splats;
    const uint64_t steps = fluid.simulator->getStats().steps;
    const int brush = 0x7ffffff0;
    fluid.simulator->pointerDown(brush, Vector2(0.2f, 0.3f));
    fluid.simulator->pointerMove(brush, Vector2(0.4f, 0.3f));
    GpuEnvironment::drawFrames(2);
    fluid.simulator->pointerUp(brush);
    EXPECT_EQ(fluid.simulator->getStats().splats, splats + 1);
    EXPECT_EQ(fluid.simulator->getStats().steps, steps);
    system->setPaused(false);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);

    // another output: the sprite shows its own texture again (white, untextured)
    fluid.output = FluidOutput::TEXTURE;
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(system->getMaterialTexture(entity), nullptr);
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), w, h, rgba));
    EXPECT_GT((int)probe.at(200, 129)[1], 150) << "the sprite's own (white) look is back";
    fluid.output = FluidOutput::MATERIAL;
    GpuEnvironment::drawFrames(2);

    // removed: the GPU side goes
    const sg_image dyeImage = fluid.simulator->getDyeFramebuffer()->getRender().getColorTexture().backend.get();
    scene.removeComponent<FluidSimulationComponent>(entity);
    EXPECT_EQ(sg_query_image_state(dyeImage), SG_RESOURCESTATE_INVALID);
    EXPECT_EQ(system->getMaterialTexture(entity), nullptr);
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}


TEST_F(FluidSimulatorTest, TransparentMaterialHasNoDarkFringe){
    TestSystem::get().clearLogs();
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    ensureFluidComponent(scene);
    scene.setBackgroundColor(0.6f, 0.6f, 0.6f, 1.0f);
    Sprite& sprite = fixture.add<Sprite>();
    sprite.setSize(160, 160);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(320.0f, 180.0f, 0.0f);

    // a soft red splat on a transparent fluid: its edge fades out through every alpha
    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::MATERIAL;
    c.transparent = true;
    c.paused = true;
    FluidSplat red = splatAt(0.5f, 0.5f, Vector3(1.0f, 0.0f, 0.0f));
    red.radius = 3.0f;
    c.pendingSplats.push_back(red);
    const Entity entity = sprite.getEntity();
    scene.addComponent<FluidSimulationComponent>(entity, c);
    FluidSystem* system = scene.getSystem<FluidSystem>().get();
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil([&](){ return system->getMaterialTexture(entity) != nullptr; }));
    GpuEnvironment::drawFrames(4);

    // the output itself: straight alpha (the colour stays red where alpha fades)
    FluidImage output = readOutput(*scene.getComponent<FluidSimulationComponent>(entity).simulator);
    const uint8_t* edge = output.at(0.5f, 0.5f + 0.12f);
    ASSERT_GT((int)edge[3], 20);
    ASSERT_LT((int)edge[3], 235);
    EXPECT_GT((int)edge[0], 240) << "straight alpha: full red at a partial alpha";

    Framebuffer& target = GpuEnvironment::target();
    const int w = (int)target.getWidth(), h = (int)target.getHeight();
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), w, h, rgba));
    PixelProbe probe{rgba, w};
    const uint8_t* background = probe.at(20, 20);
    const int bg = background[0];
    // red over grey never gets darker than the grey (a premultiplied colour blended as straight
    // alpha would: the dark fringe), nor greener; the centre is red
    int darkest = 255, greenest = 0;
    for (int y = 100; y < 260; y += 2){
        for (int x = 240; x < 400; x += 2){
            const uint8_t* p = probe.at(x, y);
            darkest = std::min<int>(darkest, p[0]);
            greenest = std::max<int>(greenest, p[1]);
        }
    }
    EXPECT_GE(darkest, bg - 2) << "a dark fringe (background red " << bg << ")";
    EXPECT_LE(greenest, (int)background[1] + 2);
    const uint8_t* centre = probe.at(320, 180);
    EXPECT_GT((int)centre[0], 240);
    EXPECT_LT((int)centre[1], 30);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
    scene.removeComponent<FluidSimulationComponent>(entity);
}

// ---------------------------------------------------------------- reviewers' findings

namespace {

    // the main view's pixels now
    std::vector<uint8_t> readTarget(int& w, int& h){
        Framebuffer& target = GpuEnvironment::target();
        w = (int)target.getWidth();
        h = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), w, h, rgba));
        return rgba;
    }

}

TEST_F(FluidSimulatorTest, RemovedFluidLeavesItsLinkedTexturesSafe){
    // a sprite showing a TEXTURE fluid's output (a fluid|<e> link, getTexture(), the exported
    // game's setFramebuffer(simulator->getOutputFramebuffer())); the fluid is then deleted
    TestSystem::get().clearLogs();
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    ensureFluidComponent(scene);
    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::TEXTURE;
    c.paused = true;
    FluidSplat red = splatAt(0.5f, 0.5f, Vector3(1.0f, 0.0f, 0.0f));
    red.radius = 20.0f;
    c.pendingSplats.push_back(red);
    c.simulator = std::make_shared<FluidSimulator>();
    Framebuffer* output = c.simulator->getOutputFramebuffer();
    const std::weak_ptr<Framebuffer> shared = output->weak_from_this();
    ASSERT_FALSE(shared.expired()) << "the output is shared (std::make_shared)";
    const Entity fluidEntity = scene.createEntity();
    scene.addComponent<FluidSimulationComponent>(fluidEntity, c);
    c.simulator.reset();    // (the component's is the only one)
    FluidSimulationComponent& fluid = scene.getComponent<FluidSimulationComponent>(fluidEntity);

    Sprite& sprite = fixture.add<Sprite>();
    sprite.setSize(120, 60);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(200.0f, 150.0f, 0.0f);
    sprite.setTexture(output);
    // a script's copy of the texture (Lua's getTexture() returns one)
    Texture scriptCopy = fluid.simulator->getOutputTexture();
    EXPECT_EQ(scriptCopy.getFramebuffer(), output);

    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil([&](){ return fluid.simulator->getStats().splats == 1 && output->isCreated(); }))
        << fluid.simulator->getError();
    GpuEnvironment::drawFrames(3);
    int w = 0, h = 0;
    std::vector<uint8_t> rgba = readTarget(w, h);
    PixelProbe probe{rgba, w};
    EXPECT_GT((int)probe.at(200, 150)[0], 150) << "the sprite shows the fluid";
    EXPECT_LT((int)probe.at(200, 150)[1], 80);

    // deleted: its GPU side goes, the Framebuffer object stays while textures show it
    const sg_image outputImage = output->getRender().getColorTexture().backend.get();
    const unsigned long version = output->getVersion();
    scene.destroyEntity(fluidEntity);
    EXPECT_EQ(sg_query_image_state(outputImage), SG_RESOURCESTATE_INVALID);
    ASSERT_FALSE(shared.expired()) << "the textures linked to the output keep it alive";
    EXPECT_FALSE(output->isCreated());
    EXPECT_GT(output->getVersion(), version) << "a new version: the textures bound to it rebind";
    EXPECT_EQ(sprite.getComponent<MeshComponent>().submeshes[0].material.baseColorTexture.getFramebuffer(), output);

    // drawn: no stale image, the transparent placeholder (nothing, as before; black where
    // the sprite is drawn opaque), not the freed fluid nor an opaque white
    GpuEnvironment::drawFrames(3);
    rgba = readTarget(w, h);
    const uint8_t* shown = probe.at(200, 150);
    EXPECT_LT((int)shown[0], 60) << "the sprite shows the placeholder, not the freed fluid";
    EXPECT_LT((int)shown[1], 60);
    EXPECT_LT((int)shown[2], 60);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);

    // the last texture gone, the object goes too
    sprite.getComponent<MeshComponent>().submeshes[0].material.baseColorTexture = Texture();
    scriptCopy = Texture();
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(shared.expired());
}

TEST_F(FluidSimulatorTest, MaterialFluidSurvivesAViewLoss){
    // the shaders ready at once (an exported game's precompiled ones): the output is back at
    // the first frame of the new view
    TestSystem::get().clearLogs();
    const bool async = ShaderPool::isAsyncCompilation();
    ShaderPool::setAsyncCompilation(false);
    {
        SceneFixture fixture;
        Scene& scene = fixture.getScene();
        ensureFluidComponent(scene);
        Sprite& sprite = fixture.add<Sprite>();
        sprite.setSize(120, 60);
        sprite.setPivotPreset(PivotPreset::CENTER);
        sprite.setPosition(200.0f, 150.0f, 0.0f);

        FluidSimulationComponent c = plainFluid();
        c.output = FluidOutput::MATERIAL;
        c.paused = true;
        FluidSplat red = splatAt(0.5f, 0.5f, Vector3(1.0f, 0.0f, 0.0f));
        red.radius = 20.0f;
        c.pendingSplats.push_back(red);
        const Entity entity = sprite.getEntity();
        scene.addComponent<FluidSimulationComponent>(entity, c);
        FluidSimulationComponent& fluid = scene.getComponent<FluidSimulationComponent>(entity);
        FluidSystem* system = scene.getSystem<FluidSystem>().get();

        ASSERT_TRUE(fixture.waitUntilLoaded());
        ASSERT_TRUE(framesUntil([&](){ return system->getMaterialTexture(entity) != nullptr; }));
        GpuEnvironment::drawFrames(2);
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = readTarget(w, h);
        PixelProbe probe{rgba, w};
        ASSERT_GT((int)probe.at(200, 150)[0], 150) << "the untextured sprite shows the fluid";
        ASSERT_LT((int)probe.at(200, 150)[1], 80);

        // the platform drops the view (Android pause, Web context loss) and gives it back
        Engine::systemViewDestroyed();
        EXPECT_FALSE(system->hasMaterialFluids()) << "nothing is lent without a view";
        Engine::systemViewLoaded();
        Engine::systemViewChanged();
        // (the fields restarted: paint again)
        fluid.pendingSplats.push_back(red);
        ASSERT_TRUE(fixture.waitUntilLoaded());
        ASSERT_TRUE(framesUntil([&](){ return system->getMaterialTexture(entity) != nullptr; }));
        GpuEnvironment::drawFrames(3);
        rgba = readTarget(w, h);
        EXPECT_GT((int)probe.at(200, 150)[0], 150) << "the sprite shows the fluid again";
        EXPECT_LT((int)probe.at(200, 150)[1], 80);
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
        scene.removeComponent<FluidSimulationComponent>(entity);
    }
    ShaderPool::setAsyncCompilation(async);
}

TEST_F(FluidSimulatorTest, ScreenFluidStaysUnderTheUI){
    TestSystem::get().clearLogs();
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    ensureFluidComponent(scene);
    // a red box at (40, 30), 220 x 120 (ecs/basic.rml)
    fixture.addDocument("ecs/basic.rml");
    // the scene: a white sprite; its UI: a green image (both made before the fluid)
    Sprite& sprite = fixture.add<Sprite>();
    sprite.setSize(80, 60);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(140.0f, 280.0f, 0.0f);
    jasperengine::Image& image = fixture.add<jasperengine::Image>();
    image.setPosition(400, 40);
    image.setSize(150, 80);
    image.setColor(0.0f, 1.0f, 0.0f, 1.0f);

    // an opaque SCREEN fluid: its blue back colour over the whole view
    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::SCREEN;
    c.paused = true;
    c.backColor = Vector3(0.0f, 0.0f, 1.0f);
    const Entity entity = scene.createEntity();
    scene.addComponent<FluidSimulationComponent>(entity, c);
    FluidSystem* system = scene.getSystem<FluidSystem>().get();
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil([&](){ return system->hasScreenFluids(); }));
    GpuEnvironment::drawFrames(3);

    int w = 0, h = 0;
    std::vector<uint8_t> rgba = readTarget(w, h);
    PixelProbe probe{rgba, w};
    EXPECT_TRUE(probe.is(150, 90, 255, 0, 0, 255, 8)) << "the document's box over the fluid";
    EXPECT_TRUE(probe.is(475, 80, 0, 255, 0, 255, 8)) << "the component UI image over the fluid";
    EXPECT_TRUE(probe.is(500, 300, 0, 0, 255, 255, 8)) << "the fluid over the scene's background";
    EXPECT_TRUE(probe.is(140, 280, 0, 0, 255, 255, 8)) << "the fluid over the scene's sprite";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);

    // transparent: the scene shows through where there is no dye, the UI stays on top
    scene.getComponent<FluidSimulationComponent>(entity).transparent = true;
    GpuEnvironment::drawFrames(3);
    rgba = readTarget(w, h);
    EXPECT_TRUE(probe.isBackground(500, 300));
    EXPECT_GT((int)probe.at(140, 280)[0], 200) << "the sprite shows through";
    EXPECT_TRUE(probe.is(150, 90, 255, 0, 0, 255, 8));
    EXPECT_TRUE(probe.is(475, 80, 0, 255, 0, 255, 8));
    scene.removeComponent<FluidSimulationComponent>(entity);
}

TEST_F(FluidSimulatorTest, PausedLookTargetsRedrawTheOutput){
    // the same dye, paused: a new bloom or sunrays size redraws the output at once
    auto made = [](FluidSimulator& sim, FluidSimulationComponent& c){
        c.paused = true;
        c.pendingSplats.push_back(splatAt(0.4f, 0.6f, Vector3(1.5f, 0.6f, 0.2f)));
        c.pendingSplats.push_back(splatAt(0.7f, 0.3f, Vector3(0.2f, 0.8f, 1.6f)));
        return warmUp(sim, c);
    };
    for (int field = 0; field < 3; field++){
        FluidSimulationComponent a = smallFluid(5);
        a.splatsOnStart = false;
        FluidSimulator simA;
        ASSERT_TRUE(made(simA, a));
        ASSERT_TRUE(frame(simA, a, 1.0f, 0.1));
        const FluidImage before = readOutput(simA);

        FluidSimulationComponent b = a;
        b.pendingSplats.clear();
        b.needReset = true;
        b.simulator.reset();
        if (field == 0)
            b.sunraysResolution = 16;
        else if (field == 1)
            b.bloomResolution = 32;
        else
            b.bloomIterations = 2;
        FluidSimulationComponent reference = b;
        FluidSimulator simB;
        ASSERT_TRUE(made(simB, reference));
        const FluidImage expected = readOutput(simB);
        ASSERT_GT(maxDifference(before, expected), 1) << "field " << field << " does not change the look: no test";

        // the running one, paused: changed at a frame
        a.sunraysResolution = b.sunraysResolution;
        a.bloomResolution = b.bloomResolution;
        a.bloomIterations = b.bloomIterations;
        ASSERT_TRUE(frame(simA, a, 1.0f, 0.1));
        EXPECT_EQ(simA.getStats().steps, 0u);
        EXPECT_LE(maxDifference(readOutput(simA), expected), 1) << "field " << field << ": the output is stale";
    }
}

TEST_F(FluidSimulatorTest, TargetsFitTheDevice){
    // a device whose largest texture is 256: every target fits, the aspect kept
    FluidSimulator::setMaxTextureSizeForTesting(256);
    FluidSimulator sim;
    FluidSimulationComponent c = smallFluid(8);
    c.dyeResolution = 1024;
    c.simResolution = 512;
    c.bloomResolution = 1024;
    c.sunraysResolution = 1024;
    ASSERT_TRUE(warmUp(sim, c, 2.0f)) << sim.getError();
    const FluidStats& stats = sim.getStats();
    EXPECT_EQ(stats.dyeWidth, 256);
    EXPECT_EQ(stats.dyeHeight, 128);
    EXPECT_EQ(stats.simWidth, 256);
    EXPECT_EQ(stats.simHeight, 128);
    EXPECT_EQ(stats.outputWidth, 256);
    EXPECT_TRUE(stats.bloomActive);
    EXPECT_TRUE(stats.sunraysActive);
    EXPECT_EQ(stats.targetFailures, 0);
    FluidSimulator::setMaxTextureSizeForTesting(0);
}

TEST_F(FluidSimulatorTest, OptionalTargetFailureTurnsTheFeatureOff){
    FluidSimulator::setDisabledFeaturesForTesting(FLUID_TEST_FAIL_OPTIONAL_TARGETS);
    FluidSimulator sim;
    FluidSimulationComponent c = smallFluid(9);
    ASSERT_TRUE(warmUp(sim, c)) << sim.getError();
    // drawn without bloom and sunrays, said so
    EXPECT_FALSE(sim.getStats().bloomActive);
    EXPECT_FALSE(sim.getStats().sunraysActive);
    EXPECT_TRUE(sim.getStats().shadingActive);
    EXPECT_NE(sim.getError().find("bloom is off"), std::string::npos) << sim.getError();
    EXPECT_NE(sim.getError().find("sunrays are off"), std::string::npos) << sim.getError();
    EXPECT_GT(brightest(readOutput(sim)), 50);
    // not tried again every frame
    const int failures = sim.getStats().targetFailures;
    EXPECT_GE(failures, 2);
    for (int i = 0; i < 10; i++)
        ASSERT_TRUE(frame(sim, c, 1.0f, 1.0 / 60.0));
    EXPECT_EQ(sim.getStats().targetFailures, failures);
    EXPECT_GT(sim.getStats().steps, 0u);

    // another size is tried again (and works on a device that can)
    FluidSimulator::setDisabledFeaturesForTesting(0);
    c.bloomResolution = 32;
    c.sunraysResolution = 32;
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.0));
    EXPECT_TRUE(sim.getStats().bloomActive);
    EXPECT_TRUE(sim.getStats().sunraysActive);
    EXPECT_TRUE(sim.getError().empty()) << sim.getError();
}

TEST_F(FluidSimulatorTest, RequiredTargetFailureWaitsBeforeRetrying){
    FluidSimulationComponent c = smallFluid(10);
    FluidSimulator sim;
    // the programs first (their compile is not a target failure); then a new size the device
    // cannot make
    ASSERT_TRUE(warmUp(sim, c));
    EXPECT_EQ(sim.getStats().targetFailures, 0);
    FluidSimulator::setDisabledFeaturesForTesting(FLUID_TEST_FAIL_TARGETS);
    c.dyeResolution = 128;
    EXPECT_FALSE(frame(sim, c, 1.0f, 0.0));
    const std::string error = sim.getError();
    EXPECT_NE(error.find("could not make"), std::string::npos) << error;
    const int failures = sim.getStats().targetFailures;
    EXPECT_EQ(failures, 1);
    for (int i = 0; i < 20; i++){
        EXPECT_FALSE(frame(sim, c, 1.0f, 0.0));
        EXPECT_EQ(sim.getError(), error);
    }
    EXPECT_EQ(sim.getStats().targetFailures, failures) << "retried every frame";
    // the device recovers: made again after the wait
    FluidSimulator::setDisabledFeaturesForTesting(0);
    bool drawn = false;
    for (int i = 0; i < 200 && !drawn; i++)
        drawn = frame(sim, c, 1.0f, 0.0);
    EXPECT_TRUE(drawn) << sim.getError();
}


TEST_F(FluidSimulatorTest, ScreenFluidIgnoresUIElementsNotDrawn){
    // a hidden UI element first, then a sprite: the fluid covers the sprite (it goes before
    // the first UI element the pass draws, none here). A canvas camera (no depth test) draws
    // everything in the hierarchy's order, the sprite after the hidden element
    TestSystem::get().clearLogs();
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    ensureFluidComponent(scene);
    fixture.getCamera().getComponent<CameraComponent>().depthTest = false;
    jasperengine::Image& hidden = fixture.add<jasperengine::Image>();
    hidden.setPosition(400, 40);
    hidden.setSize(150, 80);
    hidden.setColor(0.0f, 1.0f, 0.0f, 1.0f);
    hidden.setVisible(false);
    jasperengine::Image& inactive = fixture.add<jasperengine::Image>();
    inactive.setPosition(400, 200);
    inactive.setSize(150, 80);
    inactive.setActive(false);
    Sprite& sprite = fixture.add<Sprite>();
    sprite.setSize(80, 60);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(140.0f, 280.0f, 0.0f);

    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::SCREEN;
    c.paused = true;
    c.backColor = Vector3(0.0f, 0.0f, 1.0f);
    const Entity entity = scene.createEntity();
    scene.addComponent<FluidSimulationComponent>(entity, c);
    FluidSystem* system = scene.getSystem<FluidSystem>().get();
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil([&](){ return system->hasScreenFluids(); }));
    GpuEnvironment::drawFrames(3);

    int w = 0, h = 0;
    std::vector<uint8_t> rgba = readTarget(w, h);
    PixelProbe probe{rgba, w};
    EXPECT_TRUE(probe.is(140, 280, 0, 0, 255, 255, 8)) << "the fluid over the sprite (hidden UI does not pull it forward)";
    EXPECT_TRUE(probe.is(475, 80, 0, 0, 255, 255, 8));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
    scene.removeComponent<FluidSimulationComponent>(entity);
}

TEST_F(FluidSimulatorTest, TerrainMapsFollowARemadeFramebuffer){
    // a terrain whose height and blend maps show a fluid's output; the view is lost and
    // given back: the output is made again and the terrain binds it again
    TestSystem::get().clearLogs();
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    ensureFluidComponent(scene);
    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::TEXTURE;
    c.paused = true;
    c.simulator = std::make_shared<FluidSimulator>();
    Framebuffer* output = c.simulator->getOutputFramebuffer();
    const Entity fluidEntity = scene.createEntity();
    scene.addComponent<FluidSimulationComponent>(fluidEntity, c);
    c.simulator.reset();

    Terrain& terrain = fixture.add<Terrain>();
    terrain.setHeightMap(output);
    terrain.setBlendMap(output);
    TerrainComponent& tc = terrain.getComponent<TerrainComponent>();
    MeshComponent& mesh = terrain.getComponent<MeshComponent>();
    auto bound = [&](){
        return output->isCreated() && mesh.loaded && !tc.needUpdateTexture && !tc.heightMap.isFramebufferOutdated() &&
               !tc.blendMaps[0].isFramebufferOutdated();
    };
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil(bound)) << "loaded " << mesh.loaded << ", needUpdateTexture " << tc.needUpdateTexture;

    Engine::systemViewDestroyed();
    Engine::systemViewLoaded();
    Engine::systemViewChanged();
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil([&](){ return output->isCreated() && mesh.loaded; }));
    GpuEnvironment::drawFrames(4);
    EXPECT_FALSE(tc.heightMap.isFramebufferOutdated()) << "the height map stays on the placeholder";
    EXPECT_FALSE(tc.blendMaps[0].isFramebufferOutdated()) << "the blend map stays on the placeholder";
    EXPECT_FALSE(tc.needUpdateTexture);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
    scene.destroyEntity(fluidEntity);
}
