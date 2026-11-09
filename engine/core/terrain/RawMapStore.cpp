// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/RawMapStore.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::terrain;

RawMapStore::RawMapStore(float terrainSize, float maxHeight): size(terrainSize), maxHeight(maxHeight) {}

RawMapStore::Entry* RawMapStore::find(MapId map) {
    for (std::unique_ptr<Entry>& entry : entries) {
        if (entry->id == map) return entry.get();
    }
    return nullptr;
}

const RawMapStore::Entry* RawMapStore::find(MapId map) const {
    for (const std::unique_ptr<Entry>& entry : entries) {
        if (entry->id == map) return entry.get();
    }
    return nullptr;
}

size_t RawMapStore::offsetOf(const Entry& entry, int x, int y) const {
    return (static_cast<size_t>(y) * entry.format.bounds.w + x) * bytesPerTexel(entry.format);
}

void RawMapStore::addMap(MapId map, int width, int height, int channels, int bitsPerChannel, std::vector<uint8_t> bytes) {
    MapFormat format{TexelRect{0, 0, width, height}, channels, bitsPerChannel, true};
    const size_t count = static_cast<size_t>(format.bounds.area()) * bytesPerTexel(format);
    bytes.resize(count, 0);

    Entry* entry = find(map);
    if (!entry) {
        entries.push_back(std::make_unique<Entry>());
        entry = entries.back().get();
    }
    entry->id = map;
    entry->format = format;
    entry->owned = std::move(bytes);
    entry->pixels = entry->owned.data();
    entry->dirty = TexelRect{};
}

void RawMapStore::borrowMap(MapId map, int width, int height, int channels, int bitsPerChannel, uint8_t* pixels) {
    Entry* entry = find(map);
    if (!entry) {
        entries.push_back(std::make_unique<Entry>());
        entry = entries.back().get();
    }
    entry->id = map;
    const MapFormat format{TexelRect{0, 0, width, height}, channels, bitsPerChannel, true};
    // Lent again as it was: what it has written since the last take still counts
    if (entry->pixels != pixels || !(entry->format.bounds == format.bounds) || entry->format.channels != channels ||
        entry->format.bitsPerChannel != bitsPerChannel) {
        entry->dirty = TexelRect{};
    }
    entry->format = format;
    entry->owned.clear();
    entry->pixels = pixels;
}

const uint8_t* RawMapStore::mapBytes(MapId map) const {
    const Entry* entry = find(map);
    return entry ? entry->pixels : nullptr;
}

size_t RawMapStore::mapByteCount(MapId map) const {
    const Entry* entry = find(map);
    return entry ? static_cast<size_t>(entry->format.bounds.area()) * bytesPerTexel(entry->format) : 0;
}

TerrainFrame RawMapStore::frame() const {
    return TerrainFrame{maxHeight};
}

MapFormat RawMapStore::format(MapId map) const {
    const Entry* entry = find(map);
    return entry ? entry->format : MapFormat{};
}

bool RawMapStore::read(MapId map, TexelRect rect, std::span<float> out) const {
    const Entry* entry = find(map);
    if (!entry || !entry->format.bounds.contains(rect)) return false;
    const MapFormat& format = entry->format;
    const int count = floatChannels(map, format);
    if (out.size() < static_cast<size_t>(rect.area()) * count) return false;

    const int stride = bytesPerTexel(format);
    float* target = out.data();
    for (int y = rect.y; y < rect.bottom(); y++) {
        const uint8_t* stored = entry->pixels + offsetOf(*entry, rect.x, y);
        for (int x = 0; x < rect.w; x++) {
            decodeTexel(map, format, stored, target);
            stored += stride;
            target += count;
        }
    }
    return true;
}

WriteResult RawMapStore::write(MapId map, TexelRect rect, std::span<const float> in) {
    Entry* entry = find(map);
    if (!entry) return WriteResult{false, "map does not exist", 0};
    if (!entry->format.bounds.contains(rect)) return WriteResult{false, "rectangle outside the map", 0};
    const MapFormat& format = entry->format;
    const int count = floatChannels(map, format);
    if (in.size() < static_cast<size_t>(rect.area()) * count) return WriteResult{false, "too few values", 0};

    const int stride = bytesPerTexel(format);
    const float* source = in.data();
    for (int y = rect.y; y < rect.bottom(); y++) {
        uint8_t* stored = entry->pixels + offsetOf(*entry, rect.x, y);
        // the row's first and last changed texels: one dirty rectangle a row, not one a texel (T1 R48)
        int first = -1, last = -1;
        for (int x = rect.x; x < rect.right(); x++) {
            if (encodeTexel(map, format, source, stored)) {
                if (first < 0) first = x;
                last = x;
            }
            stored += stride;
            source += count;
        }
        if (first >= 0) entry->dirty = unite(entry->dirty, TexelRect{first, y, last - first + 1, 1});
    }
    return WriteResult{true, "", 0};
}

bool RawMapStore::readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const {
    const Entry* entry = find(map);
    if (!entry || !entry->format.bounds.contains(rect)) return false;
    const size_t rowBytes = static_cast<size_t>(rect.w) * bytesPerTexel(entry->format);
    if (out.size() < rowBytes * rect.h) return false;
    for (int y = 0; y < rect.h; y++) {
        std::memcpy(out.data() + rowBytes * y, entry->pixels + offsetOf(*entry, rect.x, rect.y + y), rowBytes);
    }
    return true;
}

WriteResult RawMapStore::writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) {
    Entry* entry = find(map);
    if (!entry) return WriteResult{false, "map does not exist", 0};
    if (!entry->format.bounds.contains(rect)) return WriteResult{false, "rectangle outside the map", 0};
    const int stride = bytesPerTexel(entry->format);
    const size_t rowBytes = static_cast<size_t>(rect.w) * stride;
    if (in.size() < rowBytes * rect.h) return WriteResult{false, "too few bytes", 0};
    for (int y = 0; y < rect.h; y++) {
        uint8_t* target = entry->pixels + offsetOf(*entry, rect.x, rect.y + y);
        const uint8_t* source = in.data() + rowBytes * y;
        if (std::memcmp(target, source, rowBytes) != 0) {
            int first = 0, last = rect.w - 1;
            while (std::memcmp(target + first * stride, source + first * stride, stride) == 0) first++;
            while (std::memcmp(target + last * stride, source + last * stride, stride) == 0) last--;
            entry->dirty = unite(entry->dirty, TexelRect{rect.x + first, rect.y + y, last - first + 1, 1});
        }
        std::memcpy(target, source, rowBytes);
    }
    return WriteResult{true, "", 0};
}

bool RawMapStore::canWrite(MapId map, TexelRect rect, std::string* why) const {
    const Entry* entry = find(map);
    if (!entry || !entry->format.bounds.contains(rect)) {
        if (why) *why = entry ? "rectangle outside the map" : "map does not exist";
        return false;
    }
    return true;
}

WriteResult RawMapStore::createMap(MapId map, MapFormat format, float fill) {
    if (find(map)) return WriteResult{false, "map already exists", 0};
    if (format.bounds.empty() || format.channels <= 0) return WriteResult{false, "empty map", 0};
    addMap(map, format.bounds.w, format.bounds.h, format.channels, format.bitsPerChannel);
    Entry* entry = find(map);
    entry->format.bounds.x = 0;
    entry->format.bounds.y = 0;
    const int stride = bytesPerTexel(entry->format);
    const int step = format.bitsPerChannel >= 16 ? 2 : 1;
    const uint32_t quantized = quantizeChannel(fill, format.bitsPerChannel);
    const size_t texels = static_cast<size_t>(entry->format.bounds.area());
    for (size_t i = 0; i < texels; i++) {
        uint8_t* stored = entry->pixels + i * stride;
        for (int c = 0; c < format.channels; c++) {
            encodeChannel(stored + c * step, c == 3 ? (format.bitsPerChannel >= 16 ? 65535u : 255u) : quantized, format.bitsPerChannel);
        }
    }
    entry->dirty = entry->format.bounds;
    return WriteResult{true, "", 0};
}

bool RawMapStore::canRemoveMap(MapId map) const {
    return find(map) != nullptr;
}

WriteResult RawMapStore::removeMap(MapId map) {
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i]->id == map) {
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
            return WriteResult{true, "", 0};
        }
    }
    return WriteResult{false, "map does not exist", 0};
}

void RawMapStore::takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) {
    for (std::unique_ptr<Entry>& entry : entries) {
        if (!entry->dirty.empty()) {
            out.emplace_back(entry->id, entry->dirty);
            entry->dirty = TexelRect{};
        }
    }
}

Vector2 RawMapStore::worldOfTexel(MapId map, int x, int y) const {
    const Entry* entry = find(map);
    const float width = entry ? static_cast<float>(entry->format.bounds.w) : 1.0f;
    const float height = entry ? static_cast<float>(entry->format.bounds.h) : 1.0f;
    return Vector2((static_cast<float>(x) + 0.5f) * size / width - size * 0.5f,
                   (static_cast<float>(y) + 0.5f) * size / height - size * 0.5f);
}

Vector2 RawMapStore::texelOfWorld(MapId map, float localX, float localZ) const {
    const Entry* entry = find(map);
    const float width = entry ? static_cast<float>(entry->format.bounds.w) : 1.0f;
    const float height = entry ? static_cast<float>(entry->format.bounds.h) : 1.0f;
    return Vector2((localX + size * 0.5f) / size * width - 0.5f, (localZ + size * 0.5f) / size * height - 0.5f);
}

TexelRect RawMapStore::preferredBlock(MapId map) const {
    const Entry* entry = find(map);
    return entry ? entry->format.bounds : TexelRect{};
}

std::unique_ptr<TerrainLargeEdit> RawMapStore::beginLarge(MapId, TexelRect) {
    return nullptr;
}

std::unique_ptr<TerrainStoreUndoHook> RawMapStore::takeUndoHook() {
    return nullptr;
}
