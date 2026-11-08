// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite Atlases and the Sliced draw mode on the headless device (docs/specs/S45-sprite-atlas.md
// AC2, AC3, R2, R6, R7), on the editor's SpriteAtlasBuilder output (fixtures/sprite-atlas/, which
// SpriteAtlasBuild.GpuFixtureIsTheBuildersOutput pins): twelve sprites showing frames of three
// name@12x12 source textures draw the same picture, pixel for pixel, from their sources and from
// the trimmed atlas page that packs them (the remaps through ImportedAssets, as the editor's
// provider and an export's index give them); with the remaps' trim offsets dropped the picture
// differs (the red control). Drawn from the one page, the twelve batch into fewer draws, batched
// equal to unbatched. The atlas's bordered panel drawn Sliced at three sizes keeps its corners at
// their own size.

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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int kCell = 12, kFrames = 4;

    struct AtlasScene {
        std::vector<std::string> sources;                  // the source textures (asset paths)
        std::string page;                                  // the atlas page (in the folder)
        std::map<std::string, std::string> remaps;         // member -> its remapped sheet file
        std::map<std::string, std::string> noOffsets;      // the same with the trim offsets dropped
    };

    std::string writeText(const std::filesystem::path& path, const std::string& text) {
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        EXPECT_TRUE(io::writeFileAtomic(path, text, options).ok());
        return path.string();
    }

    std::string readText(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // the builder's atlas copied into the asset folder: three 4-frame strips, a bordered panel, one page
    AtlasScene loadAtlas(const std::filesystem::path& dir) {
        AtlasScene out;
        const std::filesystem::path fixture = std::filesystem::path(JASPER_TEST_ASSET_DIR) / "sprite-atlas";
        std::error_code ec;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(fixture, ec)) {
            std::filesystem::copy_file(entry.path(), dir / entry.path().filename(), std::filesystem::copy_options::overwrite_existing, ec);
        }
        out.page = "page0.png";
        for (const std::string& member : {std::string("source0@12x12.png"), std::string("source1@12x12.png"), std::string("source2@12x12.png"),
                                          std::string("panel.png")}) {
            const std::filesystem::path remapFile = dir / (std::filesystem::path(member).stem().string() + ".remap.json");
            out.remaps[member] = remapFile.string();
            SpriteSheetParseResult parsed = parseSheet(readText(remapFile));
            EXPECT_TRUE(parsed.ok()) << member << ": " << parsed.error;
            if (!parsed.ok()) continue;
            for (SpriteFrame& f : parsed.sheet->frames) f.offset = Vector2(0, 0);
            out.noOffsets[member] = writeText(dir / (std::filesystem::path(member).stem().string() + ".nooffsets.json"), writeSheet(*parsed.sheet));
            if (member != "panel.png") out.sources.push_back(member);
        }
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
    const AtlasScene atlas = loadAtlas(dir.path());
    ASSERT_EQ(atlas.sources.size(), 3u);
    struct RestoreBatching {
        bool was = Engine::isDynamicBatching();
        ~RestoreBatching() { Engine::setDynamicBatching(was); }
    } restoreBatching;
    // unpacked (no sheets given): the sources' own grids, by their name@12x12 file names
    auto use = [&](const std::map<std::string, std::string>& sheets, bool page) {
        provided.clear();
        for (const std::string& source : atlas.sources) {
            if (sheets.empty()) {
                SpriteSheetPool::invalidate(source);
                continue;
            }
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
    const float cellH = static_cast<float>(GpuEnvironment::target().getHeight()) / 4.0f;
    std::vector<std::unique_ptr<Sprite>> sprites;
    use({}, false);
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
    // a fourth row: legacy sprites (inline frames of the same cells) with a texture scale factor:
    // drawn from the page they sample the same inner texels (R8)
    std::vector<std::unique_ptr<Sprite>> legacy;
    for (int i = 0; i < 4; i++) {
        auto sprite = std::make_unique<Sprite>(&scene);
        sprite->setTexture(atlas.sources[static_cast<size_t>(i % 3)]);
        for (int f = 0; f < kFrames; f++) sprite->addFrame(static_cast<float>(f * kCell), 0.0f, static_cast<float>(kCell), static_cast<float>(kCell));
        sprite->setFrame(i);
        sprite->setSize(kCell * 3, kCell * 3);
        sprite->setPivotPreset(PivotPreset::CENTER);
        // (1 and 3 texels: the inner 10 or 6 texels over 36 pixels put no pixel centre on a texel edge)
        sprite->setTextureScaleFactor(i % 2 == 0 ? 1.0f : 3.0f);
        Texture& texture = sprite->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture;
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        sprite->setPosition(cellW * (static_cast<float>(i) + 0.5f), cellH * 3.5f, 0.0f);
        legacy.push_back(std::move(sprite));
    }
    Engine::setDynamicBatching(false);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> fromSources = capture();
    size_t drawn = 0;
    for (size_t p = 0; p + 3 < fromSources.size(); p += 4) drawn += (fromSources[p] == 255 && fromSources[p + 1] == 0 && fromSources[p + 2] == 255) ? 0 : 1;
    ASSERT_GT(drawn, 16u * 9u * 20u) << "the sprites are on screen";
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
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(legacy[static_cast<size_t>(i)]->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture.getPath(),
                  (dir.path() / atlas.page).string()) << "legacy sprite " << i << " draws from the page";
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
    legacy.clear();
    provided.clear();
    for (const std::string& source : atlas.sources) SpriteSheetPool::invalidate(source);
}

TEST_F(SpriteAtlasRenderGpu, SlicedKeepsItsCorners) {
    JASPER_REQUIRE_GPU();
    TestTempDir dir("sliced-gpu");
    TestSystem::get().setAssetPath(dir.path().string());
    struct RestoreAssets {
        ~RestoreAssets() { TestSystem::get().setAssetPath(""); }
    } restoreAssets;
    // the atlas's 12 x 12 panel (border 4): 4-pixel corners red, green, blue, yellow; edges grey;
    // centre white; drawn from the page through its remap
    const AtlasScene atlas = loadAtlas(dir.path());
    const std::string path = "panel.png";
    provided[path + "#sheet"] = atlas.remaps.at(path);
    provided[path + "#atlas"] = atlas.remaps.at(path);
    provided[path + "#image#0"] = (dir.path() / atlas.page).string();
    SpriteSheetPool::invalidate(path);

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
    for (const auto& sprite : sprites) {
        EXPECT_EQ(sprite->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture.getPath(), (dir.path() / atlas.page).string())
            << "the panel draws from the atlas page";
    }
    sprites.clear();
    provided.clear();
    SpriteSheetPool::invalidate(path);
}
