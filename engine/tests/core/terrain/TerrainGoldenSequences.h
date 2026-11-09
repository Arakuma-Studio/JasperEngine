// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_TERRAIN_GOLDEN_SEQUENCES_H
#define TESTS_TERRAIN_GOLDEN_SEQUENCES_H

// T1 AC5: the op sequences TerrainGolden.* pins, on a fixed seed map. The core suite compares them on
// every CI platform, and tools/test-projects/terrain-edit runs the same list in wasm, so this header
// includes nothing of the test framework.

#include "terrain/RawMapStore.h"
#include "terrain/TerrainDeterministic.h"
#include "terrain/TerrainOps.h"
#include "util/Fnv.h"

#include <cstdint>
#include <string>
#include <vector>

namespace terraingolden {

    using namespace jasperengine;
    using namespace jasperengine::terraincore;

    inline constexpr int SAMPLES = 65;
    inline constexpr float TERRAIN_SIZE = 65.0f;  // one world unit per texel: the (i * h) lattice lands on the same floats
    inline constexpr float MAX_HEIGHT = 16.0f;

    struct Sequence {
        std::string name;
        std::vector<Op> ops;
        bool usesBlocksFreely = true;   // erode's droplets are per block: same block size or no comparison
    };

    // Rolling ground from PCG32 octaves: the same on every platform, with slopes for the filters
    inline void fillSeedMaps(TerrainMapStore& store) {
        Pcg32 random(12345);
        float lattice[9][9];
        for (auto& row : lattice) {
            for (float& value : row) {
                value = random.nextFloat();
            }
        }
        const TexelRect bounds = store.format(MapId{MapKind::Height, 0}).bounds;
        std::vector<float> heights(static_cast<size_t>(SAMPLES) * SAMPLES);
        for (int y = 0; y < SAMPLES; y++) {
            for (int x = 0; x < SAMPLES; x++) {
                const float fx = static_cast<float>(x) / static_cast<float>(SAMPLES - 1) * 8.0f;
                const float fy = static_cast<float>(y) / static_cast<float>(SAMPLES - 1) * 8.0f;
                const int ix = x == SAMPLES - 1 ? 7 : static_cast<int>(fx);
                const int iy = y == SAMPLES - 1 ? 7 : static_cast<int>(fy);
                const float tx = detSmooth(fx - static_cast<float>(ix));
                const float ty = detSmooth(fy - static_cast<float>(iy));
                const float top = lattice[iy][ix] + (lattice[iy][ix + 1] - lattice[iy][ix]) * tx;
                const float bottom = lattice[iy + 1][ix] + (lattice[iy + 1][ix + 1] - lattice[iy + 1][ix]) * tx;
                heights[static_cast<size_t>(y) * SAMPLES + x] = 0.15f + (top + (bottom - top) * ty) * 0.7f;
            }
        }
        store.write(MapId{MapKind::Height, 0}, bounds, heights);

        const MapId blend{MapKind::Blend, 0};
        const TexelRect blendBounds = store.format(blend).bounds;
        std::vector<float> weights(static_cast<size_t>(SAMPLES) * SAMPLES * 4);
        for (int y = 0; y < SAMPLES; y++) {
            for (int x = 0; x < SAMPLES; x++) {
                float* texel = weights.data() + (static_cast<size_t>(y) * SAMPLES + x) * 4;
                texel[0] = static_cast<float>(x) / static_cast<float>(SAMPLES - 1);
                texel[1] = 1.0f - texel[0];
                texel[2] = 0.0f;
                texel[3] = 1.0f;
            }
        }
        store.write(blend, blendBounds, weights);

        const MapId densityMap{MapKind::Density, 0};
        std::vector<float> densities(static_cast<size_t>(SAMPLES) * SAMPLES, 0.25f);
        store.write(densityMap, store.format(densityMap).bounds, densities);
    }

    inline Shape circleShape(float x, float z, float radius) {
        Shape shape;
        shape.type = ShapeType::Circle;
        shape.center = Vector2(x, z);
        shape.radius = radius;
        return shape;
    }

    inline Shape rectShape(float x0, float z0, float x1, float z1, float rotation = 0.0f) {
        Shape shape;
        shape.type = ShapeType::Rect;
        shape.min = Vector2(x0, z0);
        shape.max = Vector2(x1, z1);
        shape.rotation = rotation;
        return shape;
    }

    inline Shape polylineShape(std::vector<Vector2> points, float radius, bool closed = false) {
        Shape shape;
        shape.type = ShapeType::Polyline;
        shape.points = std::move(points);
        shape.radius = radius;
        shape.closed = closed;
        return shape;
    }

    inline Shape maskShape(float x, float z, float size, float rotation = 0.0f) {
        Shape shape;
        shape.type = ShapeType::Mask;
        shape.center = Vector2(x, z);
        shape.size = Vector2(size, size);
        shape.rotation = rotation;
        shape.maskWidth = 4;
        shape.maskHeight = 4;
        shape.maskPixels = {0.0f, 0.25f, 0.5f, 0.75f, 0.25f, 0.5f, 0.75f, 1.0f, 0.5f, 0.75f, 1.0f, 0.75f, 0.75f, 1.0f, 0.75f, 0.5f};
        return shape;
    }

    inline Shape strokeShape(std::vector<Vector2> points, float radius) {
        Shape shape;
        shape.type = ShapeType::Stroke;
        shape.points = std::move(points);
        shape.radius = radius;
        return shape;
    }

    inline std::vector<Sequence> sequences() {
        std::vector<Sequence> list;
        auto add = [&](const std::string& name, Op op, bool blocksFree = true) {
            list.push_back(Sequence{name, {std::move(op)}, blocksFree});
        };
        auto make = [](OpType type, Shape shape) {
            Op op;
            op.type = type;
            op.shape = std::move(shape);
            return op;
        };

        // heights, once per shape and filter
        { Op op = make(OpType::Raise, circleShape(4.0f, -6.0f, 14.0f)); op.delta = 3.0f; add("raise_circle", op); }
        { Op op = make(OpType::Lower, rectShape(-20.0f, -10.0f, 18.0f, 12.0f, 30.0f)); op.delta = 2.5f; add("lower_rect_rotated", op); }
        { Op op = make(OpType::Raise, polylineShape({Vector2(-25.0f, -25.0f), Vector2(0.0f, 5.0f), Vector2(25.0f, -5.0f)}, 6.0f)); op.delta = 1.5f; add("raise_polyline", op); }
        { Op op = make(OpType::SetHeight, polylineShape({Vector2(-15.0f, -15.0f), Vector2(15.0f, -15.0f), Vector2(0.0f, 15.0f)}, 0.0f, true)); op.height = 8.0f; add("set_height_polygon", op); }
        { Op op = make(OpType::Raise, Shape{}); op.delta = 0.25f; add("raise_all", op); }
        { Op op = make(OpType::Raise, rectShape(-14.0f, -14.0f, 14.0f, 14.0f)); op.shape.falloff = Falloff::Linear; op.shape.feather = 0.5f; op.delta = 2.0f; add("raise_rect_feathered", op); }
        { Op op = make(OpType::Raise, circleShape(0.0f, 0.0f, 20.0f)); op.delta = 40.0f; add("raise_clips_high", op); }
        { Op op = make(OpType::Lower, circleShape(0.0f, 0.0f, 20.0f)); op.delta = 40.0f; add("lower_clips_low", op); }
        { Op op = make(OpType::LiftFloors, circleShape(-10.0f, 10.0f, 16.0f)); op.height = 7.0f; add("lift_floors", op); }
        { Op op = make(OpType::FlattenPeaks, circleShape(10.0f, -10.0f, 16.0f)); op.height = 6.0f; add("flatten_peaks", op); }
        { Op op = make(OpType::Smooth, circleShape(0.0f, 0.0f, 24.0f)); op.iterations = 3; add("smooth", op); }
        { Op op = make(OpType::Sharpen, rectShape(-12.0f, -12.0f, 12.0f, 12.0f)); add("sharpen", op); }
        { Op op = make(OpType::AddNoise, circleShape(-8.0f, 8.0f, 20.0f)); op.amplitude = 2.0f; op.noiseSize = 5.0f; op.seed = 7; add("add_noise", op); }
        { Op op = make(OpType::Terrace, Shape{}); op.steps = 6; add("terrace_all", op); }
        { Op op = make(OpType::Stamp, maskShape(5.0f, 5.0f, 24.0f, 45.0f)); op.delta = 4.0f; add("stamp_mask", op); }
        { Op op = make(OpType::Raise, maskShape(-10.0f, -5.0f, 18.0f)); op.delta = 2.0f; add("raise_mask", op); }
        { Op op = make(OpType::Erode, circleShape(0.0f, 0.0f, 20.0f)); op.seed = 3; op.iterations = 2; add("erode", op, false); }
        { Op op = make(OpType::Ramp, polylineShape({Vector2(-20.0f, -20.0f), Vector2(20.0f, 20.0f)}, 7.0f)); op.shape.falloff = Falloff::Constant; add("ramp_surface_ends", op); }
        { Op op = make(OpType::Ramp, polylineShape({Vector2(-20.0f, 20.0f), Vector2(20.0f, -20.0f)}, 5.0f)); op.heights = FilterRange{2.0f, 12.0f}; add("ramp_given_ends", op); }
        { Op op = make(OpType::RemapHeights, Shape{}); op.inRange = FilterRange{2.0f, 14.0f}; op.outRange = FilterRange{4.0f, 10.0f}; add("remap_heights", op); }
        { Op op = make(OpType::Raise, circleShape(0.0f, 0.0f, 28.0f)); op.delta = 3.0f; op.filter.slope = FilterRange{0.0f, 20.0f}; add("raise_slope_filter", op); }
        { Op op = make(OpType::Raise, circleShape(0.0f, 0.0f, 28.0f)); op.delta = 3.0f; op.filter.slope = FilterRange{10.0f, 40.0f}; op.filter.softness = 0.5f; add("raise_slope_soft", op); }
        { Op op = make(OpType::SetHeight, Shape{}); op.height = 5.0f; op.filter.height = FilterRange{4.0f, 9.0f}; op.filter.invert = true; add("set_height_inverted_filter", op); }
        { Op op = make(OpType::Raise, Shape{}); op.delta = 1.0f; op.filter.layer = LayerRange{1, 0.25f, 0.75f}; add("raise_layer_filter", op); }
        { Op op = make(OpType::Raise, strokeShape({Vector2(-25.0f, 0.0f), Vector2(25.0f, 8.0f)}, 8.0f)); op.strength = 0.8f; add("raise_stroke", op); }
        { Op op = make(OpType::Smooth, strokeShape({Vector2(0.0f, -25.0f), Vector2(-5.0f, 25.0f)}, 9.0f)); add("smooth_stroke", op); }

        // internal
        { Op op = make(OpType::FillNoise, Shape{}); op.fill = FillMode::FractalNoise; op.baseHeight = 0.5f; op.amplitude = 0.3f; op.frequency = 3.0f; op.seed = 7; add("fill_fractal", op); }
        { Op op = make(OpType::FillNoise, Shape{}); op.fill = FillMode::RandomNoise; op.baseHeight = 0.5f; op.amplitude = 0.2f; op.seed = 11; add("fill_random", op); }

        // blend and density
        { Op op = make(OpType::PaintLayer, circleShape(0.0f, 0.0f, 18.0f)); op.layer = 2; add("paint_layer", op); }
        { Op op = make(OpType::PaintLayer, rectShape(-8.0f, -8.0f, 8.0f, 8.0f)); op.layer = 1; op.normalize = false; op.strength = 0.5f; add("paint_layer_keep", op); }
        { Op op = make(OpType::PaintLayer, circleShape(10.0f, 10.0f, 12.0f)); op.layer = 4; add("paint_second_blend_map", op); }
        { Op op = make(OpType::PaintBase, circleShape(-6.0f, 6.0f, 14.0f)); op.strength = 0.7f; add("paint_base", op); }
        { Op op = make(OpType::PaintLayer, strokeShape({Vector2(-20.0f, -20.0f), Vector2(20.0f, 20.0f)}, 6.0f)); op.layer = 0; add("paint_stroke", op); }
        {
            Op op = make(OpType::PaintByRules, Shape{});
            PaintRule grass;
            grass.layer = 0;
            PaintRule rock;
            rock.layer = 1;
            rock.slope = FilterRange{18.0f, 90.0f};
            rock.noise = 0.5f;
            PaintRule snow;
            snow.layer = 2;
            snow.height = FilterRange{11.0f, 16.0f};
            op.rules = {grass, rock, snow};
            op.seed = 5;
            add("paint_by_rules", op);
        }
        { Op op = make(OpType::RemapLayers, Shape{}); op.order = {2, 0, 1}; add("remap_layers", op); }
        { Op op = make(OpType::PaintDensity, circleShape(3.0f, 3.0f, 15.0f)); op.layer = 0; add("paint_density", op); }
        { Op op = make(OpType::EraseDensity, rectShape(-30.0f, -30.0f, 0.0f, 0.0f)); op.layer = 0; add("erase_density", op); }

        // holes
        { Op op = make(OpType::CutHoles, circleShape(0.0f, 0.0f, 10.0f)); add("cut_holes", op); }
        { Op op = make(OpType::CutHoles, Shape{}); op.filter.height = FilterRange{0.0f, 5.0f}; add("cut_holes_filtered", op); }

        // a list, one op after another on the overlay's float working values
        {
            Op a = make(OpType::Raise, circleShape(-5.0f, 0.0f, 12.0f));
            a.delta = 0.0004f;
            Op c = make(OpType::Smooth, Shape{});
            Op d = make(OpType::CutHoles, circleShape(5.0f, 5.0f, 6.0f));
            Op e = make(OpType::FillHoles, Shape{});
            list.push_back(Sequence{"a_list", {a, a, c, d, e}, true});
        }

        return list;
    }

    inline void makeStore(RawMapStore& store) {
        store.addMap(MapId{MapKind::Height, 0}, SAMPLES, SAMPLES, 1, 16);
        store.addMap(MapId{MapKind::Blend, 0}, SAMPLES, SAMPLES, 4, 8);
        store.addMap(MapId{MapKind::Density, 0}, SAMPLES, SAMPLES, 1, 8);
    }

    // Every map a sequence can touch
    inline uint64_t stateHash(const TerrainMapStore& store) {
        uint64_t hash = mapHash(store, MapId{MapKind::Height, 0});
        const uint64_t others[] = {
            mapHash(store, MapId{MapKind::Blend, 0}), mapHash(store, MapId{MapKind::Blend, 1}),
            mapHash(store, MapId{MapKind::Density, 0}), mapHash(store, MapId{MapKind::Hole, 0})};
        for (uint64_t other : others) {
            hash = fnv1a64(&other, sizeof(other), hash);
        }
        return hash;
    }

    // The digest the wasm gate prints, over every sequence's hash in order
    inline uint64_t digestOf(const std::vector<uint64_t>& hashes) {
        uint64_t digest = FNV1A64_SEED;
        for (uint64_t hash : hashes) {
            digest = fnv1a64(&hash, sizeof(hash), digest);
        }
        return digest;
    }

    inline std::vector<uint64_t> runAll(bool parallel = true) {
        std::vector<uint64_t> hashes;
        for (const Sequence& sequence : sequences()) {
            RawMapStore store(TERRAIN_SIZE, MAX_HEIGHT);
            makeStore(store);
            fillSeedMaps(store);
            OpOptions options;
            options.parallel = parallel;
            applyOps(store, sequence.ops, options);
            hashes.push_back(stateHash(store));
        }
        return hashes;
    }

}

#endif // TESTS_TERRAIN_GOLDEN_SEQUENCES_H
