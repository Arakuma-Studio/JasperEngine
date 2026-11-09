// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// PCSS on GLES3/WebGL2 (draw-perf spec, "Follow-ups (2026-11-07)"): a sun-only scene at the Ultra
// tier draws the same contact-hardening shadow through the GLES3 build of the built-in Lit as
// through the desktop one. The GL core device runs the GLSL ES 3.00 programs (Mesa takes
// "#version 300 es" in a core context): the mesh variants are built for GLES3 by a ShaderPool
// builder, the rest of the frame is the desktop's. The High tier's image (the Vogel disk) shows
// that the Ultra one is PCSS. The frame times on the test device are printed ([pcss-gles] lines:
// llvmpipe, not a mobile GPU). A painted terrain takes PCSS on desktop too, and its heaviest variant
// stays within the 16 samplers sokol has for both stages and the 16 fragment textures GLES3 and
// macOS GL have (the view left out), which the test device (32 units) would not show by itself:
// the counts are asserted.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "object/Terrain.h"
#include "object/environment/Skybox.h"
#include "pool/ShaderPool.h"
#include "render/LightingTiers.h"
#include "render/shadow/ShadowTypes.h"
#include "shader/RuntimeShaderCompiler.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"
#include "texture/Framebuffer.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

// GLSL ES programs on the GL core device: a Vulkan or Metal build of the tests has no such test
// (a skip fails the unit gate, which allows none)
#if defined(SOKOL_GLCORE)

namespace {

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    struct Difference {
        int maxDiff = 0;
        size_t pixels = 0;   // pixels with a channel more than 8 apart
    };

    Difference differenceOf(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        Difference out;
        if (a.size() != b.size()){
            out.maxDiff = 255;
            return out;
        }
        for (size_t p = 0; p + 3 < a.size(); p += 4){
            int pixelMax = 0;
            for (size_t c = 0; c < 3; c++)
                pixelMax = std::max(pixelMax, std::abs((int)a[p + c] - (int)b[p + c]));
            out.maxDiff = std::max(out.maxDiff, pixelMax);
            if (pixelMax > 8)
                out.pixels++;
        }
        return out;
    }

    class ShadowPcssGlesGpu : public ::testing::Test {
    public:
        Scene scene;
        Camera camera{&scene};
        std::unique_ptr<Light> sun;
        std::vector<std::unique_ptr<Primitive>> boxes;
        std::unique_ptr<Terrain> terrain;
        std::unique_ptr<Skybox> sky;
        std::unique_ptr<Light> bulb;

        void SetUp() override{
            // tools/test-projects/pcss draws this scene in WebGL2 (GATE=pcss tools/build.sh web-gate):
            // keep the two alike. The camera looks at the floor beside the far half of the pole's
            // shadow, where PCSS widens the penumbra past the Vogel disk's
            scene.setBackgroundColor(0.42f, 0.58f, 0.82f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(-0.5f, 4.5f, 2.5f);
            camera.setTarget(-2.74f, 0.0f, -2.02f);
            camera.activate();
            // the object lists: a directional-only variant is never the tiled form
            LightingTiers::setLightMode(LightMode::PER_OBJECT);
            // each mesh draws with its own variant (a batch would draw with the batcher's)
            Engine::setGpuInstancingEnabled(false);
            Engine::setDynamicBatching(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            boxes.clear();
            terrain.reset();
            sky.reset();
            bulb.reset();
            sun.reset();
            Engine::removeScene(&scene);
            LightingTiers::setLightMode(LightMode::AUTOMATIC);
            Engine::setGpuInstancingEnabled(true);
            Engine::setDynamicBatching(true);
        }

        // a sun with a wide disc (8 degrees) and fixed maps, so High and Ultra differ only in the
        // soft filter; a pole on a floor: its shadow is hard at the base and wide at the tip
        void build(){
            makeSun();
            boxes.push_back(std::make_unique<Primitive>(&scene));
            boxes.back()->createBox(12.0f, 0.2f, 12.0f);
            boxes.back()->setColor(0.85f, 0.85f, 0.85f);
            boxes.back()->setPosition(0.0f, -0.1f, 0.0f);
            makePole();
        }

        void makeSun(){
            if (!sun){
                sun = std::make_unique<Light>(&scene);
                sun->setType(LightType::DIRECTIONAL);
                sun->setDirection(-0.6f, -1.0f, -0.3f);
                sun->setIntensity(2.0f);
                sun->setShadows(true);
                sun->setShadowType(ShadowType::SOFT);
                sun->setShadowAngularDiameter(8.0f);
                sun->setShadowMapSize(512);
                sun->setShadowDistance(20.0f);
            }
        }

        void makePole(){
            boxes.push_back(std::make_unique<Primitive>(&scene));
            boxes.back()->createBox(0.5f, 6.0f, 0.5f);
            boxes.back()->setColor(0.8f, 0.4f, 0.2f);
            boxes.back()->setPosition(0.0f, 3.0f, 0.0f);
        }

        // a flat painted terrain in place of the floor: one PBR layer, the terrain's material
        // normal-mapped (the heaviest painted terrain), with the pole on it
        void buildTerrain(const TempDir& dir){
            makeSun();
            auto flat = [&dir](const char* name, uint8_t r, uint8_t g, uint8_t b){
                std::vector<uint8_t> rgba(16 * 16 * 4);
                for (size_t p = 0; p < rgba.size(); p += 4){
                    rgba[p] = r; rgba[p + 1] = g; rgba[p + 2] = b; rgba[p + 3] = 255;
                }
                const std::string path = dir.file(name).generic_string();
                EXPECT_TRUE(writePng(path, rgba, 16, 16)) << path;
                return path;
            };
            const std::string height = flat("height.png", 0, 0, 0);
            const std::string blend = flat("blend.png", 255, 0, 0);
            const std::string grey = flat("grey.png", 217, 217, 217);
            const std::string normal = flat("normal.png", 128, 128, 255);
            terrain = std::make_unique<Terrain>(&scene);
            terrain->setSize(12.0f);
            terrain->setMaxHeight(1.0f);
            terrain->setHeightMap(height);
            terrain->setBlendMap(blend);
            terrain->setTexture(grey);
            terrain->setNormalTexture(normal);
            TerrainSurfaceLayer layer;
            layer.pbr = true;
            layer.colorTexture.setPath(grey);
            layer.normalTexture.setPath(normal);
            terrain->setSurfaceLayer(0, layer);
            terrain->createTerrain();
            makePole();
        }

        // the terrain's program as the test device's compiler builds it: its fragment textures,
        // its samplers across both stages, and whether it has PCSS's raw-depth view
        struct Bindings { size_t fragmentTextures = 0; size_t samplers = 0; bool raw = false; std::string name; };
        Bindings terrainBindings(){
            const Submesh& submesh = terrain->getComponent<MeshComponent>().submeshes[0];
            CustomShaderSources sources;
            if (submesh.customShaderId != 0)
                sources = ShaderVariantCompiler::resolveCustomShaderSources(ShaderPool::getCustomShaderName(submesh.customShaderId), nullptr);
            ShaderData data;
            std::string error, log;
            Bindings out;
            out.name = ShaderPool::getShaderStr(ShaderType::MESH, submesh.shaderProperties, submesh.customShaderId);
            EXPECT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, submesh.shaderProperties, submesh.customShaderId),
                                                       ShaderPool::getShaderBackend(), submesh.customShaderId ? &sources : nullptr, data, &error, &log))
                << out.name << ": " << error;
            for (const ShaderStage& stage : data.stages){
                out.samplers += stage.samplers.size();
                if (stage.type == ShaderStageType::FRAGMENT)
                    out.fragmentTextures = stage.textures.size();
            }
            out.raw = data.getTextureIndex(TextureShaderType::SHADOWATLASRAW).first >= 0;
            std::printf("[pcss-gles] %s: %zu fragment textures, %zu samplers in both stages, %s\n", out.name.c_str(), out.fragmentTextures,
                        out.samplers, out.raw ? "PCSS" : "no PCSS");
            return out;
        }

        // the image (for the report, beside the test's other output) and its centre pixel
        void keep(const std::vector<uint8_t>& rgba, const char* name){
            Framebuffer& target = GpuEnvironment::target();
            const int width = (int)target.getWidth();
            const int height = (int)target.getHeight();
            std::error_code ignored;
            std::filesystem::create_directories(getOutputDirectory(), ignored);
            writePng(getOutputDirectory() + "/" + name + ".png", rgba, width, height);
            const uint8_t* centre = rgba.data() + ((size_t)(height / 2) * width + width / 2) * 4;
            std::printf("[pcss-gles] %s: centre pixel %d %d %d\n", name, centre[0], centre[1], centre[2]);
        }

        void settle(){
            Scene* current = &scene;
            ASSERT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            })) << "the scene never finished loading";
            GpuEnvironment::drawFrames(2);
        }

        // the mean wall time of a frame, the last read back so the device has drawn them all
        // (printed only: the test device is a software rasterizer)
        double frameMs(){
            constexpr int frames = 10;
            const auto start = std::chrono::steady_clock::now();
            GpuEnvironment::drawFrames(frames);
            readTarget();
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
        }

        uint64_t floorVariant(){
            return boxes.front()->getComponent<MeshComponent>().submeshes[0].shaderProperties;
        }
    };

}

TEST_F(ShadowPcssGlesGpu, GlesProgramsDrawTheDesktopContactHardening){
    JASPER_REQUIRE_GPU();
    build();
    scene.setShadowQuality(ShadowQuality::HIGH);
    settle();
    const std::vector<uint8_t> high = readTarget();
    const double highMs = frameMs();

    scene.setShadowQuality(ShadowQuality::ULTRA);
    settle();
    const std::vector<uint8_t> desktop = readTarget();
    const double desktopMs = frameMs();
    // the floor's program: a variant of the built-in Lit, whose "builtin:lit#..." reference (its
    // Lit Quality) is a custom shader id
    const Submesh& floorSubmesh = boxes.front()->getComponent<MeshComponent>().submeshes[0];
    const uint64_t variant = floorSubmesh.shaderProperties;
    const uint16_t customId = floorSubmesh.customShaderId;
    ASSERT_NE(variant & ShaderPool::getDirectionalOnlyProperty(), 0u) << "a sun-only scene draws the 'Dlo' Lit";
    ASSERT_NE(variant & ShaderPool::getShadowsHQProperty(), 0u);
    const Difference pcss = differenceOf(high, desktop);
    std::printf("[pcss-gles] Ultra against High (PCSS against the Vogel disk): max %d, %zu pixels\n", pcss.maxDiff, pcss.pixels);
    EXPECT_GT(pcss.pixels, 200u) << "Ultra draws the High tier's shadow: no PCSS on desktop to compare with";

    // the same scene through the GLES3 programs of the mesh variants
    boxes.clear();
    GpuEnvironment::drawFrames(2);
    ShaderPool::remove(ShaderType::MESH, variant, customId);
    const ShaderKey variantKey = ShaderPool::getShaderKey(ShaderType::MESH, variant, customId);
    std::atomic<int> glesBuilds{0};
    ShaderPool::setShaderBuilder([&glesBuilds, variantKey](ShaderKey key){
        if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH)
            return RuntimeShaderCompiler::request(key);
        const uint16_t keyCustomId = ShaderPool::getCustomIdFromKey(key);
        CustomShaderSources sources;
        if (keyCustomId != 0)
            sources = ShaderVariantCompiler::resolveCustomShaderSources(ShaderPool::getCustomShaderName(keyCustomId), nullptr);
        ShaderData data;
        std::string error, log;
        if (!ShaderVariantCompiler::compile(key, ShaderBackend::GLES3, keyCustomId != 0 ? &sources : nullptr, data, &error, &log)){
            ADD_FAILURE() << error << "\n" << log;
            return ShaderBuildResult({}, ResourceLoadState::Failed);
        }
        if (key == variantKey)
            glesBuilds.fetch_add(1);
        return ShaderBuildResult(data, ResourceLoadState::Finished);
    });
    struct Restore{
        ShadowPcssGlesGpu* test;
        uint64_t variant;
        uint16_t customId;
        ~Restore(){
            ShaderPool::setShaderBuilder(nullptr);
            test->boxes.clear();
            GpuEnvironment::drawFrames(2);
            // the next suites draw the desktop program again
            ShaderPool::remove(ShaderType::MESH, variant, customId);
        }
    } restore{this, variant, customId};

    // made before the meshes load, so they never draw a ready stand-in while it is created
    std::shared_ptr<ShaderRender> glesProgram;
    ASSERT_TRUE(GpuEnvironment::runFrames([&glesProgram, variant, customId](){
        glesProgram = ShaderPool::get(ShaderType::MESH, variant, customId);
        return glesProgram->isCreated();
    })) << "the GLES3 program was never created";
    ASSERT_GT(glesBuilds.load(), 0) << "the variant kept the desktop program (still in the pool)";
    build();
    settle();
    ASSERT_EQ(floorVariant(), variant);
    ASSERT_EQ(boxes.front()->getComponent<MeshComponent>().submeshes[0].shader.get(), glesProgram.get())
        << "the floor draws another program than the GLES3 one";
    const std::vector<uint8_t> gles = readTarget();
    const double glesMs = frameMs();
    glesProgram.reset();

    keep(high, "shadow_pcss_gles_high");
    keep(desktop, "shadow_pcss_gles_ultra_desktop");
    keep(gles, "shadow_pcss_gles_ultra_gles3");
    std::printf("[pcss-gles] llvmpipe %ux%u, sun-only Lit 'Dlo': High (Vogel) %.2f ms, Ultra (PCSS) %.2f ms, Ultra with the GLES3 programs %.2f ms a frame\n",
                GpuEnvironment::target().getWidth(), GpuEnvironment::target().getHeight(), highMs, desktopMs, glesMs);
    const Difference fromDesktop = differenceOf(desktop, gles);
    const Difference fromHigh = differenceOf(high, gles);
    std::printf("[pcss-gles] GLES3 programs against desktop: max %d, %zu pixels; against High: max %d, %zu pixels\n",
                fromDesktop.maxDiff, fromDesktop.pixels, fromHigh.maxDiff, fromHigh.pixels);
    EXPECT_LE(fromDesktop.maxDiff, 2) << "the GLES3 programs draw another shadow than desktop's (" << fromDesktop.pixels << " pixels)";
    EXPECT_NO_ENGINE_ERRORS();
}

// a painted terrain takes PCSS at Ultra on desktop too: the pole's shadow on it hardens at contact
TEST_F(ShadowPcssGlesGpu, PaintedTerrainTakesContactHardening){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    buildTerrain(dir);
    scene.setShadowQuality(ShadowQuality::HIGH);
    settle();
    const std::vector<uint8_t> high = readTarget();
    scene.setShadowQuality(ShadowQuality::ULTRA);
    settle();
    const std::vector<uint8_t> ultra = readTarget();
    const uint64_t properties = terrain->getComponent<MeshComponent>().submeshes[0].shaderProperties;
    ASSERT_NE(properties & shaderPropertyBit(27), 0u) << "the terrain does not draw its painted ('Tpb') variant";
    ASSERT_NE(properties & ShaderPool::getShadowsHQProperty(), 0u);
    const Bindings bindings = terrainBindings();
    EXPECT_TRUE(bindings.raw) << bindings.name << " has no PCSS";
    keep(high, "shadow_pcss_terrain_high");
    keep(ultra, "shadow_pcss_terrain_ultra");
    const Difference pcss = differenceOf(high, ultra);
    std::printf("[pcss-gles] painted terrain, Ultra against High: max %d, %zu pixels\n", pcss.maxDiff, pcss.pixels);
    EXPECT_GT(pcss.pixels, 200u) << "the painted terrain draws the High tier's shadow at Ultra";
    EXPECT_NO_ENGINE_ERRORS();
}

// the heaviest painted terrain (a point light possible: no 'Dlo'; IBL, SSAO, the colour LUT, a
// normal-mapped material) at Ultra: built without the view, within 16 samplers and 16 fragment
// textures, created and drawn with every slot bound
TEST_F(ShadowPcssGlesGpu, HeaviestPaintedTerrainStaysWithinTheBindSlots){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    std::vector<uint8_t> blue(16 * 16 * 4);
    for (size_t p = 0; p < blue.size(); p += 4){
        blue[p] = 60; blue[p + 1] = 90; blue[p + 2] = 200; blue[p + 3] = 255;
    }
    const std::string face = dir.file("sky.png").generic_string();
    ASSERT_TRUE(writePng(face, blue, 16, 16));
    sky = std::make_unique<Skybox>(&scene);
    sky->setTextures(face, face, face, face, face, face);
    bulb = std::make_unique<Light>(&scene);
    bulb->setType(LightType::POINT);
    bulb->setEnabled(false);
    scene.setSSAOEnabled(true);
    PostProcessingSettings post = scene.getPostProcessing();
    post.vignetteEnabled = true;
    scene.setPostProcessing(post);
    scene.setShadowQuality(ShadowQuality::ULTRA);
    buildTerrain(dir);
    // the sky's environment lights it (off by default)
    terrain->getComponent<MeshComponent>().receiveIBL = true;
    settle();

    const Submesh& submesh = terrain->getComponent<MeshComponent>().submeshes[0];
    const uint64_t properties = submesh.shaderProperties;
    ASSERT_NE(properties & shaderPropertyBit(27), 0u) << "the terrain does not draw its painted ('Tpb') variant";
    EXPECT_EQ(properties & ShaderPool::getDirectionalOnlyProperty(), 0u) << "a point light in the scene, and the terrain is 'Dlo'";
    for (int bit : {19, 21, 31})
        EXPECT_NE(properties & shaderPropertyBit(bit), 0u) << "the terrain's variant lacks bit " << bit << " (IBL, SSAO, colour LUT)";
    ASSERT_NE(properties & ShaderPool::getShadowsHQProperty(), 0u);
    const Bindings bindings = terrainBindings();
    EXPECT_LE(bindings.samplers, 16u) << bindings.name << ": over sokol's 16 sampler slots";
    EXPECT_LE(bindings.fragmentTextures, 16u) << bindings.name << ": over 16 fragment texture units";
    EXPECT_TRUE(ShaderPool::get(ShaderType::MESH, properties, submesh.customShaderId)->isCreated()) << bindings.name << " was not created";
    // the terrain is drawn: the centre is its floor (grey, lit by the blue sky), not the sky (60 90 200)
    const std::vector<uint8_t> image = readTarget();
    keep(image, "shadow_pcss_terrain_heaviest");
    Framebuffer& target = GpuEnvironment::target();
    const uint8_t* centre = image.data() + ((size_t)(target.getHeight() / 2) * target.getWidth() + target.getWidth() / 2) * 4;
    EXPECT_GT(std::abs((int)centre[0] - 60), 40) << "the centre is the sky, not the terrain";
    EXPECT_NO_ENGINE_ERRORS();
}

#endif
