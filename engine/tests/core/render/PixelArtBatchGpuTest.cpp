// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The pixel-art per-sprite parameters inside Dynamic Batching (docs/specs/S52-sprite-instancing.md
// R18-R23, AC5-AC9): pixel-snapped sprites (S41) batch, a run holding one snap mode, the pivot-
// snapped ones with their snap pivot per vertex; Steps90's rounded rotations reach the bake;
// library:sprite batches (@batchable) with its instance= amounts (FLASH, the Palette Swap row, the
// fx.z / fx.w slots of a test shader and S48's Dither Fade and Dissolve) baked per vertex while the run's uniforms are 0, its
// OUTLINE / OVERLAY frame rect per vertex; a mesh that is not a Sprite stays alone
// (ineligible_frame_rect). Every scene is drawn with Dynamic Batching on and off: 0 differing
// pixels (R23). glcore and Vulkan (lavapipe).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/MeshPolygon.h"
#include "object/PixelPerfectCamera.h"
#include "object/Sprite.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "ImageCompare.h"
#include "Readback.h"
#include "TestSystem.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int ART_VIEW = 256;

    std::vector<std::unique_ptr<std::vector<uint8_t>>>& artPixels(){
        static std::vector<std::unique_ptr<std::vector<uint8_t>>> pixels;
        return pixels;
    }

    // a 16x8 sheet of two 8x8 frames with a transparent border (outlines show), asymmetric marks
    TextureData artSheet(){
        auto pixels = std::make_unique<std::vector<uint8_t>>(16 * 8 * 4);
        for (int y = 0; y < 8; y++){
            for (int x = 0; x < 16; x++){
                uint8_t* p = pixels->data() + (y * 16 + x) * 4;
                const int lx = x % 8;
                const bool inside = lx > 0 && lx < 7 && y > 0 && y < 7;
                const bool mark = (x < 8) ? (lx < 3) : (y < 3);
                p[0] = mark ? 230 : 40;
                p[1] = x < 8 ? 90 : 200;
                p[2] = mark ? 20 : 180;
                p[3] = inside ? 255 : 0;
            }
        }
        TextureData data(16, 8, (unsigned int)pixels->size(), ColorFormat::RGBA, 4, pixels->data());
        artPixels().push_back(std::move(pixels));
        return data;
    }

    class PixelArtBatchGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<Sprite>> sprites;
        BatchStats onStats;

        void SetUp() override{
            GpuEnvironment::setView(ART_VIEW, ART_VIEW, ART_VIEW, ART_VIEW, Scaling::STRETCH);
            scene.setBackgroundColor(0.05f, 0.05f, 0.08f, 1.0f);
            Engine::setDynamicBatching(true);
            Engine::setScene(&scene);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(0.0f, (float)ART_VIEW, 0.0f, (float)ART_VIEW, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.setDepthTest(false);
            camera.setTransparentSort(false);
            camera.activate();
        }

        void TearDown() override{
            sprites.clear();
            Engine::removeScene(&scene);
            Engine::setDynamicBatching(true);
            Engine::setPixelPerfectForcedScale(0);
            GpuEnvironment::resetView();
        }

        Sprite& sprite(float x, float y, int frame = 0, PivotPreset pivot = PivotPreset::CENTER){
            sprites.push_back(std::make_unique<Sprite>(&scene));
            Sprite& s = *sprites.back();
            s.setTexture("pixel-art-batch|sheet", artSheet());
            Material material = s.getMaterial();
            material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
            material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
            s.setMaterial(material);
            s.addFrame("a", 0.0f, 0.0f, 8.0f, 8.0f);
            s.addFrame("b", 8.0f, 0.0f, 8.0f, 8.0f);
            s.setFrame(frame);
            s.setSize(16, 16);
            s.setPivotPreset(pivot);
            s.setPosition(x, y, 0.0f);
            return s;
        }

        void librarySprite(Sprite& s, const std::vector<std::string>& features, const ShaderUniformValues& params, const std::string& shader = "library:sprite"){
            MeshComponent& mesh = s.getComponent<MeshComponent>();
            Material& material = mesh.submeshes[0].material;
            material.shader = shader;
            material.shaderFeatures = features;
            material.shaderParams = params;
            mesh.needReload = true;
        }

        void pixelPerfect(PixelRotation rotation){
            PixelPerfectCamera ppc(&scene, camera.getEntity());
            ppc.setReferenceResolution(Vector2(128.0f, 128.0f));
            ppc.setRotationMode(rotation);
        }

        bool settle(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        std::vector<uint8_t> frame(bool batching, uint32_t* draws = nullptr){
            Engine::setDynamicBatching(batching);
            GpuEnvironment::drawFrames(3);
            if (draws)
                *draws = Engine::getFrameStats().drawCalls;
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        const BatchStats& stats(){
            static BatchStats none;
            const RenderSystem::BatchFrame* batchFrame = scene.getSystem<RenderSystem>()->getBatchFrame(SystemRender::FRAME_STATS_FRAME);
            return batchFrame ? batchFrame->stats : none;
        }

        // on a difference the two images go to the test output (<test>.<n>.a.png, .b.png)
        static size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
            if (a.size() != b.size())
                return a.size() / 4 + 1;
            size_t count = 0;
            for (size_t i = 0; i < a.size(); i += 4){
                if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3])
                    count++;
            }
            if (count > 0){
                static int written = 0;
                const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
                const int side = (int)std::lround(std::sqrt((double)(a.size() / 4)));
                const std::string base = getOutputDirectory() + "/" + info->test_suite_name() + "." + info->name() + "." + std::to_string(written++);
                if (side * side * 4 == (int)a.size()){
                    writePng(base + ".a.png", a, side, side);
                    writePng(base + ".b.png", b, side, side);
                }
            }
            return count;
        }

        static size_t lit(const std::vector<uint8_t>& rgba){
            size_t count = 0;
            if (rgba.size() < 4)
                return 0;
            for (size_t i = 0; i < rgba.size(); i += 4)
                count += (rgba[i] != rgba[0] || rgba[i + 1] != rgba[1] || rgba[i + 2] != rgba[2]) ? 1 : 0;
            return count;
        }

        // the sprites' draws of the last frame: its runs and what drew alone (a Pixel Perfect
        // Camera's blit is not one of them)
        static uint32_t spriteDraws(const BatchStats& s){
            return s.batches + s.singleDraws + s.aloneDraws;
        }

        // batching on and off: 0 differing pixels (R23); the on frame's sprite draws returned
        uint32_t expectParity(){
            uint32_t onDraws = 0, offDraws = 0;
            const std::vector<uint8_t> on = frame(true, &onDraws);
            onStats = stats();
            const std::vector<uint8_t> off = frame(false, &offDraws);
            EXPECT_GT(lit(off), 300u) << "the scene should draw something";
            EXPECT_EQ(differing(on, off), 0u);
            EXPECT_LT(onDraws, offDraws);
            return spriteDraws(onStats);
        }
    };

}

// AC5 (R18 mode 1): 500 sprites at fractional positions under a Pixel Perfect Camera, one sheet:
// two runs at most (before S52 each drew alone, ineligible_shader_property), the image batching off's
TEST_F(PixelArtBatchGpu, SnappedSpritesBatch){
    JASPER_REQUIRE_GPU();
    pixelPerfect(PixelRotation::STEPS_90);
    for (int i = 0; i < 500; i++)
        sprite(4.0f + (float)(i % 25) * 10.0f + 0.37f * (float)(i % 7), 4.0f + (float)(i / 25) * 12.0f + 0.21f * (float)(i % 5), i % 2);
    ASSERT_TRUE(settle());
    EXPECT_LE(expectParity(), 2u);
    EXPECT_EQ(onStats.breaks[(size_t)BatchBreak::INELIGIBLE_SHADER_PROPERTY], 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC5 (R18): a run holds one snap mode: sprites on the grid (mode 1) and rotated ones snapping
// their pivot (mode 2, with a_snapPivot) interleaved batch apart, each the image drawn alone
TEST_F(PixelArtBatchGpu, MixedSnapModesBatchApart){
    JASPER_REQUIRE_GPU();
    pixelPerfect(PixelRotation::ON_GRID);
    for (int i = 0; i < 60; i++){
        Sprite& s = sprite(10.0f + (float)(i % 10) * 24.0f + 0.4f, 12.0f + (float)(i / 10) * 30.0f + 0.3f, 0);
        if (i % 2)
            s.setRotation(0.0f, 0.0f, 20.0f);
    }
    ASSERT_TRUE(settle());
    expectParity();
    EXPECT_GE(onStats.breaks[(size_t)BatchBreak::PIXEL_SNAP], 1u);
    EXPECT_EQ(onStats.breaks[(size_t)BatchBreak::INELIGIBLE_PIXEL_SNAP_PIVOT], 0u);
}

// AC6 (R18 Steps90, R22 pivot): Steps90's rounded sprites batch and match; Rotate On Grid's
// rotated ones batch with their snap pivot (mode-2 runs) and match
TEST_F(PixelArtBatchGpu, Steps90AndOnGrid){
    JASPER_REQUIRE_GPU();
    for (PixelRotation policy : {PixelRotation::STEPS_90, PixelRotation::ON_GRID}){
        SCOPED_TRACE(policy == PixelRotation::STEPS_90 ? "Steps90" : "Rotate On Grid");
        pixelPerfect(policy);
        for (int i = 0; i < 40; i++){
            Sprite& s = sprite(14.0f + (float)(i % 8) * 30.0f + 0.25f, 16.0f + (float)(i / 8) * 40.0f + 0.5f, i % 2);
            s.setRotation(0.0f, 0.0f, 15.0f + (float)i * 11.0f);
        }
        ASSERT_TRUE(settle());
        EXPECT_LE(expectParity(), 4u);
        EXPECT_EQ(onStats.breaks[(size_t)BatchBreak::INELIGIBLE_PIXEL_SNAP_PIVOT], 0u);
        sprites.clear();
        scene.removeComponent<PixelPerfectCameraComponent>(camera.getEntity());
    }
}

// AC7 (R19-R21, R28): 100 library:sprite FLASH sprites with amounts 0 to 1 are one run, the image
// drawn alone; an outline colour that differs breaks with custom_params; the sprites' vertex
// buffers are not written while the amounts animate
TEST_F(PixelArtBatchGpu, FlashAmountsBatch){
    JASPER_REQUIRE_GPU();
    for (int i = 0; i < 100; i++){
        Sprite& s = sprite(10.0f + (float)(i % 10) * 24.0f, 10.0f + (float)(i / 10) * 24.0f, i % 2);
        librarySprite(s, {"FLASH"}, {{"flashColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f)}, {"flashAmount", Vector4((float)i / 99.0f, 0, 0, 0)}});
    }
    ASSERT_TRUE(settle());
    EXPECT_LE(expectParity(), 1u);
    EXPECT_EQ(onStats.breaks[(size_t)BatchBreak::INELIGIBLE_CUSTOM_SHADER], 0u);

    // animated amounts: re-baked on the CPU, the buffers (shared zeros) never written
    std::vector<uint64_t> writes;
    for (const auto& s : sprites)
        writes.push_back(s->getComponent<MeshComponent>().buffer.getWriteCount());
    Engine::setDynamicBatching(true);
    for (int step = 0; step < 10; step++){
        for (size_t i = 0; i < sprites.size(); i++){
            MeshComponent& mesh = sprites[i]->getComponent<MeshComponent>();
            mesh.submeshes[0].material.shaderParams[1].second.x = std::fmod((float)(i + step) * 0.13f, 1.0f);
            mesh.needUpdateShaderUniforms = true;
        }
        GpuEnvironment::drawFrames(1);
        EXPECT_LE(spriteDraws(stats()), 1u);
    }
    for (size_t i = 0; i < sprites.size(); i++)
        EXPECT_EQ(sprites[i]->getComponent<MeshComponent>().buffer.getWriteCount(), writes[i]) << i;
    expectParity();

    // a value outside the vertices (the flash colour) is in the key
    librarySprite(*sprites[50], {"FLASH"}, {{"flashColor", Vector4(1.0f, 0.0f, 0.0f, 1.0f)}, {"flashAmount", Vector4(0.5f, 0, 0, 0)}});
    ASSERT_TRUE(settle());
    expectParity();
    EXPECT_GE(onStats.breaks[(size_t)BatchBreak::CUSTOM_PARAMS], 1u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8 (R19, S48 to come): a test shader claiming fx.z and fx.w and writing them to its colour
// batches with parity (the slots S48's DITHER_FADE and DISSOLVE take)
TEST_F(PixelArtBatchGpu, FxSlotsZW){
    JASPER_REQUIRE_GPU();
    TestTempDir dir("pixel-art-batch-fx");
    TestSystem::get().setAssetPath(dir.path().string());
    struct RestoreAssets {
        ~RestoreAssets() { TestSystem::get().setAssetPath(""); }
    } restoreAssets;
    std::filesystem::create_directories(dir.path() / "shaders");
    {
        std::ofstream vert(dir.path() / "shaders" / "FxSlots.vert");
        vert << "#version 450\n#include \"includes/surface_vs.glsl\"\n";
        std::ofstream frag(dir.path() / "shaders" / "FxSlots.frag");
        frag << "#version 450\n"
                "// @shader mesh\n"
                "// @name \"FX Slots\"\n"
                "// @require uv1 color\n"
                "// @state blend=alpha zwrite=off cull=off shadows=off\n"
                "// @batchable\n"
                "// @feature FADE \"Fade\"\n"
                "// @feature DISSOLVE \"Dissolve\"\n"
                "// @property fadeAmount range(0, 1) 0 \"Fade\" feature=FADE instance=fx.z\n"
                "// @property dissolveAmount range(0, 1) 0 \"Dissolve\" feature=DISSOLVE instance=fx.w\n"
                "uniform u_fs_customParams {\n"
                "    float time;\n"
                "    float fadeAmount;\n"
                "    float dissolveAmount;\n"
                "} customParams;\n"
                "#include \"includes/surface_uv.glsl\"\n"
                "SURFACE_UV_IN vec2 v_uv1;\n"
                "in vec4 v_color;\n"
                "out vec4 g_finalColor;\n"
                "#include \"includes/output.glsl\"\n"
                "#include \"includes/base_color_fs.glsl\"\n"
                "#include \"includes/sprite_fx.glsl\"\n"
                "void main() {\n"
                "    vec4 texel = sampleBaseColorTexture(v_uv1);\n"
                "    float fade = customParams.fadeAmount + spriteFx(z);\n"
                "    float dissolve = customParams.dissolveAmount + spriteFx(w);\n"
                "    g_finalColor = finalizeStraightColor(vec4(fade, dissolve, texel.b, texel.a));\n"
                "}\n";
    }
    for (int i = 0; i < 64; i++){
        Sprite& s = sprite(12.0f + (float)(i % 8) * 30.0f, 12.0f + (float)(i / 8) * 30.0f, i % 2);
        librarySprite(s, {"FADE", "DISSOLVE"}, {{"fadeAmount", Vector4((float)(i % 8) / 7.0f, 0, 0, 0)},
                                                {"dissolveAmount", Vector4((float)(i / 8) / 7.0f, 0, 0, 0)}}, "shaders/FxSlots");
    }
    ASSERT_TRUE(settle());
    EXPECT_LE(expectParity(), 1u);
    EXPECT_NO_ENGINE_ERRORS();
}

// S48 AC11 (R10): library:sprite's own fx.z / fx.w amounts, Dither Fade and Dissolve, with the
// Pixel FX features that read the frame rect (pixel outline, drop shadow, grown quads): 64 sprites
// differing only in the two amounts (and their frame) stay in the runs of one material and match
// the unbatched draws, which read the amounts from their uniforms (an absent stream is 0)
TEST_F(PixelArtBatchGpu, FxSlotsDitherFadeDissolve){
    JASPER_REQUIRE_GPU();
    for (int i = 0; i < 64; i++){
        Sprite& s = sprite(12.0f + (float)(i % 8) * 30.0f, 12.0f + (float)(i / 8) * 30.0f, i % 2);
        librarySprite(s, {"DITHER_FADE", "DISSOLVE", "DROP_SHADOW", "OUTLINE", "OUTLINE_STYLE_PIXEL4"},
                      {{"ditherFadeAmount", Vector4((float)(i % 8) / 8.0f, 0, 0, 0)},
                       {"dissolveProgress", Vector4((float)(i / 8) / 8.0f, 0, 0, 0)},
                       {"dissolveEdgeWidth", Vector4(1.0f, 0, 0, 0)},
                       {"outlineColor", Vector4(1.0f, 1.0f, 0.0f, 1.0f)}, {"outlineWidth", Vector4(1.0f, 0, 0, 0)},
                       {"shadowOffset", Vector4(1.0f, -1.0f, 0, 0)}});
    }
    ASSERT_TRUE(settle());
    EXPECT_LE(expectParity(), 2u);
    EXPECT_EQ(onStats.breaks[(size_t)BatchBreak::INELIGIBLE_FRAME_RECT], 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC9 (R22): OUTLINE and OVERLAY sprites of one sheet, frames differing per sprite, batch (the
// frame rect per vertex) and match; a MeshPolygon with OUTLINE has no frame rect attribute and
// draws alone (ineligible_frame_rect)
TEST_F(PixelArtBatchGpu, OutlineOverlayBatch){
    JASPER_REQUIRE_GPU();
    for (int i = 0; i < 48; i++){
        Sprite& s = sprite(12.0f + (float)(i % 8) * 30.0f, 12.0f + (float)(i / 8) * 30.0f, (i / 3) % 2);
        librarySprite(s, {i % 2 ? "OUTLINE" : "OVERLAY", "FLASH"},
                      {{"outlineColor", Vector4(1.0f, 1.0f, 0.0f, 1.0f)}, {"outlineWidth", Vector4(1.0f, 0, 0, 0)},
                       {"overlayColor", Vector4(1.0f, 0.78f, 0.3f, 1.0f)}, {"overlayTiling", Vector4(1, 1, 0, 0)},
                       {"overlayAmount", Vector4(0.5f, 0, 0, 0)}, {"flashColor", Vector4(1, 1, 1, 1)}, {"flashAmount", Vector4(0.1f * (i % 4), 0, 0, 0)}});
    }
    ASSERT_TRUE(settle());
    EXPECT_LE(expectParity(), 2u);
    EXPECT_EQ(onStats.breaks[(size_t)BatchBreak::INELIGIBLE_FRAME_RECT], 0u);

    MeshPolygon polygon(&scene);
    polygon.addVertex(0.0f, 0.0f);
    polygon.addVertex(30.0f, 0.0f);
    polygon.addVertex(0.0f, 30.0f);
    polygon.setTexture("pixel-art-batch|sheet", artSheet());
    MeshComponent& mesh = polygon.getComponent<MeshComponent>();
    mesh.submeshes[0].material.shader = "library:sprite";
    mesh.submeshes[0].material.shaderFeatures = {"OUTLINE"};
    mesh.needReload = true;
    ASSERT_TRUE(settle());
    frame(true);
    EXPECT_EQ(scene.getSystem<RenderSystem>()->getBatchEligibility(polygon.getEntity()), BatchBreak::INELIGIBLE_FRAME_RECT);
}
