// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sheet sprites on the headless device (docs/specs/S44-aseprite-import.md AC21, R48-R49): twelve
// Sprites drawn from a trimmed, packed page and the same twelve from a page of the untrimmed
// frames give the same picture, pixel for pixel, for every pivot and both flips. Both pages are
// made here from one set of frames, so the test needs no golden image. A sheet reimported over and
// over (R22: saves alternating between two versions) draws the newest page every time.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "subsystem/RenderSystem.h"
#include "texture/Texture.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "Readback.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int kFrameW = 12, kFrameH = 10, kFrames = 4;

    // frame f's pixel (x, y), y down: an opaque shape that never touches the frame's edges, so
    // trimming moves it, with a different inset per frame
    std::array<uint8_t, 4> sheetFramePixel(int f, int x, int y) {
        const int left = 1 + f, top = 2 + (f % 2), right = kFrameW - 2, bottom = kFrameH - 1 - f / 2;
        if (x < left || x >= right || y < top || y >= bottom) return {0, 0, 0, 0};
        return {static_cast<uint8_t>(40 + x * 17), static_cast<uint8_t>(30 + y * 21), static_cast<uint8_t>(60 * f + 20), 255};
    }

    struct SheetPages {
        SpriteSheetData trimmed, untrimmed;
    };

    // the untrimmed page: the frames side by side; the trimmed page: each frame's opaque box,
    // packed with a pixel between them
    SheetPages makeSheets(const std::filesystem::path& dir) {
        SheetPages out;
        std::vector<uint8_t> full(static_cast<size_t>(kFrameW * kFrames) * kFrameH * 4, 0);
        std::vector<uint8_t> packed(static_cast<size_t>((kFrameW + 1) * kFrames) * kFrameH * 4, 0);
        const int packedW = (kFrameW + 1) * kFrames;
        int packX = 0;
        for (int f = 0; f < kFrames; f++) {
            int x0 = kFrameW, y0 = kFrameH, x1 = 0, y1 = 0;
            for (int y = 0; y < kFrameH; y++) {
                for (int x = 0; x < kFrameW; x++) {
                    const std::array<uint8_t, 4> p = sheetFramePixel(f, x, y);
                    std::copy(p.begin(), p.end(), full.begin() + ((static_cast<size_t>(y) * kFrameW * kFrames) + f * kFrameW + x) * 4);
                    if (p[3]) {
                        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1);
                    }
                }
            }
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    const std::array<uint8_t, 4> p = sheetFramePixel(f, x, y);
                    std::copy(p.begin(), p.end(), packed.begin() + ((static_cast<size_t>(y - y0) * packedW) + packX + (x - x0)) * 4);
                }
            }
            SpriteFrame whole;
            whole.name = "f" + std::to_string(f);
            whole.rect = Rect(static_cast<float>(f * kFrameW), 0.0f, kFrameW, kFrameH);
            whole.sourceSize = Vector2(kFrameW, kFrameH);
            whole.pivot = Vector2(3.0f + f, 7.0f);
            SpriteFrame cut = whole;
            cut.rect = Rect(static_cast<float>(packX), 0.0f, static_cast<float>(x1 - x0), static_cast<float>(y1 - y0));
            cut.offset = Vector2(static_cast<float>(x0), static_cast<float>(y0));
            out.untrimmed.frames.push_back(whole);
            out.trimmed.frames.push_back(cut);
            packX += (x1 - x0) + 1;
        }
        const std::filesystem::path fullPng = dir / "untrimmed.png", packedPng = dir / "trimmed.png";
        EXPECT_TRUE(writePng(fullPng.string(), full, kFrameW * kFrames, kFrameH));
        EXPECT_TRUE(writePng(packedPng.string(), packed, packedW, kFrameH));
        out.untrimmed.pages.push_back({fullPng.string(), kFrameW * kFrames, kFrameH});
        out.trimmed.pages.push_back({packedPng.string(), packedW, kFrameH});
        return out;
    }

    class SpriteSheetRenderGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override {
            scene.setBackgroundColor(1.0f, 0.0f, 1.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override {
            Engine::removeScene(&scene);
        }

        bool render() {
            Scene* current = &scene;
            const bool ready = GpuEnvironment::runFrames([current]() { return current->getSystem<RenderSystem>()->isAllLoaded(); });
            GpuEnvironment::drawFrames(2);
            return ready;
        }

        std::vector<uint8_t> capture() {
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), int(target.getWidth()), int(target.getHeight()), rgba));
            return rgba;
        }
    };

}

TEST_F(SpriteSheetRenderGpu, TrimmedEqualsUntrimmed) {
    JASPER_REQUIRE_GPU();
    TestTempDir dir("sheet-gpu");
    const SheetPages sheets = makeSheets(dir.path());
    const std::string trimmedPath = "gpu-test://trimmed", untrimmedPath = "gpu-test://untrimmed";
    auto trimmed = std::make_shared<const SpriteSheetData>(sheets.trimmed);
    auto untrimmed = std::make_shared<const SpriteSheetData>(sheets.untrimmed);
    SpriteSheetPool::registerSource([trimmedPath, untrimmedPath](const std::string& p) { return p == trimmedPath || p == untrimmedPath; },
                                    [trimmed, untrimmed, trimmedPath](const std::string& p) { return p == trimmedPath ? trimmed : untrimmed; });

    // ten pivots (the nine presets and the frame's own) and two flips, each at 3x
    const PivotPreset pivots[] = {PivotPreset::CENTER, PivotPreset::TOP_CENTER, PivotPreset::BOTTOM_CENTER, PivotPreset::LEFT_CENTER,
                                  PivotPreset::RIGHT_CENTER, PivotPreset::TOP_LEFT, PivotPreset::BOTTOM_LEFT, PivotPreset::TOP_RIGHT,
                                  PivotPreset::BOTTOM_RIGHT, PivotPreset::FRAME, PivotPreset::FRAME, PivotPreset::CENTER};
    std::vector<std::unique_ptr<Sprite>> sprites;
    for (int i = 0; i < 12; i++) {
        auto sprite = std::make_unique<Sprite>(&scene);
        sprite->setSheet(trimmedPath);
        sprite->setFrame(i % kFrames);
        sprite->setSize(kFrameW * 3, kFrameH * 3);
        sprite->setPivotPreset(pivots[i]);
        // pixel art's sampling: a linear filter would read the packed page's neighbours at its edges
        Texture& texture = sprite->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture;
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        if (i >= 10) {
            SpriteComponent& c = sprite->getComponent<SpriteComponent>();
            c.automaticFlipY = false;
            c.flipY = i == 10;
        }
        // a 4 x 3 grid over the target (the orthographic camera's units are its pixels, from the
        // bottom-left), each cell wider than a sprite whatever its pivot
        const float cellW = static_cast<float>(GpuEnvironment::target().getWidth()) / 4.0f;
        const float cellH = static_cast<float>(GpuEnvironment::target().getHeight()) / 3.0f;
        sprite->setPosition(cellW * (static_cast<float>(i % 4) + 0.5f), cellH * (static_cast<float>(i / 4) + 0.5f), 0.0f);
        sprites.push_back(std::move(sprite));
    }
    ASSERT_TRUE(render());
    const std::vector<uint8_t> fromTrimmed = capture();
    size_t drawn = 0;
    for (size_t p = 0; p + 3 < fromTrimmed.size(); p += 4) {
        if (!(fromTrimmed[p] == 255 && fromTrimmed[p + 1] == 0 && fromTrimmed[p + 2] == 255)) drawn++;
    }
    // every sprite covers at least its smallest frame's 30 pixels at 3x
    ASSERT_GT(drawn, 12u * 30u * 9u) << "the sprites are on screen";

    for (std::unique_ptr<Sprite>& sprite : sprites) {
        const int frame = sprite->getFrameIndex();
        sprite->setSheet(untrimmedPath);
        sprite->setFrame(frame);
    }
    ASSERT_TRUE(render());
    const std::vector<uint8_t> fromUntrimmed = capture();
    ASSERT_EQ(fromTrimmed.size(), fromUntrimmed.size());
    size_t different = 0;
    for (size_t p = 0; p < fromTrimmed.size(); p++) different += fromTrimmed[p] != fromUntrimmed[p] ? 1 : 0;
    EXPECT_EQ(different, 0u) << "a trimmed page draws the frames where the untrimmed one does";
    SpriteSheetPool::invalidate(trimmedPath);
    SpriteSheetPool::invalidate(untrimmedPath);
}

// R22: the editor's importer swaps a new import in once its pages are loaded (a Texture of each
// page, loaded and dropped), then invalidates the sheet; saves that alternate between two versions
// go back to pages the GPU drew before and released
TEST_F(SpriteSheetRenderGpu, ReimportsAlternatingBetweenTwoPagesDrawTheNewest) {
    JASPER_REQUIRE_GPU();
    TestTempDir dir("sheet-gpu-reimport");
    const std::string path = "gpu-test://reimported";
    std::array<std::shared_ptr<const SpriteSheetData>, 2> versions;
    const std::array<std::array<uint8_t, 3>, 2> colours = {{{220, 40, 40}, {40, 60, 220}}};
    for (int v = 0; v < 2; v++) {
        const std::filesystem::path png = dir.path() / ("page" + std::to_string(v) + ".png");
        std::vector<uint8_t> pixels(static_cast<size_t>(kFrameW) * kFrameH * 4, 255);
        for (size_t p = 0; p < pixels.size(); p += 4) std::copy(colours[v].begin(), colours[v].end(), pixels.begin() + static_cast<std::ptrdiff_t>(p));
        ASSERT_TRUE(writePng(png.string(), pixels, kFrameW, kFrameH));
        SpriteSheetData sheet;
        SpriteFrame frame;
        frame.name = "f0";
        frame.rect = Rect(0.0f, 0.0f, kFrameW, kFrameH);
        frame.sourceSize = Vector2(kFrameW, kFrameH);
        sheet.frames.push_back(frame);
        sheet.pages.push_back({png.string(), kFrameW, kFrameH});
        versions[v] = std::make_shared<const SpriteSheetData>(std::move(sheet));
    }
    // (pages read in the background, as the editor's are)
    struct RestoreAsyncLoading {
        bool was = Engine::isAsyncLoading();
        ~RestoreAsyncLoading() { Engine::setAsyncLoading(was); }
    } restoreAsyncLoading;
    Engine::setAsyncLoading(true);
    auto current = std::make_shared<std::shared_ptr<const SpriteSheetData>>(versions[0]);
    SpriteSheetPool::registerSource([path](const std::string& p) { return p == path; }, [current](const std::string&) { return *current; });

    Sprite sprite(&scene);
    sprite.setSheet(path);
    sprite.setSize(kFrameW * 4, kFrameH * 4);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(static_cast<float>(GpuEnvironment::target().getWidth()) / 2.0f, static_cast<float>(GpuEnvironment::target().getHeight()) / 2.0f, 0.0f);
    auto centre = [&]() {
        const std::vector<uint8_t> rgba = capture();
        const size_t w = GpuEnvironment::target().getWidth(), h = GpuEnvironment::target().getHeight();
        const size_t at = ((h / 2) * w + w / 2) * 4;
        return std::array<uint8_t, 3>{rgba[at], rgba[at + 1], rgba[at + 2]};
    };
    ASSERT_TRUE(render());
    ASSERT_EQ(centre(), colours[0]);

    for (int save = 1; save <= 8; save++) {
        SCOPED_TRACE("save " + std::to_string(save));
        const int v = save % 2;
        {
            // the importer's swap: the page loads before the sheet changes, its Texture then dropped
            auto page = std::make_shared<Texture>(versions[v]->pages[0].texture);
            ASSERT_TRUE(GpuEnvironment::runFrames([&]() { return page->load().state == ResourceLoadState::Finished; }));
        }
        *current = versions[v];
        SpriteSheetPool::invalidate(path);
        // the page reads in the background: drawn within a few frames, never the previous one for good
        EXPECT_TRUE(GpuEnvironment::runFrames([&]() { return centre() == colours[v]; }, 120));
        EXPECT_EQ(centre(), colours[v]);
    }
    SpriteSheetPool::invalidate(path);
}
