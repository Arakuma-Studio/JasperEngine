// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R42, R46: the Scene view's Terrain World overlay (visualize_terrain's tiles, tile_state, lod and
// collision): Tile Borders, Tile State, LOD Level (the level each part of the main view draws at, and
// what draws from the overview) and Collision Chunks (the fine chunks and coarse tiles of the World's
// collider, which exists while the scene simulates). What it shows is read from the runtime as it is
// now, so it works in Play; TerrainOverlay drapes the image over the world as a sheet.

#include "math/Vector4.h"
#include "terrain/TerrainOverlay.h"
#include "terrain/world/TileStreamer.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {
    class TerrainWorldRuntime;
}

namespace jasperengine::editor {

    enum class TerrainWorldOverlayMode { TileBorders, TileState, LodLevel, CollisionChunks };

    // visualize_terrain's names, in the order of the modes
    const std::vector<std::string>& terrainWorldOverlaySources();
    bool terrainWorldOverlayMode(const std::string& source, TerrainWorldOverlayMode& out);
    // The Scene view's (Unity-like) name of a source: "Tile Borders", ...; empty for no World overlay
    const char* terrainWorldOverlayLabel(const std::string& source);

    // R41's colour of a tile state, the panel's minimap and the overlay alike
    Vector4 terrainWorldStateColour(terrain::TileState state, bool pinned);

    // What one bake shows: the box of tiles drawn, each tile's state, and the rectangles of a mode
    // (LOD: drawn nodes, value the level, overview when drawn from it; collision: value 1 for coarse)
    struct TerrainWorldOverlayState {
        TerrainWorldOverlayMode mode = TerrainWorldOverlayMode::TileBorders;
        terrain::TileCoord low, high;
        float tileSize = 1.0f;
        std::vector<uint8_t> states;    // per tile of the box, row by row (TileState)
        std::vector<uint8_t> pinned;
        struct Rect {
            float minX = 0.0f, minZ = 0.0f, size = 0.0f;
            int value = 0;
            bool overview = false;
        };
        std::vector<Rect> rects;
        bool valid = false;

        int columns() const { return high.x - low.x + 1; }
        int rows() const { return high.z - low.z + 1; }
    };

    // The runtime's state for a mode: the box of present tiles and a ring round it
    TerrainWorldOverlayState captureTerrainWorldOverlay(const TerrainWorldRuntime& runtime, TerrainWorldOverlayMode mode);
    // Changes when what the image shows changes
    uint64_t terrainWorldOverlaySignature(const TerrainWorldOverlayState& state);
    // The image's size: texels per tile so the box fits TerrainOverlay::MAX_IMAGE
    int terrainWorldOverlayTexelsPerTile(const TerrainWorldOverlayState& state);
    // RGBA8 of columns x rows tiles at that many texels each; row y at local z = low.z * S + (y + 0.5) * S / k
    void bakeTerrainWorldOverlay(const TerrainWorldOverlayState& state, std::vector<uint8_t>& rgba, int& width, int& height);
    std::vector<TerrainOverlayLegendStop> terrainWorldOverlayLegend(const TerrainWorldOverlayState& state);

}
