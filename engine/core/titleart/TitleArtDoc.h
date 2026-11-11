// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.1)
// - Purpose: the `.titleart` document in memory, its constants, limits, enum names and value
//   types, the problems and warnings every Title Art surface reports, and helpers that walk the
//   layer tree.
// - Design: the document is its normalized JSON tree (TitleArtDoc::root, an ordered_json).
//   normalizeTitleArt (TitleArtIO.h) makes every member the schema knows (TitleArtSchema.h)
//   present, typed and clamped, in the schema's order, with unknown members kept after the
//   known ones; so a member a later spec registers (S81's `runtime`, S85's `stretch`, S86's
//   `style_3d`, S87's `translations`) gets defaults, clamping, canonical writing, ops
//   validation and content hashing from one table entry, without a struct of its own, and
//   field paths (`style.rims[1].width`, the ops' and exposed targets' addressing) are paths in
//   this tree. Consumers read typed values with the decoders below (Color, Vec2, Gradient,
//   Curve, AssetRef, Transform) or json accessors; after normalize every member read exists
//   with its schema type.
// - Coordinates: document space is canvas pixels, origin at the canvas centre, +x right, +y up;
//   image space (PNG rows, crops, frames in results) is top-left, +y down. TitleFrame
//   (render/TitleFrame.h) is the one conversion.
// - Thread-safety: TitleArtDoc is a value (copyable, the renderer's snapshots are
//   shared_ptr<const TitleArtDoc>); the free functions are pure.

#pragma once

#include "Export.h"
#include "json.hpp"

#include <cstdint>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::titleart {

    using Json = nlohmann::ordered_json;

    constexpr const char* TITLE_ART_FORMAT = "jasperengine.titleart";
    constexpr int TITLE_ART_VERSION = 1;
    constexpr const char* TITLE_ART_EXTENSION = ".titleart";
    // the base of TITLE_ART_BAKE_VERSION (§6.9): plus the passVersion of every definition a
    // document uses, so a changed kernel makes only the titles that use it stale
    constexpr int TITLE_ART_BAKE_VERSION_BASE = 1;

    // §6.1 limits (normalize clamps to them with a warning)
    constexpr size_t TITLE_MAX_LAYERS = 256;            // in total, every depth
    constexpr int TITLE_MAX_GROUP_DEPTH = 16;
    constexpr size_t TITLE_MAX_EFFECTS = 32;            // per stack (layer effects or one part's)
    constexpr size_t TITLE_MAX_RIMS = 16;
    constexpr size_t TITLE_MAX_MASKS = 8;               // per layer
    constexpr size_t TITLE_MAX_EXPOSED = 64;
    constexpr size_t TITLE_MAX_OUTPUTS = 8;
    constexpr size_t TITLE_MAX_TEXT_CODEPOINTS = 1024;  // per text layer
    constexpr int TITLE_MIN_CANVAS_SIDE = 16;
    constexpr int TITLE_MAX_CANVAS_SIDE = 8192;
    constexpr int64_t TITLE_MAX_CANVAS_PIXELS = 33554432;

    // ---- enums of the document (stored as snake_case strings) ----

    enum class LayerType : uint8_t { Text, Shape, Image, Vector, Group, Generator, Adjustment, Unknown };
    JASPERENGINE_API const char* layerTypeName(LayerType type);    // "text" ... ("" for Unknown)
    JASPERENGINE_API const char* layerTypeLabel(LayerType type);   // "Text", "Vector (SVG)" ...
    JASPERENGINE_API LayerType layerTypeFromName(std::string_view name);   // Unknown when none
    // Text, Shape, Vector and Image layers have `dimension` (2D | 3D) and a `style` (§6.1)
    JASPERENGINE_API bool layerTypeHasStyle(LayerType type);

    // §6.4: the 18 modes, enum values 0..17 in fam_sim_blend.glsl's order
    enum class TitleBlend : uint8_t {
        Normal, Add, Screen, Multiply, Overlay, SoftLight, HardLight, Darken, Lighten,
        ColorDodge, ColorBurn, LinearBurn, Difference, Exclusion, Hue, Saturation, Color, Luminosity
    };
    constexpr int TITLE_BLEND_COUNT = 18;
    JASPERENGINE_API const char* titleBlendName(TitleBlend blend);     // "normal", "color_dodge" ...
    JASPERENGINE_API const char* titleBlendLabel(TitleBlend blend);    // "Normal", "Add (Linear Dodge)" ...
    JASPERENGINE_API bool titleBlendFromName(std::string_view name, TitleBlend& out);

    // an output's content (§6.1 Outputs), also RenderRequest::content
    enum class OutputContent : uint8_t { All, Layers, Ink, Glow, Mask };
    JASPERENGINE_API const char* outputContentName(OutputContent content);
    JASPERENGINE_API bool outputContentFromName(std::string_view name, OutputContent& out);

    enum class MaskType : uint8_t { Shape, Gradient, Layer, Image, Unknown };
    JASPERENGINE_API const char* maskTypeName(MaskType type);
    JASPERENGINE_API MaskType maskTypeFromName(std::string_view name);

    // ---- value types (§6.1 "Value types") ----

    // sRGB-encoded channels 0..1 (the file's "#rrggbb"; alpha is always a separate member)
    struct TitleColor {
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };
    // "#rrggbb", "#rgb" (any case) or [r, g, b] in 0..1 (MCP's forms); false otherwise
    JASPERENGINE_API bool parseTitleColor(const Json& value, TitleColor& out);
    // "#rrggbb", lowercase
    JASPERENGINE_API std::string formatTitleColor(const TitleColor& color);
    // the sRGB transfer function, per channel
    JASPERENGINE_API float srgbToLinear(float c);
    JASPERENGINE_API float linearToSrgb(float c);

    struct TitleVec2 {
        double x = 0.0, y = 0.0;
    };
    JASPERENGINE_API TitleVec2 decodeVec2(const Json& value);

    // S04's AssetRef: `path` relative to the assets directory, `guid` 32 lowercase hex; the GUID
    // wins when both resolve. Null in the file is an empty ref.
    struct TitleAssetRef {
        std::string path;
        std::string guid;
        bool empty() const { return path.empty() && guid.empty(); }
    };
    JASPERENGINE_API TitleAssetRef decodeAssetRef(const Json& value);
    JASPERENGINE_API Json encodeAssetRef(const TitleAssetRef& ref);    // null when empty

    struct TitleGradientStop {
        double pos = 0.0;
        TitleColor color;
        double alpha = 1.0;
        double mid = 0.5;       // Photoshop midpoint to the next stop, 0.05..0.95
    };
    struct TitleGradient {
        std::vector<TitleGradientStop> stops;   // sorted by pos, 2..32
        bool vivid = false;                     // interp "vivid" (OKLCH, shorter hue arc) else "rgb"
    };
    JASPERENGINE_API TitleGradient decodeGradient(const Json& value);

    struct TitleCurvePoint {
        double x = 0.0, y = 0.0;
        bool corner = false;
    };
    // 0..1 both axes, sorted by x, x = 0 and x = 1 present; monotone cubic (Fritsch-Butland)
    // between smooth points, one-sided tangents at corners
    struct TitleCurve {
        std::vector<TitleCurvePoint> points;
        // the curve at x (clamped to 0..1)
        double evaluate(double x) const;
        // its slope at x: the segment's on the left of a key when `fromLeft` (a corner key has
        // a slope on each side), else the one on the right (S86's custom bevel normals)
        double derivative(double x, bool fromLeft = false) const;
        // `count` samples from x = 0 to 1 (the 256-entry LUTs of the renderer)
        std::vector<float> sample(int count) const;
    };
    JASPERENGINE_API TitleCurve decodeCurve(const Json& value);

    // Transform (§6.1): position px (y up), rotation deg clockwise, uniform scale and S85's
    // stretch, flips (mirror about the position, before rotation)
    struct TitleTransform {
        TitleVec2 position;
        double rotation = 0.0;
        double scale = 1.0;
        bool flipX = false, flipY = false;
        TitleVec2 stretch{1.0, 1.0};    // S85: a non-uniform scale on top of `scale` (identity [1, 1])
    };
    JASPERENGINE_API TitleTransform decodeTransform(const Json& transform);

    // ---- what parse, normalize, ops, renders and checks report ----

    // a lenient read's or normalize's note: the JSON path and what was done
    // ("layers[2].style.rims[0].width: 140 clamped to 100")
    struct JASPERENGINE_API TitleWarning {
        std::string path;
        std::string message;

        std::string toString() const;
    };

    enum class TitleSeverity : uint8_t { Info, Warning, Error };
    JASPERENGINE_API const char* titleSeverityName(TitleSeverity severity);    // "info", "warning", "error"

    // A problem of a document, a render or a check, the one shape every surface shows
    // (Inspector, get_title, check_title, check --titles): `code` snake_case (glyph_missing,
    // asset_missing, unknown_type, mask_cycle, exposed_target_missing, dimension_unavailable...),
    // `layer` the layer id ("" for the document), `path` the field path, `detail` the code's
    // extra members (glyph_missing: {chars: [...], fonts_with_it: [...]}).
    struct JASPERENGINE_API TitleProblem {
        TitleSeverity severity = TitleSeverity::Warning;
        std::string code;
        std::string layer;
        std::string path;
        std::string message;
        Json detail;

        Json toJson() const;
    };

    // A value applied to the snapshot copy of one render (S81's exposed values, S85's sampled
    // poses, S87's resolved languages): `path` a field path of §6.2 (shorthands accepted) in layer
    // `layerId`, "" for a document member. TitleArtIO's applyFieldOverrides applies them.
    struct FieldOverride {
        std::string layerId;
        std::string path;
        Json value;
    };

    // ---- the document ----

    class JASPERENGINE_API TitleArtDoc {
    public:
        // the normalized tree (format, version, guid, name, canvas, global_light, seed,
        // material_links, layers, exposed, outputs, then reserved and unknown members)
        Json root = Json::object();

        const std::string& guid() const;
        int canvasWidth() const;
        int canvasHeight() const;
        // top-level layers, bottom first (draw order)
        const Json& layers() const;
        Json& layers();
    };

    // 32 lowercase hex characters (S04's GUID form)
    JASPERENGINE_API std::string generateTitleArtGuid();
    JASPERENGINE_API bool isTitleArtGuid(std::string_view text);
    // 8 lowercase hex characters (a layer, rim, mask or effect id; one id space per document)
    JASPERENGINE_API bool isTitleArtId(std::string_view text);

    // New ids that collide with none a document holds. Ids are identity only: rendering never
    // depends on them, so they are random (a seeded generator gives tests a fixed sequence).
    class JASPERENGINE_API TitleIdGenerator {
    public:
        TitleIdGenerator();
        explicit TitleIdGenerator(uint64_t seed);
        // every id in `doc` (layers, rims, masks, effects, at every depth) becomes taken
        void reserveFrom(const Json& root);
        void reserve(const std::string& id);
        bool taken(const std::string& id) const;
        std::string next();

    private:
        std::mt19937_64 random;
        std::set<std::string> used;
    };

    // ---- the layer tree ----

    JASPERENGINE_API LayerType layerTypeOf(const Json& layer);
    // a layer's children (groups), else an empty array
    JASPERENGINE_API const Json& layerChildren(const Json& layer);
    // the layer with this id at any depth; null when none
    JASPERENGINE_API Json* findLayer(Json& root, std::string_view id);
    JASPERENGINE_API const Json* findLayer(const Json& root, std::string_view id);
    // every layer in document order (a group before its children, children bottom first), with
    // its parent (null at the top level) and depth (0 at the top level); return false to stop
    using LayerVisitor = std::function<bool(const Json& layer, const Json* parent, int depth)>;
    JASPERENGINE_API void forEachLayer(const Json& root, const LayerVisitor& visit);
    JASPERENGINE_API size_t countLayers(const Json& root);
    // the layer's source size (text em size, shape height, image/SVG larger side): the unit of
    // its `%` members; 0 for layers without one
    JASPERENGINE_API double layerSourceSize(const Json& layer);
    // ids of every rim, mask and effect of a layer (not its children)
    JASPERENGINE_API void collectLayerIds(const Json& layer, std::vector<std::string>& out);

}
