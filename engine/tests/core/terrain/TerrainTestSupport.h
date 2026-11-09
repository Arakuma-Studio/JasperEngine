// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_TERRAIN_SUPPORT_H
#define TESTS_TERRAIN_SUPPORT_H

// Maps and stores the terrain engine's tests share (docs/specs/T1-terrain-authoring.md §7)

#include "terrain/OverlayMapStore.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainDeterministic.h"
#include "terrain/TerrainOps.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace terraintest {

    using namespace jasperengine;
    using namespace jasperengine::terraincore;

    inline constexpr MapId HEIGHT{MapKind::Height, 0};
    inline constexpr MapId HOLE{MapKind::Hole, 0};
    inline MapId blend(int index) { return MapId{MapKind::Blend, index}; }
    inline MapId density(int index) { return MapId{MapKind::Density, index}; }

    // A 16-bit height map filled with one normalized value
    inline void addFlatHeight(RawMapStore& store, int size, float value) {
        store.addMap(HEIGHT, size, size, 1, 16);
        std::vector<float> values(static_cast<size_t>(size) * size, value);
        store.write(HEIGHT, TexelRect{0, 0, size, size}, values);
        std::vector<std::pair<MapId, TexelRect>> dirty;
        store.takeDirty(dirty);
    }

    // value(x, y) into the height map
    template<typename F>
    void fillHeight(TerrainMapStore& store, int width, int height, F&& value) {
        const TexelRect bounds = store.format(HEIGHT).bounds;
        std::vector<float> values(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                values[static_cast<size_t>(y) * width + x] = value(x, y);
            }
        }
        store.write(HEIGHT, TexelRect{bounds.x, bounds.y, width, height}, values);
    }

    inline std::vector<float> readHeights(const TerrainMapStore& store) {
        const TexelRect bounds = store.format(HEIGHT).bounds;
        std::vector<float> values(static_cast<size_t>(bounds.area()));
        store.read(HEIGHT, bounds, values);
        return values;
    }

    inline std::vector<uint8_t> readAllBytes(const TerrainMapStore& store, MapId map) {
        const MapFormat format = store.format(map);
        std::vector<uint8_t> bytes(static_cast<size_t>(format.bounds.area()) * bytesPerTexel(format));
        store.readBytes(map, format.bounds, bytes);
        return bytes;
    }

    inline Op circleOp(OpType type, float x, float z, float radius) {
        Op op;
        op.type = type;
        op.shape.type = ShapeType::Circle;
        op.shape.center = Vector2(x, z);
        op.shape.radius = radius;
        return op;
    }

    inline TerrainOpResult apply(TerrainMapStore& store, const Op& op, const OpOptions& options = {}) {
        return applyOps(store, std::span<const Op>(&op, 1), options);
    }

    // T1 R86, AC68: what T4's store looks like to the engine: signed texels on an (i * h) lattice,
    // small blocks, refused rectangles, tiles created on write that undo removes. Backed by a
    // RawMapStore shifted by half the map, so its bytes can be compared with the plain store's.
    class FakeTiledStore final : public TerrainMapStore {
    public:
        FakeTiledStore(int samples, float spacing, float maxHeight, int blockSize)
            : half(samples / 2), spacing(spacing), block(blockSize), raw(spacing * static_cast<float>(samples), maxHeight) {}

        RawMapStore& backing() { return raw; }

        void refuse(TexelRect rect) { refused = rect; }
        void lendUndoHook(std::unique_ptr<TerrainStoreUndoHook> hook) { nextHook = std::move(hook); }
        void lendLargeEdit(bool lend) { lendLarge = lend; }

        int largeEditsGiven = 0;
        int tilesPerWrite = 0;
        mutable int largestRead = 0;

        TerrainFrame frame() const override { return raw.frame(); }

        MapFormat format(MapId map) const override {
            MapFormat f = raw.format(map);
            if (f.exists) {
                f.bounds.x -= half;
                f.bounds.y -= half;
            }
            return f;
        }

        bool read(MapId map, TexelRect rect, std::span<float> out) const override {
            largestRead = std::max(largestRead, std::max(rect.w, rect.h));
            return raw.read(map, shift(rect), out);
        }

        WriteResult write(MapId map, TexelRect rect, std::span<const float> in) override {
            std::string why;
            if (!canWrite(map, rect, &why)) return WriteResult{false, why, 0};
            WriteResult result = raw.write(map, shift(rect), in);
            result.tilesCreated = tilesPerWrite;
            return result;
        }

        bool readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const override {
            return raw.readBytes(map, shift(rect), out);
        }

        WriteResult writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) override {
            std::string why;
            if (!canWrite(map, rect, &why)) return WriteResult{false, why, 0};
            WriteResult result = raw.writeBytes(map, shift(rect), in);
            result.tilesCreated = tilesPerWrite;
            return result;
        }

        bool canWrite(MapId map, TexelRect rect, std::string* why) const override {
            if (!intersect(rect, refused).empty()) {
                if (why) *why = "past the world's limit";
                return false;
            }
            return raw.canWrite(map, shift(rect), why);
        }

        WriteResult createMap(MapId map, MapFormat f, float fill) override {
            f.bounds.x = 0;
            f.bounds.y = 0;
            return raw.createMap(map, f, fill);
        }

        bool canRemoveMap(MapId map) const override { return raw.canRemoveMap(map); }
        WriteResult removeMap(MapId map) override { return raw.removeMap(map); }

        void takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) override {
            const size_t first = out.size();
            raw.takeDirty(out);
            for (size_t i = first; i < out.size(); i++) {
                out[i].second.x -= half;
                out[i].second.y -= half;
            }
        }

        Vector2 worldOfTexel(MapId, int x, int y) const override {
            return Vector2(static_cast<float>(x) * spacing, static_cast<float>(y) * spacing);
        }

        Vector2 texelOfWorld(MapId, float localX, float localZ) const override {
            return Vector2(localX / spacing, localZ / spacing);
        }

        TexelRect preferredBlock(MapId) const override {
            return TexelRect{-half, -half, block, block};
        }

        std::unique_ptr<TerrainLargeEdit> beginLarge(MapId, TexelRect) override {
            if (!lendLarge) return nullptr;
            largeEditsGiven++;
            return std::make_unique<LargeEdit>();
        }

        std::unique_ptr<TerrainStoreUndoHook> takeUndoHook() override { return std::move(nextHook); }

    private:
        class LargeEdit final : public TerrainLargeEdit {
        public:
            void undo() override {}
            void redo() override {}
        };

        TexelRect shift(TexelRect rect) const {
            rect.x += half;
            rect.y += half;
            return rect;
        }

        int half;
        float spacing;
        int block;
        RawMapStore raw;
        TexelRect refused;
        std::unique_ptr<TerrainStoreUndoHook> nextHook;
        bool lendLarge = false;
    };

}

#endif // TESTS_TERRAIN_SUPPORT_H
