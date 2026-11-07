// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_OPS_H
#define TERRAIN_OPS_H

// The operation engine (T1 R1-R13, R40, R87-R89, programme D4): every write to a terrain means the
// same thing whoever asks for it, and can be tested without a GPU. Each op is one application with a
// stated value, not a rate. Lists are all-or-nothing: validation names ops[i].field before anything
// runs, the ops run on an overlay, and a failure leaves the store byte-identical (DC1). The same store
// bytes and the same list give the same output bytes on any thread count (R8).

#include "Export.h"
#include "terrain/OverlayMapStore.h"
#include "terrain/TerrainMapStore.h"
#include "terrain/TerrainPlacement.h"
#include "terrain/TerrainShape.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::terrain {

    // §6 Table 1: 24 public ops, then the internal one with its own type. apply_field and set_heights
    // are their own functions below.
    enum class OpType : uint8_t {
        Raise, Lower, SetHeight, LiftFloors, FlattenPeaks, Smooth, Sharpen, AddNoise, Terrace, Stamp, Erode, Ramp, RemapHeights,
        PaintLayer, PaintBase, PaintByRules, RemapLayers, PaintDensity, EraseDensity, CutHoles, FillHoles, PlaceObjects, EraseObjects,
        ImportHeightmap,
        FillNoise,
        Registered
    };

    // The name an op has on MCP and in Lua; null for Registered, which has its own
    JASPERENGINE_API const char* opTypeName(OpType type);
    // false for a name that is not built in (it may be registered: registeredOpExists)
    JASPERENGINE_API bool opTypeFromName(std::string_view name, OpType& out);

    // A value of a registered op's own keys, as MCP's params or a Lua table hand them over (R89)
    struct OpParam {
        enum class Kind : uint8_t { None, Bool, Number, String, List, Object };

        Kind kind = Kind::None;
        bool boolean = false;
        double number = 0.0;
        std::string text;
        std::vector<OpParam> items;                             // List
        std::vector<std::pair<std::string, OpParam>> fields;    // Object

        const OpParam* find(std::string_view key) const {
            for (const auto& [name, value] : fields) {
                if (name == key) return &value;
            }
            return nullptr;
        }
    };

    // paint_by_rules: in order, later over earlier
    struct PaintRule {
        int layer = 0;                      // -1: the base, which clears what came before
        std::optional<FilterRange> slope;   // degrees
        std::optional<FilterRange> height;  // terrain-local
        bool invert = false;
        float opacity = 1.0f;
        float noise = 0.0f;                 // 0-1: how much a fractal field breaks the rule's edge
    };

    // fill_noise (R19): what set_terrain_maps generates, as normalized values
    enum class FillMode : uint8_t { Flat, RandomNoise, FractalNoise };

    struct Op {
        OpType type = OpType::Raise;
        std::string registeredName;     // Registered
        OpParam params;                 // Registered: its own keys

        Shape shape;
        Filter filter;
        float strength = 1.0f;

        // Heights are terrain-local, 0..maxHeight (DC5)
        std::optional<float> height;            // set_height, lift_floors, flatten_peaks
        std::optional<float> delta;             // raise, lower, stamp; signed. A stroke's stamps default to a brush's deposit
        std::optional<FilterRange> heights;     // ramp: its ends; default the surface there
        std::optional<FilterRange> inRange;     // remap_heights
        std::optional<FilterRange> outRange;    // remap_heights

        int iterations = 1;                     // smooth, sharpen, erode
        int steps = 8;                          // terrace
        std::optional<float> amplitude;         // add_noise: terrain-local. fill_noise: normalized
        float noiseSize = 4.0f;                 // add_noise, paint_by_rules: world units per noise cell
        int octaves = 4;
        uint32_t seed = 0;

        std::optional<int> layer;               // paint_layer, paint_base (optional), paint_density, erase_density
        bool normalize = true;                  // paint_layer
        std::vector<PaintRule> rules;           // paint_by_rules
        std::vector<int> order;                 // remap_layers: new index per layer

        // erode: a whole application is one second of the brush's flow; a brush asks for less and
        // carries the fraction itself
        float erodeSeconds = 1.0f;
        std::optional<int> droplets;

        // import_heightmap: normalized, decoded by the caller (no file IO here), the map's own size.
        // Another size replaces the map, which is a structural step, not an op (R1, R61).
        std::vector<float> importedHeights;
        int importedWidth = 0, importedHeight = 0;

        // fill_noise
        FillMode fill = FillMode::Flat;
        float baseHeight = 0.0f;                // normalized
        float frequency = 1.0f;

        // place_objects
        float spacing = 1.0f;
        int count = 1;
        float minScale = 1.0f, maxScale = 1.0f;
        float rotationJitter = 1.0f;
        std::vector<Vector2> existingObjects;   // place_objects: spacing checks them; erase_objects: its candidates
    };

    struct OpOptions {
        bool parallel = true;
        // Runs everything on an overlay and discards it: same result, nothing changes (R10, R69)
        bool dryRun = false;
        // The caller already holds a transaction: run straight on it, keeping its float working
        // values between calls (a stroke's stamps). Nothing commits and changed stays empty; the
        // caller reads its overlay.
        bool inPlace = false;
        // New blend and density maps; 0 is the height map's resolution
        int newMapResolution = 0;
    };

    struct OpError {
        int op = -1;
        std::string field;      // "ops[2].shape.radius"
        std::string message;
    };

    // §6 Table 2's codes, nothing else
    struct OpWarning {
        int op = -1;
        std::string code;
        std::string message;
    };

    // R9, R12, R89: one entry per op
    struct OpEntry {
        std::string op;
        int64_t changedTexels = 0;
        int64_t clippedLow = 0;
        int64_t clippedHigh = 0;
        int64_t filteredOut = 0;
        int64_t outside = 0;        // texels of the shape's bounding box past the map
        std::optional<uint32_t> seed;

        // cut_holes, fill_holes
        int64_t holeTexels = 0;
        float holeFraction = 0.0f;
        bool mapCreated = false;
        bool mapRemoved = false;
        int collisionSamplesOpen = 0;

        // place_objects, erase_objects: pure data the editor turns into commands (R21)
        PlacementPlan plan;
        std::vector<int> erasedObjects;

        // remap_layers: the order the caller gives surfaceLayers in the same step
        std::vector<int> layerOrder;

        // A registered op's own result fields
        std::vector<std::pair<std::string, OpParam>> fields;
    };

    // R9: one rectangle per changed map
    struct MapChangeReport {
        MapId map;
        TexelRect rect;
        Vector2 localMin = Vector2(0.0f, 0.0f);
        Vector2 localMax = Vector2(0.0f, 0.0f);
        int64_t texels = 0;
    };

    // Over the height texels that changed, terrain-local
    struct HeightStats {
        bool valid = false;
        float minBefore = 0.0f, maxBefore = 0.0f;
        float minAfter = 0.0f, maxAfter = 0.0f;
        float meanDelta = 0.0f, maxDelta = 0.0f;
    };

    struct TerrainOpResult {
        bool ok = false;
        bool dryRun = false;
        std::vector<OpError> errors;
        std::string error;      // the first error, as "ops[i].field: message"
        std::vector<MapChangeReport> changed;
        std::vector<MapId> createdMaps;
        std::vector<MapId> removedMaps;
        std::vector<OpEntry> ops;
        HeightStats stats;
        std::vector<OpWarning> warnings;
        int tilesCreated = 0;

        // Undo of a block-by-block edit (R87): the caller keeps them with its step
        std::vector<std::shared_ptr<TerrainLargeEdit>> largeEdits;
        // Undo of a registered op's non-map data (R89), joined to the same step. store.takeUndoHook()
        // stays the caller's to take, for what the store itself created.
        std::vector<std::shared_ptr<TerrainStoreUndoHook>> undoHooks;

        bool hasWarning(std::string_view code) const {
            for (const OpWarning& warning : warnings) {
                if (warning.code == code) return true;
            }
            return false;
        }
    };

    // What a registered op sees while it runs (R89)
    class OpContext {
    public:
        virtual ~OpContext() = default;
        // The transaction: writes here join the list's validation, overlay and result
        virtual TerrainMapStore& store() = 0;
        virtual bool dryRun() const = 0;
        virtual bool parallel() const = 0;
        virtual void warn(const std::string& code, const std::string& message) = 0;
        // Undo of data outside the store (T5's TreeStore), joined to the step
        virtual void addUndoHook(std::unique_ptr<TerrainStoreUndoHook> hook) = 0;
        // Stops the list; everything it did is discarded
        virtual void fail(const std::string& message) = 0;
    };

    // False with error filled (field and message) to refuse the list before anything runs
    using OpValidateFn = bool (*)(const TerrainMapStore& store, const Op& op, OpError& error);
    using OpApplyFn = void (*)(OpContext& context, const Op& op, OpEntry& entry);

    struct RegisteredOpTraits {
        // It works over several frames (T5's bake_impostors renders a view per frame): the handler
        // makes the call a job whatever its texel count
        bool spansFrames = false;
    };

    // R89: adds a kind from another file, which runs inside the same validation, overlay, result and
    // undo. Main thread, before any op uses it. False when the name is one of Table 1's.
    JASPERENGINE_API bool registerOp(std::string_view name, OpValidateFn validate, OpApplyFn apply, RegisteredOpTraits traits = {});
    JASPERENGINE_API bool registeredOpExists(std::string_view name);
    JASPERENGINE_API bool registeredOpSpansFrames(std::string_view name);
    // Tests: the registry as it was, so one test's op cannot reach the next
    JASPERENGINE_API void unregisterOp(std::string_view name);

    // Validates without touching the store: ok false with ops[i].field errors, a shape wholly
    // outside the terrain stating the bounds (R10), a refused rectangle (R86)
    JASPERENGINE_API TerrainOpResult validateOps(const TerrainMapStore& store, std::span<const Op> ops);
    JASPERENGINE_API TerrainOpResult applyOps(TerrainMapStore& store, std::span<const Op> ops, const OpOptions& options = {});

    // Absolute write of a rectangle of terrain-local heights (import, Lua). Clamps count and warn (R7).
    JASPERENGINE_API TerrainOpResult setHeights(TerrainMapStore& store, TexelRect rect, std::span<const float> localHeights);

    // R40: the C++ route for spline bakes and T2. Per texel, from the callback: new = current +
    // (target - current) * weight, with the same quantization, dirty tracking and overlay. Serial, in
    // row order. current and target hold floatChannels values.
    struct FieldSample {
        float weight = 0.0f;
        float target[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };
    using FieldFn = FieldSample (*)(void* user, float localX, float localZ, std::span<const float> current);
    JASPERENGINE_API TerrainOpResult applyField(TerrainMapStore& store, MapId map, TexelRect rect, FieldFn field, void* user);

    // The stored bytes, row by row (util/Fnv.h), whatever blocks a store keeps them in
    JASPERENGINE_API uint64_t mapHash(const TerrainMapStore& store, MapId map);

    // R88's single encoder of layer weights (T2 R30): clamp 0..1, normalize a sum above 1, and layer
    // 0 takes the rest unless it has its own value. perLayer is indexed by layer, must reach every
    // listed layer and layer 0, and gets 0 for the unlisted ones.
    JASPERENGINE_API void encodeLayerWeights(std::span<const int> layers, std::span<const float> values, std::span<float> perLayer);

    // R88: the absolute write route for generators (T2 N9). Everything lands in one transaction; the
    // editor wraps the commit as one undo step with the caller's name. Without a commit, nothing changes.
    class JASPERENGINE_API Batch {
    public:
        explicit Batch(TerrainMapStore& store);

        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;

        // Normalized, one value per texel; out-of-range values clamp and count
        bool setHeights(TexelRect rect, std::span<const float> normalized);
        // weights: rect.w * rect.h * layers.size() values, interleaved per texel. Blend map texels;
        // every blend map it needs is created, and every existing one is rewritten over the rect.
        bool setLayerWeights(TexelRect rect, std::span<const int> layers, std::span<const float> weights);
        bool setDensity(TexelRect rect, int foliageLayer, std::span<const float> values);
        bool readHeights(TexelRect rect, std::span<float> out) const;

        // The overlay, to read what the batch would leave or cut deltas from
        OverlayMapStore& overlay() { return transaction; }

        // A failed call makes the commit fail: nothing of the batch lands
        TerrainOpResult commit();
        void discard();

    private:
        bool fail(const std::string& message);

        TerrainMapStore& base;
        OverlayMapStore transaction;
        OpEntry entry;
        std::vector<OpError> errors;
        std::vector<OpWarning> warnings;
    };

}

#endif // TERRAIN_OPS_H
