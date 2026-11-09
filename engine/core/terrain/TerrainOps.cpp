// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainOps.h"

#include "jobs/JobSystem.h"
#include "terrain/TerrainDeterministic.h"
#include "terrain/TerrainErosion.h"
#include "terrain/TerrainHoles.h"
#include "terrain/TerrainLayers.h"
#include "noise/NoiseCompat.h"
#include "terrain/TerrainStroke.h"
#include "util/Fnv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // TEW:1546-1555: one stamp of a brush deposits strength * falloff * flow * 1/60 s (R34)
    constexpr float HEIGHT_FLOW = 1.5f;
    constexpr float BLEND_FLOW = 10.0f;
    constexpr float STAMP_SECONDS = 1.0f / 60.0f;
    // TEW:1559-1561: droplets per square texel of brush area per second at full strength, and the
    // slope a thermal pass leaves standing, in world height units per texel of run
    constexpr float ERODE_DROPLETS_PER_SECOND = 0.4f;
    constexpr float ERODE_TALUS = 0.7f;
    // A stamp above this many texels is worth running on the job system (R38)
    constexpr int64_t PARALLEL_TEXELS = 64 * 1024;
    // T1 R48: a commit's report measures this many tiles or more on the job system's workers
    constexpr size_t PARALLEL_STAT_TILES = 32;
    constexpr int MAX_BLEND_MAPS_SCANNED = 64;

    constexpr MapId HEIGHT_MAP{MapKind::Height, 0};
    constexpr MapId HOLE_MAP{MapKind::Hole, 0};

    struct OpNameRow {
        OpType type;
        const char* name;
    };

    constexpr OpNameRow OP_NAMES[] = {
        {OpType::Raise, "raise"}, {OpType::Lower, "lower"}, {OpType::SetHeight, "set_height"},
        {OpType::LiftFloors, "lift_floors"}, {OpType::FlattenPeaks, "flatten_peaks"}, {OpType::Smooth, "smooth"},
        {OpType::Sharpen, "sharpen"}, {OpType::AddNoise, "add_noise"}, {OpType::Terrace, "terrace"},
        {OpType::Stamp, "stamp"}, {OpType::Erode, "erode"}, {OpType::Ramp, "ramp"},
        {OpType::RemapHeights, "remap_heights"}, {OpType::PaintLayer, "paint_layer"}, {OpType::PaintBase, "paint_base"},
        {OpType::PaintByRules, "paint_by_rules"}, {OpType::RemapLayers, "remap_layers"}, {OpType::PaintDensity, "paint_density"},
        {OpType::EraseDensity, "erase_density"}, {OpType::CutHoles, "cut_holes"}, {OpType::FillHoles, "fill_holes"},
        {OpType::PlaceObjects, "place_objects"}, {OpType::EraseObjects, "erase_objects"}, {OpType::ImportHeightmap, "import_heightmap"}, {OpType::FillNoise, "fill_noise"},
    };

    // ------------------------------------------------------------------ the registry (R89)

    struct RegisteredOp {
        std::string name;
        OpValidateFn validate = nullptr;
        OpApplyFn apply = nullptr;
        RegisteredOpTraits traits;
    };

    std::mutex& registryMutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<RegisteredOp>& registry() {
        static std::vector<RegisteredOp> ops;
        return ops;
    }

    bool findRegistered(std::string_view name, RegisteredOp& out) {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (const RegisteredOp& op : registry()) {
            if (op.name == name) {
                out = op;
                return true;
            }
        }
        return false;
    }

    std::string opNameOf(const Op& op) {
        return op.type == OpType::Registered ? op.registeredName : std::string(opTypeName(op.type));
    }

    // ------------------------------------------------------------------ what an op targets

    bool isHeightOp(OpType type) {
        switch (type) {
            case OpType::Raise: case OpType::Lower: case OpType::SetHeight: case OpType::LiftFloors: case OpType::FlattenPeaks:
            case OpType::Smooth: case OpType::Sharpen: case OpType::AddNoise: case OpType::Terrace: case OpType::Stamp:
            case OpType::Erode: case OpType::Ramp: case OpType::RemapHeights: case OpType::FillNoise: case OpType::ImportHeightmap:
                return true;
            default:
                return false;
        }
    }

    bool isDensityOp(OpType type) {
        return type == OpType::PaintDensity || type == OpType::EraseDensity;
    }

    // T3 R27: what the map's holes open on the lattice of the default collider: a sample per height
    // texel of the smaller side, at most 1025 (JoltShapeBuilder::defaultHeightFieldSamples)
    int collisionSamplesOpen(const TerrainMapStore& store) {
        const MapFormat holes = store.format(HOLE_MAP);
        const MapFormat heights = store.format(HEIGHT_MAP);
        if (!holes.exists || !heights.exists) return 0;
        std::vector<uint8_t> texels(static_cast<size_t>(holes.bounds.area()));
        if (!store.readBytes(HOLE_MAP, holes.bounds, texels)) return 0;
        const int samples = std::clamp(std::min(heights.bounds.w, heights.bounds.h), 4, 1025);
        std::vector<uint8_t> open;
        std::vector<uint8_t> gaps;
        openSquares(HoleMapView{texels.data(), holes.bounds.w, holes.bounds.h}, samples, 1.0f, 1.0f, open);
        return noCollisionSamples(open, samples, gaps);
    }

    bool isHoleOp(OpType type) {
        return type == OpType::CutHoles || type == OpType::FillHoles;
    }

    // The map an op's shape is measured on: its own, or the height map's lattice for maps that may
    // not exist yet
    MapId latticeMapOf(const Op& op, const TerrainMapStore& store) {
        if (isDensityOp(op.type) && op.layer) {
            const MapId density{MapKind::Density, *op.layer};
            if (store.format(density).exists) return density;
        }
        if (op.type == OpType::PaintLayer && op.layer) {
            const MapId blend{MapKind::Blend, terrainLayerChannel(*op.layer).map};
            if (store.format(blend).exists) return blend;
        }
        if (op.type == OpType::PaintBase || op.type == OpType::PaintByRules || op.type == OpType::RemapLayers) {
            const MapId blend{MapKind::Blend, op.layer ? terrainLayerChannel(*op.layer).map : 0};
            if (store.format(blend).exists) return blend;
        }
        if (isHoleOp(op.type) && store.format(HOLE_MAP).exists) return HOLE_MAP;
        return HEIGHT_MAP;
    }

    // floor(value / step)
    int floorDivide(int value, int step) {
        return value >= 0 ? value / step : -((-value + step - 1) / step);
    }

    // The cells of a store's block grid a rectangle reaches: always the same cells for the same
    // texels, and one when the store's block holds the whole rectangle
    struct BlockGrid {
        int stepX = 1, stepY = 1;
        int firstX = 0, firstY = 0;
        TexelRect rect;

        BlockGrid(const TerrainMapStore& store, MapId map, const TexelRect& rect): rect(rect) {
            const TexelRect preferred = store.preferredBlock(map);
            stepX = preferred.w > 0 ? preferred.w : std::max(rect.w, 1);
            stepY = preferred.h > 0 ? preferred.h : std::max(rect.h, 1);
            firstX = preferred.x + floorDivide(rect.x - preferred.x, stepX) * stepX;
            firstY = preferred.y + floorDivide(rect.y - preferred.y, stepY) * stepY;
        }

        bool many() const {
            return firstX + stepX < rect.right() || firstY + stepY < rect.bottom();
        }
    };

    int blendMapCountOf(const TerrainMapStore& store) {
        int count = 0;
        while (count < MAX_BLEND_MAPS_SCANNED && store.format(MapId{MapKind::Blend, count}).exists) {
            count++;
        }
        return count;
    }

    MapFormat newMapFormat(const TerrainMapStore& store, const OpOptions& options, int channels) {
        MapFormat format = store.format(HEIGHT_MAP);
        if (options.newMapResolution > 0) {
            format.bounds = TexelRect{0, 0, options.newMapResolution, options.newMapResolution};
        }
        format.channels = channels;
        format.bitsPerChannel = 8;
        format.exists = true;
        return format;
    }

    std::string mapBoundsText(const LocalBounds& bounds) {
        return "[" + std::to_string(bounds.min.x) + ", " + std::to_string(bounds.min.y) + "] to [" +
               std::to_string(bounds.max.x) + ", " + std::to_string(bounds.max.y) + "]";
    }

    // ------------------------------------------------------------------ validation (R10)

    struct Validator {
        const TerrainMapStore& store;
        int index;
        std::vector<OpError>& errors;

        std::string prefix() const {
            return "ops[" + std::to_string(index) + "]";
        }

        bool fail(const std::string& field, const std::string& message) {
            errors.push_back(OpError{index, field.empty() ? prefix() : prefix() + "." + field, message});
            return false;
        }

        bool finite(float value) const {
            return value == value && value < std::numeric_limits<float>::infinity() && value > -std::numeric_limits<float>::infinity();
        }

        bool checkShape(const Op& op) {
            const Shape& shape = op.shape;
            switch (shape.type) {
                case ShapeType::All:
                    break;
                case ShapeType::Circle:
                    if (!(shape.radius > 0.0f) || !finite(shape.radius)) return fail("shape.radius", "must be above 0");
                    if (!finite(shape.center.x) || !finite(shape.center.y)) return fail("shape.center", "must be finite");
                    break;
                case ShapeType::Rect:
                    if (!finite(shape.min.x) || !finite(shape.min.y) || !finite(shape.max.x) || !finite(shape.max.y)) {
                        return fail("shape.min", "must be finite");
                    }
                    if (!(shape.max.x > shape.min.x) || !(shape.max.y > shape.min.y)) return fail("shape.max", "must be above shape.min");
                    break;
                case ShapeType::Polyline:
                    if (shape.points.empty()) return fail("shape.points", "needs at least one point");
                    if (shape.closed && shape.radius <= 0.0f) {
                        if (shape.points.size() < 3) return fail("shape.points", "a polygon needs at least three points");
                    } else if (!(shape.radius > 0.0f)) {
                        return fail("shape.radius", "must be above 0 for an open polyline");
                    }
                    break;
                case ShapeType::Mask:
                    if (shape.maskPixels.empty() || shape.maskWidth <= 0 || shape.maskHeight <= 0 ||
                        shape.maskPixels.size() < static_cast<size_t>(shape.maskWidth) * shape.maskHeight) {
                        return fail("shape.path", "the mask has no pixels");
                    }
                    if (!(shape.size.x > 0.0f) || !(shape.size.y > 0.0f)) return fail("shape.size", "must be above 0");
                    break;
                case ShapeType::Stroke:
                    if (shape.points.empty()) return fail("shape.points", "needs at least one point");
                    if (!(shape.radius > 0.0f)) return fail("shape.radius", "must be above 0");
                    if (!shape.pressures.empty() && shape.pressures.size() != shape.points.size()) {
                        return fail("shape.points", "needs a pressure per point, or none");
                    }
                    if (!shape.times.empty() && shape.times.size() != shape.points.size()) {
                        return fail("shape.times", "needs a time per point, or none");
                    }
                    break;
            }
            if (shape.feather > 1.0f) return fail("shape.feather", "must be at most 1");
            return true;
        }

        bool checkFilter(const Op& op) {
            const Filter& filter = op.filter;
            if (filter.slope && !(filter.slope->max >= filter.slope->min)) return fail("filter.slope", "max must not be below min");
            if (filter.height && !(filter.height->max >= filter.height->min)) return fail("filter.height", "max must not be below min");
            if (filter.layer) {
                if (filter.layer->index < 0) return fail("filter.layer.index", "must not be negative");
                if (!(filter.layer->max >= filter.layer->min)) return fail("filter.layer", "max must not be below min");
            }
            if (!(filter.softness >= 0.0f && filter.softness <= 1.0f)) return fail("filter.softness", "must be within 0 and 1");
            return true;
        }

        bool heightOpNeeds(const Op& op) {
            if (!store.format(HEIGHT_MAP).exists) return fail("", std::string(opTypeName(op.type)) + " needs a height map, and the terrain has none");
            if (store.frame().maxHeight == 0.0f && op.type != OpType::FillNoise) return fail("", "the terrain's maxHeight is 0");
            return true;
        }

        bool checkFields(const Op& op) {
            if (!(op.strength >= 0.0f && op.strength <= 1.0f)) return fail("strength", "must be within 0 and 1");
            const bool stroke = op.shape.type == ShapeType::Stroke;
            switch (op.type) {
                case OpType::Raise:
                case OpType::Lower:
                    if (!heightOpNeeds(op)) return false;
                    if (!op.delta && !stroke) return fail("delta", "is required outside a stroke");
                    break;
                case OpType::SetHeight:
                case OpType::LiftFloors:
                case OpType::FlattenPeaks:
                    if (!heightOpNeeds(op)) return false;
                    if (!op.height) return fail("height", "is required");
                    break;
                case OpType::Smooth:
                case OpType::Sharpen:
                case OpType::Erode:
                    if (!heightOpNeeds(op)) return false;
                    if (op.iterations < 1) return fail("iterations", "must be at least 1");
                    break;
                case OpType::AddNoise:
                    if (!heightOpNeeds(op)) return false;
                    if (!op.amplitude) return fail("amplitude", "is required");
                    if (!(op.noiseSize > 0.0f)) return fail("noise_size", "must be above 0");
                    if (op.octaves < 1 || op.octaves > 8) return fail("octaves", "must be within 1 and 8");
                    break;
                case OpType::Terrace:
                    if (!heightOpNeeds(op)) return false;
                    if (op.steps < 2) return fail("steps", "must be at least 2");
                    break;
                case OpType::Stamp:
                    if (!heightOpNeeds(op)) return false;
                    if (op.shape.type != ShapeType::Mask) return fail("shape.type", "stamp takes a mask");
                    if (!op.delta) return fail("delta", "is required");
                    break;
                case OpType::Ramp:
                    if (!heightOpNeeds(op)) return false;
                    if (op.shape.type != ShapeType::Polyline || op.shape.points.size() != 2 || op.shape.closed) {
                        return fail("shape", "ramp takes an open polyline of 2 points");
                    }
                    break;
                case OpType::RemapHeights:
                    if (!heightOpNeeds(op)) return false;
                    if (!op.inRange) return fail("in_range", "is required");
                    if (!op.outRange) return fail("out_range", "is required");
                    if (!(op.inRange->max > op.inRange->min)) return fail("in_range", "max must be above min");
                    break;
                case OpType::ImportHeightmap: {
                    if (!heightOpNeeds(op)) return false;
                    if (op.shape.type != ShapeType::All) return fail("shape", "import_heightmap replaces the whole map and takes no shape");
                    if (op.importedWidth <= 0 || op.importedHeight <= 0 ||
                        op.importedHeights.size() != static_cast<size_t>(op.importedWidth) * op.importedHeight) {
                        return fail("path", "the import holds no heights of its stated size");
                    }
                    const TexelRect bounds = store.format(HEIGHT_MAP).bounds;
                    if (op.importedWidth != bounds.w || op.importedHeight != bounds.h) {
                        return fail("resolution", "the import is " + std::to_string(op.importedWidth) + " x " + std::to_string(op.importedHeight) +
                                    " and the height map " + std::to_string(bounds.w) + " x " + std::to_string(bounds.h) +
                                    ": another size replaces the map before the op");
                    }
                    break;
                }
                case OpType::FillNoise:
                    if (!heightOpNeeds(op)) return false;
                    if (op.fill != FillMode::Flat && !op.amplitude) return fail("amplitude", "is required");
                    if (op.octaves < 1 || op.octaves > 8) return fail("octaves", "must be within 1 and 8");
                    break;
                case OpType::PaintLayer:
                    if (!op.layer) return fail("layer", "is required");
                    if (*op.layer < 0) return fail("layer", "must not be negative");
                    if (!blendTargetExists(terrainLayerChannel(*op.layer).map)) return false;
                    break;
                case OpType::PaintBase:
                    if (op.layer && *op.layer < 0) return fail("layer", "must not be negative");
                    if (op.layer && !blendTargetExists(terrainLayerChannel(*op.layer).map)) return false;
                    if (!op.layer && blendMapCountOf(store) == 0) return fail("", "the terrain has no blend map to clear");
                    break;
                case OpType::PaintByRules: {
                    if (op.rules.empty()) return fail("rules", "needs at least one rule");
                    if (!store.format(HEIGHT_MAP).exists) return fail("", "paint_by_rules reads the height map, and the terrain has none");
                    for (size_t r = 0; r < op.rules.size(); r++) {
                        const PaintRule& rule = op.rules[r];
                        const std::string where = "rules[" + std::to_string(r) + "]";
                        if (rule.layer < -1) return fail(where + ".layer", "must be -1 (the base) or a layer");
                        if (rule.layer >= 0 && !blendTargetExists(terrainLayerChannel(rule.layer).map)) return false;
                        if (rule.slope && !(rule.slope->max >= rule.slope->min)) return fail(where + ".slope", "max must not be below min");
                        if (rule.height && !(rule.height->max >= rule.height->min)) return fail(where + ".height", "max must not be below min");
                        if (!(rule.opacity >= 0.0f && rule.opacity <= 1.0f)) return fail(where + ".opacity", "must be within 0 and 1");
                        if (!(rule.noise >= 0.0f && rule.noise <= 1.0f)) return fail(where + ".noise", "must be within 0 and 1");
                    }
                    break;
                }
                case OpType::RemapLayers: {
                    if (op.order.empty()) return fail("order", "needs one new index per layer");
                    std::vector<char> seen(op.order.size(), 0);
                    for (int target : op.order) {
                        if (target < 0 || target >= static_cast<int>(op.order.size()) || seen[target]) {
                            return fail("order", "must hold each index from 0 to " + std::to_string(op.order.size() - 1) + " once");
                        }
                        seen[target] = 1;
                    }
                    break;
                }
                case OpType::PaintDensity:
                case OpType::EraseDensity: {
                    if (!op.layer) return fail("layer", "is required");
                    if (*op.layer < 0) return fail("layer", "must not be negative");
                    const MapId density{MapKind::Density, *op.layer};
                    if (!store.format(density).exists) {
                        std::string why;
                        if (!store.canCreateMap(density, &why)) return fail("layer", why.empty() ? "no such foliage layer" : why);
                        if (!store.format(HEIGHT_MAP).exists) return fail("", "a new density map takes the height map's size, and the terrain has none");
                    }
                    break;
                }
                case OpType::CutHoles:
                case OpType::FillHoles:
                    if (op.strength != 1.0f) return fail("strength", "holes have no strength");
                    if (op.shape.feather >= 0.0f || op.shape.falloff != Falloff::Smooth) return fail("shape.falloff", "holes have no falloff");
                    if (!store.format(HEIGHT_MAP).exists && !store.format(HOLE_MAP).exists) {
                        return fail("", "holes need a height map, and the terrain has none");
                    }
                    break;
                case OpType::PlaceObjects:
                    if (!store.format(HEIGHT_MAP).exists) return fail("", "place_objects needs a height map, and the terrain has none");
                    if (op.shape.type == ShapeType::All) return fail("shape.type", "place_objects needs a region");
                    if (!(op.spacing >= 0.0f)) return fail("spacing", "must not be negative");
                    if (op.count < 1) return fail("count", "must be at least 1");
                    break;
                case OpType::EraseObjects:
                    if (!store.format(HEIGHT_MAP).exists) return fail("", "erase_objects needs a height map, and the terrain has none");
                    break;
                case OpType::Registered: {
                    RegisteredOp registered;
                    if (!findRegistered(op.registeredName, registered)) return fail("op", "unknown operation \"" + op.registeredName + "\"");
                    OpError error;
                    if (registered.validate && !registered.validate(store, op, error)) {
                        return fail(error.field, error.message);
                    }
                    break;
                }
            }
            return true;
        }

        bool blendTargetExists(int map) {
            const MapId blend{MapKind::Blend, map};
            if (store.format(blend).exists) return true;
            std::string why;
            if (!store.canCreateMap(blend, &why)) return fail("layer", why.empty() ? "blend map " + std::to_string(map) + " cannot be made" : why);
            if (!store.format(HEIGHT_MAP).exists) return fail("", "a new blend map takes the height map's size, and the terrain has none");
            return true;
        }

        // R10's bounds error, and R86's refusal before anything runs
        bool checkPlace(const Op& op) {
            if (op.type == OpType::Registered || op.shape.type == ShapeType::All) return true;
            if (op.type == OpType::RemapLayers) return true;
            const MapId map = latticeMapOf(op, store);
            const MapFormat format = store.format(map);
            if (!format.exists) return true;
            const ShapeEvaluator evaluator(op.shape);
            const TexelRect rect = shapeTexels(store, map, format, evaluator);
            if (rect.empty()) {
                return fail("shape", "lies wholly outside the terrain, whose bounds are " +
                                     mapBoundsText(localBoundsOf(latticeOf(store, map, format), format)));
            }
            std::string why;
            if (store.format(map).exists && !store.canWrite(map, rect, &why)) {
                return fail("shape", why.empty() ? "the store refused this rectangle" : why);
            }
            return true;
        }

        bool check(const Op& op) {
            return checkShape(op) && checkFilter(op) && checkFields(op) && checkPlace(op);
        }
    };

    // ------------------------------------------------------------------ running

    // What one op's kernel adds up, row by row
    struct Counters {
        int64_t touched = 0;
        int64_t changed = 0;
        int64_t clippedLow = 0;
        int64_t clippedHigh = 0;
        int64_t filteredOut = 0;
        // heights only: the changed texels, normalized
        float minBefore = std::numeric_limits<float>::max();
        float maxBefore = std::numeric_limits<float>::lowest();
        float minAfter = std::numeric_limits<float>::max();
        float maxAfter = std::numeric_limits<float>::lowest();
        double sumDelta = 0.0;
        float maxDelta = 0.0f;

        void add(const Counters& other) {
            touched += other.touched;
            changed += other.changed;
            clippedLow += other.clippedLow;
            clippedHigh += other.clippedHigh;
            filteredOut += other.filteredOut;
            minBefore = std::min(minBefore, other.minBefore);
            maxBefore = std::max(maxBefore, other.maxBefore);
            minAfter = std::min(minAfter, other.minAfter);
            maxAfter = std::max(maxAfter, other.maxAfter);
            sumDelta += other.sumDelta;
            maxDelta = std::max(maxDelta, other.maxDelta);
        }
    };

    // What a kernel sees of a block
    struct Block {
        MapId map;
        MapFormat format;
        MapLattice lattice;
        TexelRect rect;         // the texels written
        TexelRect source;       // rect plus the kernel's margin, clipped to the map
        const float* values = nullptr;  // source, interleaved
        int channels = 1;

        const float* at(int x, int y) const {
            return values + (static_cast<size_t>(y - source.y) * source.w + (x - source.x)) * channels;
        }
        float scalarClamped(int x, int y) const {
            return *at(std::clamp(x, source.x, source.right() - 1), std::clamp(y, source.y, source.bottom() - 1));
        }
    };

    class Run final : public OpContext {
    public:
        Run(TerrainMapStore& target, const OpOptions& options, TerrainOpResult& result): target(target), options(options), result(result) {}

        // OpContext
        TerrainMapStore& store() override { return target; }
        bool dryRun() const override { return options.dryRun; }
        bool parallel() const override { return options.parallel; }
        void warn(const std::string& code, const std::string& message) override {
            result.warnings.push_back(OpWarning{currentOp, code, message});
        }
        void addUndoHook(std::unique_ptr<TerrainStoreUndoHook> hook) override {
            result.undoHooks.push_back(std::shared_ptr<TerrainStoreUndoHook>(std::move(hook)));
        }
        void fail(const std::string& message) override {
            if (error.empty()) error = message;
        }

        bool failed() const { return !error.empty(); }
        const std::string& failure() const { return error; }

        void applyOp(int index, const Op& op, OpEntry& entry);

        Counters total;     // over the list, for the stats of a run without an overlay

    private:
        // A block's pre-op values for whoever reads across its border later (R6, R87): with a
        // store's small blocks, the block above must not leak its result into a kernel's margin
        struct Saved {
            MapId map;
            TexelRect rect;
            std::vector<float> values;
            int channels;
        };

        bool readPreOp(MapId map, TexelRect rect, std::span<float> out);
        void saveBorders(const Block& block, int margin);
        void forgetBorders(int top, int margin);
        static bool readPreOpThunk(void* self, MapId map, TexelRect rect, std::span<float> out);

        bool ensureMap(MapId map, int channels, OpEntry& entry);
        void writeBlock(const Block& block, std::span<const float> out);

        // Runs texel over every sample of the shape, by blocks. margin: what the kernel reads around a
        // texel. gain multiplies the weight. forced: one texel to hit whatever the shape says (a brush
        // smaller than a texel), weighed by what the shape gives at its centre. useFalloff: weigh by
        // the falloff alone (stamp reads the mask itself). prepare: called with each block before its
        // rows run.
        template<typename Prepare, typename Texel>
        Counters forEachSample(const Op& op, MapId map, const ShapeEvaluator& shape, float gain, int margin, bool useFalloff,
                               const TexelRect* forced, const ShapeSample& forcedSample, Prepare&& prepare, Texel&& texel);

        // One application of a scalar or blend kernel on its map, with the fallback for a sub-texel brush
        template<typename Texel>
        void applyKernel(const Op& op, MapId map, const Shape& shapeValue, float gain, int margin, bool useFalloff, OpEntry& entry, Texel&& texel);

        // The same over a stroke's stamps, or once for any other shape
        template<typename Texel>
        void applyShaped(const Op& op, MapId map, float flow, int margin, bool useFalloff, OpEntry& entry, Texel&& texel);

        void applyHeightOp(const Op& op, OpEntry& entry);
        void applySmooth(const Op& op, OpEntry& entry);
        void applyErode(const Op& op, const Shape& shape, float seconds, float strength, OpEntry& entry);
        void applyFillNoise(const Op& op, OpEntry& entry);
        void applyImport(const Op& op, OpEntry& entry);
        void applyPaintLayer(const Op& op, OpEntry& entry);
        void applyPaintBase(const Op& op, OpEntry& entry);
        void applyPaintByRules(const Op& op, OpEntry& entry);
        void applyRemapLayers(const Op& op, OpEntry& entry);
        void applyDensity(const Op& op, OpEntry& entry);
        void applyHoles(const Op& op, OpEntry& entry);
        void applyPlace(const Op& op, OpEntry& entry);
        void applyErase(const Op& op, OpEntry& entry);

        void finishEntry(const Counters& counters, OpEntry& entry, const Op& op);
        std::vector<StrokeStamp> stampsOf(const Op& op, MapId map);
        void pairHeights(float before, float after, Counters& counters) const;

        TerrainMapStore& target;
        const OpOptions& options;
        TerrainOpResult& result;
        std::string error;
        int currentOp = -1;
        std::vector<Saved> saved;
        float erodeCarry = 0.0f;
        uint32_t erodeSeed = 0;
    };

    bool Run::readPreOpThunk(void* self, MapId map, TexelRect rect, std::span<float> out) {
        return static_cast<Run*>(self)->readPreOp(map, rect, out);
    }

    bool Run::readPreOp(MapId map, TexelRect rect, std::span<float> out) {
        if (!target.read(map, rect, out)) return false;
        for (const Saved& entry : saved) {
            if (entry.map != map) continue;
            const TexelRect part = intersect(entry.rect, rect);
            for (int y = part.y; y < part.bottom(); y++) {
                std::memcpy(out.data() + (static_cast<size_t>(y - rect.y) * rect.w + (part.x - rect.x)) * entry.channels,
                            entry.values.data() + (static_cast<size_t>(y - entry.rect.y) * entry.rect.w + (part.x - entry.rect.x)) * entry.channels,
                            static_cast<size_t>(part.w) * entry.channels * sizeof(float));
            }
        }
        return true;
    }

    void Run::saveBorders(const Block& block, int margin) {
        auto keep = [&](const TexelRect& strip) {
            if (strip.empty()) return;
            Saved entry;
            entry.map = block.map;
            entry.rect = strip;
            entry.channels = block.channels;
            entry.values.resize(static_cast<size_t>(strip.area()) * block.channels);
            for (int y = strip.y; y < strip.bottom(); y++) {
                std::memcpy(entry.values.data() + static_cast<size_t>(y - strip.y) * strip.w * block.channels, block.at(strip.x, y),
                            static_cast<size_t>(strip.w) * block.channels * sizeof(float));
            }
            saved.push_back(std::move(entry));
        };
        const TexelRect& rect = block.rect;
        const int rows = std::min(margin, rect.h);
        const int columns = std::min(margin, rect.w);
        keep(TexelRect{rect.x, rect.bottom() - rows, rect.w, rows});
        keep(TexelRect{rect.right() - columns, rect.y, columns, rect.h - rows});
    }

    void Run::forgetBorders(int top, int margin) {
        saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const Saved& entry) { return entry.rect.bottom() + margin <= top; }), saved.end());
    }

    bool Run::ensureMap(MapId map, int channels, OpEntry& entry) {
        if (target.format(map).exists) return true;
        const WriteResult made = target.createMap(map, newMapFormat(target, options, channels), 0.0f);
        if (!made.ok) {
            fail(made.error);
            return false;
        }
        result.tilesCreated += made.tilesCreated;
        entry.mapCreated = true;
        warn("map_created", "This op created " + std::string(map.kind == MapKind::Blend ? "blend" : "density") + " map " + std::to_string(map.index) + ".");
        return true;
    }

    void Run::writeBlock(const Block& block, std::span<const float> out) {
        const WriteResult wrote = target.write(block.map, block.rect, out);
        if (!wrote.ok) {
            fail(wrote.error);
            return;
        }
        result.tilesCreated += wrote.tilesCreated;
    }

    void Run::pairHeights(float before, float after, Counters& counters) const {
        counters.minBefore = std::min(counters.minBefore, before);
        counters.maxBefore = std::max(counters.maxBefore, before);
        counters.minAfter = std::min(counters.minAfter, after);
        counters.maxAfter = std::max(counters.maxAfter, after);
        const float delta = after - before;
        counters.sumDelta += delta;
        counters.maxDelta = std::max(counters.maxDelta, std::fabs(delta));
    }

    template<typename Prepare, typename Texel>
    Counters Run::forEachSample(const Op& op, MapId map, const ShapeEvaluator& shape, float gain, int margin, bool useFalloff,
                                const TexelRect* forced, const ShapeSample& forcedSample, Prepare&& prepare, Texel&& texel) {
        Counters counters;
        const MapFormat format = target.format(map);
        if (!format.exists) return counters;

        const MapLattice lattice = latticeOf(target, map, format);
        const TexelRect rect = forced ? *forced : shapeTexels(target, map, format, shape);
        if (rect.empty()) return counters;

        const int channels = floatChannels(map, format);
        const BlockGrid grid(target, map, rect);
        const bool manyBlocks = grid.many();
        // the filter reads heights one texel past a block for its forward difference
        const int border = std::max(margin, op.filter.active() ? 2 : 0);
        saved.clear();

        std::vector<float> source;
        std::vector<float> weights;
        std::vector<float> out;

        for (int cellY = grid.firstY; cellY < rect.bottom() && !failed(); cellY += grid.stepY) {
            if (manyBlocks) forgetBorders(cellY, border);
            for (int cellX = grid.firstX; cellX < rect.right() && !failed(); cellX += grid.stepX) {
                const TexelRect blockRect = intersect(TexelRect{cellX, cellY, grid.stepX, grid.stepY}, rect);
                if (blockRect.empty()) continue;

                Block block;
                block.map = map;
                block.format = format;
                block.lattice = lattice;
                block.rect = blockRect;
                block.source = intersect(grow(blockRect, margin), format.bounds);
                block.channels = channels;

                source.resize(static_cast<size_t>(block.source.area()) * channels);
                if (!readPreOp(map, block.source, source)) {
                    fail("could not read the map");
                    return counters;
                }
                block.values = source.data();

                const bool filtered = op.filter.active();
                if (filtered) {
                    weights.resize(static_cast<size_t>(blockRect.area()));
                    evaluateFilter(target, op.filter, lattice, blockRect, weights, &Run::readPreOpThunk, this);
                }

                prepare(block);

                out.resize(static_cast<size_t>(blockRect.area()) * channels);
                const bool heights = map.kind == MapKind::Height;

                std::vector<Counters> rowCounters(static_cast<size_t>(blockRect.h));
                auto row = [&](size_t rowIndex) {
                    Counters& rowCount = rowCounters[rowIndex];
                    const int y = blockRect.y + static_cast<int>(rowIndex);
                    const float z = lattice.z(y);
                    const size_t rowStart = rowIndex * blockRect.w;
                    // the row's samples in one call; a worker keeps its buffer from row to row
                    thread_local std::vector<ShapeSample> rowSamples;
                    if (!forced) {
                        rowSamples.resize(static_cast<size_t>(blockRect.w));
                        shape.sampleRow(lattice, blockRect.x, blockRect.w, z, rowSamples.data());
                    }
                    for (int x = blockRect.x; x < blockRect.right(); x++) {
                        const size_t column = static_cast<size_t>(x - blockRect.x);
                        const float* current = block.at(x, y);
                        float* next = out.data() + (rowStart + column) * channels;
                        for (int c = 0; c < channels; c++) {
                            next[c] = current[c];
                        }

                        const ShapeSample& sample = forced ? forcedSample : rowSamples[column];
                        const float base = useFalloff ? sample.falloff : sample.weight;
                        if (base <= 0.0f) continue;
                        rowCount.touched++;

                        const float filterWeight = filtered ? weights[rowStart + column] : 1.0f;
                        if (filterWeight <= 0.0f) {
                            rowCount.filteredOut++;
                            continue;
                        }
                        const float weight = detClamp01(base * filterWeight * gain);
                        texel(block, x, y, sample, weight, current, next, rowCount);

                        bool changed = false;
                        for (int c = 0; c < channels; c++) {
                            if (quantizeChannel(current[c], format.bitsPerChannel) != quantizeChannel(next[c], format.bitsPerChannel)) {
                                changed = true;
                            }
                        }
                        if (changed) {
                            rowCount.changed++;
                            if (heights) pairHeights(current[0], next[0], rowCount);
                        }
                    }
                };
                if (options.parallel && blockRect.area() >= PARALLEL_TEXELS) {
                    jobs::parallelFor(static_cast<size_t>(blockRect.h), row);
                } else {
                    for (size_t rowIndex = 0; rowIndex < static_cast<size_t>(blockRect.h); rowIndex++) {
                        row(rowIndex);
                    }
                }
                for (const Counters& rowCount : rowCounters) {
                    counters.add(rowCount);
                }

                if (manyBlocks) saveBorders(block, border);
                writeBlock(block, out);
            }
        }
        saved.clear();
        return counters;
    }

    template<typename Texel>
    void Run::applyKernel(const Op& op, MapId map, const Shape& shapeValue, float gain, int margin, bool useFalloff, OpEntry& entry, Texel&& texel) {
        const ShapeEvaluator shape(shapeValue);
        auto noPreparation = [](const Block&) {};
        Counters counters = forEachSample(op, map, shape, gain, margin, useFalloff, nullptr, ShapeSample{}, noPreparation, texel);

        // A brush smaller than a texel can miss every sample: the texel nearest its centre, inside
        // the box it reaches, still gets the stamp, so tiny brushes keep working
        const bool round = shapeValue.type == ShapeType::Circle || shapeValue.type == ShapeType::Stroke || shapeValue.nearestTexel;
        if (counters.touched == 0 && round && !failed()) {
            const MapFormat format = target.format(map);
            const TexelRect reach = format.exists ? shapeTexels(target, map, format, shape) : TexelRect{};
            if (!reach.empty()) {
                const Vector2 center = target.texelOfWorld(map, shapeValue.center.x, shapeValue.center.y);
                const TexelRect forced{std::clamp(static_cast<int>(std::round(center.x)), reach.x, reach.right() - 1),
                                       std::clamp(static_cast<int>(std::round(center.y)), reach.y, reach.bottom() - 1), 1, 1};
                counters = forEachSample(op, map, shape, gain, margin, useFalloff, &forced, shape.sample(shapeValue.center.x, shapeValue.center.y),
                                         noPreparation, texel);
                warn("brush_under_one_texel", "The brush is smaller than a texel: it reached only the nearest one.");
            }
        }
        finishEntry(counters, entry, op);
    }

    std::vector<StrokeStamp> Run::stampsOf(const Op& op, MapId map) {
        const Shape& shape = op.shape;
        const MapFormat format = target.format(map);
        const MapLattice lattice = latticeOf(target, map, format);
        const float spacing = shape.spacing > 0.0f ? shape.spacing : StrokeResampler::defaultSpacing(shape.radius, std::fabs(lattice.stepX));
        StrokeResampler resampler(spacing);
        std::vector<StrokeStamp> stamps;
        for (size_t i = 0; i < shape.points.size(); i++) {
            StrokeSample sample;
            sample.x = shape.points[i].x;
            sample.z = shape.points[i].y;
            if (!shape.pressures.empty()) sample.pressure = shape.pressures[i];
            if (!shape.times.empty()) sample.time = shape.times[i];
            resampler.add(sample, stamps);
        }
        return stamps;
    }

    // Each stamp of a stroke is a circle that accumulates as a person's drag does (R5, R34); its
    // filter sees the state before that stamp (R6)
    template<typename Texel>
    void Run::applyShaped(const Op& op, MapId map, float flow, int margin, bool useFalloff, OpEntry& entry, Texel&& texel) {
        if (op.shape.type != ShapeType::Stroke) {
            applyKernel(op, map, op.shape, op.strength, margin, useFalloff, entry, texel);
            return;
        }
        Shape stamp = op.shape;
        stamp.type = ShapeType::Circle;
        for (const StrokeStamp& at : stampsOf(op, map)) {
            if (failed()) return;
            stamp.center = Vector2(at.x, at.z);
            stamp.radius = op.shape.pressureSize ? op.shape.radius * at.pressure : op.shape.radius;
            const float pressure = op.shape.pressureStrength ? at.pressure : 1.0f;
            if (stamp.radius <= 0.0f) continue;
            applyKernel(op, map, stamp, op.strength * pressure * flow * STAMP_SECONDS, margin, useFalloff, entry, texel);
        }
    }

    void Run::finishEntry(const Counters& counters, OpEntry& entry, const Op&) {
        entry.changedTexels += counters.changed;
        entry.clippedLow += counters.clippedLow;
        entry.clippedHigh += counters.clippedHigh;
        entry.filteredOut += counters.filteredOut;
        total.add(counters);
    }

    void Run::applyHeightOp(const Op& op, OpEntry& entry) {
        const float maxHeight = target.frame().maxHeight;
        const float toNormalized = maxHeight != 0.0f ? 1.0f / maxHeight : 0.0f;

        auto store = [](float value, float* next, Counters& counters) {
            if (value < 0.0f) counters.clippedLow++;
            else if (value > 1.0f) counters.clippedHigh++;
            next[0] = detClamp01(value);
        };

        const MapId map = HEIGHT_MAP;
        switch (op.type) {
            case OpType::Raise:
            case OpType::Lower: {
                // A stroke's stamp without a delta deposits as a brush does: its weight in map units
                const float amount = (op.delta ? *op.delta * toNormalized : 1.0f) * (op.type == OpType::Lower ? -1.0f : 1.0f);
                applyShaped(op, map, HEIGHT_FLOW, 0, false, entry, [&](const Block&, int, int, const ShapeSample&, float w, const float* cur, float* next, Counters& c) {
                    store(cur[0] + amount * w, next, c);
                });
                break;
            }
            case OpType::SetHeight:
            case OpType::LiftFloors:
            case OpType::FlattenPeaks: {
                const float goal = *op.height * toNormalized;
                const bool onlyBelow = op.type == OpType::LiftFloors;
                const bool onlyAbove = op.type == OpType::FlattenPeaks;
                applyShaped(op, map, HEIGHT_FLOW, 0, false, entry, [&](const Block&, int, int, const ShapeSample&, float w, const float* cur, float* next, Counters& c) {
                    if ((onlyBelow && cur[0] >= goal) || (onlyAbove && cur[0] <= goal)) return;
                    store(cur[0] + (goal - cur[0]) * w, next, c);
                });
                break;
            }
            case OpType::AddNoise: {
                const float amplitude = *op.amplitude * toNormalized;
                const float noiseSize = op.noiseSize;
                const uint32_t seed = op.seed;
                const int octaves = op.octaves;
                entry.seed = seed;
                applyShaped(op, map, HEIGHT_FLOW, 0, false, entry, [&](const Block& block, int x, int y, const ShapeSample&, float w, const float* cur, float* next, Counters& c) {
                    // One noise cell every noise size, so the field stays put as a brush moves
                    const float fieldX = static_cast<float>(x - block.format.bounds.x) * std::fabs(block.lattice.stepX) / noiseSize;
                    const float fieldZ = static_cast<float>(y - block.format.bounds.y) * std::fabs(block.lattice.stepZ) / noiseSize;
                    store(cur[0] + noise::compat::fractal(fieldX, fieldZ, seed, octaves) * amplitude * w, next, c);
                });
                break;
            }
            case OpType::Terrace: {
                const float intervals = static_cast<float>(op.steps - 1);
                applyShaped(op, map, HEIGHT_FLOW, 0, false, entry, [&](const Block&, int, int, const ShapeSample&, float w, const float* cur, float* next, Counters& c) {
                    store(cur[0] + (std::round(cur[0] * intervals) / intervals - cur[0]) * w, next, c);
                });
                break;
            }
            case OpType::Stamp: {
                // The mask is the relief here, not the weight
                const float amount = *op.delta * toNormalized;
                applyKernel(op, map, op.shape, op.strength, 0, true, entry, [&](const Block&, int, int, const ShapeSample& sample, float w, const float* cur, float* next, Counters& c) {
                    store(cur[0] + sample.mask * amount * w, next, c);
                });
                break;
            }
            case OpType::Ramp: {
                const MapFormat format = target.format(map);
                const MapLattice lattice = latticeOf(target, map, format);
                const Vector2 a = op.shape.points[0];
                const Vector2 b = op.shape.points[1];
                float atA = 0.0f;
                float atB = 0.0f;
                if (op.heights) {
                    atA = op.heights->min * toNormalized;
                    atB = op.heights->max * toNormalized;
                } else {
                    // Both ends are read the same way, or they disagree by a texel; only the texels
                    // around each, however far apart they are
                    auto surfaceAt = [&](const Vector2& point) {
                        HeightWindow window;
                        window.bounds = format.bounds;
                        window.lattice = lattice;
                        window.invStepX = lattice.stepX != 0.0f ? 1.0f / lattice.stepX : 1.0f;
                        window.invStepZ = lattice.stepZ != 0.0f ? 1.0f / lattice.stepZ : 1.0f;
                        const Vector2 texel = target.texelOfWorld(map, point.x, point.y);
                        const int x = std::clamp(static_cast<int>(std::floor(texel.x)), format.bounds.x, format.bounds.right() - 1);
                        const int y = std::clamp(static_cast<int>(std::floor(texel.y)), format.bounds.y, format.bounds.bottom() - 1);
                        window.rect = intersect(TexelRect{x, y, 2, 2}, format.bounds);
                        float corners[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        if (!target.read(map, window.rect, corners)) fail("could not read the map");
                        window.values = corners;
                        return sampleWindow(window, point.x, point.y);
                    };
                    atA = surfaceAt(a);
                    atB = surfaceAt(b);
                    if (failed()) return;
                }
                applyKernel(op, map, op.shape, op.strength, 0, false, entry, [&](const Block&, int, int, const ShapeSample& sample, float w, const float* cur, float* next, Counters& c) {
                    store(cur[0] + (atA + (atB - atA) * sample.along - cur[0]) * w, next, c);
                });
                break;
            }
            case OpType::RemapHeights: {
                const float inLow = op.inRange->min * toNormalized;
                const float inSpan = (op.inRange->max - op.inRange->min) * toNormalized;
                const float outLow = op.outRange->min * toNormalized;
                const float outSpan = (op.outRange->max - op.outRange->min) * toNormalized;
                applyKernel(op, map, op.shape, op.strength, 0, false, entry, [&](const Block&, int, int, const ShapeSample&, float w, const float* cur, float* next, Counters& c) {
                    const float goal = outLow + (cur[0] - inLow) / inSpan * outSpan;
                    store(cur[0] + (goal - cur[0]) * w, next, c);
                });
                break;
            }
            default:
                break;
        }
    }

    void Run::applySmooth(const Op& op, OpEntry& entry) {
        const bool sharpen = op.type == OpType::Sharpen;
        const MapFormat format = target.format(HEIGHT_MAP);
        const MapLattice lattice = latticeOf(target, HEIGHT_MAP, format);
        const ShapeEvaluator shape(op.shape);
        const float texelX = std::fabs(lattice.stepX) > 0.0f ? std::fabs(lattice.stepX) : 1.0f;
        const float texelZ = std::fabs(lattice.stepZ) > 0.0f ? std::fabs(lattice.stepZ) : 1.0f;
        const float radius = shape.characteristicRadius();
        // TEW:1837-1847: a radius-scaled kernel, so a big brush smooths big features
        const int step = std::max(1, static_cast<int>(std::round(std::min(radius / texelX, radius / texelZ) * 0.25f)));

        for (int iteration = 0; iteration < op.iterations && !failed(); iteration++) {
            applyShaped(op, HEIGHT_MAP, HEIGHT_FLOW, step, false, entry, [&](const Block& block, int x, int y, const ShapeSample&, float w, const float* cur, float* next, Counters&) {
                float sum = 0.0f;
                for (int oy = -1; oy <= 1; oy++) {
                    for (int ox = -1; ox <= 1; ox++) {
                        sum += block.scalarClamped(x + ox * step, y + oy * step);
                    }
                }
                const float blurred = sum / 9.0f;
                // Sharpen is the same kernel pushed the other way
                next[0] = detClamp01(sharpen ? cur[0] + (cur[0] - blurred) * w : cur[0] + (blurred - cur[0]) * w);
            });
        }
    }

    void Run::applyErode(const Op& op, const Shape& shapeValue, float seconds, float strength, OpEntry& entry) {
        const ShapeEvaluator shape(shapeValue);
        const MapFormat format = target.format(HEIGHT_MAP);
        const MapLattice lattice = latticeOf(target, HEIGHT_MAP, format);
        const float maxHeight = target.frame().maxHeight;
        const float texelSize = std::fabs(lattice.stepX) > 0.0f ? std::fabs(lattice.stepX) : 1.0f;
        const float heightScale = texelSize > std::numeric_limits<float>::epsilon() ? maxHeight / texelSize : 1.0f;
        const TexelRect rect = shapeTexels(target, HEIGHT_MAP, format, shape);
        if (rect.empty()) return;

        const BlockGrid grid(target, HEIGHT_MAP, rect);

        Counters counters;
        std::vector<float> window;
        std::vector<float> weights;
        std::vector<float> out;
        const bool filtered = op.filter.active();

        for (int y0 = grid.firstY; y0 < rect.bottom() && !failed(); y0 += grid.stepY) {
            for (int x0 = grid.firstX; x0 < rect.right() && !failed(); x0 += grid.stepX) {
                const TexelRect block = intersect(TexelRect{x0, y0, grid.stepX, grid.stepY}, rect);
                if (block.empty()) continue;
                const TexelRect source = intersect(grow(block, 2), format.bounds);
                window.resize(static_cast<size_t>(source.area()));
                if (!target.read(HEIGHT_MAP, source, window)) {
                    fail("could not read the map");
                    return;
                }
                std::vector<float> before = window;
                if (filtered) {
                    weights.resize(static_cast<size_t>(block.area()));
                    evaluateFilter(target, op.filter, lattice, block, weights);
                }

                // The kernels work relative to the map's corner; T4's signed texels stay out of them
                const int cornerX = format.bounds.x;
                const int cornerY = format.bounds.y;
                ErosionField field;
                field.heights = window.data();
                field.windowX = source.x - cornerX;
                field.windowY = source.y - cornerY;
                field.windowWidth = source.w;
                field.windowHeight = source.h;
                field.mapWidth = format.bounds.w;
                field.mapHeight = format.bounds.h;
                const ErosionRegion region{block.x - cornerX, block.y - cornerY, block.right() - 1 - cornerX, block.bottom() - 1 - cornerY};

                auto weightAt = [&](int x, int y) {
                    const int mapX = x + cornerX;
                    const int mapY = y + cornerY;
                    if (mapX < block.x || mapY < block.y || mapX >= block.right() || mapY >= block.bottom()) return 0.0f;
                    const ShapeSample sample = shape.sample(lattice.x(mapX), lattice.z(mapY));
                    const float filter = filtered ? weights[static_cast<size_t>(mapY - block.y) * block.w + (mapX - block.x)] : 1.0f;
                    return sample.weight * filter;
                };

                const float halfWidth = static_cast<float>(block.w) * 0.5f;
                const float halfHeight = static_cast<float>(block.h) * 0.5f;
                int droplets = 0;
                if (op.droplets) {
                    droplets = *op.droplets;
                } else {
                    erodeCarry += halfWidth * halfHeight * ERODE_DROPLETS_PER_SECOND * strength * seconds;
                    droplets = static_cast<int>(erodeCarry);
                    erodeCarry -= static_cast<float>(droplets);
                }
                if (droplets > 0) {
                    TerrainErosion::hydraulic(field, region, heightScale, droplets, op.seed + (++erodeSeed), weightAt);
                }
                // Talus is scaled on its own, so it settles on every application
                TerrainErosion::thermal(field, region, heightScale, ERODE_TALUS, detClamp01(strength * HEIGHT_FLOW * seconds), weightAt);

                out.resize(static_cast<size_t>(block.area()));
                for (int y = 0; y < block.h; y++) {
                    for (int x = 0; x < block.w; x++) {
                        const size_t sourceIndex = static_cast<size_t>(y + block.y - source.y) * source.w + (x + block.x - source.x);
                        const float next = window[sourceIndex];
                        const float previous = before[sourceIndex];
                        out[static_cast<size_t>(y) * block.w + x] = next;
                        if (quantizeChannel(previous, format.bitsPerChannel) != quantizeChannel(next, format.bitsPerChannel)) {
                            counters.changed++;
                            pairHeights(previous, next, counters);
                        }
                    }
                }
                Block written;
                written.map = HEIGHT_MAP;
                written.rect = block;
                writeBlock(written, out);
            }
        }
        finishEntry(counters, entry, op);
    }

    void Run::applyFillNoise(const Op& op, OpEntry& entry) {
        const MapFormat format = target.format(HEIGHT_MAP);
        const float baseHeight = detClamp01(op.baseHeight);
        const float amplitude = op.amplitude ? detClamp01(*op.amplitude) : 0.0f;
        const float frequency = std::max(0.01f, op.frequency);
        const int octaves = std::clamp(op.octaves, 1, 8);
        const uint32_t seed = op.seed;
        const FillMode mode = op.fill;
        const float spanX = static_cast<float>(format.bounds.w - 1);
        const float spanY = static_cast<float>(format.bounds.h - 1);
        if (mode != FillMode::Flat) entry.seed = seed;

        Shape whole;
        Op opaque = op;
        opaque.filter = Filter{};
        opaque.strength = 1.0f;
        const ShapeEvaluator shape(whole);
        const Counters counters = forEachSample(opaque, HEIGHT_MAP, shape, 1.0f, 0, false, nullptr, ShapeSample{}, [](const Block&) {},
            [&](const Block& block, int x, int y, const ShapeSample&, float, const float*, float* next, Counters& c) {
                const int texelX = x - block.format.bounds.x;
                const int texelY = y - block.format.bounds.y;
                float value = baseHeight;
                if (mode == FillMode::RandomNoise) {
                    value = baseHeight + (detHash01(texelX, texelY, seed) * 2.0f - 1.0f) * amplitude;
                } else if (mode == FillMode::FractalNoise) {
                    // set_terrain_maps' own loop, kept so its maps keep their bytes (R19)
                    float sum = 0.0f;
                    float weight = 1.0f;
                    float weightSum = 0.0f;
                    float f = frequency;
                    for (int o = 0; o < octaves; o++) {
                        const float nx = (static_cast<float>(texelX) / spanX) * f;
                        const float ny = (static_cast<float>(texelY) / spanY) * f;
                        sum += (noise::compat::value(nx, ny, seed + static_cast<uint32_t>(o * 1013)) * 2.0f - 1.0f) * weight;
                        weightSum += weight;
                        weight *= 0.5f;
                        f *= 2.0f;
                    }
                    value = baseHeight + sum / weightSum * amplitude;
                }
                if (value < 0.0f) c.clippedLow++;
                else if (value > 1.0f) c.clippedHigh++;
                next[0] = detClamp01(value);
            });
        finishEntry(counters, entry, op);
    }

    void Run::applyImport(const Op& op, OpEntry& entry) {
        Shape whole;
        Op opaque;
        const ShapeEvaluator shape(whole);
        const float* heights = op.importedHeights.data();
        const int width = op.importedWidth;
        const Counters counters = forEachSample(opaque, HEIGHT_MAP, shape, 1.0f, 0, false, nullptr, ShapeSample{}, [](const Block&) {},
            [&](const Block& block, int x, int y, const ShapeSample&, float, const float*, float* next, Counters& c) {
                const float value = heights[static_cast<size_t>(y - block.format.bounds.y) * width + (x - block.format.bounds.x)];
                if (value < 0.0f) c.clippedLow++;
                else if (value > 1.0f) c.clippedHigh++;
                next[0] = detClamp01(value);
            });
        finishEntry(counters, entry, op);
    }

    void Run::applyPaintLayer(const Op& op, OpEntry& entry) {
        const LayerChannel channel = terrainLayerChannel(*op.layer);
        const MapId map{MapKind::Blend, channel.map};
        if (!ensureMap(map, 4, entry)) return;
        if (op.smoothBlend) {
            // T1 R56: Paint Texture's alternate blurs every weight of the layer's map by a true average
            applyShaped(op, map, BLEND_FLOW, 1, false, entry, [&](const Block& block, int x, int y, const ShapeSample&, float w, const float* cur, float* next, Counters&) {
                for (int c = 0; c < block.channels; c++) {
                    float sum = 0.0f;
                    for (int oy = -1; oy <= 1; oy++) {
                        for (int ox = -1; ox <= 1; ox++) {
                            sum += block.at(std::clamp(x + ox, block.source.x, block.source.right() - 1),
                                            std::clamp(y + oy, block.source.y, block.source.bottom() - 1))[c];
                        }
                    }
                    next[c] = cur[c] + (sum / 9.0f - cur[c]) * w;
                }
            });
            return;
        }
        const bool normalize = op.normalize;
        const int painted = channel.channel;
        const float goal = channel.inverted ? 0.0f : 1.0f;
        applyShaped(op, map, BLEND_FLOW, 0, false, entry, [&](const Block& block, int, int, const ShapeSample&, float w, const float* cur, float* next, Counters&) {
            for (int c = 0; c < std::min(block.channels, 3); c++) {
                const float wanted = c == painted ? goal : (normalize ? 0.0f : cur[c]);
                next[c] = cur[c] + (wanted - cur[c]) * w;
            }
            // T3 R15: a layer in the alpha takes its inverted weight there; a layer in the RGB leaves
            // what the alpha holds, and brings an unwritten map's alpha to opaque with its normalize
            if (block.channels >= 4) {
                if (painted == 3) {
                    next[3] = cur[3] + (goal - cur[3]) * w;
                } else if (normalize) {
                    next[3] = cur[3] + (1.0f - cur[3]) * w;
                }
            }
        });
    }

    void Run::applyPaintBase(const Op& op, OpEntry& entry) {
        auto clear = [&](const Block& block, int, int, const ShapeSample&, float w, const float* cur, float* next, Counters&) {
            for (int c = 0; c < std::min(block.channels, 3); c++) {
                next[c] = cur[c] - cur[c] * w;
            }
            if (block.channels >= 4) next[3] = 1.0f;
        };
        if (op.layer) {
            const MapId map{MapKind::Blend, terrainLayerChannel(*op.layer).map};
            if (!ensureMap(map, 4, entry)) return;
            applyShaped(op, map, BLEND_FLOW, 0, false, entry, clear);
            return;
        }
        const int count = blendMapCountOf(target);
        for (int m = 0; m < count && !failed(); m++) {
            applyShaped(op, MapId{MapKind::Blend, m}, BLEND_FLOW, 0, false, entry, clear);
        }
    }

    void Run::applyPaintByRules(const Op& op, OpEntry& entry) {
        // The rules' layers, each once, the way the encoder takes them
        std::vector<int> layers;
        int topLayer = 0;
        for (const PaintRule& rule : op.rules) {
            if (rule.layer >= 0 && std::find(layers.begin(), layers.end(), rule.layer) == layers.end()) {
                layers.push_back(rule.layer);
                topLayer = std::max(topLayer, rule.layer);
            }
        }
        std::sort(layers.begin(), layers.end());

        const int existing = blendMapCountOf(target);
        const int needed = std::max(existing, terrainBlendMapCount(topLayer + 1));
        for (int m = 0; m < needed; m++) {
            if (!ensureMap(MapId{MapKind::Blend, m}, 4, entry)) return;
        }

        entry.seed = op.seed;
        const size_t layerSlots = static_cast<size_t>(std::max(topLayer + 1, needed * 3));

        for (int m = 0; m < needed && !failed(); m++) {
            const MapId map{MapKind::Blend, m};
            const MapFormat format = target.format(map);
            const MapLattice lattice = latticeOf(target, map, format);
            const ShapeEvaluator shape(op.shape);

            // Each rule's own mask for the block in hand, from the state before the op
            std::vector<std::vector<float>> ruleWeights(op.rules.size());
            auto prepareRules = [&](const Block& block) {
                for (size_t r = 0; r < op.rules.size(); r++) {
                    const PaintRule& rule = op.rules[r];
                    Filter ruleFilter;
                    ruleFilter.slope = rule.slope;
                    ruleFilter.height = rule.height;
                    ruleFilter.invert = rule.invert;
                    ruleFilter.softness = op.filter.softness;
                    ruleWeights[r].resize(static_cast<size_t>(block.rect.area()));
                    evaluateFilter(target, ruleFilter, lattice, block.rect, ruleWeights[r]);
                }
            };

            const float noiseSize = op.noiseSize;
            const uint32_t seed = op.seed;
            const int octaves = op.octaves;
            const Counters counters = forEachSample(op, map, shape, op.strength, 0, false, nullptr, ShapeSample{}, prepareRules,
                [&](const Block& block, int x, int y, const ShapeSample&, float w, const float* cur, float* next, Counters&) {
                    std::array<float, 16> small{};
                    std::vector<float> large;
                    float* mixed = small.data();
                    if (layerSlots > small.size()) {
                        large.assign(layerSlots, 0.0f);
                        mixed = large.data();
                    }
                    const size_t index = static_cast<size_t>(y - block.rect.y) * block.rect.w + (x - block.rect.x);
                    // Later over earlier: a rule's strength takes that share from all before it
                    for (size_t r = 0; r < op.rules.size(); r++) {
                        const PaintRule& rule = op.rules[r];
                        float share = ruleWeights[r][index] * rule.opacity;
                        if (rule.noise > 0.0f && share > 0.0f) {
                            const float fieldX = static_cast<float>(x - block.format.bounds.x) * std::fabs(block.lattice.stepX) / noiseSize;
                            const float fieldZ = static_cast<float>(y - block.format.bounds.y) * std::fabs(block.lattice.stepZ) / noiseSize;
                            const float grain = noise::compat::fractal(fieldX, fieldZ, seed, octaves) * 0.5f + 0.5f;
                            share *= 1.0f - rule.noise * (1.0f - grain);
                        }
                        for (size_t l = 0; l < layerSlots; l++) {
                            mixed[l] *= 1.0f - share;
                        }
                        if (rule.layer >= 0) mixed[rule.layer] += share;
                    }

                    std::array<float, 16> encodedSmall{};
                    std::vector<float> encodedLarge;
                    float* encoded = encodedSmall.data();
                    if (layerSlots > encodedSmall.size()) {
                        encodedLarge.assign(layerSlots, 0.0f);
                        encoded = encodedLarge.data();
                    }
                    std::array<float, 16> listedSmall{};
                    std::vector<float> listedLarge;
                    float* listedValues = listedSmall.data();
                    if (layers.size() > listedSmall.size()) {
                        listedLarge.resize(layers.size());
                        listedValues = listedLarge.data();
                    }
                    for (size_t l = 0; l < layers.size(); l++) {
                        listedValues[l] = mixed[layers[l]];
                    }
                    encodeLayerWeights(layers, std::span<const float>(listedValues, layers.size()), std::span<float>(encoded, layerSlots));

                    float wanted[3] = {0.0f, 0.0f, 0.0f};
                    for (size_t l = 0; l < layerSlots; l++) {
                        const LayerChannel channel = terrainLayerChannel(static_cast<int>(l));
                        if (channel.map == m && channel.channel < 3) wanted[channel.channel] = encoded[l];
                    }
                    for (int c = 0; c < std::min(block.channels, 3); c++) {
                        next[c] = cur[c] + (wanted[c] - cur[c]) * w;
                    }
                    if (block.channels >= 4) next[3] = 1.0f;
                });
            finishEntry(counters, entry, op);
        }
    }

    void Run::applyRemapLayers(const Op& op, OpEntry& entry) {
        const int layerCount = static_cast<int>(op.order.size());
        const int existing = blendMapCountOf(target);
        const int needed = std::max(existing, terrainBlendMapCount(layerCount));
        std::vector<char> hadMap(static_cast<size_t>(needed), 0);
        for (int m = 0; m < needed; m++) {
            hadMap[m] = target.format(MapId{MapKind::Blend, m}).exists ? 1 : 0;
        }

        std::vector<int> sourceOf(static_cast<size_t>(layerCount));
        for (int layer = 0; layer < layerCount; layer++) {
            sourceOf[op.order[layer]] = layer;
        }

        // Reads go through a transaction of their own: a channel moved early must not be read as
        // the source of a later one
        OverlayMapStore work(target);
        for (int m = 0; m < needed && !failed(); m++) {
            const MapId destination{MapKind::Blend, m};
            // A map a moved layer does not reach stays unmade: it would hold nothing
            bool any = false;
            for (int layer = 0; layer < layerCount; layer++) {
                const LayerChannel dest = terrainLayerChannel(layer);
                if (dest.map != m) continue;
                const LayerChannel from = terrainLayerChannel(sourceOf[layer]);
                if (hadMap[m] || (from.map < needed && hadMap[from.map])) any = true;
            }
            if (!any) continue;

            MapFormat format = target.format(destination);
            if (!format.exists) {
                const WriteResult made = work.createMap(destination, newMapFormat(target, options, 4), 0.0f);
                if (!made.ok) {
                    fail(made.error);
                    return;
                }
                entry.mapCreated = true;
                warn("map_created", "This op created blend map " + std::to_string(m) + ".");
                format = work.format(destination);
            }
            const BlockGrid grid(work, destination, format.bounds);
            const int count = floatChannels(destination, format);
            const MapLattice lattice = latticeOf(work, destination, format);

            std::vector<float> out;
            for (int y0 = grid.firstY; y0 < format.bounds.bottom() && !failed(); y0 += grid.stepY) {
                for (int x0 = grid.firstX; x0 < format.bounds.right() && !failed(); x0 += grid.stepX) {
                    const TexelRect block = intersect(TexelRect{x0, y0, grid.stepX, grid.stepY}, format.bounds);
                    if (block.empty()) continue;
                    out.assign(static_cast<size_t>(block.area()) * count, 0.0f);
                    if (hadMap[m]) {
                        if (!target.read(destination, block, out)) {
                            fail("could not read the map");
                            return;
                        }
                    } else if (count >= 4) {
                        // a new blend map is opaque
                        for (size_t i = 3; i < out.size(); i += static_cast<size_t>(count)) {
                            out[i] = 1.0f;
                        }
                    }
                    // Each channel takes the texels of the layer that moves here, where its map
                    // puts that position: maps of different sizes stay aligned by place
                    for (int c = 0; c < 3 && c < count; c++) {
                        int layer = -1;
                        for (int l = 0; l < layerCount; l++) {
                            const LayerChannel dest = terrainLayerChannel(l);
                            if (dest.map == m && dest.channel == c) layer = l;
                        }
                        if (layer < 0) continue;
                        const LayerChannel from = terrainLayerChannel(sourceOf[layer]);
                        const MapId sourceMap{MapKind::Blend, from.map};
                        const MapFormat sourceFormat = target.format(sourceMap);
                        if (!sourceFormat.exists) {
                            for (int64_t i = 0; i < block.area(); i++) {
                                out[static_cast<size_t>(i) * count + c] = 0.0f;
                            }
                            continue;
                        }
                        const MapLattice sourceLattice = latticeOf(target, sourceMap, sourceFormat);
                        const int sourceCount = floatChannels(sourceMap, sourceFormat);
                        const float invX = sourceLattice.stepX != 0.0f ? 1.0f / sourceLattice.stepX : 1.0f;
                        const float invZ = sourceLattice.stepZ != 0.0f ? 1.0f / sourceLattice.stepZ : 1.0f;
                        auto nearestX = [&](int x) {
                            return std::clamp(static_cast<int>(std::round(static_cast<float>(sourceLattice.baseX) + (lattice.x(x) - sourceLattice.originX) * invX)),
                                              sourceFormat.bounds.x, sourceFormat.bounds.right() - 1);
                        };
                        auto nearestY = [&](int y) {
                            return std::clamp(static_cast<int>(std::round(static_cast<float>(sourceLattice.baseY) + (lattice.z(y) - sourceLattice.originZ) * invZ)),
                                              sourceFormat.bounds.y, sourceFormat.bounds.bottom() - 1);
                        };
                        const int sx0 = nearestX(block.x), sx1 = nearestX(block.right() - 1);
                        const int sy0 = nearestY(block.y), sy1 = nearestY(block.bottom() - 1);
                        const TexelRect sourceRect{std::min(sx0, sx1), std::min(sy0, sy1), std::abs(sx1 - sx0) + 1, std::abs(sy1 - sy0) + 1};
                        std::vector<float> sourceValues(static_cast<size_t>(sourceRect.area()) * sourceCount);
                        if (!target.read(sourceMap, sourceRect, sourceValues)) {
                            fail("could not read the map");
                            return;
                        }
                        for (int y = 0; y < block.h; y++) {
                            const int sy = nearestY(block.y + y);
                            for (int x = 0; x < block.w; x++) {
                                const int sx = nearestX(block.x + x);
                                out[(static_cast<size_t>(y) * block.w + x) * count + c] =
                                    sourceValues[(static_cast<size_t>(sy - sourceRect.y) * sourceRect.w + (sx - sourceRect.x)) * sourceCount + from.channel];
                            }
                        }
                    }
                    const WriteResult wrote = work.write(destination, block, out);
                    if (!wrote.ok) {
                        fail(wrote.error);
                        return;
                    }
                }
            }
        }
        if (failed()) return;

        for (const OverlayMapStore::MapChange& change : work.changes()) {
            entry.changedTexels += change.texels;
        }
        const WriteResult committed = work.commit();
        if (!committed.ok) {
            fail(committed.error);
            return;
        }
        entry.layerOrder = op.order;
    }

    void Run::applyDensity(const Op& op, OpEntry& entry) {
        const MapId map{MapKind::Density, *op.layer};
        if (!ensureMap(map, 1, entry)) return;
        const float goal = op.type == OpType::PaintDensity ? 1.0f : 0.0f;
        applyShaped(op, map, BLEND_FLOW, 0, false, entry, [&](const Block&, int, int, const ShapeSample&, float w, const float* cur, float* next, Counters&) {
            next[0] = cur[0] + (goal - cur[0]) * w;
        });
    }

    void Run::applyHoles(const Op& op, OpEntry& entry) {
        HoleRegionResult detail;
        Shape region = op.shape;
        rasterizeRegion(target, region, op.type == OpType::CutHoles ? HOLE_VALUE : SOLID_VALUE, nullptr,
                        op.filter.active() ? &op.filter : nullptr, &detail);
        entry.changedTexels = detail.changed;
        entry.filteredOut = detail.filteredOut;
        entry.holeTexels = detail.holeTexels;
        entry.holeFraction = detail.holeFraction;
        entry.mapCreated = detail.mapCreated;
        entry.mapRemoved = detail.mapRemoved;
        if (detail.mapCreated) warn("map_created", "This op created the hole map.");
        entry.collisionSamplesOpen = collisionSamplesOpen(target);
        // T3 R22, R27: a hole with no interior sample is drawn and opens nothing for a body
        if (op.type == OpType::CutHoles && detail.changed > 0 && entry.collisionSamplesOpen == 0) {
            warn("hole_no_collision", "This hole opens no collision sample: a body stays on the collider. Cut at least 3 collision cells across.");
        }
    }

    void Run::applyPlace(const Op& op, OpEntry& entry) {
        const MapFormat format = target.format(HEIGHT_MAP);
        const MapLattice lattice = latticeOf(target, HEIGHT_MAP, format);
        const LocalBounds bounds = localBoundsOf(lattice, format);
        PlacementParams params;
        params.spacing = op.spacing;
        params.minScale = op.minScale;
        params.maxScale = op.maxScale;
        params.rotationJitter = op.rotationJitter;
        params.seed = op.seed;
        entry.seed = op.seed;

        std::vector<PlacementStamp> stamps;
        const ShapeEvaluator shape(op.shape);
        if (op.shape.type == ShapeType::Stroke) {
            for (const StrokeStamp& at : stampsOf(op, HEIGHT_MAP)) {
                stamps.push_back(PlacementStamp{at.x, at.z, op.shape.radius, false});
            }
        } else {
            // count attempts over the region's box; the region itself decides who stays
            LocalBounds box;
            shape.bounds(box);
            const float centerX = (box.min.x + box.max.x) * 0.5f;
            const float centerZ = (box.min.y + box.max.y) * 0.5f;
            const float extent = std::max(box.max.x - box.min.x, box.max.y - box.min.y) * 0.5f;
            for (int i = 0; i < op.count; i++) {
                stamps.push_back(PlacementStamp{centerX, centerZ, extent, op.shape.type != ShapeType::Circle});
            }
        }

        struct Accept {
            const ShapeEvaluator* shape;
            bool checkShape;
        } accept{&shape, op.shape.type != ShapeType::Stroke};
        entry.plan = planPlacement(stamps, params, op.existingObjects, bounds.min, bounds.max,
            [](void* user, float x, float z) {
                const Accept& a = *static_cast<const Accept*>(user);
                return !a.checkShape || a.shape->sample(x, z).inside;
            }, &accept);
    }

    void Run::applyErase(const Op& op, OpEntry& entry) {
        const ShapeEvaluator shape(op.shape);
        for (size_t i = 0; i < op.existingObjects.size(); i++) {
            const Vector2& point = op.existingObjects[i];
            if (shape.sample(point.x, point.y).inside) entry.erasedObjects.push_back(static_cast<int>(i));
        }
    }

    void Run::applyOp(int index, const Op& op, OpEntry& entry) {
        currentOp = index;
        entry.op = opNameOf(op);

        switch (op.type) {
            case OpType::Raise: case OpType::Lower: case OpType::SetHeight: case OpType::LiftFloors: case OpType::FlattenPeaks:
            case OpType::AddNoise: case OpType::Terrace: case OpType::Stamp: case OpType::Ramp: case OpType::RemapHeights:
                applyHeightOp(op, entry);
                break;
            case OpType::Smooth:
            case OpType::Sharpen:
                applySmooth(op, entry);
                break;
            case OpType::Erode: {
                entry.seed = op.seed;
                for (int iteration = 0; iteration < op.iterations && !failed(); iteration++) {
                    if (op.shape.type == ShapeType::Stroke) {
                        Shape stamp = op.shape;
                        stamp.type = ShapeType::Circle;
                        for (const StrokeStamp& at : stampsOf(op, HEIGHT_MAP)) {
                            stamp.center = Vector2(at.x, at.z);
                            applyErode(op, stamp, STAMP_SECONDS, op.strength * at.pressure, entry);
                        }
                    } else {
                        applyErode(op, op.shape, op.erodeSeconds, op.strength, entry);
                    }
                }
                break;
            }
            case OpType::FillNoise:
                applyFillNoise(op, entry);
                break;
            case OpType::ImportHeightmap:
                applyImport(op, entry);
                break;
            case OpType::PaintLayer:
                applyPaintLayer(op, entry);
                break;
            case OpType::PaintBase:
                applyPaintBase(op, entry);
                break;
            case OpType::PaintByRules:
                applyPaintByRules(op, entry);
                break;
            case OpType::RemapLayers:
                applyRemapLayers(op, entry);
                break;
            case OpType::PaintDensity:
            case OpType::EraseDensity:
                applyDensity(op, entry);
                break;
            case OpType::CutHoles:
            case OpType::FillHoles:
                applyHoles(op, entry);
                break;
            case OpType::PlaceObjects:
                applyPlace(op, entry);
                break;
            case OpType::EraseObjects:
                applyErase(op, entry);
                break;
            case OpType::Registered: {
                RegisteredOp registered;
                if (!findRegistered(op.registeredName, registered) || !registered.apply) {
                    fail("unknown operation \"" + op.registeredName + "\"");
                    return;
                }
                registered.apply(*this, op, entry);
                break;
            }
        }
    }

    // ------------------------------------------------------------------ results

    void warnFromEntry(int index, const Op& op, const OpEntry& entry, const TerrainMapStore& store, TerrainOpResult& result) {
        auto warn = [&](const char* code, const std::string& message) {
            result.warnings.push_back(OpWarning{index, code, message});
        };
        if (entry.clippedLow > 0) warn("clipped_low", std::to_string(entry.clippedLow) + " texels were clamped at height 0.");
        if (entry.clippedHigh > 0) warn("clipped_high", std::to_string(entry.clippedHigh) + " texels were clamped at the terrain's maximum height.");
        if (entry.outside > 0) warn("outside_terrain", "Part of the shape lies outside the terrain.");
        if (op.filter.active() && entry.changedTexels == 0 && entry.filteredOut > 0) {
            warn("filter_excluded_all", "The filter excluded every texel in the shape.");
        } else if (entry.changedTexels == 0 && op.type != OpType::PlaceObjects && op.type != OpType::EraseObjects &&
                   op.type != OpType::Registered) {
            warn("no_change", "This op changed no texel.");
        }
        if (isHeightOp(op.type)) {
            const MapFormat format = store.format(HEIGHT_MAP);
            if (format.exists && format.bitsPerChannel < 16) {
                warn("map_8bit_steps", "The height map is 8-bit: heights move in visible steps.");
            }
        }
    }

    void fillOutside(const TerrainMapStore& store, const Op& op, OpEntry& entry) {
        if (op.shape.type == ShapeType::All || op.type == OpType::Registered) return;
        if (op.type == OpType::PlaceObjects || op.type == OpType::EraseObjects || op.type == OpType::RemapLayers) return;
        const MapId map = latticeMapOf(op, store);
        const MapFormat format = store.format(map);
        if (!format.exists) return;
        const ShapeEvaluator shape(op.shape);
        int64_t raw = 0;
        const TexelRect rect = shapeTexels(store, map, format, shape, &raw);
        entry.outside = std::max<int64_t>(0, raw - rect.area());
    }

    void reportOverlay(const OverlayMapStore& overlay, TerrainOpResult& result, float maxHeight) {
        result.createdMaps = overlay.createdMaps();
        result.removedMaps = overlay.removedMaps();

        for (const OverlayMapStore::MapChange& change : overlay.changes()) {
            MapChangeReport report;
            report.map = change.map;
            report.rect = change.rect;
            report.texels = change.texels;
            // a map removed by this list still has a lattice in its base
            const Vector2 a = overlay.worldOfTexel(change.map, change.rect.x, change.rect.y);
            const Vector2 b = overlay.worldOfTexel(change.map, change.rect.right() - 1, change.rect.bottom() - 1);
            const Vector2 next = overlay.worldOfTexel(change.map, change.rect.x + 1, change.rect.y + 1);
            const float halfX = std::fabs(next.x - a.x) * 0.5f;
            const float halfZ = std::fabs(next.y - a.y) * 0.5f;
            report.localMin = Vector2(std::min(a.x, b.x) - halfX, std::min(a.y, b.y) - halfZ);
            report.localMax = Vector2(std::max(a.x, b.x) + halfX, std::max(a.y, b.y) + halfZ);
            result.changed.push_back(report);
        }

        // Over the texels that changed, from the transaction's own before and after: each tile's on the
        // workers, then summed in the tiles' order, so the mean is the same double sum as one walk's
        HeightStats& stats = result.stats;
        double sum = 0.0;
        int64_t count = 0;
        const std::vector<OverlayMapStore::TileView> views = overlay.tileViews();
        struct TileStats {
            bool valid = false;
            float minBefore = 0.0f, maxBefore = 0.0f, minAfter = 0.0f, maxAfter = 0.0f, maxDelta = 0.0f;
            std::vector<float> deltas;
        };
        std::vector<TileStats> perTile(views.size());
        auto measure = [&](size_t index) {
            const OverlayMapStore::TileView& view = views[index];
            if (view.map.kind != MapKind::Height) return;
            const MapFormat format = overlay.format(view.map);
            const int stride = bytesPerTexel(format);
            TileStats& tile = perTile[index];
            // in the tile's order; only where a write reached can a texel differ
            for (int y = view.written.y; y < view.written.bottom(); y++) {
                for (int x = view.written.x; x < view.written.right(); x++) {
                    const size_t i = static_cast<size_t>(y - view.rect.y) * view.rect.w + (x - view.rect.x);
                    if (std::memcmp(view.before.data() + i * stride, view.after.data() + i * stride, stride) == 0) continue;
                    const float was = decodeChannel(view.before.data() + i * stride, format.bitsPerChannel) * maxHeight;
                    const float now = decodeChannel(view.after.data() + i * stride, format.bitsPerChannel) * maxHeight;
                    if (!tile.valid) {
                        tile.valid = true;
                        tile.minBefore = tile.maxBefore = was;
                        tile.minAfter = tile.maxAfter = now;
                    }
                    tile.minBefore = std::min(tile.minBefore, was);
                    tile.maxBefore = std::max(tile.maxBefore, was);
                    tile.minAfter = std::min(tile.minAfter, now);
                    tile.maxAfter = std::max(tile.maxAfter, now);
                    tile.maxDelta = std::max(tile.maxDelta, std::fabs(now - was));
                    tile.deltas.push_back(now - was);
                }
            }
        };
        if (views.size() >= PARALLEL_STAT_TILES) {
            jobs::parallelFor(views.size(), measure);
        } else {
            for (size_t i = 0; i < views.size(); i++) measure(i);
        }
        for (const TileStats& tile : perTile) {
            if (!tile.valid) continue;
            if (!stats.valid) {
                stats.valid = true;
                stats.minBefore = tile.minBefore;
                stats.maxBefore = tile.maxBefore;
                stats.minAfter = tile.minAfter;
                stats.maxAfter = tile.maxAfter;
            }
            stats.minBefore = std::min(stats.minBefore, tile.minBefore);
            stats.maxBefore = std::max(stats.maxBefore, tile.maxBefore);
            stats.minAfter = std::min(stats.minAfter, tile.minAfter);
            stats.maxAfter = std::max(stats.maxAfter, tile.maxAfter);
            stats.maxDelta = std::max(stats.maxDelta, tile.maxDelta);
            for (float delta : tile.deltas) sum += delta;
            count += static_cast<int64_t>(tile.deltas.size());
        }
        if (count > 0) stats.meanDelta = static_cast<float>(sum / static_cast<double>(count));
    }

    void reportCounters(const Counters& total, TerrainOpResult& result, float maxHeight) {
        if (total.changed == 0) return;
        HeightStats& stats = result.stats;
        stats.valid = true;
        stats.minBefore = total.minBefore * maxHeight;
        stats.maxBefore = total.maxBefore * maxHeight;
        stats.minAfter = total.minAfter * maxHeight;
        stats.maxAfter = total.maxAfter * maxHeight;
        stats.meanDelta = static_cast<float>(total.sumDelta / static_cast<double>(total.changed)) * maxHeight;
        stats.maxDelta = total.maxDelta * maxHeight;
    }

    void failResult(TerrainOpResult& result, const std::string& message) {
        result.ok = false;
        if (result.error.empty()) result.error = message;
    }

    void firstError(TerrainOpResult& result) {
        if (!result.errors.empty()) {
            result.ok = false;
            result.error = result.errors.front().field + ": " + result.errors.front().message;
        }
    }

}

const char* jasperengine::terrain::opTypeName(OpType type) {
    for (const OpNameRow& row : OP_NAMES) {
        if (row.type == type) return row.name;
    }
    return nullptr;
}

bool jasperengine::terrain::opTypeFromName(std::string_view name, OpType& out) {
    for (const OpNameRow& row : OP_NAMES) {
        if (name == row.name) {
            out = row.type;
            return true;
        }
    }
    return false;
}

bool jasperengine::terrain::registerOp(std::string_view name, OpValidateFn validate, OpApplyFn apply, RegisteredOpTraits traits) {
    OpType builtin;
    if (opTypeFromName(name, builtin)) return false;
    std::lock_guard<std::mutex> lock(registryMutex());
    for (RegisteredOp& op : registry()) {
        if (op.name == name) {
            op.validate = validate;
            op.apply = apply;
            op.traits = traits;
            return true;
        }
    }
    registry().push_back(RegisteredOp{std::string(name), validate, apply, traits});
    return true;
}

bool jasperengine::terrain::registeredOpExists(std::string_view name) {
    RegisteredOp found;
    return findRegistered(name, found);
}

bool jasperengine::terrain::registeredOpSpansFrames(std::string_view name) {
    RegisteredOp found;
    return findRegistered(name, found) && found.traits.spansFrames;
}

void jasperengine::terrain::unregisterOp(std::string_view name) {
    std::lock_guard<std::mutex> lock(registryMutex());
    std::vector<RegisteredOp>& ops = registry();
    ops.erase(std::remove_if(ops.begin(), ops.end(), [&](const RegisteredOp& op) { return op.name == name; }), ops.end());
}

TerrainOpResult jasperengine::terrain::validateOps(const TerrainMapStore& store, std::span<const Op> ops) {
    TerrainOpResult result;
    result.ok = true;
    for (size_t i = 0; i < ops.size(); i++) {
        Validator validator{store, static_cast<int>(i), result.errors};
        validator.check(ops[i]);
    }
    firstError(result);
    return result;
}

TerrainOpResult jasperengine::terrain::applyOps(TerrainMapStore& store, std::span<const Op> ops, const OpOptions& options) {
    TerrainOpResult result = validateOps(store, ops);
    result.dryRun = options.dryRun;
    if (!result.ok) return result;

    // R87: a store that holds undo itself for this size of edit works straight on its own bytes
    std::vector<std::shared_ptr<TerrainLargeEdit>> tokens;
    if (!options.dryRun && !options.inPlace) {
        for (const Op& op : ops) {
            if (op.type == OpType::Registered || op.type == OpType::PlaceObjects || op.type == OpType::EraseObjects) continue;
            const MapId map = latticeMapOf(op, store);
            const MapFormat format = store.format(map);
            if (!format.exists) continue;
            const ShapeEvaluator shape(op.shape);
            const TexelRect rect = shapeTexels(store, map, format, shape);
            if (std::unique_ptr<TerrainLargeEdit> token = store.beginLarge(map, rect)) {
                tokens.push_back(std::shared_ptr<TerrainLargeEdit>(std::move(token)));
            }
        }
    }
    const bool straight = options.inPlace || !tokens.empty();

    std::unique_ptr<OverlayMapStore> overlay;
    if (!straight) overlay = std::make_unique<OverlayMapStore>(store);
    TerrainMapStore& target = straight ? store : static_cast<TerrainMapStore&>(*overlay);

    result.ops.resize(ops.size());
    Run run(target, options, result);
    for (size_t i = 0; i < ops.size(); i++) {
        fillOutside(store, ops[i], result.ops[i]);
        run.applyOp(static_cast<int>(i), ops[i], result.ops[i]);
        if (run.failed()) {
            for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
                (*it)->undo();
            }
            failResult(result, "ops[" + std::to_string(i) + "]: " + run.failure());
            result.undoHooks.clear();
            return result;
        }
        warnFromEntry(static_cast<int>(i), ops[i], result.ops[i], target, result);
    }

    const float maxHeight = store.frame().maxHeight;
    if (overlay) {
        reportOverlay(*overlay, result, maxHeight);
        if (!options.dryRun) {
            const WriteResult committed = overlay->commit();
            if (!committed.ok) {
                failResult(result, committed.error);
                result.changed.clear();
                result.createdMaps.clear();
                result.removedMaps.clear();
                result.undoHooks.clear();
                return result;
            }
            result.tilesCreated += committed.tilesCreated;
        }
    } else {
        reportCounters(run.total, result, maxHeight);
    }
    if (result.tilesCreated > 0) {
        result.warnings.push_back(OpWarning{-1, "tile_created", std::to_string(result.tilesCreated) + " tiles were created."});
    }
    result.largeEdits = std::move(tokens);
    result.ok = true;
    return result;
}

TerrainOpResult jasperengine::terrain::setHeights(TerrainMapStore& store, TexelRect rect, std::span<const float> localHeights) {
    const float maxHeight = store.frame().maxHeight;
    const float toNormalized = maxHeight != 0.0f ? 1.0f / maxHeight : 0.0f;
    std::vector<float> normalized(localHeights.size());
    for (size_t i = 0; i < localHeights.size(); i++) {
        normalized[i] = localHeights[i] * toNormalized;
    }
    Batch batch(store);
    batch.setHeights(rect, normalized);
    return batch.commit();
}

TerrainOpResult jasperengine::terrain::applyField(TerrainMapStore& store, MapId map, TexelRect rect, FieldFn field, void* user) {
    TerrainOpResult result;
    const MapFormat format = store.format(map);
    if (!format.exists) {
        failResult(result, "map does not exist");
        return result;
    }
    if (!format.bounds.contains(rect)) {
        failResult(result, "rectangle outside the map");
        return result;
    }

    OverlayMapStore overlay(store);
    OpEntry entry;
    entry.op = "apply_field";
    const MapLattice lattice = latticeOf(store, map, format);
    const int channels = floatChannels(map, format);

    // Blocks of the store's own size, so no buffer outgrows it (R87)
    const BlockGrid grid(store, map, rect);
    std::vector<float> current;
    std::vector<float> next;
    for (int y0 = grid.firstY; y0 < rect.bottom(); y0 += grid.stepY) {
        for (int x0 = grid.firstX; x0 < rect.right(); x0 += grid.stepX) {
            const TexelRect block = intersect(TexelRect{x0, y0, grid.stepX, grid.stepY}, rect);
            if (block.empty()) continue;
            current.resize(static_cast<size_t>(block.area()) * channels);
            if (!overlay.read(map, block, current)) {
                failResult(result, "could not read the map");
                return result;
            }
            next = current;
            for (int y = 0; y < block.h; y++) {
                const float z = lattice.z(block.y + y);
                for (int x = 0; x < block.w; x++) {
                    const size_t index = (static_cast<size_t>(y) * block.w + x) * channels;
                    const FieldSample sample = field(user, lattice.x(block.x + x), z, std::span<const float>(current.data() + index, static_cast<size_t>(channels)));
                    if (!(sample.weight > 0.0f)) continue;
                    const float weight = detClamp01(sample.weight);
                    bool changed = false;
                    for (int c = 0; c < channels; c++) {
                        const float value = current[index + c] + (sample.target[c] - current[index + c]) * weight;
                        if (value < 0.0f) entry.clippedLow++;
                        else if (value > 1.0f) entry.clippedHigh++;
                        next[index + c] = detClamp01(value);
                        if (quantizeChannel(current[index + c], format.bitsPerChannel) != quantizeChannel(next[index + c], format.bitsPerChannel)) changed = true;
                    }
                    if (changed) entry.changedTexels++;
                }
            }
            const WriteResult wrote = overlay.write(map, block, next);
            if (!wrote.ok) {
                failResult(result, wrote.error);
                return result;
            }
        }
    }

    result.ops.push_back(entry);
    if (entry.clippedLow > 0) result.warnings.push_back(OpWarning{0, "clipped_low", std::to_string(entry.clippedLow) + " values were clamped at 0."});
    if (entry.clippedHigh > 0) result.warnings.push_back(OpWarning{0, "clipped_high", std::to_string(entry.clippedHigh) + " values were clamped at 1."});
    reportOverlay(overlay, result, store.frame().maxHeight);
    const WriteResult committed = overlay.commit();
    if (!committed.ok) {
        failResult(result, committed.error);
        result.changed.clear();
        return result;
    }
    result.tilesCreated = committed.tilesCreated;
    result.ok = true;
    return result;
}

uint64_t jasperengine::terrain::mapHash(const TerrainMapStore& store, MapId map) {
    const MapFormat format = store.format(map);
    uint64_t hash = FNV1A64_SEED;
    if (!format.exists) return hash;
    std::vector<uint8_t> row(static_cast<size_t>(format.bounds.w) * bytesPerTexel(format));
    for (int y = format.bounds.y; y < format.bounds.bottom(); y++) {
        if (!store.readBytes(map, TexelRect{format.bounds.x, y, format.bounds.w, 1}, row)) continue;
        hash = fnv1a64(row.data(), row.size(), hash);
    }
    return hash;
}

void jasperengine::terrain::encodeLayerWeights(std::span<const int> layers, std::span<const float> values, std::span<float> perLayer) {
    std::fill(perLayer.begin(), perLayer.end(), 0.0f);
    float sum = 0.0f;
    bool hasLayerZero = false;
    for (size_t i = 0; i < layers.size(); i++) {
        const float value = detClamp01(values[i]);
        perLayer[layers[i]] = value;
        sum += value;
        hasLayerZero |= layers[i] == 0;
    }
    if (sum > 1.0f) {
        for (size_t i = 0; i < layers.size(); i++) {
            perLayer[layers[i]] /= sum;
        }
        sum = 1.0f;
    }
    if (!hasLayerZero && !perLayer.empty()) {
        perLayer[0] = 1.0f - sum;
    }
}

Batch::Batch(TerrainMapStore& store): base(store), transaction(store) {
    entry.op = "batch";
}

bool Batch::fail(const std::string& message) {
    errors.push_back(OpError{-1, "batch", message});
    return false;
}

bool Batch::setHeights(TexelRect rect, std::span<const float> normalized) {
    const MapFormat format = transaction.format(HEIGHT_MAP);
    if (!format.exists) return fail("the terrain has no height map");
    if (!format.bounds.contains(rect)) return fail("rectangle outside the height map");
    if (normalized.size() < static_cast<size_t>(rect.area())) return fail("too few height values");

    std::vector<float> previous(static_cast<size_t>(rect.area()));
    if (!transaction.read(HEIGHT_MAP, rect, previous)) return fail("could not read the height map");
    for (size_t i = 0; i < previous.size(); i++) {
        const float value = normalized[i];
        if (value < 0.0f) entry.clippedLow++;
        else if (value > 1.0f) entry.clippedHigh++;
        if (quantizeChannel(previous[i], format.bitsPerChannel) != quantizeChannel(value, format.bitsPerChannel)) entry.changedTexels++;
    }
    const WriteResult wrote = transaction.write(HEIGHT_MAP, rect, normalized);
    return wrote.ok ? true : fail(wrote.error);
}

bool Batch::setLayerWeights(TexelRect rect, std::span<const int> layers, std::span<const float> weights) {
    if (rect.empty()) return true;
    if (layers.empty()) return fail("no layers");
    if (weights.size() < static_cast<size_t>(rect.area()) * layers.size()) return fail("too few layer weights");
    int topLayer = 0;
    for (int layer : layers) {
        if (layer < 0) return fail("a layer index is negative");
        topLayer = std::max(topLayer, layer);
    }

    const int existing = blendMapCountOf(transaction);
    const int needed = std::max(existing, terrainBlendMapCount(topLayer + 1));
    OpOptions defaults;
    for (int m = 0; m < needed; m++) {
        const MapId map{MapKind::Blend, m};
        if (!transaction.format(map).exists) {
            std::string why;
            if (!transaction.canCreateMap(map, &why)) return fail(why);
            if (!transaction.format(HEIGHT_MAP).exists) return fail("a new blend map takes the height map's size, and the terrain has none");
            const WriteResult made = transaction.createMap(map, newMapFormat(transaction, defaults, 4), 0.0f);
            if (!made.ok) return fail(made.error);
        }
    }

    const size_t slots = static_cast<size_t>(std::max(topLayer + 1, needed * 3));
    std::vector<float> perLayer(slots);
    for (int m = 0; m < needed; m++) {
        const MapId map{MapKind::Blend, m};
        const MapFormat format = transaction.format(map);
        if (!format.bounds.contains(rect)) return fail("rectangle outside blend map " + std::to_string(m));
        const int channels = floatChannels(map, format);
        std::vector<float> values(static_cast<size_t>(rect.area()) * channels);
        if (!transaction.read(map, rect, values)) return fail("could not read a blend map");

        for (size_t texel = 0; texel < static_cast<size_t>(rect.area()); texel++) {
            encodeLayerWeights(layers, weights.subspan(texel * layers.size(), layers.size()), perLayer);
            float* stored = values.data() + texel * channels;
            bool changed = false;
            for (size_t l = 0; l < slots; l++) {
                const LayerChannel channel = terrainLayerChannel(static_cast<int>(l));
                if (channel.map != m || channel.channel >= channels) continue;
                const float value = channel.inverted ? 1.0f - perLayer[l] : perLayer[l];
                if (quantizeChannel(stored[channel.channel], format.bitsPerChannel) != quantizeChannel(value, format.bitsPerChannel)) changed = true;
                stored[channel.channel] = value;
            }
            // up to 9 layers the alpha is unused, and saved opaque; past them it holds a layer's weight
            if (channels >= 4 && slots <= static_cast<size_t>(TERRAIN_LAYERS_IN_RGB)) stored[3] = 1.0f;
            if (changed) entry.changedTexels++;
        }
        const WriteResult wrote = transaction.write(map, rect, values);
        if (!wrote.ok) return fail(wrote.error);
    }
    return true;
}

bool Batch::setDensity(TexelRect rect, int foliageLayer, std::span<const float> values) {
    if (foliageLayer < 0) return fail("a foliage layer index is negative");
    const MapId map{MapKind::Density, foliageLayer};
    if (!transaction.format(map).exists) {
        std::string why;
        if (!transaction.canCreateMap(map, &why)) return fail(why.empty() ? "no such foliage layer" : why);
        if (!transaction.format(HEIGHT_MAP).exists) return fail("a new density map takes the height map's size, and the terrain has none");
        OpOptions defaults;
        const WriteResult made = transaction.createMap(map, newMapFormat(transaction, defaults, 1), 0.0f);
        if (!made.ok) return fail(made.error);
    }
    const MapFormat format = transaction.format(map);
    if (!format.bounds.contains(rect)) return fail("rectangle outside the density map");
    if (values.size() < static_cast<size_t>(rect.area())) return fail("too few density values");

    std::vector<float> previous(static_cast<size_t>(rect.area()));
    if (!transaction.read(map, rect, previous)) return fail("could not read the density map");
    for (size_t i = 0; i < previous.size(); i++) {
        if (values[i] < 0.0f) entry.clippedLow++;
        else if (values[i] > 1.0f) entry.clippedHigh++;
        if (quantizeChannel(previous[i], format.bitsPerChannel) != quantizeChannel(values[i], format.bitsPerChannel)) entry.changedTexels++;
    }
    const WriteResult wrote = transaction.write(map, rect, values);
    return wrote.ok ? true : fail(wrote.error);
}

bool Batch::readHeights(TexelRect rect, std::span<float> out) const {
    return transaction.read(HEIGHT_MAP, rect, out);
}

TerrainOpResult Batch::commit() {
    TerrainOpResult result;
    result.ops.push_back(entry);
    result.warnings = warnings;
    result.errors = errors;
    firstError(result);
    if (!errors.empty()) {
        transaction.discard();
        return result;
    }
    if (entry.clippedLow > 0) result.warnings.push_back(OpWarning{0, "clipped_low", std::to_string(entry.clippedLow) + " values were clamped at 0."});
    if (entry.clippedHigh > 0) result.warnings.push_back(OpWarning{0, "clipped_high", std::to_string(entry.clippedHigh) + " values were clamped at 1."});
    reportOverlay(transaction, result, base.frame().maxHeight);
    const WriteResult committed = transaction.commit();
    if (!committed.ok) {
        failResult(result, committed.error);
        result.changed.clear();
        result.createdMaps.clear();
        return result;
    }
    result.tilesCreated = committed.tilesCreated;
    result.ok = true;
    return result;
}

void Batch::discard() {
    transaction.discard();
    errors.clear();
    warnings.clear();
    entry = OpEntry{};
    entry.op = "batch";
}
