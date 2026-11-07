// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite Atlases and the Sliced draw mode on the headless device (docs/specs/S45-sprite-atlas.md
// AC2, AC3, R2, R6, R7): twelve sprites showing frames of three source textures draw the same
// picture, pixel for pixel, from their sources and from a trimmed atlas page that packs them (the
// remap through ImportedAssets, as the editor's provider and an export's index give it); with the
// remap's trim offsets dropped the picture differs (the red control). Drawn from the one page,
// the twelve batch into fewer draws, batched equal to unbatched. A bordered frame drawn Sliced at
// three sizes keeps its corners at their own size.

#include "Engine.h"
#include "Scene.h"
#include "asset/ImportedAssets.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "io/AtomicFile.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "subsystem/RenderSystem.h"
#include "texture/Texture.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "Readback.h"
#include "TestSystem.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int kCell = 12, kFrames = 4;

    // source s, frame f, pixel (x, y): an opaque box inset differently per frame, so trimming moves it
    std::array<uint8_t, 4> atlasPixel(int s, int f, int x, int y) {
        const int left = 1 + f, top = 2 + (f + s) % 3, right = kCell - 1 - s, bottom = kCell - 1 - f / 2;
        if (x < left || x >= right || y < top || y >= bottom) return {0, 0, 0, 0};
        return {static_cast<uint8_t>(30 + x * 15 + s * 40), static_cast<uint8_t>(20 + y * 17), static_cast<uint8_t>(50 * f + 30), 255};
    }

    struct AtlasScene {
        std::vector<std::string> sources;                  // the source textures (absolute paths)
        std::string page;                                  // the atlas page (in the folder)
        std::map<std::string, std::string> remaps;         // source -> its remapped sheet file
        std::map<std::string, std::string> noOffsets;      // the same with the trim offsets dropped
    };

    std::string writeText(const std::filesystem::path& path, const std::string& text) {
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        EXPECT_TRUE(io::writeFileAtomic(path, text, options).ok());
        return path.string();
    }

    // three sources of 4 frames side by side (grid textures with "rect:" keys), one page holding
    // every frame's opaque box, a pixel apart
    AtlasScene makeAtlas(const std::filesystem::path& dir) {
        AtlasScene out;
        const int pageW = 3 * kFrames * (kCell + 1), pageH = kCell;
        std::vector<uint8_t> page(static_cast<size_t>(pageW) * pageH * 4, 0);
        int packX = 0;
        for (int s = 0; s < 3; s++) {
            const std::string name = "source" + std::to_string(s) + ".png";
            const std::filesystem::path png = dir / name;
            std::vector<uint8_t> rgba(static_cast<size_t>(kCell * kFrames) * kCell * 4, 0);
            SpriteSheetData remap;
            remap.source = name;
            for (int f = 0; f < kFrames; f++) {
                int x0 = kCell, y0 = kCell, x1 = 0, y1 = 0;
                for (int y = 0; y < kCell; y++) {
                    for (int x = 0; x < kCell; x++) {
                        const std::array<uint8_t, 4> p = atlasPixel(s, f, x, y);
                        std::copy(p.begin(), p.end(), rgba.begin() + (static_cast<size_t>(y) * kCell * kFrames + f * kCell + x) * 4);
                        if (p[3]) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1); }
                    }
                }
                for (int y = y0; y < y1; y++) {
                    for (int x = x0; x < x1; x++) {
                        const std::array<uint8_t, 4> p = atlasPixel(s, f, x, y);
                        std::copy(p.begin(), p.end(), page.begin() + (static_cast<size_t>(y - y0) * pageW + packX + (x - x0)) * 4);
                    }
                }
                SpriteFrame frame;
                frame.name = "s" + std::to_string(s) + "_" + std::to_string(f);
                frame.key = "rect:" + std::to_string(f * kCell) + ",0," + std::to_string(kCell) + "," + std::to_string(kCell);
                frame.rect = Rect(static_cast<float>(packX), 0.0f, static_cast<float>(x1 - x0), static_cast<float>(y1 - y0));
                frame.sourceSize = Vector2(kCell, kCell);
                frame.offset = Vector2(static_cast<float>(x0), static_cast<float>(y0));
                frame.pivot = Vector2(kCell / 2, kCell / 2);
                remap.frames.push_back(frame);
                packX += (x1 - x0) + 1;
            }
            EXPECT_TRUE(writePng(png.string(), rgba, kCell * kFrames, kCell));
            out.sources.push_back(name);
            remap.pages.push_back({"page", pageW, pageH});
            out.remaps[name] = writeText(dir / ("remap" + std::to_string(s) + ".json"), writeSheet(remap));
            for (SpriteFrame& f : remap.frames) f.offset = Vector2(0, 0);
            out.noOffsets[name] = writeText(dir / ("nooffsets" + std::to_string(s) + ".json"), writeSheet(remap));
        }
        out.page = "page0.png";
        EXPECT_TRUE(writePng((dir / out.page).string(), page, pageW, pageH));
        return out;
    }

    class SpriteAtlasRenderGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::map<std::string, std::string> provided;

        void SetUp() override {
            scene.setBackgroundColor(1.0f, 0.0f, 1.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.activate();
            Engine::setScene(&scene);
            ImportedAssets::setSheetProvider([this](const std::string& ref, std::string& out) {
                auto it = provided.find(ref);
                if (it == provided.end()) return false;
                out = it->second;
                return true;
            });
        }

        void TearDown() override {
            ImportedAssets::setSheetProvider(nullptr);
            Engine::removeScene(&scene);
        }

        bool render() {
            Scene* current = &scene;
            const bool ready = GpuEnvironment::runFrames([current]() { return current->getSystem<RenderSystem>()->isAllLoaded(); });
            GpuEnvironment::drawFrames(3);
            return ready;
        }

        std::vector<uint8_t> capture() {
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), int(target.getWidth()), int(target.getHeight()), rgba));
            return rgba;
        }

    };

    size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        size_t n = 0;
        for (size_t p = 0; p < std::min(a.size(), b.size()); p++) n += a[p] != b[p] ? 1 : 0;
        return n + (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
    }

}

TEST_F(SpriteAtlasRenderGpu, AtlasEqualsSources) {
    JASPER_REQUIRE_GPU();
    TestTempDir dir("atlas-gpu");
    // the sources by asset path, as sprites name them (the remaps are keyed by it)
    TestSystem::get().setAssetPath(dir.path().string());
    struct RestoreAssets {
        ~RestoreAssets() { TestSystem::get().setAssetPath(""); }
    } restoreAssets;
    const AtlasScene atlas = makeAtlas(dir.path());
    struct RestoreBatching {
        bool was = Engine::isDynamicBatching();
        ~RestoreBatching() { Engine::setDynamicBatching(was); }
    } restoreBatching;
    // the sources' own sheets: a grid of their 4 cells (a texture's Sprite Mode)
    std::map<std::string, std::string> grids;
    for (size_t s = 0; s < atlas.sources.size(); s++) {
        SpriteSheetData grid;
        grid.source = atlas.sources[s];
        grid.pages.push_back({atlas.sources[s], kCell * kFrames, kCell});
        for (int f = 0; f < kFrames; f++) {
            SpriteFrame frame;
            frame.name = "s" + std::to_string(s) + "_" + std::to_string(f);
            frame.rect = Rect(static_cast<float>(f * kCell), 0.0f, kCell, kCell);
            frame.sourceSize = Vector2(kCell, kCell);
            frame.pivot = Vector2(kCell / 2, kCell / 2);
            grid.frames.push_back(frame);
        }
        grids[atlas.sources[s]] = writeText(dir.path() / ("grid" + std::to_string(s) + ".json"), writeSheet(grid));
    }
    auto use = [&](const std::map<std::string, std::string>& sheets, bool page) {
        provided.clear();
        for (const std::string& source : atlas.sources) {
            provided[source + "#sheet"] = sheets.at(source);
            if (page) {
                provided[source + "#atlas"] = sheets.at(source);
                provided[source + "#image#0"] = (dir.path() / atlas.page).string();
            }
            SpriteSheetPool::invalidate(source);
        }
    };

    const PivotPreset pivots[] = {PivotPreset::CENTER, PivotPreset::TOP_CENTER, PivotPreset::BOTTOM_CENTER, PivotPreset::LEFT_CENTER,
                                  PivotPreset::RIGHT_CENTER, PivotPreset::TOP_LEFT, PivotPreset::BOTTOM_LEFT, PivotPreset::TOP_RIGHT,
                                  PivotPreset::BOTTOM_RIGHT, PivotPreset::FRAME, PivotPreset::FRAME, PivotPreset::CENTER};
    const float cellW = static_cast<float>(GpuEnvironment::target().getWidth()) / 4.0f;
    const float cellH = static_cast<float>(GpuEnvironment::target().getHeight()) / 3.0f;
    std::vector<std::unique_ptr<Sprite>> sprites;
    use(grids, false);
    for (int i = 0; i < 12; i++) {
        auto sprite = std::make_unique<Sprite>(&scene);
        sprite->setSheet(atlas.sources[static_cast<size_t>(i % 3)]);
        sprite->setFrame(i % kFrames);
        sprite->setSize(kCell * 3, kCell * 3);
        sprite->setPivotPreset(pivots[i]);
        Texture& texture = sprite->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture;
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        sprite->setPosition(cellW * (static_cast<float>(i % 4) + 0.5f), cellH * (static_cast<float>(i / 4) + 0.5f), 0.0f);
        sprites.push_back(std::move(sprite));
    }
    Engine::setDynamicBatching(false);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> fromSources = capture();
    size_t drawn = 0;
    for (size_t p = 0; p + 3 < fromSources.size(); p += 4) drawn += (fromSources[p] == 255 && fromSources[p + 1] == 0 && fromSources[p + 2] == 255) ? 0 : 1;
    ASSERT_GT(drawn, 12u * 9u * 20u) << "the sprites are on screen";
    for (int i = 0; i < 12; i++) ASSERT_EQ(sprites[static_cast<size_t>(i)]->getFrameIndex(), i % kFrames);

    // packed: the same sources, drawn from their remaps into the one page (names, sizes, pivots kept)
    use(atlas.remaps, true);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> fromAtlas = capture();
    EXPECT_EQ(differing(fromAtlas, fromSources), 0u) << "the trimmed atlas page draws what the source textures draw";
    for (int i = 0; i < 12; i++) {
        EXPECT_EQ(sprites[static_cast<size_t>(i)]->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture.getPath(),
                  (dir.path() / atlas.page).string()) << "sprite " << i << " draws from the page";
    }

    // the red control: the remap's trim offsets dropped
    use(atlas.noOffsets, true);
    ASSERT_TRUE(render());
    EXPECT_GT(differing(capture(), fromSources), 0u) << "without the offsets the atlas draws elsewhere, and the comparison sees it";

    // the twelve on one page batch: fewer draws, the same picture
    use(atlas.remaps, true);
    ASSERT_TRUE(render());
    Engine::setDynamicBatching(true);
    GpuEnvironment::drawFrames(3);
    const uint32_t batchedDraws = Engine::getFrameStats().drawCalls;
    const std::vector<uint8_t> batched = capture();
    Engine::setDynamicBatching(false);
    GpuEnvironment::drawFrames(3);
    const uint32_t unbatchedDraws = Engine::getFrameStats().drawCalls;
    EXPECT_EQ(differing(batched, fromSources), 0u) << "batched draws what unbatched does";
    EXPECT_LT(batchedDraws, unbatchedDraws) << "one page, one run: " << batchedDraws << " draws batched, " << unbatchedDraws << " not";
    sprites.clear();
    provided.clear();
    for (const std::string& source : atlas.sources) SpriteSheetPool::invalidate(source);
}

TEST_F(SpriteAtlasRenderGpu, SlicedKeepsItsCorners) {
    JASPER_REQUIRE_GPU();
    TestTempDir dir("sliced-gpu");
    // a 12 x 12 frame: 4-pixel corners red, green, blue, yellow; edges grey; centre white
    std::vector<uint8_t> rgba(12 * 12 * 4, 255);
    auto colourAt = [](int x, int y) -> std::array<uint8_t, 4> {
        const bool left = x < 4, right = x >= 8, top = y < 4, bottom = y >= 8;
        if (left && top) return {255, 0, 0, 255};
        if (right && top) return {0, 255, 0, 255};
        if (left && bottom) return {0, 0, 255, 255};
        if (right && bottom) return {255, 255, 0, 255};
        if (left || right || top || bottom) return {120, 120, 120, 255};
        return {255, 255, 255, 255};
    };
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            const std::array<uint8_t, 4> c = colourAt(x, y);
            std::copy(c.begin(), c.end(), rgba.begin() + (static_cast<size_t>(y) * 12 + x) * 4);
        }
    }
    const std::string png = (dir.path() / "panel.png").string();
    ASSERT_TRUE(writePng(png, rgba, 12, 12));
    SpriteSheetData sheet;
    sheet.pages.push_back({png, 12, 12});
    SpriteFrame frame;
    frame.name = "panel";
    frame.rect = Rect(0, 0, 12, 12);
    frame.sourceSize = Vector2(12, 12);
    frame.border = Vector4(4, 4, 4, 4);
    sheet.frames.push_back(frame);
    const std::string path = "gpu-test://sliced";
    auto shared = std::make_shared<const SpriteSheetData>(sheet);
    SpriteSheetPool::registerSource([path](const std::string& p) { return p == path; }, [shared](const std::string&) { return shared; });

    const unsigned sizes[3][2] = {{12, 12}, {40, 24}, {90, 60}};
    std::vector<std::unique_ptr<Sprite>> sprites;
    const float targetH = static_cast<float>(GpuEnvironment::target().getHeight());
    float x = 10.0f;
    for (const auto& size : sizes) {
        auto sprite = std::make_unique<Sprite>(&scene);
        sprite->setSheet(path);
        sprite->setDrawMode(SpriteDrawMode::SLICED);
        sprite->setSize(size[0], size[1]);
        sprite->setPivotPreset(PivotPreset::BOTTOM_LEFT);
        Texture& texture = sprite->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture;
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        sprite->setPosition(x, 10.0f, 0.0f);
        x += static_cast<float>(size[0]) + 10.0f;
        sprites.push_back(std::move(sprite));
    }
    ASSERT_TRUE(render());
    const std::vector<uint8_t> image = capture();
    const int w = static_cast<int>(GpuEnvironment::target().getWidth());
    // (x, y) in the camera's units: pixels from the bottom-left; the readback's rows go top-down
    auto pixel = [&](int px, int py) {
        const int row = static_cast<int>(targetH) - 1 - py;
        const size_t at = (static_cast<size_t>(row) * w + px) * 4;
        return std::array<uint8_t, 3>{image[at], image[at + 1], image[at + 2]};
    };
    x = 10.0f;
    for (const auto& size : sizes) {
        const int x0 = static_cast<int>(x), y0 = 10, x1 = x0 + static_cast<int>(size[0]), y1 = y0 + static_cast<int>(size[1]);
        SCOPED_TRACE("sprite " + std::to_string(size[0]) + "x" + std::to_string(size[1]));
        // each corner is 4 x 4 pixels at every size: its last pixel is the corner, the next is an edge
        EXPECT_EQ(pixel(x0 + 3, y1 - 4), (std::array<uint8_t, 3>{255, 0, 0}));      // top-left corner
        EXPECT_EQ(pixel(x1 - 4, y1 - 4), (std::array<uint8_t, 3>{0, 255, 0}));      // top-right
        EXPECT_EQ(pixel(x0 + 3, y0 + 3), (std::array<uint8_t, 3>{0, 0, 255}));      // bottom-left
        EXPECT_EQ(pixel(x1 - 4, y0 + 3), (std::array<uint8_t, 3>{255, 255, 0}));    // bottom-right
        if (size[0] > 12) {
            EXPECT_EQ(pixel(x0 + 4, y1 - 1), (std::array<uint8_t, 3>{120, 120, 120})) << "the top edge starts after the corner";
            EXPECT_EQ(pixel((x0 + x1) / 2, (y0 + y1) / 2), (std::array<uint8_t, 3>{255, 255, 255})) << "the centre stretches";
        }
        x += static_cast<float>(size[0]) + 10.0f;
    }
    sprites.clear();
    SpriteSheetPool::invalidate(path);
}
