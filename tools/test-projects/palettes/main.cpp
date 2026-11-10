// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S43's test project (docs/specs/S43-palettes.md AC5, AC14, AC21): the palette runtime in a
// WebAssembly build without pthreads, on WebGL2. Checked in a browser (WEB_PROJECT=palettes
// tools/build.sh web, GATE=palettes tools/build.sh web-gate) by the lines below and the centre pixel:
// - "[gate] palette colour hash matches": the colour maths' determinism probe (10^5 OKLab
//   conversions, their way back and the fixed cube root) gives the Linux x86-64 golden here too;
// - "[gate] palette quantize bake within 60 ms": Palette Quantize's table (32^3, 64 colours, Bayer
//   4x4) baked in 8 ms slices, one a frame, as the renderer does without threads (R30), judged in a
//   Release build (BUILD_TYPE=Release WEB_PROJECT=palettes tools/build.sh web; the default Debug
//   web build says it is not judged);
// - "[gate] palette swap and quantize drawn": a sprite of the source's colour 2 drawn with Palette
//   Swap row 1, through Palette Quantize onto the target palette; the centre pixel is the target's
//   colour 2, #41a6f6.

#include "JasperEngine.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteQuantize.h"
#include "pixelart/PixelColor.h"
#include "../../../engine/tests/core/pixelart/PixelColorHash.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    constexpr uint64_t HASH_GOLDEN = 0xe165107b5f7284fcull;
    constexpr double BAKE_BUDGET_MS = 60.0;
    constexpr const char* SOURCE = "palettes/source.palette";
    constexpr const char* TARGET = "palettes/target.palette";

    Scene scene;
    Camera sceneCamera(&scene);
    Sprite sprite(&scene);
    std::unique_ptr<QuantizeLutJob> bake;
    double bakeMs = 0.0;
    double longestSliceMs = 0.0;
    int slices = 0;
    int frames = 0;
    bool drawn = false;

    Palette paletteOf(const char* name, std::initializer_list<const char*> hexes) {
        Palette palette;
        palette.name = name;
        for (const char* hex : hexes) {
            Rgba8 color;
            parseHex(hex, color);
            palette.colors.push_back(color);
        }
        return palette;
    }

    // 64 colours spread over hue, saturation and value
    Palette bakePalette() {
        Palette palette;
        palette.name = "Bake";
        for (int i = 0; i < 64; i++) {
            const float hue = (float)(i % 16) / 16.0f;
            const float saturation = 0.35f + 0.65f * (float)((i / 16) % 2);
            const float value = 0.3f + 0.35f * (float)(i / 32);
            palette.colors.push_back(fromHsv(Hsv{hue, saturation, value}));
        }
        return palette;
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(320, 180);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f);
    scene.setCamera(&sceneCamera);

    const uint64_t hash = pixelColorHash();
    if (hash == HASH_GOLDEN){
        Log::print("[gate] palette colour hash matches");
    }else{
        Log::error("[gate] palette colour hash %016llx, the golden is %016llx", (unsigned long long)hash, (unsigned long long)HASH_GOLDEN);
    }

    PaletteLibrary::set(SOURCE, paletteOf("Source", {"#1a1c2c", "#5d275d", "#b13e53", "#ef7d57"}));
    PaletteLibrary::set(TARGET, paletteOf("Target", {"#29366f", "#3b5dc9", "#41a6f6", "#73eff7"}));

    // a 2 x 2 texture of the source's colour 2 over the whole canvas, its row 1 the target
    static std::vector<uint8_t> pixels;
    for (int i = 0; i < 4; i++) pixels.insert(pixels.end(), {0xb1, 0x3e, 0x53, 0xff});
    sprite.setTexture("palettes|texture", TextureData(2, 2, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data()));
    // far larger than the canvas, centred on it: the whole view whatever the camera's origin
    sprite.setSize(2000.0f, 2000.0f);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(160.0f, 90.0f, 0.0f);
    MeshComponent& mesh = sprite.getComponent<MeshComponent>();
    Material& material = mesh.submeshes[0].material;
    material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
    material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
    material.shader = "library:sprite";
    material.shaderFeatures = {"PALETTE_SWAP"};
    material.shaderParams = {{"paletteRow", Vector4(1.0f, 0.0f, 0.0f, 0.0f)}};
    PaletteBinding binding;
    binding.source = SOURCE;
    binding.targets = {PaletteTarget::parse(TARGET)};
    binding.useVariants = false;
    material.shaderPalettes = ShaderPalettes{{"u_palette", binding}};

    PostProcessingSettings post;
    post.paletteEnabled = true;
    post.palette = TARGET;
    post.paletteDither = PaletteDither::BAYER4;
    scene.setPostProcessing(post);

    bake = startQuantizeBake(bakePalette(), ColorMetric::OkLab, 4, 1.0f, 32);

    Engine::onUpdate.add("palettesTest", [](){
        frames++;
        if (bake){
            const auto start = std::chrono::steady_clock::now();
            const bool done = bake->step(0.008);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            bakeMs += ms;
            longestSliceMs = std::max(longestSliceMs, ms);
            slices++;
            if (done){
                Log::print("[palettes] bake %.1f ms in %d slices, the longest %.2f ms", bakeMs, slices, longestSliceMs);
#ifdef NDEBUG
                if (bakeMs <= BAKE_BUDGET_MS){
                    Log::print("[gate] palette quantize bake within 60 ms");
                }else{
                    Log::error("[gate] palette quantize bake %.1f ms over the 60 ms budget", bakeMs);
                }
#else
                // the budget is a Release build's (BUILD_TYPE=Release WEB_PROJECT=palettes tools/build.sh web)
                Log::print("[gate] palette quantize bake within 60 ms: not judged in a Debug build");
#endif
                bake.reset();
            }
        }
        if (!drawn){
            RenderSystem* render = scene.getSystem<RenderSystem>().get();
            const bool quantized = (render->getActivePostProcessingTiers().uberFeatures & UBER_PALETTE) != 0 && !render->isPaletteQuantizeBaking();
            if (quantized && frames > 10){
                drawn = true;
                Log::print("[gate] palette swap and quantize drawn");
            }else if (frames > 1200){
                drawn = true;
                Log::error("[gate] Palette Quantize did not run in 1200 frames");
            }
        }
    });
}
