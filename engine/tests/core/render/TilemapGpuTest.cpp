// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Tilemap Layers on the GPU (docs/specs/S50-grid-tilemap.md AC5, R31-R36): chunks drawn through
// S52's instance batches. Each test renders a small map and reads pixels back; the expected picture
// comes from the spec's placement rules, computed here independently of the chunk builder.
// glcore and Vulkan (lavapipe).

#include "Engine.h"
#include "Scene.h"
#include "component/GridComponent.h"
#include "component/MeshComponent.h"
#include "component/TilemapLayerComponent.h"
#include "object/Camera.h"
#include "object/PixelPerfectCamera.h"
#include "object/Sprite.h"
#include "object/tilemap/Grid.h"
#include "object/tilemap/TilemapLayer.h"
#include "registry/SortingLayers.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/TilemapSystem.h"
#include "tilemap/TilesetPool.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int VIEW = 256;
    constexpr int TILE = 16;
    using Colour = std::array<uint8_t, 3>;
    const Colour kColours[4] = {{{220, 40, 40}}, {{40, 200, 60}}, {{50, 80, 230}}, {{230, 200, 40}}};

    // the page: a row of solid tiles (index 0-3), then tile 4: a two-colour shape no flip or
    // transpose leaves alone (the top-left quarter white, the top row's right half orange, the
    // rest dark), tile 5: the solid magenta frame next to a tile, for the bleeding tests, and tile 6:
    // an opaque centre on a transparent border (OUTLINE)
    std::array<uint8_t, 4> atlasPixel(int x, int y) {
        const int tile = x / TILE, tx = x % TILE, ty = y % TILE;
        if (tile < 4) {
            const Colour& c = kColours[tile];
            return {c[0], c[1], c[2], 255};
        }
        if (tile == 4) {
            if (tx < 8 && ty < 8) return {255, 255, 255, 255};
            if (ty < 4) return {255, 140, 0, 255};
            return {30, 30, 30, 255};
        }
        if (tile == 5) return {255, 0, 255, 255};
        if (tx >= 4 && tx < 12 && ty >= 4 && ty < 12) return {200, 40, 40, 255};
        return {0, 0, 0, 0};
    }

    std::string writeAtlas(const std::filesystem::path& dir) {
        const int w = TILE * 7, h = TILE;
        std::vector<uint8_t> pixels((size_t)w * h * 4);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const std::array<uint8_t, 4> p = atlasPixel(x, y);
                std::copy(p.begin(), p.end(), pixels.begin() + (size_t)(y * w + x) * 4);
            }
        }
        const std::string path = (dir / "atlas.png").string();
        EXPECT_TRUE(writePng(path, pixels, w, h));
        return path;
    }

    std::shared_ptr<TilesetData> makeTileset(const std::string& atlas) {
        auto data = std::make_shared<TilesetData>();
        data->name = "gpu";
        data->tileWidth = TILE;
        data->tileHeight = TILE;
        TilesetSource source;
        source.id = 0;
        source.kind = TilesetSourceKind::Texture;
        source.asset = AssetRef(atlas);
        data->sources.push_back(source);
        for (int i = 0; i < 7; i++)
            data->addTile(0, i, 0);
        return data;
    }

    class TilemapGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        TestTempDir dir{"tilemap-gpu"};
        std::string tilesetPath;
        std::string atlas;
        std::unique_ptr<TilemapLayer> layer;

        const std::string& atlasPathForTests() const { return atlas; }

        void SetUp() override {
            GpuEnvironment::setView(VIEW, VIEW, VIEW, VIEW, Scaling::STRETCH);
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            Engine::setScene(&scene);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(0.0f, (float)VIEW, 0.0f, (float)VIEW, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.setDepthTest(false);
            camera.activate();
            tilesetPath = (dir.path() / "gpu.tileset").string();
            atlas = writeAtlas(dir.path());
            TilesetPool::set(tilesetPath, makeTileset(atlas));
            layer = std::make_unique<TilemapLayer>(&scene);
            scene.addComponent<GridComponent>(layer->getEntity(), GridComponent());
            TilemapLayerComponent& component = layer->getComponent<TilemapLayerComponent>();
            component.tileset = AssetRef(tilesetPath);
            component.chunkSize = 8;
            nearest();
        }

        void TearDown() override {
            layer.reset();
            Engine::removeScene(&scene);
            TilesetPool::invalidate(tilesetPath);
            Engine::setPixelPerfectForcedScale(0);
            GpuEnvironment::resetView();
        }

        void nearest() {
            Texture& texture = layer->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture;
            texture.setMinFilter(TextureFilter::NEAREST);
            texture.setMagFilter(TextureFilter::NEAREST);
        }

        void linear() {
            Texture& texture = layer->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture;
            texture.setMinFilter(TextureFilter::LINEAR);
            texture.setMagFilter(TextureFilter::LINEAR);
        }

        GridComponent& grid() { return layer->getComponent<GridComponent>(); }
        void gridChanged() { grid().revision++; }

        bool settle() {
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current]() { return current->getSystem<RenderSystem>()->isAllLoaded(); });
        }

        std::vector<uint8_t> image(int frames = 3) {
            GpuEnvironment::drawFrames(frames);
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        // the pixel at world (x, y) (y up, 1 unit = 1 pixel with the default camera)
        static Colour at(const std::vector<uint8_t>& rgba, int x, int y) {
            const size_t row = (size_t)(VIEW - 1 - y);
            const size_t offset = (row * VIEW + (size_t)x) * 4;
            return Colour{rgba[offset], rgba[offset + 1], rgba[offset + 2]};
        }

        static bool near(const Colour& a, const Colour& b, int tolerance = 6) {
            return std::abs(a[0] - b[0]) <= tolerance && std::abs(a[1] - b[1]) <= tolerance && std::abs(a[2] - b[2]) <= tolerance;
        }

        const RenderSystem::SpriteBatchStats& stats() { return scene.getSystem<RenderSystem>()->getSpriteBatchStats(); }

        // the expected picture of `cells` drawn as full-tile solid quads in the layout's painter's
        // order, written from R7/R8 and not through the builder (the tile's colour at each pixel)
        struct Expected {
            std::vector<int> tiles;      // VIEW^2, -1: background
            int& at(int x, int y) { return tiles[(size_t)y * VIEW + x]; }
        };

        Expected rasterize(const TileGridSpec& spec, const std::vector<std::pair<CellCoord, int>>& cells, int originX = 0, int originY = 0) {
            std::vector<std::pair<CellCoord, int>> ordered = cells;
            std::stable_sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) { return tilegrid::painterLess(spec, a.first, b.first); });
            Expected out;
            out.tiles.assign((size_t)VIEW * VIEW, -1);
            for (const auto& [cell, tile] : ordered) {
                const TilePoint anchor = tilegrid::cellToLocal(spec, cell);
                const int x0 = originX + (int)std::floor(anchor.x - (spec.layout == TileGridLayout::Rectangle ? 0.0 : TILE * 0.5));
                const int y0 = originY + (int)std::floor(anchor.y);
                for (int y = y0; y < y0 + TILE; y++) {
                    for (int x = x0; x < x0 + TILE; x++) {
                        if (x >= 0 && x < VIEW && y >= 0 && y < VIEW)
                            out.at(x, y) = tile;
                    }
                }
            }
            return out;
        }

        void expectPicture(const std::vector<uint8_t>& shot, Expected& expected) {
            size_t wrong = 0;
            for (int y = 2; y < VIEW - 2; y++) {
                for (int x = 2; x < VIEW - 2; x++) {
                    const int tile = expected.at(x, y);
                    // a boundary pixel of the expected raster may round either way
                    bool uniform = true;
                    for (int dy = -1; dy <= 1 && uniform; dy++)
                        for (int dx = -1; dx <= 1 && uniform; dx++)
                            uniform = expected.at(x + dx, y + dy) == tile;
                    if (!uniform) continue;
                    const Colour want = tile < 0 ? Colour{0, 0, 0} : kColours[tile];
                    if (!near(at(shot, x, y), want)) wrong++;
                }
            }
            EXPECT_EQ(wrong, 0u);
        }
    };

}

// R7, R8: rectangle cells at their anchors, row 0 at the bottom; negative cells included
TEST_F(TilemapGpu, RectangleDrawsEachCell) {
    JASPER_REQUIRE_GPU();
    layer->setPosition(64.0f, 64.0f, 0.0f);
    TileCells& cells = layer->editCells();
    std::vector<std::pair<CellCoord, int>> placed;
    for (int y = -1; y < 3; y++) {
        for (int x = -2; x < 4; x++) {
            const int tile = (x + 2 + (y + 1) * 3) % 4;
            cells.setTile(CellCoord{x, y}, tile);
            placed.push_back({CellCoord{x + 4, y + 4}, tile});
        }
    }
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> shot = image();
    TileGridSpec spec = gridSpecOf(grid());
    Expected expected = rasterize(spec, placed);
    expectPicture(shot, expected);
    EXPECT_NO_ENGINE_ERRORS();
}

// R7, R8: the other layouts: images bottom-centred on their anchors, back to front
TEST_F(TilemapGpu, IsometricStaggeredAndHexagonPlaceTiles) {
    JASPER_REQUIRE_GPU();
    struct Case { TileGridLayout layout; StaggerAxis axis; StaggerIndex index; };
    const Case cases[] = {
        {TileGridLayout::Isometric, StaggerAxis::Y, StaggerIndex::Odd},
        {TileGridLayout::IsometricStaggered, StaggerAxis::Y, StaggerIndex::Odd},
        {TileGridLayout::IsometricStaggered, StaggerAxis::X, StaggerIndex::Even},
        {TileGridLayout::Hexagon, StaggerAxis::Y, StaggerIndex::Odd},
        {TileGridLayout::Hexagon, StaggerAxis::X, StaggerIndex::Odd},
    };
    layer->setPosition(96.0f, 48.0f, 0.0f);
    for (const Case& c : cases) {
        grid().layout = c.layout;
        grid().staggerAxis = c.axis;
        grid().staggerIndex = c.index;
        gridChanged();
        TileCells& cells = layer->editCells();
        cells.clear();
        std::vector<std::pair<CellCoord, int>> placed;
        for (int y = 0; y < 3; y++) {
            for (int x = 0; x < 3; x++) {
                const int tile = (x * 2 + y) % 4;
                cells.setTile(CellCoord{x, y}, tile);
                placed.push_back({CellCoord{x, y}, tile});
            }
        }
        ASSERT_TRUE(settle());
        const std::vector<uint8_t> shot = image();
        TileGridSpec spec = gridSpecOf(grid());
        // placed at the GameObject's origin: iso and hex cells reach left of it
        Expected world = rasterize(spec, placed, 96, 48);
        expectPicture(shot, world);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R5, D3: the transform bits in Tiled's order, in image space: transpose, then flip X, then flip Y
TEST_F(TilemapGpu, FlipsAndTransposeFollowTheImageSpaceOrder) {
    JASPER_REQUIRE_GPU();
    layer->setPosition(32.0f, 32.0f, 0.0f);
    TileCells& cells = layer->editCells();
    const uint32_t variants[] = {0, TILE_FLIP_X, TILE_FLIP_Y, TILE_FLIP_X | TILE_FLIP_Y, TILE_TRANSPOSE, TILE_TRANSPOSE | TILE_FLIP_X,
                                 TILE_TRANSPOSE | TILE_FLIP_Y, TILE_TRANSPOSE | TILE_FLIP_X | TILE_FLIP_Y};
    for (int i = 0; i < 8; i++)
        cells.setTile(CellCoord{i, 0}, 4, variants[i]);
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> shot = image();
    for (int i = 0; i < 8; i++) {
        const uint32_t flags = variants[i];
        size_t wrong = 0;
        for (int iy = 0; iy < TILE; iy++) {
            for (int ix = 0; ix < TILE; ix++) {
                // which source pixel shows at display pixel (ix, iy), image space (y down)
                int sx = ix, sy = iy;
                if (flags & TILE_FLIP_Y) sy = TILE - 1 - sy;
                if (flags & TILE_FLIP_X) sx = TILE - 1 - sx;
                if (flags & TILE_TRANSPOSE) std::swap(sx, sy);
                const std::array<uint8_t, 4> want = atlasPixel(TILE * 4 + sx, sy);
                const Colour got = at(shot, 32 + i * TILE + ix, 32 + TILE - 1 - iy);
                if (!near(got, Colour{want[0], want[1], want[2]})) wrong++;
            }
        }
        EXPECT_EQ(wrong, 0u) << "variant " << i;
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R27: a dual-grid layer draws its tiles half a cell down-left of the logical cells
TEST_F(TilemapGpu, DualGridDrawsHalfACellDownLeft) {
    JASPER_REQUIRE_GPU();
    layer->setPosition(64.0f, 64.0f, 0.0f);
    layer->getComponent<TilemapLayerComponent>().dualGrid = true;
    layer->editCells().setTile(CellCoord{1, 1}, 1);
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> shot = image();
    EXPECT_TRUE(near(at(shot, 64 + TILE - TILE / 2 + 3, 64 + TILE - TILE / 2 + 3), kColours[1]));
    EXPECT_TRUE(near(at(shot, 64 + 2 * TILE - TILE / 2 + 3, 64 + 2 * TILE - TILE / 2 + 3), Colour{0, 0, 0}));
    EXPECT_NO_ENGINE_ERRORS();
}

// R35: linear filtering next to a high-contrast frame of the atlas never bleeds it in, and shared
// edges leave no gap, at fractional camera positions and a zoom
TEST_F(TilemapGpu, SeamFreeAtFractionalCameraAndZoom) {
    JASPER_REQUIRE_GPU();
    linear();
    TileCells& cells = layer->editCells();
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 20; x++)
            cells.setTile(CellCoord{x, y}, 3); // yellow, next to a colour on the atlas's other side
    }
    ASSERT_TRUE(settle());
    for (float step : {0.0f, 0.25f, 0.5f, 0.75f}) {
        camera.setOrtho(step, step + (float)VIEW / 1.5f, step, step + (float)VIEW / 1.5f, -100.0f, 100.0f);
        const std::vector<uint8_t> shot = image();
        size_t background = 0, foreign = 0;
        for (int y = 8; y < VIEW - 8; y++) {
            for (int x = 8; x < VIEW - 8; x++) {
                const Colour c = at(shot, x, y);
                if (c[0] < 20 && c[1] < 20 && c[2] < 20) background++;
                if (!near(c, kColours[3], 40)) foreign++;
            }
        }
        EXPECT_EQ(background, 0u) << "a gap at step " << step;
        EXPECT_EQ(foreign, 0u) << "a neighbour's colour at step " << step;
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R35: under a Pixel Perfect Camera shared edges coincide at every camera position
TEST_F(TilemapGpu, PixelPerfectSharedEdgesHaveNoGaps) {
    JASPER_REQUIRE_GPU();
    TileCells& cells = layer->editCells();
    for (int y = -8; y < 24; y++) {
        for (int x = -8; x < 24; x++)
            cells.setTile(CellCoord{x, y}, 1);
    }
    PixelPerfectCamera perfect(&scene, camera.getEntity());
    perfect.setReferenceResolution(Vector2((float)VIEW, (float)VIEW));
    ASSERT_TRUE(settle());
    for (float offset : {0.0f, 0.25f, 0.5f, 0.75f}) {
        camera.setPosition(offset, offset, 10.0f);
        camera.setTarget(offset, offset, 0.0f);
        const std::vector<uint8_t> shot = image();
        size_t background = 0;
        for (int y = 16; y < VIEW - 16; y++) {
            for (int x = 16; x < VIEW - 16; x++) {
                const Colour c = at(shot, x, y);
                if (c[0] < 20 && c[1] < 20 && c[2] < 20) background++;
            }
        }
        EXPECT_EQ(background, 0u) << "a gap at offset " << offset;
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R31, R56: one draw per visible chunk and page
TEST_F(TilemapGpu, DrawsPerChunkAndCullsChunksOutOfView) {
    JASPER_REQUIRE_GPU();
    TileCells& cells = layer->editCells();
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++)
            cells.setTile(CellCoord{x, y}, 0);
    }
    for (int x = 400; x < 408; x++)
        cells.setTile(CellCoord{x, 0}, 0);    // a chunk far outside the view
    ASSERT_TRUE(settle());
    image();
    EXPECT_EQ(stats().draws, 2u);
    EXPECT_EQ(stats().instances, 16u * 8u);
    EXPECT_NO_ENGINE_ERRORS();
}

// R33: an edit re-uploads only its chunk; R36: frames without a change upload nothing
TEST_F(TilemapGpu, AnimatedChunkOnly) {
    JASPER_REQUIRE_GPU();
    auto data = std::make_shared<TilesetData>(*TilesetPool::get(tilesetPath)->data);
    TileDef* animated = data->findTile(0);
    animated->animation = {TileAnimationFrame{0, 500}, TileAnimationFrame{1, 500}};
    TilesetPool::set(tilesetPath, data);
    // the clock is driven by hand: wall-clock frames would flip the animation in the middle of a check
    layer->getComponent<TilemapLayerComponent>().animationSpeed = 0.0f;
    TileCells& cells = layer->editCells();
    cells.setTile(CellCoord{0, 0}, 0);
    for (int x = 8; x < 16; x++)
        cells.setTile(CellCoord{x, 0}, 2);    // the neighbour chunk, still
    ASSERT_TRUE(settle());
    image(3);
    EXPECT_TRUE(near(at(image(1), 3, 3), kColours[0]));
    const uint64_t oneChunk = sizeof(InstanceRenderData);
    // no frame change: no upload
    image(2);
    EXPECT_EQ(stats().uploadBytes, 0u);
    layer->getComponent<TilemapLayerComponent>().state->animationMs = 600.0;
    image(1);
    EXPECT_EQ(stats().uploadBytes, oneChunk) << "the animated tile's chunk alone re-uploads";
    EXPECT_TRUE(near(at(image(1), 3, 3), kColours[1]));
    // a cell edit re-uploads its chunk only
    image(1);
    cells.setTile(CellCoord{9, 1}, 3);
    image(1);
    EXPECT_EQ(stats().uploadBytes, sizeof(InstanceRenderData) * 9u);
    EXPECT_NO_ENGINE_ERRORS();
}

// R32: Individual mode gives each row its own sort item: a Sprite sorted by the Y axis passes between
// two rows of tiles
TEST_F(TilemapGpu, IndividualRowsSortWithSprites) {
    JASPER_REQUIRE_GPU();
    layer->getComponent<TilemapLayerComponent>().mode = TilemapRenderMode::Individual;
    TileCells& cells = layer->editCells();
    cells.setTile(CellCoord{0, 0}, 0);
    cells.setTile(CellCoord{0, 2}, 2);
    Sprite between(&scene);
    between.setTexture(atlasPathForTests());
    between.addFrame("green", 16.0f, 0.0f, 16.0f, 16.0f);
    between.setFrame(0);
    between.setSize(48, 48);
    between.setPivotPreset(PivotPreset::CENTER);
    between.setPosition(8.0f, 16.0f, 0.0f);
    between.getComponent<MeshComponent>().submeshes[0].material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
    SortingLayers::setTransparencySortMode(TransparencySortMode::CUSTOM_AXIS);
    SortingLayers::setTransparencySortAxis(Vector3(0.0f, 1.0f, 0.0f));
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> shot = image();
    SortingLayers::setTransparencySortMode(TransparencySortMode::DEFAULT);
    EXPECT_TRUE(near(at(shot, 4, 4), kColours[0])) << "the row in front of the sprite";
    EXPECT_TRUE(near(at(shot, 4, 36), kColours[1])) << "the row behind it";
    EXPECT_NO_ENGINE_ERRORS();
}

// R29, AC12: 1,000 edits in one frame rebuild each touched chunk once, and a frame without edits
// rebuilds nothing
TEST_F(TilemapGpu, ThousandEditsRebuildEachChunkOnce) {
    JASPER_REQUIRE_GPU();
    TileCells& cells = layer->editCells();
    for (int x = 0; x < 16; x++)
        cells.setTile(CellCoord{x, 0}, 0);   // two chunks of 8 in view
    ASSERT_TRUE(settle());
    image(2);
    TilemapSystem& system = *scene.getSystem<TilemapSystem>();
    const TilemapLayerComponent& component = layer->getComponent<TilemapLayerComponent>();
    for (int i = 0; i < 1000; i++)
        layer->setCell(i % 16, (i / 16) % 8, i % 4);
    image(1);
    EXPECT_EQ(system.getRebuiltChunks(component), 2u);
    image(1);
    EXPECT_EQ(system.getRebuiltChunks(component), 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// R37: the layer's tint is the material colour, and library:sprite features apply to tiles (OUTLINE
// draws inside the tile's own frame, around its opaque texels)
TEST_F(TilemapGpu, TintAndLibrarySpriteOutline) {
    JASPER_REQUIRE_GPU();
    layer->setPosition(64.0f, 64.0f, 0.0f);
    layer->getComponent<TilemapLayerComponent>().tint = Vector4(0.5f, 0.5f, 0.5f, 1.0f);
    layer->editCells().setTile(CellCoord{0, 0}, 1);
    layer->editCells().setTile(CellCoord{2, 0}, 6);
    Material& material = layer->getComponent<MeshComponent>().submeshes[0].material;
    material.shader = "library:sprite";
    material.shaderFeatures = {"OUTLINE"};
    layer->getComponent<MeshComponent>().needReload = true;
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> shot = image();
    // tint 0.5 (linear) halves the linear colour of the green tile
    const Colour tinted = at(shot, 64 + 8, 64 + 8);
    EXPECT_LT(tinted[1], kColours[1][1] - 40) << "the tint darkens the tile";
    EXPECT_GT(tinted[1], 40);
    // the outline: the transparent texel next to the opaque centre is drawn (white x tint), the
    // tile's far corner stays background
    const Colour outline = at(shot, 64 + 2 * TILE + 3, 64 + 8);
    EXPECT_GT((int)outline[0] + outline[1] + outline[2], 200) << "OUTLINE draws beside the opaque texels";
    EXPECT_TRUE(near(at(shot, 64 + 2 * TILE, 64 + TILE - 1), Colour{0, 0, 0})) << "nothing beyond the outline";
    EXPECT_NO_ENGINE_ERRORS();
}

// R62: a disabled layer draws nothing and keeps its cells
TEST_F(TilemapGpu, DisabledLayerDrawsNothing) {
    JASPER_REQUIRE_GPU();
    layer->editCells().setTile(CellCoord{1, 1}, 0);
    ASSERT_TRUE(settle());
    image();
    EXPECT_EQ(stats().draws, 1u);
    layer->getComponent<TilemapLayerComponent>().enabled = false;
    image();
    EXPECT_EQ(stats().draws, 0u);
    EXPECT_EQ(layer->getCellCount(), 1);
    EXPECT_NO_ENGINE_ERRORS();
}
