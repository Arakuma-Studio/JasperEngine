// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:sprite's Pixel FX on the GPU (docs/specs/S48-pixel-fx.md AC2, AC3, AC4, AC10): sprites
// of 16 x 16 texels drawn 4 x 4 pixels a texel into the headless target (glcore; Vulkan on
// lavapipe with GRAPHIC_BACKEND=vulkan), read back around the sprite and compared with
// golden/sprite_fx/<name>.png.
//  - Legacy*: every pre-S48 feature combination the fixtures use (tools/fixtures/shader-library),
//    tolerance 0 against references captured on the merge base before S48 (the red control: a
//    change to the legacy variants' source shows here).
//  - NewFeatures (AC3): each S48 feature at fixed values, every output pixel a sprite, ramp or
//    effect colour (no blend), and (AC4) every 4 x 4 block of one texel one colour.
//  - IdleEqualsOff (AC10), GrownQuad (R7: bounds, rebuild on a change), KeyMatchesCpu (AC5).

#include "Engine.h"
#include "Scene.h"
#include "pixelart/PixelFxKey.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"
#include "util/Color.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int kTexels = 16;
    constexpr int kScale = 4;
    // the read-back square around the sprite: its 64 pixels plus 8 texels of room on each side
    constexpr int kCrop = (kTexels + 16) * kScale;

    struct Rgba { uint8_t r, g, b, a; };

    // A 16 x 16 character with a transparent margin of 3 texels: a dark body, a lighter face, a
    // half-transparent texel and a notch, so outlines, bevels and keys have corners to follow
    std::vector<uint8_t> characterPixels(){
        const Rgba clear{0, 0, 0, 0};
        const Rgba body{58, 68, 102, 255}, face{232, 183, 150, 255}, eye{24, 20, 37, 255}, boot{139, 69, 19, 255}, glass{90, 200, 230, 128};
        std::vector<uint8_t> pixels;
        for (int y = 0; y < kTexels; y++){
            for (int x = 0; x < kTexels; x++){
                Rgba c = clear;
                if (x >= 3 && x < 13 && y >= 3 && y < 13){
                    c = body;
                    if (y >= 4 && y < 8 && x >= 5 && x < 11) c = face;
                    if (y == 5 && (x == 6 || x == 9)) c = eye;
                    if (y >= 11) c = boot;
                    if (x == 12 && y == 3) c = clear;  // a notch in a corner
                    if (x == 4 && y == 9) c = glass;   // partial alpha
                }
                pixels.insert(pixels.end(), {c.r, c.g, c.b, c.a});
            }
        }
        return pixels;
    }

    // Two 16 x 16 atlas frames side by side whose shapes touch every frame edge: an outline that
    // bled across the frame would draw the neighbour's colour along the edge
    std::vector<uint8_t> atlasPixels(){
        std::vector<uint8_t> pixels;
        for (int y = 0; y < kTexels; y++){
            for (int x = 0; x < kTexels * 2; x++){
                const bool left = x < kTexels;
                const int fx = x % kTexels;
                const bool edge = fx == 0 || fx == kTexels - 1 || y == 0 || y == kTexels - 1;
                const bool cross = fx == 7 || fx == 8 || y == 7 || y == 8;
                Rgba c{0, 0, 0, 0};
                if (left && (cross || (edge && (fx < 4 || y < 4))))
                    c = Rgba{220, 60, 60, 255};
                if (!left && (edge || cross))
                    c = Rgba{60, 220, 90, 255};
                pixels.insert(pixels.end(), {c.r, c.g, c.b, c.a});
            }
        }
        return pixels;
    }

    // 6-step ramps, dark to light (sRGB bytes): a fire, a stone, an ice and a poison
    const std::vector<Rgba> fireRamp = {{42, 14, 8, 255}, {90, 26, 12, 255}, {168, 50, 15, 255}, {232, 96, 26, 255}, {255, 154, 46, 255}, {255, 210, 90, 255}};
    const std::vector<Rgba> stoneRamp = {{30, 28, 32, 255}, {62, 58, 64, 255}, {96, 92, 98, 255}, {132, 128, 130, 255}, {170, 166, 160, 255}, {210, 206, 196, 255}};
    const std::vector<Rgba> iceRamp = {{18, 30, 66, 255}, {30, 64, 124, 255}, {52, 110, 180, 255}, {96, 170, 220, 255}, {160, 220, 245, 255}, {230, 250, 255, 255}};
    const std::vector<Rgba> poisonRamp = {{20, 36, 18, 255}, {36, 72, 30, 255}, {58, 116, 40, 255}, {96, 164, 52, 255}, {150, 210, 70, 255}, {214, 244, 120, 255}};

    struct SpriteSetup {
        std::vector<std::string> features;
        ShaderUniformValues params;
        bool atlas = false;
        MaterialBlendMode blend = MaterialBlendMode::AUTO;
        const std::vector<Rgba>* ramp = nullptr;  // u_fxRampTexture; null: the default grey ramp
    };

    class SpritePixelFxGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        // the textures' bytes, alive while the draws read them
        std::vector<uint8_t> character = characterPixels();
        std::vector<uint8_t> atlas = atlasPixels();
        std::vector<uint8_t> rampBytes;

        void SetUp() override{
            JASPER_REQUIRE_GPU();
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setOrtho(-320.0f, 320.0f, -180.0f, 180.0f, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
            Engine::setPinnedShaderTime(1.0);
        }

        void TearDown() override{
            Engine::setPinnedShaderTime(-1.0);
            Engine::removeScene(&scene);
        }

        void setup_(Sprite& sprite, const SpriteSetup& s){ setup(sprite, s); }

        void setup(Sprite& sprite, const SpriteSetup& s){
            sprite.setSize(kTexels * kScale, kTexels * kScale);
            sprite.setPivotPreset(PivotPreset::CENTER);
            sprite.setPosition(0.0f, 0.0f, 0.0f);
            MeshComponent& mesh = sprite.getComponent<MeshComponent>();
            Submesh& submesh = mesh.submeshes[0];
            std::vector<uint8_t>& pixels = s.atlas ? atlas : character;
            const int width = s.atlas ? kTexels * 2 : kTexels;
            submesh.material.baseColorTexture.setData(s.atlas ? "sprite-fx-gpu|atlas" : "sprite-fx-gpu|character",
                TextureData(width, kTexels, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data()));
            submesh.material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
            submesh.material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
            submesh.needUpdateTexture = true;
            if (s.atlas)
                sprite.setTextureRect(0.0f, 0.0f, 0.5f, 1.0f);
            Material& material = submesh.material;
            material.shader = "library:sprite";
            material.shaderFeatures = s.features;
            material.shaderParams = s.params;
            material.blendMode = s.blend;
            if (s.ramp){
                rampBytes.clear();
                for (const Rgba& c : *s.ramp)
                    rampBytes.insert(rampBytes.end(), {c.r, c.g, c.b, c.a});
                Texture ramp;
                ramp.setData("sprite-fx-gpu|ramp" + std::to_string((uintptr_t)s.ramp),
                             TextureData((int)s.ramp->size(), 1, (unsigned int)rampBytes.size(), ColorFormat::RGBA, 4, rampBytes.data()));
                ramp.setMinFilter(TextureFilter::NEAREST);
                ramp.setMagFilter(TextureFilter::NEAREST);
                material.shaderTextures = {{"u_fxRampTexture", ramp}};
            }
            mesh.needReload = true;
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

        // the kCrop x kCrop square around the target's centre, RGBA rows top first
        std::vector<uint8_t> readCrop(){
            Framebuffer& target = GpuEnvironment::target();
            const int width = int(target.getWidth()), height = int(target.getHeight());
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
            std::vector<uint8_t> crop;
            const int left = width / 2 - kCrop / 2, top = height / 2 - kCrop / 2;
            for (int y = 0; y < kCrop; y++){
                const uint8_t* row = &rgba[(size_t(top + y) * width + left) * 4];
                crop.insert(crop.end(), row, row + kCrop * 4);
            }
            return crop;
        }

        std::vector<uint8_t> render(const SpriteSetup& s){
            Sprite sprite(&scene);
            setup(sprite, s);
            EXPECT_TRUE(settle());
            return readCrop();
        }

        static CompareOptions exact(){
            CompareOptions options;
            options.pixelThreshold = 0;
            options.maxBadFraction = 0.0;
            options.maxMeanDiff = 0.0;
            return options;
        }

        void expectGolden(const std::string& name, const std::vector<uint8_t>& crop, const CompareOptions& options = exact()){
            const CompareResult result = compareWithGolden("sprite_fx/" + name, crop, kCrop, kCrop, options);
            EXPECT_TRUE(result.passed) << name << ": " << result.message;
        }
    };

    Vector4 v1(float x){ return Vector4(x, 0.0f, 0.0f, 0.0f); }

}

// AC2: the pre-S48 combinations of tools/fixtures/shader-library (each feature alone, all at
// once, the premultiplied blend, an atlas frame with OUTLINE), pixel-identical to the merge base
TEST_F(SpritePixelFxGpu, LegacyCombinationsUnchanged){
    const std::vector<std::pair<std::string, SpriteSetup>> cases = {
        {"legacy_none", {{}, {}}},
        {"legacy_premultiplied", {{}, {}, false, MaterialBlendMode::PREMULTIPLIED}},
        {"legacy_outline", {{"OUTLINE"}, {{"outlineColor", Vector4(1.0f, 0.85f, 0.1f, 1.0f)}, {"outlineWidth", v1(1.0f)}}}},
        {"legacy_outline_wide", {{"OUTLINE"}, {{"outlineColor", Vector4(1.0f, 0.85f, 0.1f, 1.0f)}, {"outlineWidth", v1(2.0f)}}}},
        {"legacy_outline_atlas", {{"OUTLINE"}, {{"outlineColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f)}, {"outlineWidth", v1(1.0f)}}, true}},
        {"legacy_hue_shift", {{"HUE_SHIFT"}, {{"hueShift", v1(150.0f)}, {"hueSpeed", v1(0.0f)}}}},
        {"legacy_flash", {{"FLASH"}, {{"flashAmount", v1(0.6f)}}}},
        {"legacy_grayscale", {{"GRAYSCALE"}, {}}},
        {"legacy_color_replace", {{"COLOR_REPLACE"}, {}}},
        {"legacy_overlay", {{"OVERLAY"}, {{"overlayAmount", v1(0.5f)}}}},
        {"legacy_glow", {{"GLOW"}, {}}},
        {"legacy_all", {{"OUTLINE", "HUE_SHIFT", "FLASH", "GRAYSCALE", "COLOR_REPLACE", "OVERLAY", "GLOW"},
                        {{"flashAmount", v1(0.3f)}, {"grayscaleAmount", v1(0.5f)}, {"hueShift", v1(60.0f)}}}},
    };
    for (const auto& [name, setupCase] : cases){
        SCOPED_TRACE(name);
        expectGolden(name, render(setupCase));
    }
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    struct NewCase {
        std::string name;
        SpriteSetup setup;
        std::vector<Rgba> extra;  // effect colours (outline, inline, shadow) besides the art's and the ramp's
        bool blocks = true;       // AC4: a screen-space dither without S41's grid works on screen pixels
    };

    // every colour a draw without blending may show: the background, the art's opaque colours,
    // the half-transparent texel over black, the ramp's and the case's own
    std::vector<Rgba> allowedColours(const NewCase& c, const std::vector<uint8_t>& art){
        std::vector<Rgba> out = {{0, 0, 0, 255}};
        for (size_t i = 0; i < art.size(); i += 4){
            if (art[i + 3] == 255)
                out.push_back({art[i], art[i + 1], art[i + 2], 255});
        }
        // the half-transparent glass texel, blended over black (the art's own partial alpha)
        out.push_back({uint8_t(90 * 128 / 255), uint8_t(200 * 128 / 255), uint8_t(230 * 128 / 255), 255});
        const std::vector<Rgba>& ramp = c.setup.ramp ? *c.setup.ramp : std::vector<Rgba>{};
        for (const Rgba& r : ramp)
            out.push_back(r);
        if (!c.setup.ramp){
            for (int i = 0; i < 8; i++){
                const uint8_t v = uint8_t((i * 255 + 3) / 7);
                out.push_back({v, v, v, 255});
            }
        }
        for (const Rgba& e : c.extra)
            out.push_back(e);
        // the glass texel's half alpha over black takes whatever colour an effect gave it
        const size_t whole = out.size();
        for (size_t i = 0; i < whole; i++)
            out.push_back({uint8_t(out[i].r * 128 / 255), uint8_t(out[i].g * 128 / 255), uint8_t(out[i].b * 128 / 255), 255});
        return out;
    }

    // a byte off at most (the gamma 2.2 round trip of the engine's linear colours)
    bool near(const uint8_t* p, const Rgba& c){
        return std::abs(int(p[0]) - c.r) <= 1 && std::abs(int(p[1]) - c.g) <= 1 && std::abs(int(p[2]) - c.b) <= 1;
    }

    // the character's half-transparent glass texel (4, 9) in the crop (8 texels of room), whose own
    // alpha blends it over whatever an effect draws behind it: the art's blend, not the effect's
    bool glassPixel(size_t pixel){
        const int x = int(pixel % kCrop) / kScale - 8, y = int(pixel / kCrop) / kScale - 8;
        return x == 4 && y == 9;
    }

    ::testing::AssertionResult noBlends(const std::vector<uint8_t>& crop, const std::vector<Rgba>& allowed, bool skipGlass = true){
        int bad = 0;
        std::string first;
        for (size_t i = 0; i < crop.size(); i += 4){
            if (skipGlass && glassPixel(i / 4))
                continue;
            bool ok = false;
            for (const Rgba& c : allowed)
                ok = ok || near(&crop[i], c);
            if (!ok && bad++ == 0){
                first = "pixel (" + std::to_string((i / 4) % kCrop) + ", " + std::to_string((i / 4) / kCrop) + ") is " +
                        std::to_string(crop[i]) + "," + std::to_string(crop[i + 1]) + "," + std::to_string(crop[i + 2]);
            }
        }
        if (bad == 0)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << bad << " pixels are no sprite, ramp or effect colour; " << first;
    }

    // AC4: each texel of the sprite (4 x 4 pixels, on a grid aligned with the crop) is one colour
    ::testing::AssertionResult uniformBlocks(const std::vector<uint8_t>& crop){
        for (int by = 0; by < kCrop; by += kScale){
            for (int bx = 0; bx < kCrop; bx += kScale){
                const uint8_t* first = &crop[(size_t(by) * kCrop + bx) * 4];
                for (int y = 0; y < kScale; y++){
                    for (int x = 0; x < kScale; x++){
                        const uint8_t* p = &crop[(size_t(by + y) * kCrop + bx + x) * 4];
                        if (std::memcmp(p, first, 4) != 0)
                            return ::testing::AssertionFailure() << "the texel block at (" << bx << ", " << by << ") is not one colour";
                    }
                }
            }
        }
        return ::testing::AssertionSuccess();
    }

    Vector4 v2(float x, float y){ return Vector4(x, y, 0.0f, 0.0f); }
    Vector4 linear(const Rgba& c){ return Color::sRGBToLinear(Vector4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f)); }

}

// AC3, AC4: each S48 feature at fixed values (golden/sprite_fx/<name>.png, tolerance 0 on GL; the
// Vulkan goldens run in the batch landing), no blend in any of them, and hard texel blocks
TEST_F(SpritePixelFxGpu, NewFeatures){
    const Rgba yellow{255, 216, 0, 255}, white{255, 255, 255, 255}, inkBlue{20, 24, 90, 255}, black{0, 0, 0, 255};
    const std::vector<NewCase> cases = {
        {"dither_4x4_sprite", {{"DITHER_FADE"}, {{"ditherFadeAmount", v1(0.5f)}}}, {}},
        {"dither_8x8_sprite", {{"DITHER_FADE"}, {{"ditherFadeAmount", v1(0.5f)}, {"ditherMatrix", v1(1.0f)}}}, {}},
        {"dither_4x4_screen", {{"DITHER_FADE"}, {{"ditherFadeAmount", v1(0.5f)}, {"ditherSpace", v1(1.0f)}}}, {}, false},
        {"dither_8x8_screen", {{"DITHER_FADE"}, {{"ditherFadeAmount", v1(0.5f)}, {"ditherMatrix", v1(1.0f)}, {"ditherSpace", v1(1.0f)}}}, {}, false},
        {"dissolve_30", {{"DISSOLVE"}, {{"dissolveProgress", v1(0.3f)}, {"dissolveSeed", v1(3.0f)}}, false, MaterialBlendMode::AUTO, &fireRamp}, {}},
        {"dissolve_60", {{"DISSOLVE"}, {{"dissolveProgress", v1(0.6f)}, {"dissolveSeed", v1(3.0f)}}, false, MaterialBlendMode::AUTO, &fireRamp}, {}},
        {"ramp_petrify", {{"RAMP_RECOLOR"}, {{"rampBevel", v1(1.0f)}}, false, MaterialBlendMode::AUTO, &stoneRamp}, {}},
        {"ramp_frozen", {{"RAMP_RECOLOR"}, {{"rampBevel", v1(1.0f)}}, false, MaterialBlendMode::AUTO, &iceRamp}, {}},
        {"ramp_poison", {{"RAMP_RECOLOR"}, {{"rampBevel", v1(0.5f)}, {"rampPulse", v1(2.0f)}}, false, MaterialBlendMode::AUTO, &poisonRamp}, {}},
        {"outline_pixel4_w1", {{"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {{"outlineColor", linear(yellow)}, {"outlineWidth", v1(1.0f)}}}, {yellow}},
        {"outline_pixel4_w2", {{"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {{"outlineColor", linear(yellow)}, {"outlineWidth", v1(2.0f)}}}, {yellow}},
        {"outline_pixel8_w1", {{"OUTLINE", "OUTLINE_STYLE_PIXEL8"}, {{"outlineColor", linear(yellow)}, {"outlineWidth", v1(1.0f)}}}, {yellow}},
        {"outline_pixel8_w2", {{"OUTLINE", "OUTLINE_STYLE_PIXEL8"}, {{"outlineColor", linear(yellow)}, {"outlineWidth", v1(2.0f)}}}, {yellow}},
        {"outline_ramp_darkest", {{"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {{"outlineColorFrom", v1(1.0f)}}, false, MaterialBlendMode::AUTO, &iceRamp}, {}},
        {"outline_selout", {{"OUTLINE", "OUTLINE_STYLE_PIXEL8"}, {{"outlineColorFrom", v1(2.0f)}}, false, MaterialBlendMode::AUTO, &stoneRamp}, {}},
        {"outline_inline", {{"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {{"outlineColor", linear(yellow)}, {"inlineWidth", v1(1.0f)}, {"inlineColor", linear(inkBlue)}}},
         {yellow, inkBlue}},
        {"drop_shadow", {{"DROP_SHADOW"}, {{"shadowColor", linear(inkBlue)}, {"shadowOffset", v2(2.0f, -1.0f)}}}, {inkBlue}},
        {"drop_shadow_outline", {{"DROP_SHADOW", "OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {{"shadowColorFrom", v1(1.0f)}, {"outlineColor", linear(white)}}, false,
                                 MaterialBlendMode::AUTO, &stoneRamp}, {white}},
        // the grown quad: an atlas frame whose art touches every edge keeps its outline whole,
        // and the neighbour frame never bleeds in
        {"outline_no_margin", {{"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {{"outlineColor", linear(white)}}, true}, {white}},
    };
    for (const NewCase& c : cases){
        SCOPED_TRACE(c.name);
        const std::vector<uint8_t> crop = render(c.setup);
        expectGolden(c.name, crop);
        EXPECT_TRUE(noBlends(crop, allowedColours(c, c.setup.atlas ? atlas : character), !c.setup.atlas));
        if (c.blocks)
            EXPECT_TRUE(uniformBlocks(crop));
    }
    (void)black;
    EXPECT_NO_ENGINE_ERRORS();
}

// AC4 on the legacy goldens too: every texel one block (they draw at the same 4 x 4 scale)
TEST_F(SpritePixelFxGpu, LegacyBlocksUniform){
    for (const char* name : {"legacy_none", "legacy_flash", "legacy_hue_shift", "legacy_grayscale"}){
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        ASSERT_TRUE(readPng(getGoldenDirectory() + "/sprite_fx/" + name + ".png", rgba, width, height)) << name;
        ASSERT_EQ(width, kCrop);
        EXPECT_TRUE(uniformBlocks(rgba)) << name;
    }
}

// AC10: an enabled feature at its neutral amount draws what the feature off draws (tolerance 0)
TEST_F(SpritePixelFxGpu, IdleEqualsOff){
    const std::vector<uint8_t> off = render({{}, {}});
    const std::vector<std::pair<std::string, SpriteSetup>> idle = {
        {"dissolve 0", {{"DISSOLVE"}, {{"dissolveProgress", v1(0.0f)}}, false, MaterialBlendMode::AUTO, &fireRamp}},
        {"dither fade 0", {{"DITHER_FADE"}, {{"ditherFadeAmount", v1(0.0f)}}}},
        {"ramp 0", {{"RAMP_RECOLOR"}, {{"rampAmount", v1(0.0f)}}, false, MaterialBlendMode::AUTO, &stoneRamp}},
        {"all three idle", {{"DISSOLVE", "DITHER_FADE", "RAMP_RECOLOR"}, {{"rampAmount", v1(0.0f)}}}},
    };
    for (const auto& [name, setup] : idle){
        const std::vector<uint8_t> crop = render(setup);
        EXPECT_TRUE(crop == off) << name << " differs from the feature off";
    }
    // the red control: the same features at a non-neutral amount do differ
    EXPECT_FALSE(render({{"DITHER_FADE"}, {{"ditherFadeAmount", v1(0.5f)}}}) == off);
    EXPECT_FALSE(render({{"RAMP_RECOLOR"}, {{"rampAmount", v1(1.0f)}}, false, MaterialBlendMode::AUTO, &stoneRamp}) == off);
    EXPECT_FALSE(render({{"DISSOLVE"}, {{"dissolveProgress", v1(0.5f)}}, false, MaterialBlendMode::AUTO, &fireRamp}) == off);
    // and progress 1 is gone, the whole sprite
    const std::vector<uint8_t> gone = render({{"DISSOLVE"}, {{"dissolveProgress", v1(1.0f)}}, false, MaterialBlendMode::AUTO, &fireRamp});
    EXPECT_TRUE(noBlends(gone, {{0, 0, 0, 255}}, false)) << "progress 1 leaves nothing";
    EXPECT_NO_ENGINE_ERRORS();
}

// R7: a pixel outline and a drop shadow grow the quad (and so its bounds) by their reach in
// texels; a change of the width rebuilds it; the smooth outline and no feature keep it
TEST_F(SpritePixelFxGpu, GrownQuad){
    auto boundsOf = [&](const SpriteSetup& setup, float& width, float& minX, float& minY){
        Sprite sprite(&scene);
        setup_(sprite, setup);
        EXPECT_TRUE(settle());
        const AABB box = sprite.getComponent<MeshComponent>().aabb;
        width = box.getMaximum().x - box.getMinimum().x;
        minX = box.getMinimum().x;
        minY = box.getMinimum().y;
    };
    const float texel = float(kScale);  // the sprite is 64 units over 16 texels
    float w = 0, x = 0, y = 0;
    boundsOf({{}, {}}, w, x, y);
    EXPECT_FLOAT_EQ(w, 64.0f);
    boundsOf({{"OUTLINE"}, {{"outlineWidth", v1(2.0f)}}}, w, x, y);
    EXPECT_FLOAT_EQ(w, 64.0f) << "the smooth outline keeps today's quad (R1)";
    boundsOf({{"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {{"outlineWidth", v1(2.0f)}}}, w, x, y);
    EXPECT_FLOAT_EQ(w, 64.0f + 4.0f * texel);
    boundsOf({{"DROP_SHADOW"}, {{"shadowOffset", v2(2.0f, -1.0f)}}}, w, x, y);
    EXPECT_FLOAT_EQ(w, 64.0f + 2.0f * texel) << "right by 2";
    EXPECT_FLOAT_EQ(x, -32.0f) << "nothing on the left";
    EXPECT_FLOAT_EQ(y, -32.0f - texel) << "down by 1";

    // a value change rebuilds the quad
    Sprite sprite(&scene);
    setup_(sprite, {{"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {{"outlineWidth", v1(1.0f)}}});
    ASSERT_TRUE(settle());
    MeshComponent& mesh = sprite.getComponent<MeshComponent>();
    EXPECT_FLOAT_EQ(mesh.aabb.getMaximum().x - mesh.aabb.getMinimum().x, 64.0f + 2.0f * texel);
    mesh.submeshes[0].material.shaderParams = {{"outlineWidth", v1(2.0f)}};
    mesh.needUpdateShaderUniforms = true;
    ASSERT_TRUE(settle());
    GpuEnvironment::drawFrames(2);
    EXPECT_FLOAT_EQ(mesh.aabb.getMaximum().x - mesh.aabb.getMinimum().x, 64.0f + 4.0f * texel);
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    // library:sprite as a project fork whose fragment writes the dissolve key of its texel as
    // colour (red the key, green its 1/255 remainder): AC5's debug variant
    bool readKeyFork(const std::string& path, std::string& content){
        const auto& sources = ShaderVariantCompiler::getLibrarySources();
        const bool vert = path == "shaders/KeySprite.vert";
        if (!vert && path != "shaders/KeySprite.frag")
            return false;
        auto found = sources.find(vert ? "library/sprite.vert" : "library/sprite.frag");
        if (found == sources.end())
            return false;
        content = found->second;
        if (vert)
            return true;
        const std::string last = "    g_finalColor = finalizeStraightColor(color);";
        const size_t at = content.rfind(last);
        if (at == std::string::npos)
            return false;
        content.replace(at, last.size(),
            "    float key = pfxDissolveKey(fxUp(fxPos), fxSize, customParams.dissolveDirection, customParams.dissolveNoise,\n"
            "                               customParams.dissolveNoiseScale, customParams.dissolveJitter,\n"
            "                               uint(max(customParams.dissolveSeed, 0.0)));\n"
            "    g_finalColor = vec4(key, fract(key * 255.0), 0.0, 1.0);");
        return true;
    }

}

// AC5: the CPU mirror (PixelFxKey) gives the key the GPU computes, within 1/255, on three frame
// sizes and four seeds
TEST_F(SpritePixelFxGpu, KeyMatchesCpu){
    ShaderManifests::setForkReader(readKeyFork);
    struct Frame { int w, h; };
    for (const Frame frame : {Frame{16, 16}, Frame{24, 16}, Frame{32, 32}}){
        std::vector<uint8_t> pixels((size_t)frame.w * frame.h * 4, 255);
        for (uint32_t seed : {0u, 1u, 7u, 1234u}){
            SCOPED_TRACE("frame " + std::to_string(frame.w) + "x" + std::to_string(frame.h) + " seed " + std::to_string(seed));
            Sprite sprite(&scene);
            sprite.setSize(frame.w * kScale, frame.h * kScale);
            sprite.setPivotPreset(PivotPreset::CENTER);
            Submesh& submesh = sprite.getComponent<MeshComponent>().submeshes[0];
            submesh.material.baseColorTexture.setData("sprite-fx-gpu|key" + std::to_string(frame.w) + "x" + std::to_string(frame.h),
                TextureData(frame.w, frame.h, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data()));
            submesh.material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
            submesh.material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
            submesh.needUpdateTexture = true;
            pixelart::PfxDissolve dissolve;
            dissolve.dirX = 0.3f;
            dissolve.dirY = 1.0f;
            dissolve.noise = 0.35f;
            dissolve.cell = 4.0f;
            dissolve.jitter = 0.08f;
            dissolve.seed = seed;
            Material& material = submesh.material;
            material.shader = "shaders/KeySprite";
            material.shaderFeatures = {"DISSOLVE"};
            material.shaderParams = {{"dissolveDirection", v2(dissolve.dirX, dissolve.dirY)}, {"dissolveSeed", v1((float)seed)}};
            sprite.getComponent<MeshComponent>().needReload = true;
            ASSERT_TRUE(settle());

            Framebuffer& target = GpuEnvironment::target();
            const int width = int(target.getWidth()), height = int(target.getHeight());
            std::vector<uint8_t> rgba;
            ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
            const int left = width / 2 - frame.w * kScale / 2, top = height / 2 - frame.h * kScale / 2;
            int worst = 0;
            for (int y = 0; y < frame.h; y++){
                for (int x = 0; x < frame.w; x++){
                    const uint8_t* p = &rgba[(size_t(top + y * kScale + kScale / 2) * width + left + x * kScale + kScale / 2) * 4];
                    // rows from the top of the image; the key counts from the frame's bottom
                    const float cpu = pixelart::pfxDissolveKey(x, frame.h - 1 - y, frame.w, frame.h, dissolve);
                    worst = std::max(worst, std::abs(int(p[0]) - int(std::lround(cpu * 255.0f))));
                }
            }
            EXPECT_LE(worst, 1) << "the GPU key is more than 1/255 from the CPU's";
        }
    }
    ShaderManifests::setForkReader(nullptr);
    ShaderManifests::clearForks();
    EXPECT_NO_ENGINE_ERRORS();
}
