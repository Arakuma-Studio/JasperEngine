// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Palette Swap and Palette Cycling on the GPU (docs/specs/S43-palettes.md R13-R16, R28, AC7,
// AC9, AC10, AC13): a 16 x 16 library:sprite texture of palette and off-palette colours drawn
// 4 x 4 pixels a texel into the headless target (glcore; Vulkan on lavapipe with
// GRAPHIC_BACKEND=vulkan), read back and compared byte for byte with PaletteSwapData::swap, the
// CPU twin of includes/palette.glsl (Apply Palette's Remap Indices equals that twin:
// editor/tests/pixelart/PaletteSwapRemapTest.cpp).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteSwap.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::pixelart;

namespace {

    constexpr int kTexels = 16;
    constexpr int kScale = 4;
    constexpr const char* kSourcePath = "tests/gpu-swap-source.palette";
    constexpr const char* kTargetPath = "tests/gpu-swap-target.palette";

    // 8 palette colours (the first fully transparent), 2 that are not in it
    Palette sourcePalette() {
        Palette palette;
        palette.name = "Hero";
        palette.colors = {{0, 0, 0, 0}, {26, 28, 44}, {93, 39, 93}, {177, 62, 83}, {239, 125, 87}, {255, 205, 117}, {167, 240, 112}, {56, 183, 100}};
        palette.variants.push_back({"Team Blue", {std::nullopt, std::nullopt, Rgba8{41, 54, 111}, Rgba8{59, 93, 201}, Rgba8{65, 166, 246},
                                                  Rgba8{115, 239, 247}}});
        palette.ramps.push_back({"glow", {4, 5, 6, 7}, 4.0f});
        return palette;
    }

    // a target that hides index 3 (alpha 0)
    Palette targetPalette() {
        Palette palette;
        palette.name = "Ghost";
        palette.colors = {{0, 0, 0, 0}, {255, 255, 255}, {200, 200, 200}, {10, 20, 30, 0}, {20, 20, 20}, {30, 30, 30}, {40, 40, 40}, {50, 50, 50}};
        return palette;
    }

    std::vector<uint8_t> texturePixels() {
        const Palette palette = sourcePalette();
        const Rgba8 offPalette[2] = {{177, 62, 84}, {12, 200, 220}};
        std::vector<uint8_t> pixels;
        for (int y = 0; y < kTexels; y++) {
            for (int x = 0; x < kTexels; x++) {
                const int pick = (x + y * 3) % 10;
                // index 0 is the transparent slot: draw its RGB opaque, it must not swap
                Rgba8 color = pick < 8 ? palette.colors[pick] : offPalette[pick - 8];
                color.a = 255;
                pixels.insert(pixels.end(), {color.r, color.g, color.b, color.a});
            }
        }
        return pixels;
    }

    class PaletteSwapGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<uint8_t> pixels = texturePixels();

        void SetUp() override{
            JASPER_REQUIRE_GPU();
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setOrtho(-320.0f, 320.0f, -180.0f, 180.0f, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
            PaletteLibrary::set(kSourcePath, sourcePalette());
            PaletteLibrary::set(kTargetPath, targetPalette());
        }

        void TearDown() override{
            Engine::setPinnedShaderTime(-1.0);
            Engine::removeScene(&scene);
        }

        // a 64 x 64 sprite at the centre (4 pixels a texel), Nearest
        Sprite& addSprite(Sprite& sprite){
            sprite.setSize(kTexels * kScale, kTexels * kScale);
            sprite.setPivotPreset(PivotPreset::CENTER);
            sprite.setPosition(0.0f, 0.0f, 0.0f);
            Submesh& submesh = sprite.getComponent<MeshComponent>().submeshes[0];
            submesh.material.baseColorTexture.setData("palette-swap-gpu|texture",
                TextureData(kTexels, kTexels, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data()));
            submesh.material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
            submesh.material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
            submesh.needUpdateTexture = true;
            return sprite;
        }

        void usePalette(Sprite& sprite, const std::vector<std::string>& features, float row, float cycleSpeed = 1.0f){
            MeshComponent& mesh = sprite.getComponent<MeshComponent>();
            Material& material = mesh.submeshes[0].material;
            material.shader = "library:sprite";
            material.shaderFeatures = features;
            material.shaderParams = {{"paletteRow", Vector4(row, 0.0f, 0.0f, 0.0f)}, {"paletteCycleSpeed", Vector4(cycleSpeed, 0.0f, 0.0f, 0.0f)}};
            PaletteBinding binding;
            binding.source = kSourcePath;
            binding.targets = {PaletteTarget::parse(kTargetPath)};
            material.shaderPalettes = ShaderPalettes{{"u_palette", binding}};
            mesh.needReload = true;
        }

        void setRow(Sprite& sprite, float row){
            MeshComponent& mesh = sprite.getComponent<MeshComponent>();
            mesh.submeshes[0].material.shaderParams[0].second.x = row;
            mesh.needUpdateShaderUniforms = true;
        }

        bool settle(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                if (!current->getSystem<RenderSystem>()->isAllLoaded())
                    return false;
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++){
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (!mesh.loaded || mesh.needReload)
                        return false;
                    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                        if (mesh.submeshes[s].needUpdateTexture)
                            return false;
                    }
                }
                return true;
            }, 600);
        }

        // the target read back: the texel (x, y) of the sprite is the 4 x 4 block whose centre is read
        std::vector<Rgba8> readTexels(){
            Framebuffer& target = GpuEnvironment::target();
            const int width = int(target.getWidth()), height = int(target.getHeight());
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
            std::vector<Rgba8> texels;
            const int left = width / 2 - kTexels * kScale / 2, top = height / 2 - kTexels * kScale / 2;
            for (int y = 0; y < kTexels; y++){
                for (int x = 0; x < kTexels; x++){
                    // the texture's first row is the sprite's top
                    const int px = left + x * kScale + kScale / 2, py = top + y * kScale + kScale / 2;
                    const uint8_t* p = &rgba[(size_t(py) * width + px) * 4];
                    texels.push_back(Rgba8{p[0], p[1], p[2], 255});
                }
            }
            return texels;
        }

        // what the draw must show for a row: the CPU twin's swap over the black background
        std::vector<Rgba8> expected(int row, float time = 0.0f, float speed = 1.0f, bool cycling = false){
            PaletteBinding binding;
            binding.source = kSourcePath;
            binding.targets = {PaletteTarget::parse(kTargetPath)};
            std::vector<Diagnostic> diagnostics;
            const Palette source = sourcePalette();
            PaletteSwapData data;
            EXPECT_TRUE(buildPaletteSwapData(source, resolvePaletteRows(source, binding, diagnostics), data, diagnostics));
            std::vector<Rgba8> texels;
            for (size_t i = 0; i < pixels.size(); i += 4){
                Rgba8 out = data.swap(Rgba8{pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]}, row, time, speed, cycling);
                if (out.a == 0) out = Rgba8{0, 0, 0, 255};
                texels.push_back(Rgba8{out.r, out.g, out.b, 255});
            }
            return texels;
        }

        static ::testing::AssertionResult sameTexels(const std::vector<Rgba8>& actual, const std::vector<Rgba8>& wanted){
            if (actual.size() != wanted.size())
                return ::testing::AssertionFailure() << "sizes differ";
            int differing = 0;
            std::string first;
            for (size_t i = 0; i < actual.size(); i++){
                if (actual[i] == wanted[i])
                    continue;
                if (differing++ == 0){
                    first = "texel (" + std::to_string(i % kTexels) + ", " + std::to_string(i / kTexels) + ") is " + toHex(actual[i]) +
                            ", expected " + toHex(wanted[i]);
                }
            }
            if (differing == 0)
                return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << differing << " texels differ; " << first;
        }
    };

}

// AC7: rows 0, 1 (the variant) and 2 (the target) give exactly the expected bytes; row 0 is the
// texture itself, the off-palette texels and the transparent slot's RGB stay, alpha 0 hides
TEST_F(PaletteSwapGpu, SwapsRowsExactly){
    Sprite sprite(&scene);
    addSprite(sprite);
    usePalette(sprite, {"PALETTE_SWAP"}, 0.0f);
    ASSERT_TRUE(settle());
    const std::vector<Rgba8> original = readTexels();
    EXPECT_TRUE(sameTexels(original, expected(0)));
    for (int row : {1, 2}){
        setRow(sprite, (float)row);
        ASSERT_TRUE(settle());
        EXPECT_TRUE(sameTexels(readTexels(), expected(row))) << "row " << row;
    }
    // the row is a uniform: changing it made no texture
    const size_t entries = PaletteSwapCache::size();
    setRow(sprite, 0.0f);
    ASSERT_TRUE(settle());
    EXPECT_EQ(PaletteSwapCache::size(), entries);
    EXPECT_TRUE(sameTexels(readTexels(), original));
    EXPECT_NO_ENGINE_ERRORS();
}

// AC7, R28: the feature off draws today's pixels (the texture's bytes) and makes no swap texture
TEST_F(PaletteSwapGpu, FeatureOffDrawsTheTextureAndMakesNothing){
    PaletteSwapCache::clear();
    Sprite sprite(&scene);
    addSprite(sprite);
    usePalette(sprite, {}, 2.0f);
    ASSERT_TRUE(settle());
    EXPECT_TRUE(sameTexels(readTexels(), expected(0)));
    EXPECT_EQ(PaletteSwapCache::size(), 0u) << "no PaletteSwapCache entry while the feature is off";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC10: a 4-colour ramp cycling 4 steps a second, the clock pinned at 0.25 s steps: the four
// rotations in order; speed -1 reverses, 0 freezes; a held clock holds
TEST_F(PaletteSwapGpu, Cycling){
    Sprite sprite(&scene);
    addSprite(sprite);
    usePalette(sprite, {"PALETTE_CYCLE"}, 0.0f, 1.0f);
    for (float speed : {1.0f, -1.0f, 0.0f}){
        MeshComponent& mesh = sprite.getComponent<MeshComponent>();
        mesh.submeshes[0].material.shaderParams[1].second.x = speed;
        mesh.needUpdateShaderUniforms = true;
        for (int step = 0; step < 4; step++){
            const float time = 10.0f + step * 0.25f;
            Engine::setPinnedShaderTime(time);
            ASSERT_TRUE(settle());
            EXPECT_TRUE(sameTexels(readTexels(), expected(0, time, speed, true))) << "speed " << speed << " step " << step;
        }
    }
    Engine::setPinnedShaderTime(11.0);
    ASSERT_TRUE(settle());
    const std::vector<Rgba8> held = readTexels();
    GpuEnvironment::drawFrames(3);
    EXPECT_TRUE(sameTexels(readTexels(), held)) << "the pinned clock holds the colours";
    EXPECT_NO_ENGINE_ERRORS();
}

// R29: a binding no draw holds any more is freed 2 s later, not at once
TEST_F(PaletteSwapGpu, UnusedTexturesAreFreedAfterTwoSeconds){
    PaletteSwapCache::clear();
    PaletteBinding binding;
    binding.source = kSourcePath;
    std::vector<Diagnostic> diagnostics;
    {
        std::shared_ptr<PaletteSwapTextures> textures = PaletteSwapCache::acquire(binding, diagnostics);
        ASSERT_TRUE(textures);
        EXPECT_FALSE(textures->identity);
        EXPECT_EQ(PaletteSwapCache::acquire(binding, diagnostics), textures) << "one set per content";
        EXPECT_EQ(PaletteSwapCache::size(), 1u);
    }
    PaletteSwapCache::sweep(100.0);
    EXPECT_EQ(PaletteSwapCache::size(), 1u);
    PaletteSwapCache::sweep(101.5);
    EXPECT_EQ(PaletteSwapCache::size(), 1u);
    PaletteSwapCache::sweep(102.1);
    EXPECT_EQ(PaletteSwapCache::size(), 0u);
}
