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
#include "profiler/GpuMemory.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
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
            Engine::setFixedDeltatime(0.0);
            Engine::setTimeScale(1.0f);
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

namespace {

    // AC9's palettes (editor/tests/pixelart/PaletteSwapRemapTest.cpp's TheGpuFixtureIsTheRemap
    // remaps palette_remap_64_input.png with the same two lists)
    Palette remapPalette(const std::vector<const char*>& hexes) {
        Palette palette;
        palette.name = "Remap";
        for (const char* hex : hexes) {
            Rgba8 color;
            EXPECT_TRUE(parseHex(hex, color)) << hex;
            palette.colors.push_back(color);
        }
        return palette;
    }

    Palette remapSource() {
        return remapPalette({"#00000000", "#1a1c2c", "#5d275d", "#b13e53", "#ef7d57", "#ffcd75", "#a7f070", "#38b764",
                             "#257179", "#29366f", "#3b5dc9", "#41a6f6", "#73eff7", "#f4f4f4", "#94b0c2", "#566c86"});
    }

    Palette remapTarget() {
        return remapPalette({"#00000000", "#e5e3d3", "#a2d8a2", "#4ec1ac00", "#108257", "#0033cd", "#58098f", "#c74838",
                             "#da8e71", "#d6c991", "#c4a235", "#be590b", "#8c0f08", "#0b0b0b", "#6b4f3d", "#a99379"});
    }

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
        // the control: a draw that swapped nothing would show row 0, which these rows are not
        ASSERT_FALSE(sameTexels(expected(row), expected(0))) << "row " << row << " must differ from the source";
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
            if (speed != 0.0f && step > 0){
                ASSERT_FALSE(sameTexels(expected(0, time, speed, true), expected(0, 10.0f, speed, true))) << "each step rotates the ramp";
            }
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

// AC9: the GPU swap of a 64 x 64 sprite equals Apply Palette's Remap Indices to the target palette
// byte for byte (the fixture the editor test checks is that remap; alpha 0 shows the background)
TEST_F(PaletteSwapGpu, MatchesRemap){
    std::vector<uint8_t> input, wanted;
    int inputWidth = 0, inputHeight = 0, wantedWidth = 0, wantedHeight = 0;
    ASSERT_TRUE(readPng(getGoldenDirectory() + "/palette_remap_64_input.png", input, inputWidth, inputHeight));
    ASSERT_TRUE(readPng(getGoldenDirectory() + "/palette_remap_64.png", wanted, wantedWidth, wantedHeight));
    ASSERT_EQ(inputWidth, 64);
    ASSERT_EQ(wantedWidth, 64);
    PaletteLibrary::set("tests/gpu-remap-source.palette", remapSource());
    PaletteLibrary::set("tests/gpu-remap-target.palette", remapTarget());

    Sprite sprite(&scene);
    constexpr int kSide = 64, kPixels = 4;
    sprite.setSize(kSide * kPixels, kSide * kPixels);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(0.0f, 0.0f, 0.0f);
    MeshComponent& mesh = sprite.getComponent<MeshComponent>();
    Submesh& submesh = mesh.submeshes[0];
    submesh.material.baseColorTexture.setData("palette-swap-gpu|remap",
        TextureData(kSide, kSide, (unsigned int)input.size(), ColorFormat::RGBA, 4, input.data()));
    submesh.material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
    submesh.material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
    submesh.needUpdateTexture = true;
    submesh.material.shader = "library:sprite";
    submesh.material.shaderFeatures = {"PALETTE_SWAP"};
    submesh.material.shaderParams = {{"paletteRow", Vector4(1.0f, 0.0f, 0.0f, 0.0f)}};
    PaletteBinding binding;
    binding.source = "tests/gpu-remap-source.palette";
    binding.targets = {PaletteTarget::parse("tests/gpu-remap-target.palette")};
    binding.useVariants = false;
    submesh.material.shaderPalettes = ShaderPalettes{{"u_palette", binding}};
    mesh.needReload = true;
    ASSERT_TRUE(settle());

    Framebuffer& target = GpuEnvironment::target();
    const int width = int(target.getWidth()), height = int(target.getHeight());
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
    const int left = width / 2 - kSide * kPixels / 2, top = height / 2 - kSide * kPixels / 2;
    int differing = 0;
    std::string first;
    for (int y = 0; y < kSide; y++){
        for (int x = 0; x < kSide; x++){
            const int px = left + x * kPixels + kPixels / 2, py = top + y * kPixels + kPixels / 2;
            const uint8_t* got = &rgba[(size_t(py) * width + px) * 4];
            const uint8_t* want = &wanted[(size_t(y) * kSide + x) * 4];
            const Rgba8 shown = want[3] == 0 ? Rgba8{0, 0, 0, 255} : Rgba8{want[0], want[1], want[2], 255};
            if (got[0] != shown.r || got[1] != shown.g || got[2] != shown.b){
                if (differing++ == 0)
                    first = "texel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + toHex(Rgba8{got[0], got[1], got[2], 255}) +
                            ", the remap gives " + toHex(shown);
            }
        }
    }
    EXPECT_EQ(differing, 0) << first;
    EXPECT_NO_ENGINE_ERRORS();
}

// R16: Palette Cycling runs on the scene clock (game time): a fixed frame delta of 0.25 s at time
// scale 0 (a pause) moves the shader clock a step a frame but holds the colours; at time scale 1
// they rotate, and show the rotation of the scene clock
TEST_F(PaletteSwapGpu, CyclingFollowsGameTime){
    Sprite sprite(&scene);
    addSprite(sprite);
    usePalette(sprite, {"PALETTE_CYCLE"}, 0.0f, 1.0f);
    Engine::setPinnedShaderTime(-1.0);
    Engine::setFixedDeltatime(0.25);
    Engine::setTimeScale(0.0f);
    ASSERT_TRUE(settle());
    const std::vector<Rgba8> paused = readTexels();
    for (int frame = 0; frame < 3; frame++){
        GpuEnvironment::drawFrames(1);
        EXPECT_TRUE(sameTexels(readTexels(), paused)) << "paused frame " << frame << ": the cycle holds";
    }
    EXPECT_TRUE(sameTexels(paused, expected(0, (float)scene.getSceneTime(), 1.0f, true)));

    Engine::setTimeScale(1.0f);
    for (int frame = 0; frame < 3; frame++){
        GpuEnvironment::drawFrames(1);
        const float gameTime = (float)scene.getSceneTime();
        EXPECT_TRUE(sameTexels(readTexels(), expected(0, gameTime, 1.0f, true))) << "running frame " << frame << " at " << gameTime << " s";
    }
    EXPECT_FALSE(sameTexels(readTexels(), paused)) << "the cycle moves again";
    EXPECT_NO_ENGINE_ERRORS();
}

// R29: a runtime palette made every frame (Palette.new) or an unrelated palette changing builds
// no swap data for a binding that names neither
TEST_F(PaletteSwapGpu, OtherPalettesRebuildNothing){
    Sprite sprite(&scene);
    addSprite(sprite);
    usePalette(sprite, {"PALETTE_SWAP"}, 1.0f);
    ASSERT_TRUE(settle());
    const size_t built = PaletteSwapCache::builds();
    std::vector<std::shared_ptr<Palette>> held;
    for (int frame = 0; frame < 5; frame++){
        held.push_back(std::make_shared<Palette>(targetPalette()));
        PaletteLibrary::addRuntime(held.back());
        PaletteLibrary::set("tests/gpu-swap-unrelated.palette", sourcePalette());
        GpuEnvironment::drawFrames(1);
    }
    EXPECT_EQ(PaletteSwapCache::builds(), built);
    EXPECT_TRUE(sameTexels(readTexels(), expected(1)));
    // its own palette changing does build: the new colours show
    Palette changed = targetPalette();
    changed.colors[4] = Rgba8{1, 2, 3};
    PaletteLibrary::set(kTargetPath, changed);
    ASSERT_TRUE(settle());
    EXPECT_GT(PaletteSwapCache::builds(), built);
    PaletteLibrary::set(kTargetPath, targetPalette());
    EXPECT_NO_ENGINE_ERRORS();
}

#if JASPERENGINE_PROFILER
// AC13, R28: the features present but off (a binding without PALETTE_SWAP, a palette set with
// Palette Quantize off) allocate nothing on the GPU and make no swap entry: the VRAM tracker's
// count and bytes equal those of the same scene without them
TEST_F(PaletteSwapGpu, PaletteOffAllocatesNothing){
    PaletteSwapCache::clear();
    Sprite sprite(&scene);
    addSprite(sprite);
    MeshComponent& mesh = sprite.getComponent<MeshComponent>();
    mesh.submeshes[0].material.shader = "library:sprite";
    mesh.needReload = true;
    ASSERT_TRUE(settle());
    GpuEnvironment::drawFrames(3);
    const gpumem::Report before = gpumem::report(false, false);

    usePalette(sprite, {}, 2.0f);
    PostProcessingSettings settings = scene.getPostProcessing();
    settings.palette = kSourcePath;
    settings.paletteEnabled = false;
    scene.setPostProcessing(settings);
    ASSERT_TRUE(settle());
    GpuEnvironment::drawFrames(3);
    const gpumem::Report after = gpumem::report(false, false);
    EXPECT_EQ(after.count, before.count);
    EXPECT_EQ(after.totalBytes, before.totalBytes);
    EXPECT_EQ(PaletteSwapCache::size(), 0u);
    EXPECT_FALSE(scene.getSystem<RenderSystem>()->isPaletteQuantizeBaking());
    EXPECT_NO_ENGINE_ERRORS();
}
#endif

// S43 AC11 through S52 (R19-R21, AC8): 64 Palette Swap sprites of rows 0 to 3 draw in one run,
// their rows baked per vertex (fx.x) while the run's uniform row is 0; the image is batching off's
TEST_F(PaletteSwapGpu, Batched){
    JASPER_REQUIRE_GPU();
    struct RestoreBatching {
        ~RestoreBatching() { Engine::setDynamicBatching(true); }
    } restore;
    camera.setOrtho(0.0f, 640.0f, 0.0f, 360.0f, -100.0f, 100.0f);
    std::vector<std::unique_ptr<Sprite>> sprites;
    for (int i = 0; i < 64; i++){
        sprites.push_back(std::make_unique<Sprite>(&scene));
        Sprite& sprite = addSprite(*sprites.back());
        sprite.setSize(kTexels * 2, kTexels * 2);
        sprite.setPosition(24.0f + float(i % 16) * 38.0f, 40.0f + float(i / 16) * 70.0f, 0.0f);
        usePalette(sprite, {"PALETTE_SWAP"}, float(i % 4));
    }
    ASSERT_TRUE(settle());
    auto capture = [](bool batching, uint32_t& draws){
        Engine::setDynamicBatching(batching);
        GpuEnvironment::drawFrames(3);
        draws = Engine::getFrameStats().drawCalls;
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), int(target.getWidth()), int(target.getHeight()), rgba));
        return rgba;
    };
    uint32_t onDraws = 0, offDraws = 0;
    const std::vector<uint8_t> on = capture(true, onDraws);
    const std::vector<uint8_t> off = capture(false, offDraws);
    size_t differing = 0;
    for (size_t i = 0; i < on.size() && i < off.size(); i++)
        differing += on[i] != off[i] ? 1 : 0;
    EXPECT_EQ(on.size(), off.size());
    EXPECT_EQ(differing, 0u);
    EXPECT_LE(onDraws, 1u);
    EXPECT_EQ(offDraws, 64u);
    EXPECT_NO_ENGINE_ERRORS();
}
