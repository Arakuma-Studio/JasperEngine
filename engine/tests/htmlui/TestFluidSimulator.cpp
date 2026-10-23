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

#include "System.h"
#include "component/MeshComponent.h"
#include "fluid/FluidSimulator.h"
#include "object/Sprite.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "subsystem/FluidSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
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

    struct Image {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;

        // the texel at fluid coordinates (0,0 bottom-left): rows are top first
        const uint8_t* at(float fx, float fy) const{
            const int x = std::clamp((int)(fx * (float)width), 0, width - 1);
            const int y = std::clamp((int)((1.0f - fy) * (float)height), 0, height - 1);
            return &rgba[((size_t)y * (size_t)width + (size_t)x) * 4];
        }
    };

    Image readOutput(FluidSimulator& sim){
        Image image;
        Framebuffer* output = sim.getOutputFramebuffer();
        image.width = (int)output->getWidth();
        image.height = (int)output->getHeight();
        EXPECT_TRUE(readTexture(output->getRender().getColorTexture(), image.width, image.height, image.rgba));
        return image;
    }

    size_t differingBytes(const Image& a, const Image& b){
        if (a.rgba.size() != b.rgba.size())
            return (size_t)-1;
        size_t n = 0;
        for (size_t i = 0; i < a.rgba.size(); i++)
            n += a.rgba[i] != b.rgba[i] ? 1 : 0;
        return n;
    }

    // the largest channel difference (INT_MAX when the sizes differ)
    int maxDifference(const Image& a, const Image& b){
        if (a.rgba.size() != b.rgba.size())
            return 1 << 30;
        int worst = 0;
        for (size_t i = 0; i < a.rgba.size(); i++)
            worst = std::max(worst, std::abs((int)a.rgba[i] - (int)b.rgba[i]));
        return worst;
    }

    int brightest(const Image& image){
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

    const Image ia = readOutput(a);
    const Image ib = readOutput(b);
    const Image io = readOutput(other);
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
    Image empty = readOutput(sim);
    const uint8_t* black = empty.at(0.25f, 0.75f);
    EXPECT_EQ(black[0], 0);
    EXPECT_EQ(black[3], 255) << "opaque over the back colour";

    // red at the top left: queued splats apply at the next update, with no step
    c.pendingSplats.push_back(splatAt(0.25f, 0.75f, Vector3(1.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(frame(sim, c, 1.0f, 0.0));
    EXPECT_TRUE(c.pendingSplats.empty());
    EXPECT_EQ(sim.getStats().splats, 1u);
    EXPECT_EQ(sim.getStats().steps, 0u);
    Image image = readOutput(sim);
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
    const Image image = readOutput(sim);
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
    const Image wide = readOutput(sim);
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
    const Image reference = readOutput(a);
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
    if (features.renderRG16F && features.filterRG16F && features.renderR16F)
        EXPECT_TRUE(a.getStats().compactFormats);
    EXPECT_LT(b.getStats().gpuBytes, compactBytes * 2);
}

TEST_F(FluidSimulatorTest, ManualFilteringFallback){
    // the reference: hardware filtering, the look off (manual filtering turns it off)
    FluidSimulationComponent linear = plainFluid();
    linear.splatsOnStart = true;
    FluidSimulator a;
    ASSERT_TRUE(warmUp(a, linear));
    steps(a, linear, 1.0f, 6);
    const Image reference = readOutput(a);
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
    // his bilerp by hand: the same fluid as the hardware's filter, to its precision
    const Image image = readOutput(sim);
    EXPECT_GT(brightest(image), 100);
    EXPECT_LE(maxDifference(image, reference), 24);
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

TEST(FluidShaderType, NamesAndKeys){
    EXPECT_EQ(ShaderPool::getShaderTypeName(ShaderType::FLUID, true), "fluid");
    EXPECT_EQ(ShaderPool::getShaderTypeName(ShaderType::FLUID), "Fluid");
    EXPECT_EQ(ShaderPool::getShaderPropertyCount(ShaderType::FLUID), 2);
    EXPECT_EQ(ShaderPool::getValidPropertyMask(ShaderType::FLUID), 3u);
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::FLUID, 0), "fluid");
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::FLUID, 1), "fluid_Lok");
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::FLUID, 2), "fluid_Mfl");
    ShaderKey key = 0;
    ASSERT_TRUE(ShaderPool::parseShaderStr("fluid_Mfl", key));
    EXPECT_EQ(ShaderPool::getShaderTypeFromKey(key), ShaderType::FLUID);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), 2u);
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

    // red over the whole fluid (a wide splat), no steps
    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::MATERIAL;
    c.paused = true;
    FluidSplat red = splatAt(0.5f, 0.5f, Vector3(1.0f, 0.0f, 0.0f));
    red.radius = 1000.0f;
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
    const uint8_t* inside = probe.at(200, 150);
    EXPECT_GT((int)inside[0], 150) << "the sprite shows the fluid";
    EXPECT_LT((int)inside[1], 60);
    EXPECT_TRUE(probe.isBackground(400, 300));

    // a paused system (the editor's edit mode) does not step; a preview does
    fluid.paused = false;
    system->setPaused(true);
    const uint64_t before = fluid.simulator->getStats().steps;
    GpuEnvironment::drawFrames(10);
    EXPECT_EQ(fluid.simulator->getStats().steps, before);
    system->setPreview(entity, true);
    EXPECT_TRUE(system->isPreview(entity));
    EXPECT_TRUE(framesUntil([&](){ return fluid.simulator->getStats().steps > before; }));
    system->setPreview(entity, false);
    system->setPaused(false);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);

    // another output: the sprite shows its own texture again (white, untextured)
    fluid.output = FluidOutput::TEXTURE;
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(system->getMaterialTexture(entity), nullptr);
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), w, h, rgba));
    EXPECT_GT((int)probe.at(200, 150)[1], 150) << "the sprite's own (white) look is back";
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

TEST_F(FluidSimulatorTest, SystemKeepsAnExistingSimulator){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    ensureFluidComponent(scene);
    // made before the system's first update (the wrapper's getTexture(), a texture link)
    FluidSimulationComponent c = plainFluid();
    c.output = FluidOutput::TEXTURE;
    c.simulator = std::make_shared<FluidSimulator>();
    FluidSimulator* made = c.simulator.get();
    Framebuffer* output = made->getOutputFramebuffer();
    const Entity entity = scene.createEntity();
    scene.addComponent<FluidSimulationComponent>(entity, c);
    FluidSimulationComponent& fluid = scene.getComponent<FluidSimulationComponent>(entity);
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(framesUntil([&](){ return output->isCreated(); }));
    EXPECT_EQ(fluid.simulator.get(), made);
    EXPECT_EQ(fluid.simulator->getOutputFramebuffer(), output);
    scene.removeComponent<FluidSimulationComponent>(entity);
}
