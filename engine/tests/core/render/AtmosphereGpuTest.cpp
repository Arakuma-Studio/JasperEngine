// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The atmosphere on the headless device (docs/specs/S12-atmosphere.md): height fog goldens in
// perspective, orthographic and Sky colour mode (AC4), the volumetric fog's handoff to the
// analytic fog (AC5), light shafts (AC6), Local Volumetric Fog volumes (AC7), the clouds in both
// modes with converged frames bit-identical (AC8), the zero-cost rule (AC2), transparents (AC16)
// and the RGBA8 targets (R14). Goldens in engine/tests/core/golden/atmosphere_*.png, made on
// glcore (JASPER_TEST_UPDATE_GOLDEN=1) and matched on Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "object/environment/Fog.h"
#include "object/environment/LocalVolumetricFog.h"
#include "object/environment/VolumetricClouds.h"
#include "object/environment/Skybox.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/AtmosphereRenderer.h"
#include "render/AtmospherePresets.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the spec's golden tolerance: mean difference 1/255, 99% of the pixels within 8/255. The
    // goldens are glcore's; another backend is held to AC11's cross-backend bound (mean 3/255),
    // with up to 5% of the pixels over 8/255 (the thin cloud edges near the horizon move)
    CompareOptions goldenTolerance(){
        CompareOptions options;
        const bool goldenBackend = std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
        options.pixelThreshold = 8;
        options.maxBadFraction = goldenBackend ? 0.01 : 0.05;
        options.maxMeanDiff = goldenBackend ? 1.0 : 3.0;
        return options;
    }

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    // mean over the pixels of the largest channel difference (0..255)
    double meanDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size() || a.empty())
            return 255.0;
        double sum = 0.0;
        const size_t pixels = a.size() / 4;
        for (size_t i = 0; i < pixels; i++){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]));
            sum += largest;
        }
        return sum / (double)pixels;
    }

    // Rec. 709 luminance of the mean colour of a w x h block at (x, y), rows top first
    double luminance(const std::vector<uint8_t>& rgba, int x, int y, int w, int h){
        const int width = (int)GpuEnvironment::target().getWidth();
        double r = 0.0, g = 0.0, b = 0.0;
        for (int j = y; j < y + h; j++){
            for (int i = x; i < x + w; i++){
                const size_t p = ((size_t)j * width + i) * 4;
                r += rgba[p];
                g += rgba[p + 1];
                b += rgba[p + 2];
            }
        }
        const double n = (double)(w * h);
        return 0.2126 * r / n + 0.7152 * g / n + 0.0722 * b / n;
    }

    // the target pixel a world point is drawn at (rows top first)
    bool project(const Camera& camera, const Vector3& world, int& x, int& y){
        const Vector4 clip = camera.getViewProjectionMatrix() * Vector4(world.x, world.y, world.z, 1.0f);
        if (clip.w <= 0.0f)
            return false;
        const float width = (float)GpuEnvironment::target().getWidth();
        const float height = (float)GpuEnvironment::target().getHeight();
        x = (int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * width);
        y = (int)std::lround((1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height);
        return x >= 0 && y >= 0 && x < (int)width && y < (int)height;
    }

    class AtmosphereGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<Primitive>> primitives;

        void SetUp() override{
            scene.setBackgroundColor(0.42f, 0.58f, 0.82f, 1.0f);
            // the goldens were drawn before S11's Lit Quality: Standard is that shader exactly
            scene.setLitQuality(LitQuality::STANDARD);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.activate();
            Engine::setScene(&scene);
            Engine::setAtmosphereQuality(AtmosphereQuality::HIGH);
        }

        void TearDown() override{
            Engine::resetAtmosphereQuality();
            AtmosphereRenderer::setForceLowPrecisionTargets(false);
            Engine::removeScene(&scene);
        }

        RenderSystem& renderSystem(){
            return *scene.getSystem<RenderSystem>();
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        Primitive& box(const Vector3& position, const Vector3& size, const Vector3& color){
            primitives.push_back(std::make_unique<Primitive>(&scene));
            Primitive& primitive = *primitives.back();
            primitive.createBox(size.x, size.y, size.z);
            primitive.setPosition(position.x, position.y + size.y * 0.5f, position.z);
            primitive.setColor(color.x, color.y, color.z);
            return primitive;
        }

        Primitive& ground(float size, const Vector3& color){
            primitives.push_back(std::make_unique<Primitive>(&scene));
            Primitive& primitive = *primitives.back();
            primitive.createPlane(size, size);
            primitive.setColor(color.x, color.y, color.z);
            return primitive;
        }

        // a ground with buildings at rising distances and a low sun
        void buildValley(Light& sun){
            ground(1200.0f, Vector3(0.35f, 0.42f, 0.30f));
            for (int i = 0; i < 8; i++){
                const float z = -15.0f - 30.0f * (float)i;
                const float x = (i % 2 ? -1.0f : 1.0f) * (6.0f + 3.0f * (float)i);
                box(Vector3(x, 0.0f, z), Vector3(5.0f, 8.0f + 4.0f * (float)i, 5.0f), Vector3(0.7f, 0.66f, 0.6f));
            }
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.35f, -0.25f, -1.0f);
            sun.setColor(1.0f, 0.86f, 0.7f);
            sun.setIntensity(2.0f);
            sun.setShadows(false);
        }

        void valleyFog(Fog& fog){
            fog.setType(FogType::HEIGHT);
            fog.setColor(0.62f, 0.68f, 0.78f);
            fog.setAttenuationDistance(90.0f);
            fog.setBaseHeight(0.0f);
            fog.setMaximumHeight(30.0f);
            fog.setDirectionalInscattering(0.5f);
            fog.setAnisotropy(0.6f);
            fog.setMaxFogDistance(3000.0f);
        }

        const AtmosphereInfo& info(){
            return renderSystem().getAtmosphereInfo(0);
        }
    };

}

TEST_F(AtmosphereGpu, NothingRunsWithoutAtmosphere){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    buildValley(sun);
    camera.setPosition(0.0f, 8.0f, 30.0f);
    camera.setTarget(0.0f, 4.0f, -100.0f);
    ASSERT_TRUE(render());
    EXPECT_TRUE(info().passes.empty());
    EXPECT_EQ(info().vramBytes, 0u);

    // a distance fog runs none either
    Fog fog(&scene);
    fog.setType(FogType::EXPONENTIAL);
    fog.setDensity(0.01f);
    ASSERT_TRUE(render());
    EXPECT_TRUE(info().passes.empty());

    // a height fog without sky fill: still none
    valleyFog(fog);
    fog.setSkyFill(false);
    ASSERT_TRUE(render());
    EXPECT_TRUE(info().passes.empty());
    EXPECT_EQ(info().vramBytes, 0u);

    // with sky fill: the sky composite alone
    fog.setSkyFill(true);
    ASSERT_TRUE(render());
    ASSERT_EQ(info().passes.size(), 1u);
    EXPECT_EQ(info().passes[0], "Atmosphere/SkyComposite");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereGpu, HeightFogValley){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    buildValley(sun);
    Fog fog(&scene);
    valleyFog(fog);
    camera.setPosition(0.0f, 8.0f, 30.0f);
    camera.setTarget(0.0f, 4.0f, -100.0f);
    camera.setFarClip(3000.0f);
    ASSERT_TRUE(render());
    EXPECT_GOLDEN_OPTIONS("atmosphere_valley", goldenTolerance());
    EXPECT_TRUE(info().heightFog);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereGpu, HeightFogOrthographic){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.5f, -1.0f, -0.3f);
    sun.setShadows(false);
    ground(200.0f, Vector3(0.4f, 0.4f, 0.42f));
    // a tall tower: its top is above the fog, its foot in it
    box(Vector3(0.0f, 0.0f, 0.0f), Vector3(4.0f, 40.0f, 4.0f), Vector3(0.85f, 0.3f, 0.25f));
    box(Vector3(14.0f, 0.0f, -10.0f), Vector3(4.0f, 6.0f, 4.0f), Vector3(0.25f, 0.5f, 0.85f));
    Fog fog(&scene);
    fog.setType(FogType::HEIGHT);
    fog.setColor(0.9f, 0.9f, 0.92f);
    fog.setAttenuationDistance(25.0f);
    fog.setBaseHeight(2.0f);
    fog.setMaximumHeight(24.0f);
    fog.setDirectionalInscattering(0.0f);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-40.0f, 40.0f, -22.5f, 22.5f, 1.0f, 400.0f);
    camera.setPosition(60.0f, 60.0f, 60.0f);
    camera.setTarget(0.0f, 10.0f, 0.0f);
    ASSERT_TRUE(render());
    EXPECT_GOLDEN_OPTIONS("atmosphere_ortho_iso", goldenTolerance());

    // the fog varies with world height (not with the ortho depth): the tower's top keeps its
    // red, its foot takes the fog's white
    const std::vector<uint8_t> image = readTarget();
    int topX = 0, topY = 0, footX = 0, footY = 0;
    ASSERT_TRUE(project(camera, Vector3(1.9f, 22.0f, 1.9f), topX, topY));
    ASSERT_TRUE(project(camera, Vector3(1.9f, 1.0f, 1.9f), footX, footY));
    const int width = (int)GpuEnvironment::target().getWidth();
    const uint8_t* top = &image[((size_t)topY * width + topX) * 4];
    const uint8_t* foot = &image[((size_t)footY * width + footX) * 4];
    // red over blue: the colour of the tower shows at the top, washed towards white at the foot
    EXPECT_GT((int)top[0] - (int)top[2], 60);
    EXPECT_LT((int)foot[0] - (int)foot[2], (int)top[0] - (int)top[2] - 40);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereGpu, HeightFogSkyColorModeFromACubemap){
    JASPER_REQUIRE_GPU();
    // a cube map sky: blue above, a warm horizon (reddish towards +X, greenish towards -X)
    const int size = 32;
    static std::vector<uint8_t> faces[6];
    for (int f = 0; f < 6; f++){
        faces[f].assign((size_t)size * size * 4, 255);
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                uint8_t* p = &faces[f][((size_t)y * size + x) * 4];
                float r = 0.25f, g = 0.45f, b = 0.85f;           // the zenith's blue (and the nadir's)
                if (f != 2 && f != 3){
                    // the side faces: a band of colour at the horizon (the middle rows, so the
                    // faces' row order does not matter)
                    const float t = std::clamp(((float)y + 0.5f - size * 0.5f) / (size * 0.5f), -1.0f, 1.0f);
                    const float warm = 1.0f - std::min(std::fabs(t) * 3.0f, 1.0f);
                    const Vector3 horizon = f == 0 ? Vector3(0.95f, 0.55f, 0.35f) : (f == 1 ? Vector3(0.55f, 0.85f, 0.45f) : Vector3(0.9f, 0.75f, 0.5f));
                    r = r + (horizon.x - r) * warm;
                    g = g + (horizon.y - g) * warm;
                    b = b + (horizon.z - b) * warm;
                }
                p[0] = (uint8_t)std::lround(r * 255.0f);
                p[1] = (uint8_t)std::lround(g * 255.0f);
                p[2] = (uint8_t)std::lround(b * 255.0f);
            }
        }
    }
    auto face = [&](int f){
        return TextureData(size, size, (unsigned int)faces[f].size(), ColorFormat::RGBA, 4, faces[f].data());
    };
    Skybox sky(&scene);
    sky.setTextures("atmosphere-test-sky", face(0), face(1), face(2), face(3), face(4), face(5));

    Light sun(&scene);
    buildValley(sun);
    Fog fog(&scene);
    valleyFog(fog);
    fog.setColorMode(FogColorMode::SKY);
    fog.setSkyFillAngle(8.0f);
    camera.setPosition(0.0f, 6.0f, 30.0f);
    camera.setTarget(40.0f, 6.0f, -60.0f);
    camera.setFarClip(3000.0f);
    ASSERT_TRUE(render());
    EXPECT_EQ(info().horizonSource, "cubemap");
    EXPECT_GOLDEN_OPTIONS("atmosphere_sky_mode_cubemap", goldenTolerance());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereGpu, VolumetricFogHandsOffToTheAnalyticFog){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    buildValley(sun);
    Fog fog(&scene);
    valleyFog(fog);
    fog.setVolumetricDistance(70.0f);
    // looking down the valley floor: every pixel is geometry
    camera.setPosition(0.0f, 14.0f, 30.0f);
    camera.setTarget(0.0f, 0.0f, -60.0f);
    camera.setFarClip(3000.0f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> analytic = readTarget();
    const int width = (int)GpuEnvironment::target().getWidth();
    const int height = (int)GpuEnvironment::target().getHeight();
    writePng(getOutputDirectory() + "/atmosphere_handoff_analytic.png", analytic, width, height);

    fog.setVolumetric(true);
    for (AtmosphereQuality quality : {AtmosphereQuality::LOW, AtmosphereQuality::MEDIUM, AtmosphereQuality::HIGH}){
        Engine::setAtmosphereQuality(quality);
        ASSERT_TRUE(render());
        EXPECT_TRUE(info().volumetricActive) << AtmosphereTiers::getName(quality);
        const std::vector<uint8_t> volumetric = readTarget();
        writePng(getOutputDirectory() + "/atmosphere_handoff_" + AtmosphereTiers::getName(quality) + ".png", volumetric, width, height);
        const double difference = meanDifference(analytic, volumetric);
        std::printf("[atmosphere] handoff %s: mean difference %.3f\n", AtmosphereTiers::getName(quality), difference);
        EXPECT_LE(difference, 2.0) << AtmosphereTiers::getName(quality);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereGpu, LowPrecisionTargetsMatchFloatOnes){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    buildValley(sun);
    Fog fog(&scene);
    valleyFog(fog);
    fog.setVolumetric(true);
    fog.setNoiseIntensity(0.6f);
    camera.setPosition(0.0f, 8.0f, 30.0f);
    camera.setTarget(0.0f, 4.0f, -100.0f);
    camera.setFarClip(3000.0f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> floatTargets = readTarget();
    AtmosphereRenderer::setForceLowPrecisionTargets(true);
    ASSERT_TRUE(render());
    EXPECT_FALSE(info().floatTargets);
    EXPECT_LE(meanDifference(floatTargets, readTarget()), 3.0);
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    // a roof over the scene with one gap the sun shines through: a lit slab of fog among the
    // shadowed one, along the view (light shafts)
    struct ShaftsScene{
        Light sun;
        Fog fog;
        explicit ShaftsScene(Scene* scene): sun(scene), fog(scene){}
    };

}

TEST_F(AtmosphereGpu, LightShafts){
    JASPER_REQUIRE_GPU();
    ShaftsScene shafts(&scene);
    ground(400.0f, Vector3(0.2f, 0.2f, 0.2f));
    box(Vector3(0.0f, 0.0f, -60.0f), Vector3(160.0f, 30.0f, 2.0f), Vector3(0.08f, 0.08f, 0.1f));
    // a roof at 12 m over the camera and the view, in two parts with a gap at x 9..17
    box(Vector3(-35.5f, 12.0f, -24.0f), Vector3(89.0f, 1.0f, 72.0f), Vector3(0.1f, 0.1f, 0.1f));
    box(Vector3(49.0f, 12.0f, -24.0f), Vector3(64.0f, 1.0f, 72.0f), Vector3(0.1f, 0.1f, 0.1f));
    shafts.sun.setType(LightType::DIRECTIONAL);
    // falling to +X: the beam through the gap reaches x 6..14 at the camera's height
    shafts.sun.setDirection(0.35f, -1.0f, 0.0f);
    shafts.sun.setIntensity(1.0f);
    shafts.sun.setShadows(true);
    shafts.fog.setType(FogType::HEIGHT);
    shafts.fog.setColor(0.02f, 0.02f, 0.03f);
    shafts.fog.setAttenuationDistance(40.0f);
    shafts.fog.setBaseHeight(20.0f);
    shafts.fog.setMaximumHeight(60.0f);
    shafts.fog.setDirectionalInscattering(0.5f);
    // isotropic: without shadows two columns mirrored about the view see the same fog
    shafts.fog.setAnisotropy(0.0f);
    shafts.fog.setVolumetric(true);
    shafts.fog.setVolumetricDistance(80.0f);
    shafts.fog.setSkyFill(false);
    camera.setPosition(0.0f, 3.0f, 10.0f);
    camera.setTarget(0.0f, 3.0f, -60.0f);
    camera.setFarClip(150.0f);

    // two columns mirrored about the view, at the horizon: the right one crosses the beam
    const int height = (int)GpuEnvironment::target().getHeight();
    int litX = 0, litY = 0, shadowX = 0, shadowY = 0;
    ASSERT_TRUE(project(camera, Vector3(25.0f, 3.0f, -59.0f), litX, litY));
    ASSERT_TRUE(project(camera, Vector3(-25.0f, 3.0f, -59.0f), shadowX, shadowY));
    const int band = height / 8;

    const char* goldens[] = {"atmosphere_shafts_low", "atmosphere_shafts_medium", "atmosphere_shafts_high"};
    int index = 0;
    for (AtmosphereQuality quality : {AtmosphereQuality::LOW, AtmosphereQuality::MEDIUM, AtmosphereQuality::HIGH}){
        Engine::setAtmosphereQuality(quality);
        shafts.fog.setLightShafts(true);
        ASSERT_TRUE(render());
        const std::vector<uint8_t> image = readTarget();
        const double lit = luminance(image, litX - 4, litY - band / 2, 8, band);
        const double shadowed = luminance(image, shadowX - 4, shadowY - band / 2, 8, band);
        std::printf("[atmosphere] shafts %s: lit %.1f shadowed %.1f\n", AtmosphereTiers::getName(quality), lit, shadowed);
        EXPECT_LE(shadowed, lit * 0.85) << AtmosphereTiers::getName(quality);
        EXPECT_GOLDEN_OPTIONS(goldens[index++], goldenTolerance());

        // without light shafts the fog is lit everywhere
        shafts.fog.setLightShafts(false);
        ASSERT_TRUE(render());
        const std::vector<uint8_t> flat = readTarget();
        const double litFlat = luminance(flat, litX - 4, litY - band / 2, 8, band);
        const double shadowedFlat = luminance(flat, shadowX - 4, shadowY - band / 2, 8, band);
        std::printf("[atmosphere] shafts off %s: lit %.1f shadowed %.1f\n", AtmosphereTiers::getName(quality), litFlat, shadowedFlat);
        EXPECT_NEAR(shadowedFlat, litFlat, std::max(litFlat, shadowedFlat) * 0.02) << AtmosphereTiers::getName(quality);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereGpu, LocalVolumetricFog){
    JASPER_REQUIRE_GPU();
    ground(200.0f, Vector3(0.15f, 0.15f, 0.15f));
    box(Vector3(0.0f, 0.0f, -40.0f), Vector3(80.0f, 30.0f, 2.0f), Vector3(0.05f, 0.05f, 0.06f));
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(0.2f, -1.0f, -0.3f);
    sun.setShadows(false);
    Fog fog(&scene);
    fog.setType(FogType::HEIGHT);
    fog.setColor(0.5f, 0.5f, 0.5f);
    fog.setAttenuationDistance(20000.0f);
    fog.setDirectionalInscattering(0.0f);
    fog.setVolumetric(true);
    fog.setVolumetricDistance(60.0f);
    fog.setSkyFill(false);
    camera.setPosition(0.0f, 3.0f, 10.0f);
    camera.setTarget(0.0f, 3.0f, -40.0f);
    camera.setFarClip(200.0f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> clear = readTarget();

    LocalVolumetricFog volume(&scene);
    volume.setPosition(0.0f, 3.0f, -10.0f);
    volume.setSize(6.0f, 6.0f, 6.0f);
    volume.setAttenuationDistance(2.0f);
    volume.setAlbedo(0.9f, 0.9f, 0.9f);
    volume.setBlendDistance(0.5f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> boxed = readTarget();
    EXPECT_EQ(info().volumesUsed, 1);

    int insideX = 0, insideY = 0, outsideX = 0, outsideY = 0, cornerX = 0, cornerY = 0;
    ASSERT_TRUE(project(camera, Vector3(0.0f, 3.0f, -10.0f), insideX, insideY));
    // 1 m outside the size plus the blend distance
    ASSERT_TRUE(project(camera, Vector3(4.5f, 3.0f, -10.0f), outsideX, outsideY));
    ASSERT_TRUE(project(camera, Vector3(3.9f, 3.0f, -10.0f), cornerX, cornerY));
    EXPECT_GT(luminance(boxed, insideX - 2, insideY - 2, 4, 4), luminance(clear, insideX - 2, insideY - 2, 4, 4) + 20.0);
    EXPECT_NEAR(luminance(boxed, outsideX - 2, outsideY - 2, 4, 4), luminance(clear, outsideX - 2, outsideY - 2, 4, 4), 1.5);
    // the box's corner is beyond 3.9 m only when it turns
    EXPECT_NEAR(luminance(boxed, cornerX - 1, cornerY - 2, 2, 4), luminance(clear, cornerX - 1, cornerY - 2, 2, 4), 1.5);
    volume.setRotation(0.0f, 45.0f, 0.0f);
    ASSERT_TRUE(render());
    EXPECT_GT(luminance(readTarget(), cornerX - 1, cornerY - 2, 2, 4), luminance(clear, cornerX - 1, cornerY - 2, 2, 4) + 3.0);

    // ten volumes: the tier's cap, the nearest first
    std::vector<std::unique_ptr<LocalVolumetricFog>> more;
    for (int i = 0; i < 9; i++){
        more.push_back(std::make_unique<LocalVolumetricFog>(&scene));
        more.back()->setPosition(-8.0f + 2.0f * (float)i, 1.0f, -14.0f - 2.0f * (float)i);
        more.back()->setSize(1.0f, 1.0f, 1.0f);
    }
    const int caps[3] = {2, 4, 8};
    int index = 0;
    for (AtmosphereQuality quality : {AtmosphereQuality::LOW, AtmosphereQuality::MEDIUM, AtmosphereQuality::HIGH}){
        Engine::setAtmosphereQuality(quality);
        ASSERT_TRUE(render());
        EXPECT_EQ(info().volumesUsed, caps[index]) << AtmosphereTiers::getName(quality);
        EXPECT_EQ(info().volumesCulled, 10 - caps[index]) << AtmosphereTiers::getName(quality);
        index++;
    }
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    // frames until the noise is baked and the clouds faded in and converged
    bool settleClouds(Scene& scene){
        RenderSystem* system = scene.getSystem<RenderSystem>().get();
        if (!GpuEnvironment::runFrames([system](){ return system->isAllLoaded() && system->getAtmosphereInfo(0).noiseReady; }, 3000))
            return false;
        const auto start = std::chrono::steady_clock::now();
        // the fade-in (0.5 s) and a whole Sky Map refresh with its crossfade
        int frames = 0;
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(800) || frames < 20){
            GpuEnvironment::drawFrames(1);
            frames++;
        }
        return true;
    }

}

TEST_F(AtmosphereGpu, VolumetricClouds){
    JASPER_REQUIRE_GPU();
    ground(20000.0f, Vector3(0.3f, 0.35f, 0.28f));
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -0.5f, -1.0f);
    sun.setIntensity(2.5f);
    sun.setShadows(false);
    camera.setPosition(0.0f, 20.0f, 0.0f);
    camera.setTarget(0.0f, 400.0f, -1500.0f);
    camera.setFarClip(50000.0f);

    VolumetricClouds clouds(&scene);
    const char* presets[3] = {"sparse", "cloudy", "overcast"};
    const AtmosphereQuality qualities[3] = {AtmosphereQuality::LOW, AtmosphereQuality::MEDIUM, AtmosphereQuality::HIGH};
    const char* qualityNames[3] = {"low", "medium", "high"};
    for (int p = 0; p < 3; p++){
        CloudsPreset preset;
        ASSERT_TRUE(AtmospherePresets::findCloudsPreset(presets[p], preset));
        clouds.setPreset(preset);
        clouds.setWindSpeed(0.0f);
        for (int q = 0; q < 3; q++){
            Engine::setAtmosphereQuality(qualities[q]);
            ASSERT_TRUE(settleClouds(scene)) << presets[p];
            EXPECT_TRUE(info().cloudsActive);
            EXPECT_EQ(info().cloudsMode, q == 2 ? CloudsRendering::SCREEN : CloudsRendering::SKY_MAP);
            const std::vector<uint8_t> first = readTarget();
            GpuEnvironment::drawFrames(1);
            const std::vector<uint8_t> second = readTarget();
            // static per-pixel jitter, no history: a converged frame is the one before it
            EXPECT_TRUE(first == second) << presets[p] << " " << qualityNames[q];
            EXPECT_GOLDEN_OPTIONS(std::string("atmosphere_clouds_") + presets[p] + "_" + qualityNames[q], goldenTolerance());
        }
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// The wrap of a component's weather map is its authored setting, saved with the scene: a draw does
// not write it (spec S63), and the clouds tile whatever it says
TEST_F(AtmosphereGpu, AWeatherMapTilesWhateverItsWrap){
    JASPER_REQUIRE_GPU();
    ground(20000.0f, Vector3(0.3f, 0.35f, 0.28f));
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -0.5f, -1.0f);
    sun.setIntensity(2.5f);
    sun.setShadows(false);
    camera.setPosition(0.0f, 20.0f, 0.0f);
    camera.setTarget(0.0f, 400.0f, -1500.0f);
    camera.setFarClip(50000.0f);

    // full cover on one half of the map, none on the other: clamped, it would stay one colour past
    // its edge; tiled, bands of cloud repeat every kilometre of the scale
    const int size = 16;
    std::vector<uint8_t> pixels((size_t)size * size * 4, 255);
    for (int y = 0; y < size; y++){
        for (int x = 0; x < size; x++){
            uint8_t* p = &pixels[((size_t)y * size + x) * 4];
            p[0] = p[1] = p[2] = x < size / 2 ? 255 : 0;
        }
    }
    const auto weatherData = [&](){
        return TextureData(size, size, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    };

    VolumetricClouds clouds(&scene);
    clouds.setPreset(CloudsPreset::OVERCAST);
    clouds.setWindSpeed(0.0f);
    clouds.setWeatherMapScale(2.0f);
    Texture& map = scene.getComponent<VolumetricCloudsComponent>(clouds.getEntity()).weatherMap;

    for (AtmosphereQuality quality : {AtmosphereQuality::MEDIUM, AtmosphereQuality::HIGH}){
        Engine::setAtmosphereQuality(quality);

        map.setData("weather-repeat", weatherData());
        ASSERT_TRUE(settleClouds(scene));
        EXPECT_TRUE(info().cloudsActive);
        const std::vector<uint8_t> tiled = readTarget();

        map.setData("weather-clamp", weatherData());
        map.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        map.setWrapV(TextureWrap::CLAMP_TO_EDGE);
        ASSERT_TRUE(settleClouds(scene));
        EXPECT_TRUE(info().cloudsActive);
        EXPECT_EQ(map.getWrapU(), TextureWrap::CLAMP_TO_EDGE) << "a draw wrote the component's wrap";
        EXPECT_EQ(map.getWrapV(), TextureWrap::CLAMP_TO_EDGE) << "a draw wrote the component's wrap";
        EXPECT_LE(meanDifference(tiled, readTarget()), 0.05) << "the clouds tile whatever the wrap";

        map.setWrapU(TextureWrap::REPEAT);
        map.setWrapV(TextureWrap::REPEAT);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereGpu, ScreenCloudsHideFarTerrain){
    JASPER_REQUIRE_GPU();
    ground(40000.0f, Vector3(0.3f, 0.35f, 0.28f));
    // a far mountain rising through the cloud layer
    box(Vector3(0.0f, 0.0f, -7000.0f), Vector3(9000.0f, 4500.0f, 1000.0f), Vector3(0.45f, 0.4f, 0.38f));
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -0.5f, -1.0f);
    sun.setShadows(false);
    camera.setPosition(0.0f, 30.0f, 0.0f);
    camera.setTarget(0.0f, 1500.0f, -7000.0f);
    camera.setFarClip(60000.0f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> bare = readTarget();

    int mountainX = 0, mountainY = 0;
    ASSERT_TRUE(project(camera, Vector3(0.0f, 2200.0f, -6500.0f), mountainX, mountainY));

    VolumetricClouds clouds(&scene);
    clouds.setPreset(CloudsPreset::OVERCAST);
    clouds.setWindSpeed(0.0f);
    Engine::setAtmosphereQuality(AtmosphereQuality::HIGH);
    ASSERT_TRUE(settleClouds(scene));
    EXPECT_EQ(info().cloudsMode, CloudsRendering::SCREEN);
    EXPECT_GT(std::fabs(luminance(readTarget(), mountainX - 8, mountainY - 8, 16, 16) - luminance(bare, mountainX - 8, mountainY - 8, 16, 16)), 4.0);

    // the Sky Map clouds are behind every surface
    clouds.setRendering(CloudsRendering::SKY_MAP);
    ASSERT_TRUE(settleClouds(scene));
    EXPECT_EQ(info().cloudsMode, CloudsRendering::SKY_MAP);
    EXPECT_NEAR(luminance(readTarget(), mountainX - 8, mountainY - 8, 16, 16), luminance(bare, mountainX - 8, mountainY - 8, 16, 16), 1.0);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AtmosphereGpu, TransparentsTakeTheAnalyticFog){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    buildValley(sun);
    camera.setPosition(0.0f, 4.0f, 30.0f);
    camera.setTarget(0.0f, 4.0f, -10.0f);
    camera.setFarClip(3000.0f);
    primitives.push_back(std::make_unique<Primitive>(&scene));
    Primitive& glass = *primitives.back();
    glass.createSphere(3.0f);
    glass.setPosition(0.0f, 4.0f, -20.0f);
    glass.setColor(0.2f, 0.8f, 0.3f, 0.5f);
    glass.setTransparent(true);
    ASSERT_TRUE(render());
    int x = 0, y = 0;
    ASSERT_TRUE(project(camera, Vector3(0.0f, 4.0f, -20.0f), x, y));
    const std::vector<uint8_t> clear = readTarget();

    Fog fog(&scene);
    valleyFog(fog);
    fog.setAttenuationDistance(30.0f);
    fog.setVolumetric(true);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> fogged = readTarget();
    // the glass's green is washed towards the fog's grey-blue
    const int width = (int)GpuEnvironment::target().getWidth();
    const uint8_t* before = &clear[((size_t)y * width + x) * 4];
    const uint8_t* after = &fogged[((size_t)y * width + x) * 4];
    EXPECT_LT((int)after[1] - (int)after[0], (int)before[1] - (int)before[0] - 20);
    EXPECT_NO_ENGINE_ERRORS();
}
