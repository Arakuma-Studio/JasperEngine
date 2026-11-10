// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Scene view's Terrain World overlay (T4 R42, R46; AC41's overlay logic): what each mode colours,
// read from the runtime's state (tiles' states and pins, the main view's drawn nodes, the collider's
// chunks), and the names visualize_terrain and the Scene view give them.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/TerrainWorldTestSupport.h"
#include "terrainworld/TerrainWorldOverlay.h"

#include "TempDir.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::terraincore::TileCoord;
using jasperengine::terraincore::TileState;

namespace {

    // a box of 3 x 3 tiles of 64 m from (-1, -1)
    TerrainWorldOverlayState overlayBox(TerrainWorldOverlayMode mode) {
        TerrainWorldOverlayState state;
        state.mode = mode;
        state.low = TileCoord(-1, -1);
        state.high = TileCoord(1, 1);
        state.tileSize = 64.0f;
        state.states.assign(9, static_cast<uint8_t>(TileState::Absent));
        state.pinned.assign(9, 0);
        state.valid = true;
        return state;
    }

    struct Texel {
        uint8_t r, g, b, a;
    };

    Texel texelAt(const std::vector<uint8_t>& rgba, int width, int x, int y) {
        const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
        return Texel{rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3]};
    }

    uint8_t overlayByteOf(float value) {
        return static_cast<uint8_t>(std::lround(value * 255.0f));
    }

    // the texel at the centre of tile (column, row) of the box
    Texel tileCentre(const TerrainWorldOverlayState& state, const std::vector<uint8_t>& rgba, int width, int column, int row) {
        const int k = terrainWorldOverlayTexelsPerTile(state);
        return texelAt(rgba, width, column * k + k / 2, row * k + k / 2);
    }

}

TEST(TerrainWorldOverlay, NamesAndLabels) {
    TerrainWorldOverlayMode mode;
    ASSERT_TRUE(terrainWorldOverlayMode("collision", mode));
    EXPECT_EQ(mode, TerrainWorldOverlayMode::CollisionChunks);
    EXPECT_FALSE(terrainWorldOverlayMode("height", mode));
    EXPECT_STREQ(terrainWorldOverlayLabel("tiles"), "Tile Borders");
    EXPECT_STREQ(terrainWorldOverlayLabel("tile_state"), "Tile State");
    EXPECT_STREQ(terrainWorldOverlayLabel("lod"), "LOD Level");
    EXPECT_STREQ(terrainWorldOverlayLabel("collision"), "Collision Chunks");
    EXPECT_STREQ(terrainWorldOverlayLabel("slope"), "");
}

TEST(TerrainWorldOverlay, TileStateColoursEachTile) {
    TerrainWorldOverlayState state = overlayBox(TerrainWorldOverlayMode::TileState);
    state.states[4] = static_cast<uint8_t>(TileState::Resident);   // (0, 0)
    state.states[3] = static_cast<uint8_t>(TileState::Cold);       // (-1, 0)
    state.states[5] = static_cast<uint8_t>(TileState::Error);      // (1, 0)
    state.states[1] = static_cast<uint8_t>(TileState::CpuReady);   // (0, -1), pinned
    state.pinned[1] = 1;
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    bakeTerrainWorldOverlay(state, rgba, width, height);
    const int k = terrainWorldOverlayTexelsPerTile(state);
    EXPECT_EQ(width, 3 * k);
    EXPECT_EQ(height, 3 * k);
    const Vector4 resident = terrainWorldStateColour(TileState::Resident, false);
    const Texel centre = tileCentre(state, rgba, width, 1, 1);
    EXPECT_EQ(centre.r, overlayByteOf(resident.x));
    EXPECT_EQ(centre.g, overlayByteOf(resident.y));
    EXPECT_EQ(centre.a, overlayByteOf(0.55f));
    EXPECT_EQ(texelAt(rgba, width, k, k).a, 255) << "the tile's edge is solid";
    EXPECT_EQ(tileCentre(state, rgba, width, 0, 1).r, overlayByteOf(terrainWorldStateColour(TileState::Cold, false).x));
    EXPECT_EQ(tileCentre(state, rgba, width, 2, 1).r, overlayByteOf(terrainWorldStateColour(TileState::Error, false).x));
    EXPECT_EQ(tileCentre(state, rgba, width, 1, 0).r, overlayByteOf(terrainWorldStateColour(TileState::CpuReady, true).x)) << "pinned";
    EXPECT_EQ(tileCentre(state, rgba, width, 0, 0).a, 0) << "an absent tile shows nothing";
    const std::vector<TerrainOverlayLegendStop> legend = terrainWorldOverlayLegend(state);
    ASSERT_FALSE(legend.empty());
    EXPECT_EQ(legend.back().label, "pinned");
}

TEST(TerrainWorldOverlay, TileBordersOutlinePresentTiles) {
    TerrainWorldOverlayState state = overlayBox(TerrainWorldOverlayMode::TileBorders);
    state.states[4] = static_cast<uint8_t>(TileState::Cold);
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    bakeTerrainWorldOverlay(state, rgba, width, height);
    const int k = terrainWorldOverlayTexelsPerTile(state);
    EXPECT_EQ(tileCentre(state, rgba, width, 1, 1).a, 0) << "inside a tile: clear";
    EXPECT_EQ(texelAt(rgba, width, k, k + k / 2).a, 255) << "its west border";
    EXPECT_EQ(texelAt(rgba, width, 2 * k - 1, k + k / 2).a, 255) << "its east border";
    EXPECT_EQ(texelAt(rgba, width, k - 1, k + k / 2).a, 0) << "the absent neighbour's border";
}

TEST(TerrainWorldOverlay, LodAndCollisionColourTheirRectangles) {
    TerrainWorldOverlayState state = overlayBox(TerrainWorldOverlayMode::LodLevel);
    // a level-0 node of 32 m at the corner of tile (0, 0); the overview over tile (1, 1)
    state.rects.push_back({0.0f, 0.0f, 32.0f, 0, false});
    state.rects.push_back({64.0f, 64.0f, 64.0f, 3, true});
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    bakeTerrainWorldOverlay(state, rgba, width, height);
    const int k = terrainWorldOverlayTexelsPerTile(state);
    const Texel near = texelAt(rgba, width, k + k / 4, k + k / 4);
    const Texel overview = tileCentre(state, rgba, width, 2, 2);
    EXPECT_GT(near.a, 0);
    EXPECT_GT(overview.a, 0);
    EXPECT_NE(near.r, overview.r) << "a level and the overview differ";
    EXPECT_EQ(texelAt(rgba, width, k + 3 * k / 4, k + 3 * k / 4).a, 0) << "outside every drawn node";
    std::vector<TerrainOverlayLegendStop> legend = terrainWorldOverlayLegend(state);
    ASSERT_EQ(legend.size(), 2u);
    EXPECT_EQ(legend[0].label, "level 0");
    EXPECT_EQ(legend[1].label, "overview");

    state.mode = TerrainWorldOverlayMode::CollisionChunks;
    state.rects = {{0.0f, 0.0f, 32.0f, 0, false}, {-64.0f, -64.0f, 64.0f, 1, false}};
    bakeTerrainWorldOverlay(state, rgba, width, height);
    const Texel fine = texelAt(rgba, width, k + k / 4, k + k / 4);
    const Texel coarse = tileCentre(state, rgba, width, 0, 0);
    EXPECT_GT(fine.g, fine.r) << "a fine chunk is green";
    EXPECT_GT(coarse.r, coarse.b) << "a coarse tile is orange";
    legend = terrainWorldOverlayLegend(state);
    ASSERT_EQ(legend.size(), 2u);
    state.rects.clear();
    legend = terrainWorldOverlayLegend(state);
    ASSERT_EQ(legend.size(), 1u);
    EXPECT_NE(legend[0].label.find("Play"), std::string::npos);
}

TEST(TerrainWorldOverlay, CapturesTheRuntime) {
    jasperengine::test::TempDir files;
    ASSERT_TRUE(terrainworldtest::writePlayerWorld(files.path() / "Seen.terrainworld", terrainworldtest::smallLattice(64, 1.0f), 50.0f, -1, 0,
                                                   [](int, int) { return static_cast<uint16_t>(30000); }));
    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<MeshComponent>(entity, {});
    scene.addComponent<TerrainComponent>(entity, {});
    TerrainWorldComponent component;
    component.worldData.path = (files.path() / "Seen.terrainworld").generic_string();
    component.useCameraFocus = false;
    scene.addComponent<TerrainWorldComponent>(entity, component);
    std::string error;
    TerrainWorldRuntime* runtime = scene.getSystem<TerrainWorldSystem>()->ensureRuntime(entity, &error);
    ASSERT_NE(runtime, nullptr) << error;

    TerrainWorldOverlayState state = captureTerrainWorldOverlay(*runtime, TerrainWorldOverlayMode::TileState);
    ASSERT_TRUE(state.valid);
    // the box of the tiles, each one's state
    EXPECT_EQ(state.low, TileCoord(-1, -1));
    EXPECT_EQ(state.high, TileCoord(0, 0));
    EXPECT_EQ(state.tileSize, 64.0f);
    auto at = [&](int x, int z) { return static_cast<size_t>(z - state.low.z) * static_cast<size_t>(state.columns()) + static_cast<size_t>(x - state.low.x); };
    EXPECT_EQ(static_cast<TileState>(state.states[at(0, 0)]), TileState::Cold);
    EXPECT_EQ(static_cast<TileState>(state.states[at(-1, -1)]), TileState::Cold);
    const uint64_t before = terrainWorldOverlaySignature(state);

    // a tile read and pinned (the brush hovers it): the state shows both, the image changes
    ASSERT_TRUE(runtime->loadNow(TileCoord(0, 0)));
    runtime->setHoverTiles({TileCoord(0, 0)});
    state = captureTerrainWorldOverlay(*runtime, TerrainWorldOverlayMode::TileState);
    EXPECT_NE(static_cast<TileState>(state.states[at(0, 0)]), TileState::Cold);
    EXPECT_EQ(state.pinned[at(0, 0)], 1);
    EXPECT_NE(terrainWorldOverlaySignature(state), before);

    // LOD Level: the nodes render kept for the main view, only while the overlay asks for them
    runtime->recordDrawnNodes(true);
    runtime->setDrawnNodes({TerrainWorldDrawnNode{16.0f, 16.0f, 32.0f, 0, false}, TerrainWorldDrawnNode{-64.0f, -64.0f, 128.0f, 2, true}});
    state = captureTerrainWorldOverlay(*runtime, TerrainWorldOverlayMode::LodLevel);
    ASSERT_EQ(state.rects.size(), 2u);
    EXPECT_EQ(state.rects[0].minX, 0.0f);
    EXPECT_EQ(state.rects[1].value, 2);
    EXPECT_TRUE(state.rects[1].overview);
    runtime->recordDrawnNodes(false);
    EXPECT_TRUE(runtime->drawnNodes().empty());
    // Collision Chunks: no collider out of Play (no physics here)
    state = captureTerrainWorldOverlay(*runtime, TerrainWorldOverlayMode::CollisionChunks);
    EXPECT_TRUE(state.rects.empty());
}
