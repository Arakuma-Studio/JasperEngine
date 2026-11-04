// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/OverlayMapStore.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // floor(value / TILE_SIZE) for a signed texel
    int tileOf(int value) {
        return value >= 0 ? value / OverlayMapStore::TILE_SIZE : -((-value + OverlayMapStore::TILE_SIZE - 1) / OverlayMapStore::TILE_SIZE);
    }

    TexelRect tileRect(int tileX, int tileY, const TexelRect& bounds) {
        return intersect(TexelRect{tileX * OverlayMapStore::TILE_SIZE, tileY * OverlayMapStore::TILE_SIZE,
                                   OverlayMapStore::TILE_SIZE, OverlayMapStore::TILE_SIZE}, bounds);
    }

    int kindOrder(MapId map) {
        return static_cast<int>(map.kind);
    }

}

OverlayMapStore::OverlayMapStore(TerrainMapStore& base): baseStore(&base) {}

OverlayMapStore::MapState* OverlayMapStore::findState(MapId map) {
    for (std::unique_ptr<MapState>& entry : maps) {
        if (entry->id == map) return entry.get();
    }
    return nullptr;
}

const OverlayMapStore::MapState* OverlayMapStore::findState(MapId map) const {
    for (const std::unique_ptr<MapState>& entry : maps) {
        if (entry->id == map) return entry.get();
    }
    return nullptr;
}

OverlayMapStore::MapState& OverlayMapStore::state(MapId map) {
    if (MapState* existing = findState(map)) return *existing;
    auto created = std::make_unique<MapState>();
    created->id = map;
    MapState& reference = *created;
    // kept sorted, so changes() and commit() walk the maps in one order
    auto position = std::lower_bound(maps.begin(), maps.end(), map, [](const std::unique_ptr<MapState>& entry, MapId value) {
        if (kindOrder(entry->id) != kindOrder(value)) return kindOrder(entry->id) < kindOrder(value);
        return entry->id.index < value.index;
    });
    maps.insert(position, std::move(created));
    return reference;
}

MapFormat OverlayMapStore::currentFormat(MapId map) const {
    if (const MapState* existing = findState(map)) {
        if (existing->removed) return MapFormat{};
        if (existing->created) return existing->format;
    }
    return baseStore->format(map);
}

void OverlayMapStore::fillCreatedBytes(const MapState& mapState, TexelRect rect, std::span<uint8_t> out) const {
    const MapFormat& format = mapState.format;
    const int stride = bytesPerTexel(format);
    const int step = format.bitsPerChannel >= 16 ? 2 : 1;
    const uint32_t quantized = quantizeChannel(mapState.fill, format.bitsPerChannel);
    const uint32_t opaque = format.bitsPerChannel >= 16 ? 65535u : 255u;
    const size_t texels = static_cast<size_t>(rect.area());
    for (size_t i = 0; i < texels; i++) {
        uint8_t* stored = out.data() + i * stride;
        for (int c = 0; c < format.channels; c++) {
            encodeChannel(stored + c * step, c == 3 ? opaque : quantized, format.bitsPerChannel);
        }
    }
}

OverlayMapStore::Tile* OverlayMapStore::ensureTile(MapState& mapState, int tileX, int tileY, const MapFormat& format) {
    auto found = mapState.tiles.find({tileX, tileY});
    if (found != mapState.tiles.end()) return &found->second;

    const TexelRect rect = tileRect(tileX, tileY, format.bounds);
    if (rect.empty()) return nullptr;

    Tile tile;
    tile.rect = rect;
    const int count = floatChannels(mapState.id, format);
    tile.values.resize(static_cast<size_t>(rect.area()) * count);
    tile.before.resize(static_cast<size_t>(rect.area()) * bytesPerTexel(format));
    if (mapState.created) {
        fillCreatedBytes(mapState, rect, tile.before);
    } else if (!baseStore->readBytes(mapState.id, rect, tile.before)) {
        return nullptr;
    }
    const int stride = bytesPerTexel(format);
    const size_t texels = static_cast<size_t>(rect.area());
    for (size_t i = 0; i < texels; i++) {
        decodeTexel(mapState.id, format, tile.before.data() + i * stride, tile.values.data() + i * count);
    }
    return &mapState.tiles.emplace(std::make_pair(tileX, tileY), std::move(tile)).first->second;
}

TerrainFrame OverlayMapStore::frame() const {
    return baseStore->frame();
}

MapFormat OverlayMapStore::format(MapId map) const {
    return currentFormat(map);
}

bool OverlayMapStore::read(MapId map, TexelRect rect, std::span<float> out) const {
    const MapFormat format = currentFormat(map);
    if (!format.exists || !format.bounds.contains(rect)) return false;
    const int count = floatChannels(map, format);
    if (out.size() < static_cast<size_t>(rect.area()) * count) return false;

    const MapState* mapState = findState(map);
    if (mapState && mapState->created) {
        float colour = detClamp01(mapState->fill);
        const size_t texels = static_cast<size_t>(rect.area());
        for (size_t i = 0; i < texels; i++) {
            for (int c = 0; c < count; c++) {
                out[i * count + c] = c == 3 ? 1.0f : colour;
            }
        }
    } else if (!baseStore->read(map, rect, out)) {
        return false;
    }
    if (!mapState || mapState->tiles.empty() || rect.empty()) return true;

    const int firstX = tileOf(rect.x), lastX = tileOf(rect.right() - 1);
    const int firstY = tileOf(rect.y), lastY = tileOf(rect.bottom() - 1);
    for (int tileY = firstY; tileY <= lastY; tileY++) {
        for (int tileX = firstX; tileX <= lastX; tileX++) {
            auto found = mapState->tiles.find({tileX, tileY});
            if (found == mapState->tiles.end()) continue;
            const Tile& tile = found->second;
            const TexelRect part = intersect(tile.rect, rect);
            for (int y = part.y; y < part.bottom(); y++) {
                const float* source = tile.values.data() + (static_cast<size_t>(y - tile.rect.y) * tile.rect.w + (part.x - tile.rect.x)) * count;
                float* target = out.data() + (static_cast<size_t>(y - rect.y) * rect.w + (part.x - rect.x)) * count;
                std::memcpy(target, source, static_cast<size_t>(part.w) * count * sizeof(float));
            }
        }
    }
    return true;
}

WriteResult OverlayMapStore::write(MapId map, TexelRect rect, std::span<const float> in) {
    const MapFormat format = currentFormat(map);
    if (!format.exists) return WriteResult{false, "map does not exist", 0};
    if (!format.bounds.contains(rect)) return WriteResult{false, "rectangle outside the map", 0};
    const int count = floatChannels(map, format);
    if (in.size() < static_cast<size_t>(rect.area()) * count) return WriteResult{false, "too few values", 0};
    if (rect.empty()) return WriteResult{true, "", 0};

    MapState* mapState = findState(map);
    const int firstX = tileOf(rect.x), lastX = tileOf(rect.right() - 1);
    const int firstY = tileOf(rect.y), lastY = tileOf(rect.bottom() - 1);
    for (int tileY = firstY; tileY <= lastY; tileY++) {
        for (int tileX = firstX; tileX <= lastX; tileX++) {
            const TexelRect part = intersect(tileRect(tileX, tileY, format.bounds), rect);
            if (part.empty()) continue;

            Tile* tile = nullptr;
            if (mapState) {
                auto found = mapState->tiles.find({tileX, tileY});
                if (found != mapState->tiles.end()) tile = &found->second;
            }
            if (!tile) {
                // copy-on-first-touch: a tile whose values do not change is never made, so an
                // edit over a large rectangle holds only what it really changed
                scratch.resize(static_cast<size_t>(part.area()) * count);
                bool same = false;
                if (mapState && mapState->created) {
                    same = false;
                } else if (baseStore->read(map, part, scratch)) {
                    same = true;
                    for (int y = part.y; y < part.bottom() && same; y++) {
                        const float* source = in.data() + (static_cast<size_t>(y - rect.y) * rect.w + (part.x - rect.x)) * count;
                        const float* current = scratch.data() + static_cast<size_t>(y - part.y) * part.w * count;
                        for (int i = 0; i < part.w * count; i++) {
                            if (detClamp01(source[i]) != current[i]) {
                                same = false;
                                break;
                            }
                        }
                    }
                }
                if (same) continue;
                if (!mapState) mapState = &state(map);
                tile = ensureTile(*mapState, tileX, tileY, format);
                if (!tile) return WriteResult{false, "could not read the base map", 0};
            }

            for (int y = part.y; y < part.bottom(); y++) {
                const float* source = in.data() + (static_cast<size_t>(y - rect.y) * rect.w + (part.x - rect.x)) * count;
                float* target = tile->values.data() + (static_cast<size_t>(y - tile->rect.y) * tile->rect.w + (part.x - tile->rect.x)) * count;
                for (int i = 0; i < part.w * count; i++) {
                    target[i] = detClamp01(source[i]);
                }
            }
            mapState->dirty = unite(mapState->dirty, part);
            mapState->unflushed = unite(mapState->unflushed, part);
        }
    }
    return WriteResult{true, "", 0};
}

bool OverlayMapStore::tileAfterBytes(const MapState& mapState, const Tile& tile, std::vector<uint8_t>& after) const {
    const MapFormat& format = mapState.created ? mapState.format : baseStore->format(mapState.id);
    after = tile.before;
    const int count = floatChannels(mapState.id, format);
    const int stride = bytesPerTexel(format);
    const size_t texels = static_cast<size_t>(tile.rect.area());
    bool changed = false;
    for (size_t i = 0; i < texels; i++) {
        changed |= encodeTexel(mapState.id, format, tile.values.data() + i * count, after.data() + i * stride);
    }
    return changed;
}

bool OverlayMapStore::readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const {
    const MapFormat format = currentFormat(map);
    if (!format.exists || !format.bounds.contains(rect)) return false;
    const int stride = bytesPerTexel(format);
    if (out.size() < static_cast<size_t>(rect.area()) * stride) return false;

    const MapState* mapState = findState(map);
    if (mapState && mapState->created) {
        fillCreatedBytes(*mapState, rect, out);
    } else if (!baseStore->readBytes(map, rect, out)) {
        return false;
    }
    if (!mapState || mapState->tiles.empty()) return true;

    std::vector<uint8_t> after;
    for (const auto& [key, tile] : mapState->tiles) {
        const TexelRect part = intersect(tile.rect, rect);
        if (part.empty()) continue;
        tileAfterBytes(*mapState, tile, after);
        for (int y = part.y; y < part.bottom(); y++) {
            const uint8_t* source = after.data() + (static_cast<size_t>(y - tile.rect.y) * tile.rect.w + (part.x - tile.rect.x)) * stride;
            uint8_t* target = out.data() + (static_cast<size_t>(y - rect.y) * rect.w + (part.x - rect.x)) * stride;
            std::memcpy(target, source, static_cast<size_t>(part.w) * stride);
        }
    }
    return true;
}

WriteResult OverlayMapStore::writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) {
    const MapFormat format = currentFormat(map);
    if (!format.exists) return WriteResult{false, "map does not exist", 0};
    if (!format.bounds.contains(rect)) return WriteResult{false, "rectangle outside the map", 0};
    const int stride = bytesPerTexel(format);
    const size_t texels = static_cast<size_t>(rect.area());
    if (in.size() < texels * stride) return WriteResult{false, "too few bytes", 0};
    const int count = floatChannels(map, format);
    std::vector<float> values(texels * count);
    for (size_t i = 0; i < texels; i++) {
        decodeTexel(map, format, in.data() + i * stride, values.data() + i * count);
    }
    return write(map, rect, values);
}

bool OverlayMapStore::canWrite(MapId map, TexelRect rect, std::string* why) const {
    const MapState* mapState = findState(map);
    if (mapState && mapState->created) {
        if (mapState->format.bounds.contains(rect)) return true;
        if (why) *why = "rectangle outside the map";
        return false;
    }
    return baseStore->canWrite(map, rect, why);
}

WriteResult OverlayMapStore::createMap(MapId map, MapFormat format, float fill) {
    if (currentFormat(map).exists) return WriteResult{false, "map already exists", 0};
    if (format.bounds.empty() || format.channels <= 0) return WriteResult{false, "empty map", 0};
    MapState& mapState = state(map);
    if (mapState.removed) {
        // removed and made again in one transaction: the base keeps its map, and the new contents
        // are an ordinary write over it
        const MapFormat existing = baseStore->format(map);
        if (!(existing.bounds == format.bounds) || existing.channels != format.channels || existing.bitsPerChannel != format.bitsPerChannel) {
            return WriteResult{false, "a removed map can only come back in its own format", 0};
        }
        mapState.removed = false;
        const int count = floatChannels(map, format);
        std::vector<float> values(static_cast<size_t>(format.bounds.area()) * count, detClamp01(fill));
        for (size_t i = 3; count == 4 && i < values.size(); i += 4) {
            values[i] = 1.0f;
        }
        return write(map, format.bounds, values);
    }
    mapState.tiles.clear();
    mapState.format = format;
    mapState.format.exists = true;
    mapState.created = true;
    mapState.fill = fill;
    mapState.dirty = format.bounds;
    return WriteResult{true, "", 0};
}

bool OverlayMapStore::canCreateMap(MapId map, std::string* why) const {
    return baseStore->canCreateMap(map, why);
}

bool OverlayMapStore::canRemoveMap(MapId map) const {
    const MapState* mapState = findState(map);
    if (mapState && mapState->removed) return false;
    if (mapState && mapState->created) return true;
    return baseStore->canRemoveMap(map);
}

WriteResult OverlayMapStore::removeMap(MapId map) {
    if (!canRemoveMap(map)) return WriteResult{false, "this store keeps the map", 0};
    MapState& mapState = state(map);
    if (mapState.created) {
        // made and removed in one transaction: the base never hears of it
        for (size_t i = 0; i < maps.size(); i++) {
            if (maps[i]->id == map) {
                maps.erase(maps.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        return WriteResult{true, "", 0};
    }
    mapState.removed = true;
    return WriteResult{true, "", 0};
}

void OverlayMapStore::takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) {
    for (std::unique_ptr<MapState>& mapState : maps) {
        if (!mapState->dirty.empty()) {
            out.emplace_back(mapState->id, mapState->dirty);
            mapState->dirty = TexelRect{};
        }
    }
}

Vector2 OverlayMapStore::worldOfTexel(MapId map, int x, int y) const {
    const MapState* mapState = findState(map);
    if (mapState && mapState->created) {
        // the lattice of a map the base has not made yet: any map of its size over the same square.
        // Height is the reference; a size of its own scales from it.
        const MapFormat height = baseStore->format(MapId{MapKind::Height, 0});
        if (height.exists && height.bounds.w > 0 && height.bounds.h > 0) {
            const float scaleX = static_cast<float>(height.bounds.w) / static_cast<float>(mapState->format.bounds.w);
            const float scaleY = static_cast<float>(height.bounds.h) / static_cast<float>(mapState->format.bounds.h);
            const Vector2 origin = baseStore->worldOfTexel(MapId{MapKind::Height, 0}, height.bounds.x, height.bounds.y);
            const Vector2 next = baseStore->worldOfTexel(MapId{MapKind::Height, 0}, height.bounds.x + 1, height.bounds.y + 1);
            const float stepX = (next.x - origin.x) * scaleX;
            const float stepY = (next.y - origin.y) * scaleY;
            const float cornerX = origin.x - (next.x - origin.x) * 0.5f;
            const float cornerY = origin.y - (next.y - origin.y) * 0.5f;
            return Vector2(cornerX + (static_cast<float>(x - mapState->format.bounds.x) + 0.5f) * stepX,
                           cornerY + (static_cast<float>(y - mapState->format.bounds.y) + 0.5f) * stepY);
        }
    }
    return baseStore->worldOfTexel(map, x, y);
}

Vector2 OverlayMapStore::texelOfWorld(MapId map, float localX, float localZ) const {
    const MapState* mapState = findState(map);
    if (mapState && mapState->created) {
        const Vector2 origin = worldOfTexel(map, mapState->format.bounds.x, mapState->format.bounds.y);
        const Vector2 next = worldOfTexel(map, mapState->format.bounds.x + 1, mapState->format.bounds.y + 1);
        const float stepX = next.x - origin.x;
        const float stepY = next.y - origin.y;
        return Vector2(static_cast<float>(mapState->format.bounds.x) + (stepX != 0.0f ? (localX - origin.x) / stepX : 0.0f),
                       static_cast<float>(mapState->format.bounds.y) + (stepY != 0.0f ? (localZ - origin.y) / stepY : 0.0f));
    }
    return baseStore->texelOfWorld(map, localX, localZ);
}

TexelRect OverlayMapStore::preferredBlock(MapId map) const {
    const MapState* mapState = findState(map);
    if (mapState && mapState->created) return mapState->format.bounds;
    return baseStore->preferredBlock(map);
}

std::unique_ptr<TerrainLargeEdit> OverlayMapStore::beginLarge(MapId, TexelRect) {
    return nullptr;
}

std::unique_ptr<TerrainStoreUndoHook> OverlayMapStore::takeUndoHook() {
    return baseStore->takeUndoHook();
}

std::vector<OverlayMapStore::MapChange> OverlayMapStore::changes() const {
    std::vector<MapChange> result;
    std::vector<uint8_t> after;
    for (const std::unique_ptr<MapState>& mapState : maps) {
        const MapFormat format = mapState->created ? mapState->format : baseStore->format(mapState->id);
        const int stride = bytesPerTexel(format);
        MapChange change;
        change.map = mapState->id;
        for (const auto& [key, tile] : mapState->tiles) {
            if (!tileAfterBytes(*mapState, tile, after)) continue;
            for (int y = 0; y < tile.rect.h; y++) {
                for (int x = 0; x < tile.rect.w; x++) {
                    const size_t offset = (static_cast<size_t>(y) * tile.rect.w + x) * stride;
                    if (std::memcmp(after.data() + offset, tile.before.data() + offset, stride) != 0) {
                        change.rect = unite(change.rect, TexelRect{tile.rect.x + x, tile.rect.y + y, 1, 1});
                        change.texels++;
                    }
                }
            }
        }
        if (change.texels > 0) result.push_back(change);
    }
    return result;
}

std::vector<MapId> OverlayMapStore::createdMaps() const {
    std::vector<MapId> result;
    for (const std::unique_ptr<MapState>& mapState : maps) {
        if (mapState->created) result.push_back(mapState->id);
    }
    return result;
}

std::vector<MapId> OverlayMapStore::removedMaps() const {
    std::vector<MapId> result;
    for (const std::unique_ptr<MapState>& mapState : maps) {
        if (mapState->removed) result.push_back(mapState->id);
    }
    return result;
}

WriteResult OverlayMapStore::commit() {
    // Everything that can refuse answers before the first byte moves
    std::vector<uint8_t> after;
    for (const std::unique_ptr<MapState>& mapState : maps) {
        if (mapState->created) continue;
        for (const auto& [key, tile] : mapState->tiles) {
            if (!tileAfterBytes(*mapState, tile, after)) continue;
            std::string why;
            if (!baseStore->canWrite(mapState->id, tile.rect, &why)) {
                return WriteResult{false, why.empty() ? "the store refused the write" : why, 0};
            }
        }
    }

    struct Written {
        MapId map;
        TexelRect rect;
        const std::vector<uint8_t>* before;
    };
    std::vector<Written> written;
    std::vector<MapId> madeMaps;
    int tilesCreated = 0;

    auto rollback = [&]() {
        for (auto it = written.rbegin(); it != written.rend(); ++it) {
            baseStore->writeBytes(it->map, it->rect, *it->before);
        }
        for (const MapId& map : madeMaps) {
            baseStore->removeMap(map);
        }
    };

    for (const std::unique_ptr<MapState>& mapState : maps) {
        if (mapState->created) {
            WriteResult made = baseStore->createMap(mapState->id, mapState->format, mapState->fill);
            if (!made.ok) {
                rollback();
                return made;
            }
            tilesCreated += made.tilesCreated;
            madeMaps.push_back(mapState->id);
        }
        for (const auto& [key, tile] : mapState->tiles) {
            if (!tileAfterBytes(*mapState, tile, after)) continue;
            WriteResult wrote = baseStore->writeBytes(mapState->id, tile.rect, after);
            if (!wrote.ok) {
                rollback();
                return wrote;
            }
            tilesCreated += wrote.tilesCreated;
            written.push_back(Written{mapState->id, tile.rect, &tile.before});
        }
    }
    for (const std::unique_ptr<MapState>& mapState : maps) {
        if (mapState->removed) {
            WriteResult removed = baseStore->removeMap(mapState->id);
            if (!removed.ok) {
                rollback();
                return removed;
            }
        }
    }

    clearAll();
    return WriteResult{true, "", tilesCreated};
}

void OverlayMapStore::discard() {
    clearAll();
}

WriteResult OverlayMapStore::flushToBase() {
    std::vector<uint8_t> after;
    for (const std::unique_ptr<MapState>& mapState : maps) {
        if (mapState->created || mapState->removed || mapState->unflushed.empty()) continue;
        const TexelRect unflushed = mapState->unflushed;
        mapState->unflushed = TexelRect{};
        for (auto& [key, tile] : mapState->tiles) {
            if (intersect(tile.rect, unflushed).empty()) continue;
            tileAfterBytes(*mapState, tile, after);
            const WriteResult wrote = baseStore->writeBytes(mapState->id, tile.rect, after);
            if (!wrote.ok) return wrote;
            tile.shown = true;
        }
    }
    return WriteResult{true, "", 0};
}

WriteResult OverlayMapStore::rewind() {
    for (const std::unique_ptr<MapState>& mapState : maps) {
        const MapFormat format = mapState->created ? mapState->format : baseStore->format(mapState->id);
        const int count = floatChannels(mapState->id, format);
        const int stride = bytesPerTexel(format);
        for (auto& [key, tile] : mapState->tiles) {
            const size_t texels = static_cast<size_t>(tile.rect.area());
            for (size_t i = 0; i < texels; i++) {
                decodeTexel(mapState->id, format, tile.before.data() + i * stride, tile.values.data() + i * count);
            }
            if (!tile.shown) continue;
            const WriteResult wrote = baseStore->writeBytes(mapState->id, tile.rect, tile.before);
            if (!wrote.ok) return wrote;
            tile.shown = false;
        }
        mapState->unflushed = TexelRect{};
    }
    return WriteResult{true, "", 0};
}

bool OverlayMapStore::readBeforeBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const {
    const MapFormat format = currentFormat(map);
    if (!format.exists || !format.bounds.contains(rect)) return false;
    const int stride = bytesPerTexel(format);
    if (out.size() < static_cast<size_t>(rect.area()) * stride) return false;

    const MapState* mapState = findState(map);
    if (mapState && mapState->created) {
        fillCreatedBytes(*mapState, rect, out);
        return true;
    }
    if (!baseStore->readBytes(map, rect, out)) return false;
    if (!mapState || mapState->tiles.empty() || rect.empty()) return true;

    const int firstX = tileOf(rect.x), lastX = tileOf(rect.right() - 1);
    const int firstY = tileOf(rect.y), lastY = tileOf(rect.bottom() - 1);
    for (int tileY = firstY; tileY <= lastY; tileY++) {
        for (int tileX = firstX; tileX <= lastX; tileX++) {
            auto found = mapState->tiles.find({tileX, tileY});
            if (found == mapState->tiles.end()) continue;
            const Tile& tile = found->second;
            const TexelRect part = intersect(tile.rect, rect);
            for (int y = part.y; y < part.bottom(); y++) {
                std::memcpy(out.data() + (static_cast<size_t>(y - rect.y) * rect.w + (part.x - rect.x)) * stride,
                            tile.before.data() + (static_cast<size_t>(y - tile.rect.y) * tile.rect.w + (part.x - tile.rect.x)) * stride,
                            static_cast<size_t>(part.w) * stride);
            }
        }
    }
    return true;
}

void OverlayMapStore::clearAll() {
    maps.clear();
    scratch.clear();
    scratch.shrink_to_fit();
}

void OverlayMapStore::visitTiles(const TileVisitor& visitor) const {
    std::vector<uint8_t> after;
    for (const std::unique_ptr<MapState>& mapState : maps) {
        for (const auto& [key, tile] : mapState->tiles) {
            tileAfterBytes(*mapState, tile, after);
            visitor(mapState->id, tile.rect, tile.before, after);
        }
    }
}

size_t OverlayMapStore::tileCount() const {
    size_t count = 0;
    for (const std::unique_ptr<MapState>& mapState : maps) {
        count += mapState->tiles.size();
    }
    return count;
}

size_t OverlayMapStore::memoryBytes() const {
    size_t bytes = 0;
    for (const std::unique_ptr<MapState>& mapState : maps) {
        for (const auto& [key, tile] : mapState->tiles) {
            bytes += tile.values.size() * sizeof(float) + tile.before.size();
        }
    }
    return bytes;
}
