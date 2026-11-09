// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrainworld/TerrainWorldOverlay.h"

#include "physics/TerrainWorldCollision.h"
#include "subsystem/TerrainWorldRuntime.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terraincore;

namespace {

    const Vector4 WORLD_BORDER_COLOUR(1.0f, 0.85f, 0.2f, 1.0f);
    const Vector4 WORLD_FINE_COLOUR(0.25f, 0.85f, 0.35f, 0.45f);
    const Vector4 WORLD_COARSE_COLOUR(0.95f, 0.55f, 0.15f, 0.5f);
    const Vector4 WORLD_OVERVIEW_COLOUR(0.55f, 0.55f, 0.6f, 0.45f);
    const Vector4 WORLD_LEVEL_COLOURS[] = {Vector4(0.9f, 0.25f, 0.25f, 0.5f), Vector4(0.95f, 0.6f, 0.15f, 0.5f), Vector4(0.95f, 0.9f, 0.2f, 0.5f),
                                           Vector4(0.35f, 0.85f, 0.3f, 0.5f), Vector4(0.2f, 0.8f, 0.85f, 0.5f), Vector4(0.3f, 0.45f, 0.95f, 0.5f),
                                           Vector4(0.7f, 0.35f, 0.9f, 0.5f), Vector4(0.9f, 0.4f, 0.7f, 0.5f)};

    uint8_t worldOverlayByte(float value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    void worldOverlayPut(std::vector<uint8_t>& rgba, int width, int x, int y, const Vector4& colour) {
        const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
        rgba[at + 0] = worldOverlayByte(colour.x);
        rgba[at + 1] = worldOverlayByte(colour.y);
        rgba[at + 2] = worldOverlayByte(colour.z);
        rgba[at + 3] = worldOverlayByte(colour.w);
    }

    Vector4 worldOverlayOpaque(const Vector4& colour) {
        return Vector4(colour.x, colour.y, colour.z, 1.0f);
    }

    Vector4 levelColour(const TerrainWorldOverlayState::Rect& rect) {
        return rect.overview ? WORLD_OVERVIEW_COLOUR : WORLD_LEVEL_COLOURS[static_cast<size_t>(std::max(0, rect.value)) % 8];
    }

}

const std::vector<std::string>& jasperengine::editor::terrainWorldOverlaySources() {
    static const std::vector<std::string> sources = {"tiles", "tile_state", "lod", "collision"};
    return sources;
}

bool jasperengine::editor::terrainWorldOverlayMode(const std::string& source, TerrainWorldOverlayMode& out) {
    const std::vector<std::string>& sources = terrainWorldOverlaySources();
    for (size_t i = 0; i < sources.size(); i++) {
        if (sources[i] != source) continue;
        out = static_cast<TerrainWorldOverlayMode>(i);
        return true;
    }
    return false;
}

const char* jasperengine::editor::terrainWorldOverlayLabel(const std::string& source) {
    TerrainWorldOverlayMode mode;
    if (!terrainWorldOverlayMode(source, mode)) return "";
    switch (mode) {
        case TerrainWorldOverlayMode::TileBorders: return "Tile Borders";
        case TerrainWorldOverlayMode::TileState: return "Tile State";
        case TerrainWorldOverlayMode::LodLevel: return "LOD Level";
        case TerrainWorldOverlayMode::CollisionChunks: return "Collision Chunks";
    }
    return "";
}

Vector4 jasperengine::editor::terrainWorldStateColour(TileState state, bool pinned) {
    if (pinned && state != TileState::Absent) return Vector4(0.9f, 0.7f, 0.24f, 1.0f);
    switch (state) {
        case TileState::Absent: return Vector4(0.16f, 0.16f, 0.16f, 1.0f);
        case TileState::Cold: return Vector4(0.27f, 0.35f, 0.47f, 1.0f);
        case TileState::Queued:
        case TileState::Loading: return Vector4(0.47f, 0.47f, 0.78f, 1.0f);
        case TileState::CpuReady:
        case TileState::Uploading: return Vector4(0.35f, 0.63f, 0.78f, 1.0f);
        case TileState::Resident: return Vector4(0.31f, 0.75f, 0.43f, 1.0f);
        case TileState::Error: return Vector4(0.82f, 0.27f, 0.27f, 1.0f);
    }
    return Vector4(0.16f, 0.16f, 0.16f, 1.0f);
}

TerrainWorldOverlayState jasperengine::editor::captureTerrainWorldOverlay(const TerrainWorldRuntime& runtime, TerrainWorldOverlayMode mode) {
    TerrainWorldOverlayState state;
    state.mode = mode;
    if (!runtime.started()) return state;
    TileCoord low, high;
    if (!runtime.occupancy().bounds(low, high)) return state;
    // the box of the tiles: a sheet past them would stand over nothing, in front of the Scene view's grid
    state.low = low;
    state.high = high;
    state.tileSize = runtime.lattice().tileSize();
    for (int z = state.low.z; z <= state.high.z; z++) {
        for (int x = state.low.x; x <= state.high.x; x++) {
            const TileCoord tile(x, z);
            const TileState tileState = runtime.tileState(tile);
            state.states.push_back(static_cast<uint8_t>(tileState));
            state.pinned.push_back(tileState != TileState::Absent && runtime.tiles().pinned(tile) ? 1 : 0);
        }
    }
    if (mode == TerrainWorldOverlayMode::LodLevel) {
        for (const TerrainWorldDrawnNode& node : runtime.drawnNodes()) {
            state.rects.push_back(TerrainWorldOverlayState::Rect{node.x - node.size * 0.5f, node.z - node.size * 0.5f, node.size, node.level, node.overview});
        }
    } else if (mode == TerrainWorldOverlayMode::CollisionChunks && runtime.collision) {
        for (const TerrainWorldColliderRect& collider : runtime.collision->colliders()) {
            state.rects.push_back(TerrainWorldOverlayState::Rect{collider.minX, collider.minZ, collider.size, collider.coarse ? 1 : 0, false});
        }
    }
    state.valid = true;
    return state;
}

uint64_t jasperengine::editor::terrainWorldOverlaySignature(const TerrainWorldOverlayState& state) {
    // FNV-1a over everything drawn
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    const int head[6] = {static_cast<int>(state.mode), state.low.x, state.low.z, state.high.x, state.high.z, state.valid ? 1 : 0};
    mix(head, sizeof(head));
    mix(&state.tileSize, sizeof(state.tileSize));
    mix(state.states.data(), state.states.size());
    mix(state.pinned.data(), state.pinned.size());
    for (const TerrainWorldOverlayState::Rect& rect : state.rects) {
        const float numbers[3] = {rect.minX, rect.minZ, rect.size};
        const int flags[2] = {rect.value, rect.overview ? 1 : 0};
        mix(numbers, sizeof(numbers));
        mix(flags, sizeof(flags));
    }
    return hash;
}

int jasperengine::editor::terrainWorldOverlayTexelsPerTile(const TerrainWorldOverlayState& state) {
    const int side = std::max(1, std::max(state.columns(), state.rows()));
    return std::clamp(TerrainOverlay::MAX_IMAGE / side, 4, 64);
}

void jasperengine::editor::bakeTerrainWorldOverlay(const TerrainWorldOverlayState& state, std::vector<uint8_t>& rgba, int& width, int& height) {
    const int k = terrainWorldOverlayTexelsPerTile(state);
    width = std::max(1, state.columns()) * k;
    height = std::max(1, state.rows()) * k;
    rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    if (!state.valid) return;
    const int columns = state.columns();
    auto stateAt = [&](int column, int row) { return static_cast<TileState>(state.states[static_cast<size_t>(row) * static_cast<size_t>(columns) + static_cast<size_t>(column)]); };

    if (state.mode == TerrainWorldOverlayMode::TileBorders || state.mode == TerrainWorldOverlayMode::TileState) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const int column = x / k, row = y / k;
                const TileState tileState = stateAt(column, row);
                if (tileState == TileState::Absent) continue;
                const bool edge = x % k == 0 || x % k == k - 1 || y % k == 0 || y % k == k - 1;
                if (state.mode == TerrainWorldOverlayMode::TileBorders) {
                    if (edge) worldOverlayPut(rgba, width, x, y, WORLD_BORDER_COLOUR);
                    continue;
                }
                Vector4 colour = terrainWorldStateColour(tileState, state.pinned[static_cast<size_t>(row) * static_cast<size_t>(columns) + static_cast<size_t>(column)] != 0);
                colour.w = edge ? 1.0f : 0.55f;
                worldOverlayPut(rgba, width, x, y, colour);
            }
        }
        return;
    }

    // LOD Level and Collision Chunks: each rectangle filled, its edge drawn solid
    const float texel = state.tileSize / static_cast<float>(k);
    const float originX = static_cast<float>(state.low.x) * state.tileSize, originZ = static_cast<float>(state.low.z) * state.tileSize;
    for (const TerrainWorldOverlayState::Rect& rect : state.rects) {
        const int x0 = std::max(0, static_cast<int>(std::lround((rect.minX - originX) / texel)));
        const int z0 = std::max(0, static_cast<int>(std::lround((rect.minZ - originZ) / texel)));
        const int x1 = std::min(width, static_cast<int>(std::lround((rect.minX + rect.size - originX) / texel))) - 1;
        const int z1 = std::min(height, static_cast<int>(std::lround((rect.minZ + rect.size - originZ) / texel))) - 1;
        const Vector4 fill = state.mode == TerrainWorldOverlayMode::LodLevel ? levelColour(rect) : (rect.value ? WORLD_COARSE_COLOUR : WORLD_FINE_COLOUR);
        for (int y = z0; y <= z1; y++) {
            for (int x = x0; x <= x1; x++) {
                const bool edge = x == x0 || x == x1 || y == z0 || y == z1;
                worldOverlayPut(rgba, width, x, y, edge ? worldOverlayOpaque(fill) : fill);
            }
        }
    }
}

std::vector<TerrainOverlayLegendStop> jasperengine::editor::terrainWorldOverlayLegend(const TerrainWorldOverlayState& state) {
    std::vector<TerrainOverlayLegendStop> stops;
    switch (state.mode) {
        case TerrainWorldOverlayMode::TileBorders:
            stops.push_back({"tile border", WORLD_BORDER_COLOUR});
            break;
        case TerrainWorldOverlayMode::TileState:
            for (TileState tileState : {TileState::Cold, TileState::Loading, TileState::CpuReady, TileState::Resident, TileState::Error}) {
                stops.push_back({tileStateName(tileState), terrainWorldStateColour(tileState, false)});
            }
            stops.push_back({"pinned", terrainWorldStateColour(TileState::Resident, true)});
            break;
        case TerrainWorldOverlayMode::LodLevel: {
            std::set<int> levels;
            bool overview = false;
            for (const TerrainWorldOverlayState::Rect& rect : state.rects) {
                if (rect.overview) overview = true;
                else levels.insert(rect.value);
            }
            for (int level : levels) stops.push_back({"level " + std::to_string(level), worldOverlayOpaque(WORLD_LEVEL_COLOURS[static_cast<size_t>(level) % 8])});
            if (overview) stops.push_back({"overview", worldOverlayOpaque(WORLD_OVERVIEW_COLOUR)});
            if (stops.empty()) stops.push_back({"nothing drawn yet", worldOverlayOpaque(WORLD_OVERVIEW_COLOUR)});
            break;
        }
        case TerrainWorldOverlayMode::CollisionChunks:
            if (state.rects.empty()) {
                // R20: the collider is built while the scene simulates
                stops.push_back({"no colliders: they are built in Play", Vector4(0.5f, 0.5f, 0.5f, 1.0f)});
                break;
            }
            stops.push_back({"fine chunk", worldOverlayOpaque(WORLD_FINE_COLOUR)});
            stops.push_back({"coarse tile", worldOverlayOpaque(WORLD_COARSE_COLOUR)});
            break;
    }
    return stops;
}
