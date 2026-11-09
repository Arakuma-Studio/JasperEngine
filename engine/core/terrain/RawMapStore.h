// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RAW_MAP_STORE_H
#define RAW_MAP_STORE_H

// T1 R3: a store over plain buffers, owned or borrowed: tests, generation and the pure functions of
// the spline bake. Each map sits at texel 0 over a centred square of terrainSize.

#include "Export.h"
#include "terrain/TerrainMapStore.h"

#include <memory>
#include <vector>

namespace jasperengine::terraincore {

    class JASPERENGINE_API RawMapStore : public TerrainMapStore {
    public:
        RawMapStore(float terrainSize, float maxHeight);

        // A map over its own buffer, zeroed when no bytes are given
        void addMap(MapId map, int width, int height, int channels, int bitsPerChannel, std::vector<uint8_t> bytes = {});
        // A map over the caller's pixels, which must outlive the store
        void borrowMap(MapId map, int width, int height, int channels, int bitsPerChannel, uint8_t* pixels);
        // The stored bytes of a map, tightly packed; null when it does not exist
        const uint8_t* mapBytes(MapId map) const;
        size_t mapByteCount(MapId map) const;

        float terrainSize() const { return size; }
        void setFrame(float terrainSize, float newMaxHeight) { size = terrainSize; maxHeight = newMaxHeight; }

        TerrainFrame frame() const override;
        MapFormat format(MapId map) const override;
        bool read(MapId map, TexelRect rect, std::span<float> out) const override;
        WriteResult write(MapId map, TexelRect rect, std::span<const float> in) override;
        bool readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const override;
        WriteResult writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) override;
        bool canWrite(MapId map, TexelRect rect, std::string* why) const override;
        WriteResult createMap(MapId map, MapFormat format, float fill) override;
        bool canRemoveMap(MapId map) const override;
        WriteResult removeMap(MapId map) override;
        void takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) override;
        Vector2 worldOfTexel(MapId map, int x, int y) const override;
        Vector2 texelOfWorld(MapId map, float localX, float localZ) const override;
        TexelRect preferredBlock(MapId map) const override;
        std::unique_ptr<TerrainLargeEdit> beginLarge(MapId map, TexelRect rect) override;
        std::unique_ptr<TerrainStoreUndoHook> takeUndoHook() override;

    private:
        struct Entry {
            MapId id;
            MapFormat format;
            std::vector<uint8_t> owned;
            uint8_t* pixels = nullptr;
            TexelRect dirty;
        };

        Entry* find(MapId map);
        const Entry* find(MapId map) const;
        size_t offsetOf(const Entry& entry, int x, int y) const;

        float size;
        float maxHeight;
        std::vector<std::unique_ptr<Entry>> entries;
    };

}

#endif // RAW_MAP_STORE_H
