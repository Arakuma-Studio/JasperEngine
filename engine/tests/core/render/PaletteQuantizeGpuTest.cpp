// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Palette Quantize in the uber pass (docs/specs/S43-palettes.md R19-R21, D5, AC12): a sprite
// covering the view, drawn through the post-processing chain into the headless target (glcore;
// Vulkan on lavapipe with GRAPHIC_BACKEND=vulkan) and read back: (a) a frame already in the
// palette is unchanged with every dither; (b) at a fixed resolution of 160 x 90 upscaled 4x every
// pixel is a palette colour and the dither is constant within each 4 x 4 block (one art pixel);
// (c) CRT on top still runs; (d) the test card's goldens for None, Bayer 2, 4 and 8.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "pixelart/Palette.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::pixelart;

namespace {

    constexpr const char* kPalettePath = "tests/gpu-quantize.palette";

    Palette quantizePalette() {
        Palette palette;
        palette.name = "Quantize";
        palette.colors = {{15, 56, 15}, {48, 98, 48}, {139, 172, 15}, {155, 188, 15}, {200, 40, 40}, {40, 40, 200}, {250, 250, 250}, {0, 0, 0}};
        return palette;
    }

    class PaletteQuantizeGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Sprite sprite{&scene};
        std::vector<uint8_t> pixels;
        int textureWidth = 0;
        int textureHeight = 0;

        void SetUp() override{
            JASPER_REQUIRE_GPU();
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setOrtho(-320.0f, 320.0f, -180.0f, 180.0f, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
            PaletteLibrary::set(kPalettePath, quantizePalette());
            sprite.setSize(640.0f, 360.0f);
            sprite.setPivotPreset(PivotPreset::CENTER);
            sprite.setPosition(0.0f, 0.0f, 0.0f);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        void setTexture(const std::string& id, int width, int height, std::vector<uint8_t> rgba){
            pixels = std::move(rgba);
            textureWidth = width;
            textureHeight = height;
            Submesh& submesh = sprite.getComponent<MeshComponent>().submeshes[0];
            submesh.material.baseColorTexture.setData(id, TextureData(width, height, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data()));
            submesh.material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
            submesh.material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
            submesh.needUpdateTexture = true;
        }

        // the palette's colours in 8 x 1 blocks
        void paletteTexture(){
            const Palette palette = quantizePalette();
            std::vector<uint8_t> rgba;
            for (int y = 0; y < 9; y++){
                for (int x = 0; x < 16; x++){
                    const Rgba8 c = palette.colors[(x / 2 + y) % palette.count()];
                    rgba.insert(rgba.end(), {c.r, c.g, c.b, 255});
                }
            }
            setTexture("palette-quantize-gpu|palette", 16, 9, rgba);
        }

        // a test card: a hue and lightness sweep
        void gradientTexture(){
            std::vector<uint8_t> rgba;
            for (int y = 0; y < 90; y++){
                for (int x = 0; x < 160; x++){
                    rgba.insert(rgba.end(), {(uint8_t)(x * 255 / 159), (uint8_t)(y * 255 / 89), (uint8_t)(255 - x * 255 / 159), 255});
                }
            }
            setTexture("palette-quantize-gpu|gradient", 160, 90, rgba);
        }

        void quantize(PaletteDither dither, bool fixed = false){
            PostProcessingSettings settings;
            settings.paletteEnabled = true;
            settings.palette = kPalettePath;
            settings.paletteDither = dither;
            scene.setPostProcessing(settings);
            scene.setFixedResolutionEnabled(fixed);
            if (fixed){
                scene.setFixedResolutionWidth(160);
                scene.setFixedResolutionHeight(90);
            }
        }

        // frames until the scene is loaded and the table is baked
        bool settle(){
            Scene* current = &scene;
            int frames = 0;
            int* counted = &frames;
            return GpuEnvironment::runFrames([current, counted](){
                std::shared_ptr<RenderSystem> render = current->getSystem<RenderSystem>();
                return ++(*counted) > 3 && render->isAllLoaded() && !render->isPaletteQuantizeBaking();
            }, 600);
        }

        std::vector<uint8_t> read(int& width, int& height){
            Framebuffer& target = GpuEnvironment::target();
            width = int(target.getWidth());
            height = int(target.getHeight());
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
            return rgba;
        }

        static bool inPalette(const uint8_t* p){
            for (const Rgba8& c : quantizePalette().colors){
                if (c.r == p[0] && c.g == p[1] && c.b == p[2])
                    return true;
            }
            return false;
        }
    };

}

// (a) the identity: an image made only of palette colours is unchanged with each dither size
TEST_F(PaletteQuantizeGpu, PaletteImagePassesThrough){
    paletteTexture();
    quantize(PaletteDither::NONE);
    ASSERT_TRUE(settle());
    int width = 0, height = 0;
    const std::vector<uint8_t> reference = read(width, height);
    for (PaletteDither dither : {PaletteDither::NONE, PaletteDither::BAYER2, PaletteDither::BAYER4, PaletteDither::BAYER8}){
        quantize(dither);
        ASSERT_TRUE(settle());
        const std::vector<uint8_t> frame = read(width, height);
        int outside = 0, changed = 0;
        for (size_t p = 0; p + 3 < frame.size(); p += 4){
            if (!inPalette(&frame[p])) outside++;
            if (frame[p] != reference[p] || frame[p + 1] != reference[p + 1] || frame[p + 2] != reference[p + 2]) changed++;
        }
        EXPECT_EQ(outside, 0) << "dither " << (int)dither;
        EXPECT_EQ(changed, 0) << "dither " << (int)dither;
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// (b) fixed resolution 160 x 90 upscaled 4x: every pixel in the palette, the pattern constant in
// each 4 x 4 block; (d) the goldens of the four dithers
TEST_F(PaletteQuantizeGpu, GradientAtFixedResolution){
    gradientTexture();
    sprite.setSize(160.0f, 90.0f);
    camera.setOrtho(-80.0f, 80.0f, -45.0f, 45.0f, -100.0f, 100.0f);
    const char* goldens[] = {"palette_quantize_none", "palette_quantize_bayer2", "palette_quantize_bayer4", "palette_quantize_bayer8"};
    int index = 0;
    std::vector<std::vector<uint8_t>> frames;
    for (PaletteDither dither : {PaletteDither::NONE, PaletteDither::BAYER2, PaletteDither::BAYER4, PaletteDither::BAYER8}){
        quantize(dither, true);
        ASSERT_TRUE(settle());
        int width = 0, height = 0;
        const std::vector<uint8_t> frame = read(width, height);
        ASSERT_EQ(width % 4, 0);
        int outside = 0, smeared = 0;
        for (int y = 0; y < height; y++){
            for (int x = 0; x < width; x++){
                const uint8_t* p = &frame[(size_t(y) * width + x) * 4];
                if (!inPalette(p)) outside++;
                const uint8_t* corner = &frame[(size_t(y - y % 4) * width + (x - x % 4)) * 4];
                if (p[0] != corner[0] || p[1] != corner[1] || p[2] != corner[2]) smeared++;
            }
        }
        EXPECT_EQ(outside, 0) << goldens[index];
        EXPECT_EQ(smeared, 0) << goldens[index] << ": the dither must not change inside an art pixel";
        EXPECT_GOLDEN(goldens[index]);
        frames.push_back(frame);
        index++;
    }
    // each dither size draws its own pattern (8 x 8 has its 64 levels, not 4 x 4's 16)
    for (size_t a = 0; a < frames.size(); a++){
        for (size_t b = a + 1; b < frames.size(); b++)
            EXPECT_NE(frames[a], frames[b]) << goldens[a] << " and " << goldens[b];
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// (c) CRT on top of the palette image still runs: its scanlines and mask leave the palette
TEST_F(PaletteQuantizeGpu, CrtRunsOnTop){
    gradientTexture();
    PostProcessingSettings settings;
    settings.paletteEnabled = true;
    settings.palette = kPalettePath;
    settings.paletteDither = PaletteDither::BAYER4;
    settings.crtEnabled = true;
    settings.crtScanlines = 0.6f;
    settings.crtMask = 0.4f;
    scene.setPostProcessing(settings);
    ASSERT_TRUE(settle());
    int width = 0, height = 0;
    const std::vector<uint8_t> frame = read(width, height);
    int outside = 0;
    for (size_t p = 0; p + 3 < frame.size(); p += 4){
        if (!inPalette(&frame[p])) outside++;
    }
    EXPECT_GT(outside, 0) << "the display emulation darkens the palette image, by design";
    EXPECT_NO_ENGINE_ERRORS();
}
