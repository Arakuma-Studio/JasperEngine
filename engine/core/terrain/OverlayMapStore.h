// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef OVERLAY_MAP_STORE_H
#define OVERLAY_MAP_STORE_H

// T1 R3, DC1, DC2, DC6: a transaction over any store. Commit, dry run, strokes, Esc, delta capture
// and live spline sessions are all this object: 64x64 tiles copied on first write, float working
// values so gentle stamps are not lost to rounding, and the stored bytes from before.

#include "Export.h"
#include "terrain/TerrainMapStore.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace jasperengine::terrain {

    class JASPERENGINE_API OverlayMapStore : public TerrainMapStore {
    public:
        static constexpr int TILE_SIZE = 64;

        // What the transaction changed in one map, by a byte diff against the base
        struct MapChange {
            MapId map;
            TexelRect rect;
            int64_t texels = 0;
        };

        explicit OverlayMapStore(TerrainMapStore& base);

        TerrainMapStore& base() { return *baseStore; }

        // Texels whose stored bytes would change, one rectangle per map, in map order
        std::vector<MapChange> changes() const;
        // Maps this transaction would create or remove
        std::vector<MapId> createdMaps() const;
        std::vector<MapId> removedMaps() const;

        // canWrite for every changed rectangle, then writes only the texels that changed. A refusal
        // or a failed write leaves the base byte-identical. The overlay is empty afterwards.
        WriteResult commit();
        // Forgets everything: the base never saw it, unless flushToBase showed it (rewind first)
        void discard();

        // The base shows the bytes written since the last flush, while the transaction stays open
        // with its floats and its before bytes: a stroke is visible as it goes (T1 R17, DC2). Maps
        // this transaction created stay out of the base until the commit.
        WriteResult flushToBase();
        // Back to the before bytes: every tile's floats, and the base wherever a flush showed it. The
        // tiles stay, so the transaction can go on from there (T1 R27, R36; a live spline's re-bake).
        WriteResult rewind();
        // The stored bytes as they were before this transaction (rect inside the map)
        bool readBeforeBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const;

        // Every touched tile in a stable order, with its stored bytes before and after: what a delta
        // undo step is cut from
        using TileVisitor = std::function<void(MapId map, TexelRect rect, std::span<const uint8_t> before, std::span<const uint8_t> after)>;
        void visitTiles(const TileVisitor& visitor) const;

        size_t tileCount() const;
        // The heap this transaction holds in tiles (T1 R26's bound)
        size_t memoryBytes() const;

        TerrainFrame frame() const override;
        MapFormat format(MapId map) const override;
        bool read(MapId map, TexelRect rect, std::span<float> out) const override;
        WriteResult write(MapId map, TexelRect rect, std::span<const float> in) override;
        bool readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const override;
        WriteResult writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) override;
        bool canWrite(MapId map, TexelRect rect, std::string* why) const override;
        WriteResult createMap(MapId map, MapFormat format, float fill) override;
        bool canCreateMap(MapId map, std::string* why) const override;
        bool canRemoveMap(MapId map) const override;
        WriteResult removeMap(MapId map) override;
        void takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) override;
        Vector2 worldOfTexel(MapId map, int x, int y) const override;
        Vector2 texelOfWorld(MapId map, float localX, float localZ) const override;
        TexelRect preferredBlock(MapId map) const override;
        std::unique_ptr<TerrainLargeEdit> beginLarge(MapId map, TexelRect rect) override;
        std::unique_ptr<TerrainStoreUndoHook> takeUndoHook() override;

    private:
        struct Tile {
            TexelRect rect;
            std::vector<float> values;
            std::vector<uint8_t> before;
            // the base holds a flush of it, not its before bytes
            bool shown = false;
        };

        struct MapState {
            MapId id;
            MapFormat format;
            bool created = false;
            bool removed = false;
            float fill = 0.0f;
            // ordered, so every walk over the tiles is the same on every platform
            std::map<std::pair<int, int>, Tile> tiles;
            TexelRect dirty;
            // written since the last flushToBase
            TexelRect unflushed;
        };

        MapState* findState(MapId map);
        const MapState* findState(MapId map) const;
        MapState& state(MapId map);
        MapFormat currentFormat(MapId map) const;
        Tile* ensureTile(MapState& mapState, int tileX, int tileY, const MapFormat& format);
        void fillCreatedBytes(const MapState& mapState, TexelRect rect, std::span<uint8_t> out) const;
        bool tileAfterBytes(const MapState& mapState, const Tile& tile, std::vector<uint8_t>& after) const;
        void clearAll();

        TerrainMapStore* baseStore;
        std::vector<std::unique_ptr<MapState>> maps;
        std::vector<float> scratch;
    };

}

#endif // OVERLAY_MAP_STORE_H
