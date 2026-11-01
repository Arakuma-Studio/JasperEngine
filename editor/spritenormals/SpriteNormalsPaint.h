// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The paint layer of a sprite normal map (Phase 5): a non-destructive correction on top of the
// generated normals. Per pixel of the output (the atlas for animated sources) it holds a normal
// (OpenGL convention, whatever `directx` says) and a coverage; regenerating with any settings
// composites the same paint over the new result. It lives inside the normal-map PNG as a second
// iTXt chunk (jasper.spritenormals.paint, after the recipe), so it survives copies, renames and
// regeneration and costs nothing at runtime (decoders skip unknown chunks).
//
// Shared by the window's brushes, the MCP tool's `paint` operations and the CLI's --paint-json:
// - PaintOp / applyPaintOps(): angle, flatten, smooth, erase on a selection (rect, pixels,
//   colours, frame; members intersect); applyToPixels() for an explicit pixel list (strokes);
// - composite(): generation + overlay (bit-exact where unpainted), then the DirectX flip;
// - generatePainted(): generate() + ops + composite, what the session's worker runs;
// - encodePaintText() / readPaint() / readPaintSummary(): the chunk.
//
// Engine-free like the rest of the core (SpriteNormals.h).

#include "SpriteNormals.h"

#include "json.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    inline constexpr const char* kPaintKeyword = "jasper.spritenormals.paint";
    inline constexpr const char* kPaintFormat = "jasperengine.spritenormals.paint";
    inline constexpr int kPaintVersion = 1;
    inline constexpr size_t kMaxPaintPngBytes = 8u * 1024u * 1024u;   // the embedded PNG, before base64
    inline constexpr int kMaxPaintOps = 256;
    inline constexpr int kMaxPaintPixels = 65536;                      // where.pixels pairs per op
    inline constexpr int kMaxPaintColors = 16;
    inline constexpr float kMaxPaintTilt = 85.0f;                      // degrees from +Z

    // ---- the overlay

    struct PaintOverlay {
        int width = 0, height = 0;          // == the normal output's size (the atlas for animated sources)
        std::vector<uint8_t> rgba;          // width*height*4. RGB: OpenGL-convention normal encoded
                                            // round((n*0.5+0.5)*255), z > 0 always. A: coverage 0..255.
                                            // A = 0: unpainted, RGB (128,128,255)

        // width x height, nothing painted
        static PaintOverlay blank(int width, int height);
        bool empty() const;                 // no pixel with A > 0
        size_t paintedCount() const;        // pixels with A > 0
        bool sized(int w, int h) const;     // width x height with its pixels
        // Every A = 0 pixel's RGB to (128,128,255): equal overlays encode to equal bytes
        void normalise();
        bool operator==(const PaintOverlay& o) const { return width == o.width && height == o.height && rgba == o.rgba; }
        bool operator!=(const PaintOverlay& o) const { return !(*this == o); }
    };

    // ---- operations

    enum class PaintOpKind { Angle, Flatten, Smooth, Erase };
    enum class PaintMode { Keep, Replace, Clear };                    // where the starting overlay comes from
    enum class PaintTool { Navigate, Angle, Flatten, Smooth, Erase };  // the window
    const char* toString(PaintOpKind kind);   // "angle", "flatten", "smooth", "erase"
    const char* toString(PaintMode mode);     // "keep", "replace", "clear"
    bool parsePaintOpKind(const std::string& text, PaintOpKind& out);
    bool parsePaintMode(const std::string& text, PaintMode& out);

    struct PaintWhere {                     // at least one member; members intersect
        std::optional<std::array<int, 4>> rect;            // x, y, w, h (w,h >= 1)
        std::vector<std::array<int, 2>> pixels;            // <= 65,536 pairs
        std::vector<std::array<uint8_t, 3>> colors;        // <= 16; matched on the source colour
        float tolerance = 0.0f;                            // 0..0.5 Oklab distance; 0 = exact RGB match
        std::optional<int> frame;                          // cell index; rect/pixels become cell-relative
    };

    struct PaintOp {
        PaintOpKind kind = PaintOpKind::Flatten;
        PaintWhere where;
        std::array<float, 3> direction{0.0f, 0.0f, 1.0f};   // Angle only; OpenGL tangent space, normalised, z > 0
        float strength = 1.0f;              // 0..1
        int radius = 1;                     // Smooth only, 1..4 (box kernel (2r+1)^2)
    };

    // Each pixel's cell (first holding it, -1 none) and 8-connected island of opaque pixels within
    // it (-1 transparent): what Smooth averages within. Depends on the source, cells and cutoff
    // only, so the window builds it once per result.
    struct PaintTopology {
        int width = 0, height = 0;
        std::vector<int> owner;
        std::vector<int> island;
    };

    struct PaintContext {                   // what selection and Smooth need
        const Image& source;                // RGBA8, output layout (the colour atlas for animated sources)
        const Image& generatedGL;           // before paint (OpenGL green)
        std::vector<Cell> cells;            // the output's frame cells (empty: one full-image cell)
        float alphaThreshold = 0.03f;
        const PaintTopology* topology = nullptr;   // optional: paintTopology() of the above (else built per Smooth)
    };
    PaintTopology paintTopology(const Image& source, const std::vector<Cell>& cells, float alphaThreshold);

    struct PaintStats {
        struct Op {
            int selected = 0, changed = 0;
        };
        std::vector<Op> ops;                // one per op, in order
        int painted = 0, hidden = 0;        // painted pixels after the ops; of those, now transparent
    };

    struct PaintBrush {                     // the window's brush
        enum class Shape { Round, Square } shape = Shape::Round;
        int size = 1;                       // 1..64 px
        float strength = 1.0f;              // 0..1
        int smoothRadius = 1;               // 1..4
        enum class Fill { Brush, Wand } fill = Fill::Brush;
        float wandTolerance = 0.0f;         // 0..0.5
        bool wandContiguous = true;
        bool wandAllFrames = false;
        bool mirrorX = false;
        std::array<float, 3> direction{0.0f, 0.0f, 1.0f};
    };

    // Every problem of the ops (kind invalid_paint, field "paint[i].where.rect" ...): counts,
    // ranges, directions; frames against `cellCount` when >= 0 (0 cells = one whole-image frame)
    std::vector<Problem> validatePaintOps(const std::vector<PaintOp>& ops, int cellCount = -1);

    // Applies `ops` in order. Problems of kind invalid_paint are errors (nothing applied); others
    // are warnings (paint_empty_selection, naming the op). `stats` (optional): per op selected /
    // changed counts, painted and hidden totals.
    std::vector<Problem> applyPaintOps(PaintOverlay& overlay, const std::vector<PaintOp>& ops, const PaintContext& context,
                                       PaintStats* stats = nullptr);

    // The selection of `where` as pixel indices (y * width + x, ascending, no duplicates):
    // clipped to the image (and the cell with `frame`), minus pixels at or below the alpha cutoff.
    // False with `error` for a frame that is not one.
    bool selectPixels(const PaintWhere& where, const PaintContext& context, std::vector<int>& pixels, std::string* error = nullptr);

    // One operation on given pixel indices (the window's strokes and wand fills; applyPaintOps'
    // work). `directions` (optional, one per pixel) overrides `direction` per pixel (mirrored
    // strokes). Transparent pixels are skipped. Returns selected / changed counts.
    PaintStats::Op applyToPixels(PaintOverlay& overlay, PaintOpKind kind, const std::vector<int>& pixels, const std::array<float, 3>& direction,
                                 float strength, int radius, const PaintContext& context,
                                 const std::vector<std::array<float, 3>>* directions = nullptr);

    // ---- directions

    // azimuth 0 = right (+x), 90 = up the image (+y, OpenGL), counter-clockwise, any value
    // (wrapped); tilt 0 = facing the viewer: x = sin(tilt)cos(az), y = sin(tilt)sin(az), z = cos(tilt)
    std::array<float, 3> directionFromAngles(float azimuthDeg, float tiltDeg);
    // The reverse: azimuth in [0, 360), tilt in [0, 90]
    void anglesOf(const std::array<float, 3>& direction, float& azimuthDeg, float& tiltDeg);
    // [x,y,z] normalised; false with `error` when the length is 0 or z <= 0 after normalising
    bool normaliseDirection(const std::array<float, 3>& in, std::array<float, 3>& out, std::string& error);
    // round((n*0.5+0.5)*255), half away from zero, clamped
    std::array<uint8_t, 3> encodeDirection(const std::array<float, 3>& n);
    // c/127.5 - 1 per channel
    std::array<float, 3> decodeDirection(const uint8_t* rgb);

    // ---- JSON (the MCP `paint` argument and the CLI's --paint-json: the same shape)

    // [{op, where: {rect, pixels, colors, tolerance, frame}, direction: [x,y,z] |
    // {azimuth_deg, tilt_deg}, strength, radius}, ...]; false with problems (kind invalid_paint,
    // field "paint[i].<key>", the valid range or enum in the message)
    bool paintOpsFromJson(const nlohmann::json& array, std::vector<PaintOp>& ops, std::vector<Problem>& problems);
    // Canonical JSON of ops (fingerprints, logs): the same shape, directions as [x,y,z]
    nlohmann::json paintOpsToJson(const std::vector<PaintOp>& ops);

    // ---- compositing

    // RGBA8 of the source's size. Per pixel: alpha = the source's; at or below the cutoff
    // (128,128,255); else coverage 0 keeps the generated bytes, 255 takes the overlay's, between
    // them the renormalised lerp (z at least 0.0175). Then with `directx` green = 255 - green on
    // opaque pixels. An overlay of another size counts as unpainted.
    Image composite(const Image& generatedGL, const PaintOverlay& overlay, const Image& source, float alphaThreshold, bool directx);
    // composite() of the pixels [x0, x1) x [y0, y1) only, into `out` (the source's size): what a
    // stroke changed, without recompositing the whole image (the same bytes as composite())
    void compositeRect(Image& out, const Image& generatedGL, const PaintOverlay& overlay, const Image& source, float alphaThreshold, bool directx, int x0,
                       int y0, int x1, int y1);
    // green = 255 - green on the pixels above the cutoff (DirectX <-> OpenGL; transparent pixels
    // stay (128,128,255))
    void flipGreen(Image& normal, const Image& source, float alphaThreshold);

    // What the session's worker adds to a generation
    struct PaintLayer {
        PaintOverlay overlay;               // in: the starting overlay; out: with the ops applied
        std::vector<PaintOp> ops;
        PaintStats stats;                   // out
        bool active() const { return !ops.empty() || !overlay.empty(); }
    };
    // generate(request, control); when layer.active(), the ops applied to the overlay and
    // result.normal replaced by the composite (warnings paint_empty_selection,
    // paint_hidden_pixels appended to result.warnings; invalid ops: Status::InvalidRequest).
    // Inactive: exactly generate(). Unpainted pixels are bit-exact generate() output in both
    // conventions (the DirectX generation is flipped to OpenGL for compositing, then back).
    Result generatePainted(const Request& request, PaintLayer& layer, const Control& control = Control());

    // ---- storage (iTXt jasper.spritenormals.paint)

    // The chunk text: {"format":"jasperengine.spritenormals.paint","version":1,"size":[W,H],
    // "painted":N,"encoding":"png-base64","data":"<base64 RGBA8 PNG>"} (keys in this order).
    // False with `problem` (kind paint_too_large: the embedded PNG over 8 MiB) . An empty overlay
    // has no chunk: false with problem.kind empty.
    bool encodePaintText(const PaintOverlay& overlay, std::string& text, Problem& problem);
    // `image` as PNG with the recipe and (non-empty overlay) the paint chunk: IHDR, recipe, paint
    bool encodeWithRecipeAndPaint(const Image& image, const nlohmann::json& recipe, const PaintOverlay* overlay, std::vector<uint8_t>& png,
                                  Problem& problem);

    // The overlay of PNG bytes / a file. nullopt with `problem` empty: no paint chunk; nullopt
    // with `problem`: the chunk can't be read (another format, a version above 1, bad base64,
    // an embedded PNG that does not decode, sizes that disagree): paint_unreadable
    std::optional<PaintOverlay> readPaint(const uint8_t* bytes, size_t size, std::string* problem = nullptr);
    std::optional<PaintOverlay> readPaint(const std::filesystem::path& png, std::string* problem = nullptr);

    struct PaintSummary {
        int painted = 0;
        int width = 0, height = 0;
    };
    // {painted, size} from the chunk's JSON without decoding its data, reading only the chunks
    // before IDAT (get_asset_info, view_asset, the Project window)
    std::optional<PaintSummary> readPaintSummary(const std::filesystem::path& png, std::string* problem = nullptr);

    // ---- the window's brush geometry

    // The pixels a brush stamps at (x, y): Round = pixel centres within size/2 of the cursor
    // pixel's centre, Square = size x size; odd sizes centred, even sizes anchored up-left
    std::vector<std::array<int, 2>> brushFootprint(PaintBrush::Shape shape, int size, int x, int y);
    // The pixels of the line from a to b (Bresenham, both ends included)
    std::vector<std::array<int, 2>> linePixels(std::array<int, 2> a, std::array<int, 2> b);
    // The cell holding (x, y) (cells empty: the whole image); -1 when none does
    int cellAt(const std::vector<Cell>& cells, int width, int height, int x, int y);
    // The wand: pixels whose source colour matches (x, y)'s (exact, or Oklab within tolerance),
    // in its cell (or every cell), 8-connected to it when contiguous; opaque ones only
    std::vector<int> wandSelect(const PaintContext& context, int x, int y, float tolerance, bool contiguous, bool allFrames);
}
