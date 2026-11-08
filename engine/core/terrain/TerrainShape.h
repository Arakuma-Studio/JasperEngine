// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_SHAPE_H
#define TERRAIN_SHAPE_H

// Where an operation applies (T1 R5, R6): shapes and filters, in terrain-local units, evaluated at
// each texel's sample. Neither knows what the operation does.

#include "Export.h"
#include "math/Vector2.h"
#include "terrain/TerrainDeterministic.h"
#include "terrain/TerrainMapStore.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::terrain {

    enum class ShapeType : uint8_t { All, Circle, Rect, Polyline, Mask, Stroke };
    enum class Falloff : uint8_t { Smooth, Linear, Constant };

    struct Shape {
        ShapeType type = ShapeType::All;

        Vector2 center = Vector2(0.0f, 0.0f);   // Circle, Mask; with nearestTexel, any shape
        float radius = 0.0f;                    // Circle, Polyline, Stroke
        Vector2 min = Vector2(0.0f, 0.0f);      // Rect
        Vector2 max = Vector2(0.0f, 0.0f);      // Rect
        float rotation = 0.0f;                  // Rect, Mask: degrees

        std::vector<Vector2> points;            // Polyline, Stroke
        std::vector<float> pressures;           // Stroke: one per point, or none for 1
        std::vector<float> times;               // Stroke: seconds, one per point, or none
        bool closed = false;                    // Polyline: with radius 0, the inside of a polygon
        float spacing = 0.0f;                   // Stroke: between stamps; 0 is R33's default

        // Mask: a grayscale image as weight, row by row. The caller decodes the file: no IO here.
        std::vector<float> maskPixels;
        int maskWidth = 0, maskHeight = 0;
        Vector2 size = Vector2(0.0f, 0.0f);     // Mask: its extent
        bool maskRound = false;                 // Mask: clipped to the inscribed ellipse, as a circular brush

        // Polyline: a square brush dragged along it, measured on the larger axis from the nearest
        // point. Its mask, when it has pixels, is sampled in that brush's frame (a ramp's stamp).
        bool squareEnds = false;
        // A brush's stamp that misses every sample still reaches the texel nearest center, as
        // Circle and Stroke always do (T1 R17)
        bool nearestTexel = false;

        // Stroke: what a stamp's pressure scales (T1 R37)
        bool pressureStrength = true;
        bool pressureSize = false;

        Falloff falloff = Falloff::Smooth;
        // Share of the radius over which weight falls to 0; below zero is the default, 1 for
        // Circle and Stroke and 0 for the rest
        float feather = -1.0f;
    };

    struct FilterRange {
        float min = 0.0f;
        float max = 0.0f;
    };

    struct LayerRange {
        int index = 0;
        float min = 0.0f;
        float max = 1.0f;
    };

    struct Filter {
        std::optional<FilterRange> slope;   // degrees
        std::optional<FilterRange> height;  // terrain-local
        std::optional<LayerRange> layer;    // weight
        bool invert = false;                // flips the whole filter
        bool invertSlope = false;           // T1 R66: the slope range keeps what is outside it
        bool invertHeight = false;          // and the height range
        float softness = 0.0f;              // 0-1: a smooth band, as a share of each range

        bool active() const { return slope || height || layer; }
    };

    // The local position of a map's texels. Worked out once from the map's whole extent, so a texel
    // lands on one position whichever block it is computed in (T1 R86, R87).
    struct MapLattice {
        float originX = 0.0f, originZ = 0.0f;
        float stepX = 1.0f, stepZ = 1.0f;
        int baseX = 0, baseY = 0;

        float x(int texelX) const { return originX + static_cast<float>(texelX - baseX) * stepX; }
        float z(int texelY) const { return originZ + static_cast<float>(texelY - baseY) * stepZ; }
    };

    JASPERENGINE_API MapLattice latticeOf(const TerrainMapStore& store, MapId map, const MapFormat& format);

    // Local rectangle a map covers, its texels' outer edges
    struct LocalBounds {
        Vector2 min = Vector2(0.0f, 0.0f);
        Vector2 max = Vector2(0.0f, 0.0f);
    };
    JASPERENGINE_API LocalBounds localBoundsOf(const MapLattice& lattice, const MapFormat& format);

    struct ShapeSample {
        float weight = 0.0f;    // falloff times the mask: 0 outside
        float falloff = 0.0f;   // without the mask
        float mask = 1.0f;      // the mask's value alone
        bool inside = false;    // the sample is in the region (R12's rule)
        float along = 0.0f;     // Polyline: how far along its first segment, 0..1 (ramp)
    };

    // One shape ready to evaluate: everything that does not depend on the texel is worked out here
    class JASPERENGINE_API ShapeEvaluator {
    public:
        explicit ShapeEvaluator(const Shape& shape);

        ShapeSample sample(float x, float z) const;
        // Terrain-local bounding box of the region; false for All, which has none
        bool bounds(LocalBounds& out) const;
        // The radius kernels scale by (smooth's step): half the smallest extent; 0 for All
        float characteristicRadius() const;

    private:
        float falloffOf(float distance01) const;
        float maskAt(float u, float v) const;

        const Shape* shape;
        float feather;
        float cosine = 1.0f, sine = 0.0f;
        Vector2 middle = Vector2(0.0f, 0.0f);
        Vector2 half = Vector2(0.0f, 0.0f);
    };

    // The texels of a map a shape's bounding box reaches, clipped to the map. rawArea gets the box's
    // texel count before clipping, which is what R9 calls outside once the clipped area is taken away.
    JASPERENGINE_API TexelRect shapeTexels(const TerrainMapStore& store, MapId map, const MapFormat& format, const ShapeEvaluator& evaluator,
                                           int64_t* rawArea = nullptr);

    // Filter weights for a block of one map, reading the state the filter must see: before the op,
    // or before the stamp (R6). The reader lets the engine substitute what it has already overwritten.
    using MapReader = bool (*)(void* user, MapId map, TexelRect rect, std::span<float> out);

    // Fills weights (block.w * block.h values, 0..1). All 1 without a filter. Reads through reader,
    // or the store when it is null. A layer whose blend map does not exist has weight 0.
    JASPERENGINE_API void evaluateFilter(const TerrainMapStore& store, const Filter& filter, const MapLattice& lattice, TexelRect block,
                                         std::span<float> weights, MapReader reader = nullptr, void* readerUser = nullptr);

    // A range test with T1 R6's band: 1 in the middle, smooth to 0 over softness * width / 2 inside
    // each end, and 0 outside. Hard at softness 0.
    JASPERENGINE_API float softRange(float value, float low, float high, float softness);

    // The same for slope, compared in cosines so no libm acos enters a kernel (R8)
    JASPERENGINE_API float softSlope(float normalY, float minDegrees, float maxDegrees, float softness);

    // Height and normal at a local position from a height buffer read for that purpose: the surface
    // query's own rule (bilinear, forward difference at the height map's spacing, R15)
    struct HeightWindow {
        const float* values = nullptr;
        TexelRect rect;
        TexelRect bounds;
        MapLattice lattice;
        float invStepX = 1.0f, invStepZ = 1.0f;
    };
    JASPERENGINE_API float sampleWindow(const HeightWindow& window, float x, float z);

}

#endif // TERRAIN_SHAPE_H
