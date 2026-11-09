// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Sprite Batch component (docs/specs/S52-sprite-instancing.md R10-R17, AC3, AC4; the suite is
// SpriteBatchComponentGpu because SpriteBatchGpu is Dynamic Batching's): instances of a Sprite's
// frames drawn with its instanced variant, one draw per run of one texture page; 64 instances
// draw the picture 64 Sprites of the same frames and transforms draw (0 differing pixels at
// pixel-aligned placements); Clear Each Frame draws what a frame wrote once; a static Retained
// batch uploads nothing after its first frame; a batch out of view is culled whole; under a Pixel
// Perfect Camera each instance snaps as its Sprite would (every vertex when grid-aligned, else
// its pivot), and so do an Instanced Mesh's instances. glcore and Vulkan (lavapipe).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/SpriteBatchComponent.h"
#include "object/Camera.h"
#include "object/Mesh.h"
#include "object/PixelPerfectCamera.h"
#include "object/Sprite.h"
#include "object/SpriteBatch.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "render/sprite/SpriteInstanceBatch.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "ImageCompare.h"
#include "TempDir.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int BATCH_VIEW = 256;
    constexpr float HALF_PI = 1.57079632679489662f;

    std::vector<std::unique_ptr<std::vector<uint8_t>>>& instancePixels(){
        static std::vector<std::unique_ptr<std::vector<uint8_t>>> pixels;
        return pixels;
    }

    // a 16x8 strip of two 8x8 frames, each a different asymmetric pattern (a flip or turn shows)
    TextureData instanceFrames(){
        auto pixels = std::make_unique<std::vector<uint8_t>>(16 * 8 * 4);
        for (int y = 0; y < 8; y++){
            for (int x = 0; x < 16; x++){
                uint8_t* p = pixels->data() + (y * 16 + x) * 4;
                const bool second = x >= 8;
                const int lx = x % 8;
                const bool mark = second ? (lx < 3 && y < 5) : (lx > 4 || y == 0);
                p[0] = mark ? 255 : (second ? 0 : 40);
                p[1] = mark ? 220 : (second ? 160 : 0);
                p[2] = second ? 255 : 30;
                p[3] = 255;
            }
        }
        TextureData data(16, 8, (unsigned int)pixels->size(), ColorFormat::RGBA, 4, pixels->data());
        instancePixels().push_back(std::move(pixels));
        return data;
    }

    void nearest(Mesh& mesh){
        Material material = mesh.getMaterial();
        material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
        material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
        mesh.setMaterial(material);
    }

    // a sheet of two trimmed frames of different sizes on one page (the strip as a PNG), each
    // with its own offset in a larger source frame, given to the pool as "s52-trimmed": an
    // instance's quad is its frame's, offset from the pivot as the Sprite's own is
    // (the page lives as long as the process: the pool keeps the sheet)
    const std::string& trimmedPagePath(){
        static TempDir dir;
        static const std::string path = [](){
            TextureData strip = instanceFrames();
            std::vector<uint8_t> pixels((uint8_t*)strip.getData(), (uint8_t*)strip.getData() + 16 * 8 * 4);
            const std::string file = dir.file("trimmed.png").generic_string();
            writePng(file, pixels, 16, 8);
            return file;
        }();
        return path;
    }

    void registerTrimmedSheet(){
        static bool registered = false;
        if (registered)
            return;
        registered = true;
        SpriteSheetPool::registerSource([](const std::string& path){ return path == "s52-trimmed"; },
            [](const std::string&) -> std::shared_ptr<const SpriteSheetData>{
                auto sheet = std::make_shared<SpriteSheetData>();
                sheet->pages.push_back({trimmedPagePath(), 16, 8});
                SpriteFrame a;
                a.name = "a";
                a.rect = Rect(0, 0, 8, 8);
                a.sourceSize = Vector2(12, 11);
                a.offset = Vector2(3, 1);
                a.pivot = Vector2(2, 9);
                SpriteFrame b;
                b.name = "b";
                b.rect = Rect(8, 1, 5, 6);
                b.sourceSize = Vector2(9, 9);
                b.offset = Vector2(1, 2);
                b.pivot = Vector2(7, 3);
                sheet->frames = {a, b};
                return sheet;
            });
    }

    class SpriteBatchComponentGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<Sprite>> sprites;

        void SetUp() override{
            GpuEnvironment::setView(BATCH_VIEW, BATCH_VIEW, BATCH_VIEW, BATCH_VIEW, Scaling::STRETCH);
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            Engine::setScene(&scene);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(0.0f, (float)BATCH_VIEW, 0.0f, (float)BATCH_VIEW, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.setDepthTest(false);
            camera.setTransparentSort(false);
            camera.activate();
        }

        void TearDown() override{
            sprites.clear();
            Engine::removeScene(&scene);
            Engine::setPixelPerfectForcedScale(0);
            GpuEnvironment::resetView();
        }

        // a Sprite of the two-frame strip (frame 0 shown), 16x16 world units, its pivot at the bottom left
        Sprite& frameSprite(PivotPreset pivot = PivotPreset::BOTTOM_LEFT){
            sprites.push_back(std::make_unique<Sprite>(&scene));
            Sprite& s = *sprites.back();
            s.setTexture("sprite-batch-component|frames", instanceFrames());
            nearest(s);
            s.addFrame("a", 0.0f, 0.0f, 8.0f, 8.0f);
            s.addFrame("b", 8.0f, 0.0f, 8.0f, 8.0f);
            s.setFrame(0);
            s.setSize(16, 16);
            s.setPivotPreset(pivot);
            return s;
        }

        // a Sprite of the trimmed sheet (2 world units a pixel), frame `frame`
        Sprite& trimmedSprite(int frame, PivotPreset pivot){
            registerTrimmedSheet();
            sprites.push_back(std::make_unique<Sprite>(&scene));
            Sprite& s = *sprites.back();
            s.setSheet("s52-trimmed");
            s.setPixelsPerUnit(0.5f);
            s.setPivotPreset(pivot);
            s.setFrame(frame);
            nearest(s);
            return s;
        }

        bool settle(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        std::vector<uint8_t> image(int frames = 3){
            GpuEnvironment::drawFrames(frames);
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        const RenderSystem::SpriteBatchStats& batchStats(){
            return scene.getSystem<RenderSystem>()->getSpriteBatchStats();
        }

        // on a difference the two images go to the test output (<test>.<n>.a.png, .b.png)
        static size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
            if (a.size() != b.size())
                return a.size() / 4 + 1;
            size_t count = 0;
            for (size_t i = 0; i < a.size(); i += 4){
                if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2])
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
            for (size_t i = 0; i < rgba.size(); i += 4)
                count += (rgba[i] | rgba[i + 1] | rgba[i + 2]) ? 1 : 0;
            return count;
        }
    };

    // a sheet of two pages (the two halves of the strip as PNG files) with a frame on each, given
    // to the pool as "s52-two-pages" (tests only); the pages are the latest test's files
    std::string& twoPagesPath(int page){
        static std::string paths[2];
        return paths[page];
    }

    void registerTwoPages(){
        static bool registered = false;
        if (registered)
            return;
        registered = true;
        SpriteSheetPool::registerSource([](const std::string& path){ return path == "s52-two-pages"; },
            [](const std::string&) -> std::shared_ptr<const SpriteSheetData>{
                auto sheet = std::make_shared<SpriteSheetData>();
                sheet->pages.push_back({twoPagesPath(0), 8, 8});
                sheet->pages.push_back({twoPagesPath(1), 8, 8});
                for (uint16_t page = 0; page < 2; page++){
                    SpriteFrame frame;
                    frame.name = page == 0 ? "a" : "b";
                    frame.page = page;
                    frame.rect = Rect(0, 0, 8, 8);
                    frame.sourceSize = Vector2(8, 8);
                    frame.pivot = Vector2(0, 8);
                    sheet->frames.push_back(frame);
                }
                return sheet;
            });
    }


}

// AC3 (R13): 10,000 instances of one page are one instanced draw
TEST_F(SpriteBatchComponentGpu, DrawsOnePerPage){
    JASPER_REQUIRE_GPU();
    Sprite& base = frameSprite();
    SpriteBatch batch(&scene, base.getEntity());
    std::vector<float> positions;
    for (int i = 0; i < 10000; i++){
        positions.push_back((float)(i % 100) * 2.5f);
        positions.push_back((float)(i / 100) * 2.5f);
    }
    ASSERT_EQ(batch.setPositions(positions), "");
    EXPECT_EQ(batch.getCount(), 10000u);
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> shot = image();
    EXPECT_EQ(batchStats().draws, 1u);
    EXPECT_EQ(batchStats().instances, 10000u);
    EXPECT_GT(lit(shot), 1000u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC3 (R13): frames on two pages interleaved: one draw per run of one page
TEST_F(SpriteBatchComponentGpu, PageRunsAreDraws){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    // the pages: the halves of the strip
    TextureData strip = instanceFrames();
    for (int page = 0; page < 2; page++){
        std::vector<uint8_t> half;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                for (int c = 0; c < 4; c++)
                    half.push_back(((uint8_t*)strip.getData())[(y * 16 + page * 8 + x) * 4 + c]);
        twoPagesPath(page) = dir.file(page == 0 ? "page-a.png" : "page-b.png").generic_string();
        ASSERT_TRUE(writePng(twoPagesPath(page), half, 8, 8));
    }
    registerTwoPages();
    sprites.push_back(std::make_unique<Sprite>(&scene));
    Sprite& base = *sprites.back();
    base.setSheet("s52-two-pages");
    base.setSize(16, 16);
    SpriteBatch batch(&scene, base.getEntity());
    std::vector<float> positions;
    std::vector<int32_t> frames;
    // a, a, b, b, a, b: four runs
    const int pattern[6] = {0, 0, 1, 1, 0, 1};
    for (int i = 0; i < 6; i++){
        positions.push_back(20.0f + (float)i * 30.0f);
        positions.push_back(100.0f);
        frames.push_back(pattern[i]);
    }
    ASSERT_EQ(batch.setPositions(positions), "");
    ASSERT_EQ(batch.setFrames(frames), "");
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> shot = image();
    EXPECT_EQ(batchStats().draws, 4u);
    EXPECT_EQ(batchStats().instances, 6u);
    // both pages drew: frame a's colours (blue 30) and frame b's (blue 255)
    size_t pageA = 0, pageB = 0;
    for (size_t i = 0; i < shot.size(); i += 4){
        pageA += shot[i + 2] == 30 ? 1 : 0;
        pageB += shot[i + 2] == 255 ? 1 : 0;
    }
    EXPECT_GT(pageA, 0u);
    EXPECT_GT(pageB, 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC3 (R11): 64 instances draw what 64 Sprites of the same frames and transforms draw: trimmed
// frames of different sizes and offsets, a bottom-left pivot (an instance's quad is offset from its
// position as its Sprite's), turned, scaled and tinted
TEST_F(SpriteBatchComponentGpu, MatchesSprites){
    JASPER_REQUIRE_GPU();
    // the reference: 64 Sprites
    std::vector<float> positions, rotations, scales, colors;
    std::vector<int32_t> frames;
    for (int i = 0; i < 64; i++){
        const float x = (float)(i % 8) * 30.0f + 14.0f;
        const float y = (float)(i / 8) * 30.0f + 14.0f;
        const float rotation = (float)(i % 4) * HALF_PI;
        const float scale = (i % 3 == 0) ? 1.0f : 0.5f;
        positions.insert(positions.end(), {x, y});
        rotations.push_back(rotation);
        scales.push_back(scale);
        frames.push_back(i % 2);
        colors.insert(colors.end(), {1.0f, (i % 5) * 0.25f, 1.0f, 1.0f});
        Sprite& s = trimmedSprite(i % 2, PivotPreset::BOTTOM_LEFT);
        s.setPosition(x, y, 0.0f);
        s.setRotation(0.0f, 0.0f, (float)(i % 4) * 90.0f);
        s.setScale(scale);
        s.setColor(1.0f, (i % 5) * 0.25f, 1.0f, 1.0f);
    }
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> reference = image();
    sprites.clear();

    Sprite& base = trimmedSprite(0, PivotPreset::BOTTOM_LEFT);
    SpriteBatch batch(&scene, base.getEntity());
    ASSERT_EQ(batch.setPositions(positions), "");
    ASSERT_EQ(batch.setRotations(rotations), "");
    ASSERT_EQ(batch.setScales(scales), "");
    ASSERT_EQ(batch.setFrames(frames), "");
    ASSERT_EQ(batch.setColors(colors), "");
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> instanced = image();
    EXPECT_GT(lit(reference), 1500u);
    EXPECT_EQ(differing(instanced, reference), 0u);
    EXPECT_EQ(batchStats().draws, 1u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC3 (R10, R14): Clear Each Frame draws what a frame wrote once
TEST_F(SpriteBatchComponentGpu, ClearEachFrame){
    JASPER_REQUIRE_GPU();
    Sprite& base = frameSprite();
    SpriteBatch batch(&scene, base.getEntity());
    batch.setMode(SpriteBatchMode::CLEAR_EACH_FRAME);
    ASSERT_GE(batch.add(40.0f, 40.0f, 1), 0);
    ASSERT_GE(batch.add(80.0f, 40.0f, 0), 0);
    ASSERT_TRUE(settle());
    // settle drew it: write again, then one frame draws it and the next draws nothing
    ASSERT_GE(batch.add(40.0f, 40.0f, 1), 0);
    EXPECT_EQ(batch.getCount(), 1u) << "the first write of a frame starts its set";
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(batchStats().instances, 1u);
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(batchStats().draws, 0u);
    EXPECT_EQ(batch.getCount(), 0u);
}

// AC3 (R14, R27): a static Retained batch uploads nothing after its first frame
TEST_F(SpriteBatchComponentGpu, RetainedUploadsOnce){
    JASPER_REQUIRE_GPU();
    Sprite& base = frameSprite();
    SpriteBatch batch(&scene, base.getEntity());
    std::vector<float> positions;
    for (int i = 0; i < 200; i++)
        positions.insert(positions.end(), {(float)(i % 20) * 12.0f, (float)(i / 20) * 12.0f});
    ASSERT_EQ(batch.setPositions(positions), "");
    ASSERT_TRUE(settle());
    uint64_t uploaded = 0;
    for (int f = 0; f < 100; f++){
        GpuEnvironment::drawFrames(1);
        uploaded += batchStats().uploadBytes;
        EXPECT_EQ(batchStats().draws, 1u);
    }
    EXPECT_EQ(uploaded, 0u);
    // a change uploads once: 96 bytes an instance
    batch.set(0, 5.0f, 5.0f);
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(batchStats().uploadBytes, 200u * sizeof(InstanceRenderData));
    EXPECT_EQ(sizeof(InstanceRenderData), 96u);
}

// R12: past its capacity a batch grows by half again, so one instance added a frame past 1,024
// reloads the mesh (a larger GPU buffer) a few times, not every frame
TEST_F(SpriteBatchComponentGpu, GrowthReloadsRarely){
    JASPER_REQUIRE_GPU();
    Sprite& base = frameSprite();
    SpriteBatch batch(&scene, base.getEntity());
    std::vector<float> positions;
    for (int i = 0; i < 1024; i++)
        positions.insert(positions.end(), {(float)(i % 32) * 8.0f, (float)(i / 32) * 8.0f});
    ASSERT_EQ(batch.setPositions(positions), "");
    ASSERT_TRUE(settle());
    const SpriteBatchState& state = *scene.getComponent<SpriteBatchComponent>(base.getEntity()).state;
    ASSERT_TRUE(state.batch);
    uint32_t gpuCapacity = state.batch->getGpuCapacity();
    EXPECT_GE(gpuCapacity, 1024u);
    int reloads = 0;
    for (int f = 0; f < 300; f++){
        batch.add((float)(f % 32) * 8.0f, 200.0f);
        GpuEnvironment::drawFrames(1);
        if (state.batch->getGpuCapacity() != gpuCapacity){
            reloads++;
            gpuCapacity = state.batch->getGpuCapacity();
        }
    }
    ASSERT_TRUE(settle());
    EXPECT_GE(state.batch->getGpuCapacity(), 1324u);
    EXPECT_LE(reloads, 2) << "a reload per growth step, not per frame";
    EXPECT_EQ(batchStats().instances, 1324u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC3 (R15): a batch out of view is culled whole (nothing drawn, nothing uploaded); back in view it draws
TEST_F(SpriteBatchComponentGpu, CulledWhole){
    JASPER_REQUIRE_GPU();
    Sprite& base = frameSprite();
    SpriteBatch batch(&scene, base.getEntity());
    std::vector<float> positions;
    for (int i = 0; i < 50; i++)
        positions.insert(positions.end(), {5000.0f + (float)i * 10.0f, 5000.0f});
    ASSERT_EQ(batch.setPositions(positions), "");
    ASSERT_TRUE(settle());
    image();
    EXPECT_EQ(batchStats().draws, 0u);
    EXPECT_EQ(batchStats().uploadBytes, 0u);
    base.setPosition(-5000.0f + 20.0f, -5000.0f + 20.0f, 0.0f);
    const std::vector<uint8_t> shot = image();
    EXPECT_EQ(batchStats().draws, 1u);
    EXPECT_GT(lit(shot), 100u);
}

// R10: the error names the component to remove; R12: a call validates once, typed errors
TEST_F(SpriteBatchComponentGpu, BulkCallsValidateOnce){
    JASPER_REQUIRE_GPU();
    Sprite& base = frameSprite();
    SpriteBatch batch(&scene, base.getEntity());
    EXPECT_EQ(batch.getFrameCount(), 2);
    EXPECT_EQ(batch.frameId("b"), 1);
    EXPECT_EQ(batch.frameId("nope"), -1);
    ASSERT_EQ(batch.setPositions(std::vector<float>{0, 0, 10, 10, 20, 20}), "");
    EXPECT_NE(batch.setFrames(std::vector<int32_t>{0, 1, 2}).find("frame_range"), std::string::npos);
    EXPECT_NE(batch.setRotations(std::vector<float>{0.0f}).find("length"), std::string::npos);
    EXPECT_NE(batch.setPositions(std::vector<float>{1, 2, 3}, 2).find("length"), std::string::npos);
    EXPECT_NE(batch.setFx("glow", std::vector<float>{0, 0, 0}).find("fx_slot"), std::string::npos);
    EXPECT_EQ(batch.getCount(), 3u) << "a refused call changes nothing";
    EXPECT_TRUE(batch.remove(0));
    EXPECT_EQ(batch.getCount(), 2u);
    batch.clear();
    EXPECT_EQ(batch.getCount(), 0u);
}

// AC4 (R16, D8): under a Pixel Perfect Camera, instances unrotated, at 90 degrees and at 30 degrees,
// at quarter-pixel offsets, equal the same Sprites drawn alone (Rotate On Grid and Steps90)
TEST_F(SpriteBatchComponentGpu, SnapPerInstance){
    JASPER_REQUIRE_GPU();
    for (PixelRotation policy : {PixelRotation::ON_GRID, PixelRotation::STEPS_90}){
        SCOPED_TRACE(policy == PixelRotation::ON_GRID ? "Rotate On Grid" : "Steps90");
        PixelPerfectCamera ppc(&scene, camera.getEntity());
        ppc.setReferenceResolution(Vector2(128.0f, 128.0f));
        ppc.setRotationMode(policy);
        std::vector<float> positions, rotations;
        const float angles[3] = {0.0f, 90.0f, 30.0f};
        for (int i = 0; i < 12; i++){
            const float x = 20.0f + (float)(i % 4) * 26.0f + 0.25f * (float)(i % 3);
            const float y = 30.0f + (float)(i / 4) * 30.0f + 0.25f;
            positions.insert(positions.end(), {x, y});
            rotations.push_back(angles[i % 3] * HALF_PI / 90.0f);
            Sprite& s = frameSprite(PivotPreset::CENTER);
            s.setPosition(x, y, 0.0f);
            s.setRotation(0.0f, 0.0f, angles[i % 3]);
        }
        ASSERT_TRUE(settle());
        const std::vector<uint8_t> reference = image();
        sprites.clear();
        Sprite& base = frameSprite(PivotPreset::CENTER);
        SpriteBatch batch(&scene, base.getEntity());
        ASSERT_EQ(batch.setPositions(positions), "");
        ASSERT_EQ(batch.setRotations(rotations), "");
        ASSERT_TRUE(settle());
        const std::vector<uint8_t> instanced = image();
        EXPECT_GT(lit(reference), 500u);
        EXPECT_EQ(differing(instanced, reference), 0u);
        sprites.clear();

        // trimmed frames of different sizes and offsets, turned off the grid: each instance snaps
        // at its position (its pivot), not at its matrix's origin (which carries the frame's offset)
        positions.clear();
        rotations.clear();
        std::vector<int32_t> frames;
        for (int i = 0; i < 12; i++){
            const float x = 24.0f + (float)(i % 4) * 30.0f + 0.25f * (float)(i % 3);
            const float y = 34.0f + (float)(i / 4) * 34.0f + 0.25f;
            positions.insert(positions.end(), {x, y});
            rotations.push_back(angles[i % 3] * HALF_PI / 90.0f);
            frames.push_back(i % 2);
            Sprite& s = trimmedSprite(i % 2, PivotPreset::BOTTOM_LEFT);
            s.setPosition(x, y, 0.0f);
            s.setRotation(0.0f, 0.0f, angles[i % 3]);
        }
        ASSERT_TRUE(settle());
        const std::vector<uint8_t> trimmedReference = image();
        sprites.clear();
        Sprite& trimmedBase = trimmedSprite(0, PivotPreset::BOTTOM_LEFT);
        SpriteBatch trimmedBatch(&scene, trimmedBase.getEntity());
        ASSERT_EQ(trimmedBatch.setPositions(positions), "");
        ASSERT_EQ(trimmedBatch.setRotations(rotations), "");
        ASSERT_EQ(trimmedBatch.setFrames(frames), "");
        ASSERT_TRUE(settle());
        EXPECT_GT(lit(trimmedReference), 300u);
        EXPECT_EQ(differing(image(), trimmedReference), 0u) << "trimmed frames";
        sprites.clear();
        scene.removeComponent<PixelPerfectCameraComponent>(camera.getEntity());
    }
}

// AC4 (D8, a deliberate change): an Instanced Mesh's rotated instances under Rotate On Grid snap
// their pivot (before S52 every vertex snapped, so a 30-degree quad's corners moved to the grid);
// its 90-degree instances, written with exact zeros, still snap every vertex
TEST_F(SpriteBatchComponentGpu, InstancedMeshRotatedInstancesSnapTheirPivot){
    JASPER_REQUIRE_GPU();
    PixelPerfectCamera ppc(&scene, camera.getEntity());
    ppc.setReferenceResolution(Vector2(128.0f, 128.0f));
    ppc.setRotationMode(PixelRotation::ON_GRID);
    // the reference: the same quads as Sprites (drawn alone, mode by their own matrix)
    const float angles[3] = {0.0f, 90.0f, 30.0f};
    for (int i = 0; i < 3; i++){
        Sprite& s = frameSprite(PivotPreset::CENTER);
        s.setPosition(40.0f + (float)i * 60.0f + 0.25f, 100.25f, 0.0f);
        s.setRotation(0.0f, 0.0f, angles[i]);
    }
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> reference = image();
    sprites.clear();
    Sprite& base = frameSprite(PivotPreset::CENTER);
    scene.addComponent<InstancedMeshComponent>(base.getEntity(), {});
    InstancedMeshComponent& instances = scene.getComponent<InstancedMeshComponent>(base.getEntity());
    for (int i = 0; i < 3; i++){
        InstanceData instance;
        instance.position = Vector3(40.0f + (float)i * 60.0f + 0.25f, 100.25f, 0.0f);
        instance.rotation = Quaternion(angles[i], Vector3(0.0f, 0.0f, 1.0f));
        instances.instances.push_back(instance);
    }
    instances.needUpdateInstances = true;
    ASSERT_TRUE(settle());
    EXPECT_EQ(differing(image(), reference), 0u);
}

// An Instanced Mesh Sprite with a library:sprite instance= feature (FLASH): its instances carry no
// FX amounts, so its variant takes no 'Sfx' (it would read an unbound i_spriteFx: undefined on
// Vulkan, invalid on Metal and D3D11) and the flash is the material's, as the Sprites alone draw it
TEST_F(SpriteBatchComponentGpu, InstancedMeshFlashReadsNoInstanceFx){
    JASPER_REQUIRE_GPU();
    auto flash = [](Sprite& s){
        MeshComponent& mesh = s.getComponent<MeshComponent>();
        mesh.submeshes[0].material.shader = "library:sprite";
        mesh.submeshes[0].material.shaderFeatures = {"FLASH"};
        mesh.submeshes[0].material.shaderParams = {{"flashColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f)}, {"flashAmount", Vector4(0.5f, 0.0f, 0.0f, 0.0f)}};
        mesh.needReload = true;
    };
    for (int i = 0; i < 3; i++){
        Sprite& s = frameSprite();
        flash(s);
        s.setPosition(30.0f + (float)i * 60.0f, 100.0f, 0.0f);
    }
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> reference = image();
    sprites.clear();
    Sprite& base = frameSprite();
    flash(base);
    scene.addComponent<InstancedMeshComponent>(base.getEntity(), {});
    InstancedMeshComponent& instances = scene.getComponent<InstancedMeshComponent>(base.getEntity());
    for (int i = 0; i < 3; i++){
        InstanceData instance;
        instance.position = Vector3(30.0f + (float)i * 60.0f, 100.0f, 0.0f);
        instances.instances.push_back(instance);
    }
    instances.needUpdateInstances = true;
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> instanced = image();
    const MeshComponent& mesh = base.getComponent<MeshComponent>();
    EXPECT_EQ(mesh.batchAttributes & MeshBatchAttribute_SpriteFx, 0) << "no a_spriteFx on an Instanced Mesh sprite";
    EXPECT_EQ(mesh.submeshes[0].shaderProperties & ShaderPool::getSpriteBatchProperty(ShaderType::MESH, ShaderPool::SpriteBatchInput::FX), 0u)
        << "no 'Sfx' with an Instanced Mesh";
    EXPECT_GT(lit(reference), 300u);
    EXPECT_EQ(differing(instanced, reference), 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// R17: a SpriteInstanceBatch another producer owns (S50's tilemap chunks) draws as the instances of
// the GameObject it is attached to: one draw per page run, that GameObject's Transform its model
// matrix (moving it moves them all), its bounds the batch's
TEST_F(SpriteBatchComponentGpu, AttachedBatchDrawsAsItsGameObject){
    JASPER_REQUIRE_GPU();
    Sprite& owner = frameSprite();
    SpriteInstanceBatch chunk;
    chunk.reserve(4);
    chunk.setCount(3);
    for (uint32_t i = 0; i < 3; i++){
        InstanceRenderData& instance = chunk.instances()[i];
        instance.instanceMatrix = Matrix4();
        instance.instanceMatrix[3][0] = 20.0f + (float)i * 40.0f;
        instance.instanceMatrix[3][1] = 40.0f;
        instance.color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        instance.textureRect = Rect(0.0f, 0.0f, 0.5f, 1.0f);
    }
    const SpritePageRun run{0, 3, 0};
    chunk.setPageRuns(std::span<const SpritePageRun>(&run, 1));
    chunk.markChanged(0.0f, 0.0f, 16.0f, 16.0f);
    RenderSystem& render = *scene.getSystem<RenderSystem>();
    render.attachInstanceBatch(owner.getEntity(), &chunk);
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> first = image();
    EXPECT_EQ(batchStats().draws, 1u);
    EXPECT_EQ(batchStats().instances, 3u);
    EXPECT_GT(lit(first), 3u * 200u);
    EXPECT_GT(owner.getComponent<MeshComponent>().aabb.getMaximum().x, 100.0f) << "the batch's bounds";
    owner.setPosition(0.0f, 100.0f, 0.0f);
    const std::vector<uint8_t> moved = image();
    EXPECT_EQ(lit(moved), lit(first));
    EXPECT_GT(differing(moved, first), 0u) << "the GameObject's Transform is the model";
    render.detachInstanceBatch(owner.getEntity());
    ASSERT_TRUE(settle());
    image();
    EXPECT_EQ(batchStats().draws, 0u);
    EXPECT_NO_ENGINE_ERRORS();
}
