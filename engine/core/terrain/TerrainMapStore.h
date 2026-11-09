// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_MAP_STORE_H
#define TERRAIN_MAP_STORE_H

// What the operation engine reads and writes (T1 R2, R86, programme D4): rectangles of height,
// blend, hole and density data in texels. Callers never touch a texture, so a tiled store (T4), a
// raw buffer (tests, generation) and an overlay (a transaction) are the same to every kernel.
//
// Texels are signed and a map's extent is format(map).bounds, not [0, W): ops place texels only
// through worldOfTexel and texelOfWorld, which give each map its own lattice.

#include "math/Vector2.h"
#include "terrain/TerrainDeterministic.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::terraincore {

    enum class MapKind : uint8_t { Height, Blend, Hole, Density };

    // Blend: the blend map; Density: the foliage layer; Height and Hole: 0
    struct MapId {
        MapKind kind = MapKind::Height;
        int index = 0;

        bool operator==(const MapId& other) const { return kind == other.kind && index == other.index; }
        bool operator!=(const MapId& other) const { return !(*this == other); }
    };

    // Signed origin (T4), w and h >= 0
    struct TexelRect {
        int x = 0, y = 0, w = 0, h = 0;

        bool empty() const { return w <= 0 || h <= 0; }
        int right() const { return x + w; }
        int bottom() const { return y + h; }
        int64_t area() const { return empty() ? 0 : static_cast<int64_t>(w) * static_cast<int64_t>(h); }
        bool contains(const TexelRect& other) const {
            return other.x >= x && other.y >= y && other.right() <= right() && other.bottom() <= bottom();
        }
        bool operator==(const TexelRect& other) const { return x == other.x && y == other.y && w == other.w && h == other.h; }
    };

    inline TexelRect intersect(const TexelRect& a, const TexelRect& b) {
        const int left = a.x > b.x ? a.x : b.x;
        const int top = a.y > b.y ? a.y : b.y;
        const int right = a.right() < b.right() ? a.right() : b.right();
        const int bottom = a.bottom() < b.bottom() ? a.bottom() : b.bottom();
        if (right <= left || bottom <= top) return TexelRect{};
        return TexelRect{left, top, right - left, bottom - top};
    }

    inline TexelRect unite(const TexelRect& a, const TexelRect& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        const int left = a.x < b.x ? a.x : b.x;
        const int top = a.y < b.y ? a.y : b.y;
        const int right = a.right() > b.right() ? a.right() : b.right();
        const int bottom = a.bottom() > b.bottom() ? a.bottom() : b.bottom();
        return TexelRect{left, top, right - left, bottom - top};
    }

    inline TexelRect grow(const TexelRect& rect, int margin) {
        return TexelRect{rect.x - margin, rect.y - margin, rect.w + margin * 2, rect.h + margin * 2};
    }

    struct MapFormat {
        TexelRect bounds;
        int channels = 0;
        int bitsPerChannel = 0;
        bool exists = false;
    };

    // heights 0..maxHeight
    struct TerrainFrame {
        float maxHeight = 0;
    };

    struct WriteResult {
        bool ok = false;
        std::string error;
        int tilesCreated = 0;
    };

    // R87: the undo of a block-by-block edit, which writes straight to the store. The caller keeps
    // it with its undo step.
    class TerrainLargeEdit {
    public:
        virtual ~TerrainLargeEdit() = default;
        virtual void undo() = 0;
        virtual void redo() = 0;
    };

    // R86: undo and redo of what a store created on write (T4's auto-created tiles), and of the
    // non-map data a registered op wrote (T5's TreeStore, R89)
    class TerrainStoreUndoHook {
    public:
        virtual ~TerrainStoreUndoHook() = default;
        virtual void undo() = 0;
        virtual void redo() = 0;
    };

    class TerrainMapStore {
    public:
        virtual ~TerrainMapStore() = default;

        virtual TerrainFrame frame() const = 0;
        virtual MapFormat format(MapId map) const = 0;

        // Normalized floats, row by row; rect inside the map. Height, Hole and Density give one
        // value per texel (the red channel of an old multi-channel source); Blend gives every
        // stored channel, interleaved (floatChannels).
        virtual bool read(MapId map, TexelRect rect, std::span<float> out) const = 0;
        // Rounded per R11 (quantizeChannel); only changed texels count as dirty
        virtual WriteResult write(MapId map, TexelRect rect, std::span<const float> in) = 0;

        // Stored bytes (deltas, hashes, raw export)
        virtual bool readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const = 0;
        virtual WriteResult writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) = 0;

        // Runs for every rectangle before an overlay commits, so a refusal changes nothing
        virtual bool canWrite(MapId map, TexelRect rect, std::string* why) const = 0;

        // format.bounds, channels and bitsPerChannel describe the new map. fill is every colour
        // channel's normalized value; a fourth channel starts opaque, as saved blend maps are.
        virtual WriteResult createMap(MapId map, MapFormat format, float fill) = 0;
        // Whether createMap could work, asked before any op runs, so a dry run tells the truth (a
        // terrain has no density map for a foliage layer it does not have)
        virtual bool canCreateMap(MapId map, std::string* why) const { (void)map; (void)why; return true; }

        // The last fill removes the hole map (T1 R12). A store may keep its maps (T3 R29's
        // holeMapHolds, T4's per-tile maps): canRemoveMap answers before anything commits, and a
        // map that stays keeps its bytes. Not abstract: most stores never remove a map.
        virtual bool canRemoveMap(MapId map) const { (void)map; return false; }
        virtual WriteResult removeMap(MapId map) { (void)map; return WriteResult{false, "this store keeps its maps", 0}; }

        // Rectangles written since the last take
        virtual void takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) = 0;

        // Terrain-local XZ of a texel's sample
        virtual Vector2 worldOfTexel(MapId map, int x, int y) const = 0;
        // Continuous texel coordinate of a terrain-local position: a texel's sample is at its
        // integer coordinate
        virtual Vector2 texelOfWorld(MapId map, float localX, float localZ) const = 0;

        // The unit a large edit works in (T4's tile rectangle); kernels hold no more than this
        // plus a margin at once. The single-map store answers the whole map.
        virtual TexelRect preferredBlock(MapId map) const = 0;
        // Null: use the overlay (R87)
        virtual std::unique_ptr<TerrainLargeEdit> beginLarge(MapId map, TexelRect rect) = 0;
        // Null: nothing created since the last take
        virtual std::unique_ptr<TerrainStoreUndoHook> takeUndoHook() = 0;
    };

    // Floats per texel through read and write
    inline int floatChannels(MapId map, const MapFormat& format) {
        return map.kind == MapKind::Blend ? format.channels : 1;
    }

    // bytes per stored channel: 8-bit and 16-bit unsigned, or a 32-bit float map (an .exr or .hdr
    // source's CPU view), which stores only read
    inline int bytesPerChannelOf(int bitsPerChannel) {
        return bitsPerChannel >= 32 ? 4 : (bitsPerChannel >= 16 ? 2 : 1);
    }

    inline int bytesPerTexel(const MapFormat& format) {
        return format.channels * bytesPerChannelOf(format.bitsPerChannel);
    }

    // R11: the one quantization rule of every map
    inline uint32_t quantizeChannel(float value, int bitsPerChannel) {
        const float top = bitsPerChannel >= 16 ? 65535.0f : 255.0f;
        // std::round on a value in [0, 65535], without its library call (T1 R47, R48): the fraction
        // below 2^23 is exact, and a half rounds up, away from zero, as std::round's does
        const float scaled = detClamp01(value) * top;
        const uint32_t whole = static_cast<uint32_t>(scaled);
        return whole + (scaled - static_cast<float>(whole) >= 0.5f ? 1u : 0u);
    }

    inline float decodeChannel(const uint8_t* stored, int bitsPerChannel) {
        if (bitsPerChannel >= 32) {
            float value;
            std::memcpy(&value, stored, sizeof(value));
            return std::isfinite(value) ? value : 0.0f;
        }
        if (bitsPerChannel >= 16) {
            return static_cast<float>(static_cast<uint32_t>(stored[0]) | (static_cast<uint32_t>(stored[1]) << 8)) / 65535.0f;
        }
        return static_cast<float>(stored[0]) / 255.0f;
    }

    inline void encodeChannel(uint8_t* stored, uint32_t quantized, int bitsPerChannel) {
        stored[0] = static_cast<uint8_t>(quantized & 0xFF);
        if (bitsPerChannel >= 16) stored[1] = static_cast<uint8_t>((quantized >> 8) & 0xFF);
    }

    // The floats of one stored texel
    inline void decodeTexel(MapId map, const MapFormat& format, const uint8_t* stored, float* values) {
        const int step = bytesPerChannelOf(format.bitsPerChannel);
        const int count = floatChannels(map, format);
        for (int c = 0; c < count; c++) {
            values[c] = decodeChannel(stored + c * step, format.bitsPerChannel);
        }
    }

    // Stores the floats of one texel, leaving its bytes alone when they round to what is there (R11:
    // an untouched texel keeps its bytes). A changed 8-bit scalar on a multi-channel source is
    // replicated to RGB and made opaque, as such a map is read. Returns whether the bytes changed.
    inline bool encodeTexel(MapId map, const MapFormat& format, const float* values, uint8_t* stored) {
        const int step = format.bitsPerChannel >= 16 ? 2 : 1;
        const int count = floatChannels(map, format);
        bool changed = false;
        for (int c = 0; c < count; c++) {
            const uint32_t quantized = quantizeChannel(values[c], format.bitsPerChannel);
            const uint32_t current = step == 2 ? static_cast<uint32_t>(stored[c * step]) | (static_cast<uint32_t>(stored[c * step + 1]) << 8)
                                               : stored[c * step];
            if (quantized != current) {
                encodeChannel(stored + c * step, quantized, format.bitsPerChannel);
                changed = true;
            }
        }
        if (changed && count == 1 && format.channels >= 3 && step == 1) {
            stored[1] = stored[0];
            stored[2] = stored[0];
            if (format.channels >= 4) stored[3] = 255;
        }
        return changed;
    }

}

#endif // TERRAIN_MAP_STORE_H
