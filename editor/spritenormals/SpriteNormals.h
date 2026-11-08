// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Sprite normal maps (editor-only, never shipped in games): a C++ port of Hugo Duprez's
// spritefusion-sprite-normals (MIT, NOTICE.md, LICENSE-spritefusion-sprite-normals.txt) plus
// JasperEngine's options (profiles, edge modes, auto radius, outline and colour regions, height map).
//
// Per frame cell, per 8-connected island of pixels above the alpha cutoff: a height field is the
// silhouette's distance to the edge through a profile (volume), or the Poisson "inflate" solution,
// plus the Oklab lightness of the painted shading, smoothed at two scales (detail); a normalized
// Sobel gives slopes, scaled by strength, encoded OpenGL style (green up the image) unless
// directx. Pixels at or below the alpha cutoff get (128,128,255) with their own alpha; alpha is
// always the source's byte.
//
// High-resolution sprites (settings version 2, 2026-10-30-sprite-normals-hires-design.md): the
// smoothing, softness and detail values are in "128 px reference" units, multiplied by
// k = max(1, S / 128) where S is the island's shorter side (resolution_scale auto); auto radius is
// 0.3 x the island's largest inscribed distance; profile auto is rounded up to 128 px and inflate
// above; soft pixels (alpha under 0.5) reachable from the transparent outside count as outside the
// silhouette of islands above 128 px. At 128 px and below, version 2 differs from version 1 only
// by the auto radius. Version 1 (recipes written before) keeps the old rules exactly.
//
// Deterministic: identical inputs and settings give identical bytes whatever the thread count.
// With profile Rounded, edges Transparent, an explicit radius, no outline handling and no
// regions, the output matches the upstream tool within 1 per channel (tests/fixtures).
//
// Usage:
//   spritenormals::Request request;
//   request.color = rgba8Image;                       // texsynth::Image, 4 channels, 8-bit
//   request.settings = spritenormals::presetSettings(spritenormals::Preset::Default);
//   spritenormals::Result result = spritenormals::generate(request);   // blocking
//   if (result.status == spritenormals::Status::Ok) { result.normal ... }

#include "pixelart/PixelProblem.h"
#include "texsynth/SynthImage.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    using Image = texsynth::Image;

    // ---- settings (contract section 3: names, ranges, defaults)

    enum class Preset { Soft, Default, Crisp, Detailed, Chunky, Toon, Painted, Props, Tile };
    // Auto: Rounded for islands (or the frames of a sequence) up to kHiresSide px, Inflate above (version 1: Rounded)
    enum class Profile { Rounded, Dome, Bevel, Inflate, Auto };
    // Auto: smoothing, softness and detail multiplied by k = max(1, S / kHiresSide); Off: as given
    enum class ResolutionScale { Auto, Off };
    // Auto: soft pixels (alpha under 0.5) reached from the outside are outside the silhouette
    // (version 2, above 128 px: glows and feathered rims don't inflate the sprite); Solid: every
    // pixel above the cutoff counts as shape (as version 1). Nothing else changes.
    enum class SoftEdge { Auto, Solid };
    enum class Edges { Transparent, Wrap, Extend };

    // A colour region: pixels whose Oklab colour is within `tolerance` of one of `colors`
    struct Region {
        std::vector<uint32_t> colors;   // 0xRRGGBB, 1..16
        float tolerance = 0.03f;        // Oklab Euclidean distance, 0..0.5
        float detail = 1.0f;            // 0..8: multiplier of the detail term (0: the colour is ignored by it)
        float lift = 0.0f;              // -16..16: pixels of height added (before strength)
        bool flat = false;              // the pixels' normals are forced to +Z
    };

    // Normal quantization ("Toon steps"): every generated normal snapped to a palette of
    // `directions` azimuths x `tilts` tilt rings, plus flat, for stepped cel-shaded light.
    // Off (directions 0) by default: unquantized output is byte-identical to before.
    struct Quantize {
        int directions = 0;                   // 0 (off), 4, 6, 8, 12, 16: azimuth steps around the circle
        int tilts = 1;                        // 1..4: tilt rings between flatDeg and 90 degrees
        float flatDeg = 15.0f;                // 0..45: normals tilted less become exactly (0, 0, 1)
        float rotationDeg = 0.0f;             // 0..360 (360 is 0: recipes record 0): the palette's first azimuth (0 = +X, right)
        bool on() const { return directions > 0; }
        bool operator==(const Quantize& o) const {
            return directions == o.directions && tilts == o.tilts && flatDeg == o.flatDeg && rotationDeg == o.rotationDeg;
        }
        bool operator!=(const Quantize& o) const { return !(*this == o); }
    };

    // The size up to which nothing scales (k = 1) and profile auto is rounded: pixel art
    inline constexpr int kHiresSide = 128;
    // The settings version new generations use; 1 = before high-resolution support (recipes
    // written then regenerate with its rules: bbox auto radius, no resolution scaling, no outer band)
    inline constexpr int kSettingsVersion = 2;

    struct Settings {
        Preset preset = Preset::Default;      // what the main values started from (recorded, never re-applied)
        float strength = 1.5f;                // 0..8
        float detail = 3.0f;                  // 0..8 (x k)
        float smoothing = 1.0f;               // 0..32 px (x k)
        float volume = 1.15f;                 // 0..8
        std::optional<float> radius;          // nullopt: auto; else 0.5..512 px (never scaled)
        float softness = 1.15f;               // 0..32 px (x k)
        float alphaThreshold = 0.03f;         // 0..0.99
        Profile profile = Profile::Auto;
        Edges edges = Edges::Transparent;
        ResolutionScale resolutionScale = ResolutionScale::Auto;
        SoftEdge softEdge = SoftEdge::Auto;
        int version = kSettingsVersion;       // 1 or 2 (above)
        bool ignoreOutline = false;
        std::vector<Region> regions;          // at most 16
        bool directx = false;
        bool height = false;                  // also produce Result::height
        Quantize quantize;                    // off unless directions > 0 (the toon preset turns it on)
    };

    // The preset's values (contract section 3 table); softness 1.15 unless the preset says
    // otherwise, radius auto, alpha 0.03, profile auto (painted: inflate, props: dome), edges
    // transparent (tile: wrap), resolution scale auto, version 2, no outline handling, no regions,
    // OpenGL, no height map; quantize off except for toon.
    // Resolving a preset with overrides (every surface does exactly this):
    //   Settings s = presetSettings(p); settingsFromJson(overrides, s, error); s.preset = p; validateSettings(s)
    Settings presetSettings(Preset preset);
    // "soft", "default", "crisp", "detailed", "chunky", "toon", "painted", "props", "tile" (and
    // back; false for an unknown name)
    const char* toString(Preset preset);
    const char* toString(Profile profile);    // "rounded", "dome", "bevel", "inflate", "auto"
    const char* toString(Edges edges);        // "transparent", "wrap", "extend"
    const char* toString(ResolutionScale scale);   // "auto", "off"
    const char* toString(SoftEdge edge);           // "auto", "solid"
    bool parsePreset(const std::string& name, Preset& out);
    bool parseProfile(const std::string& name, Profile& out);
    bool parseEdges(const std::string& name, Edges& out);
    bool parseResolutionScale(const std::string& name, ResolutionScale& out);
    bool parseSoftEdge(const std::string& name, SoftEdge& out);
    // Every preset, in the order of the window's buttons; the names joined ("soft, default, ...")
    const std::vector<Preset>& allPresets();
    std::string presetNames(const char* separator = ", ");
    // "#rrggbb" or "rrggbb" (case-insensitive) -> 0xRRGGBB; "#rrggbb" back (lower case)
    bool parseColor(const std::string& text, uint32_t& out);
    std::string colorText(uint32_t rgb);

    // ---- quantization (applied by generate() per opaque pixel, after the slopes, before the
    // DirectX flip; paint composites over it)

    // The quantized direction of a unit OpenGL-convention normal (z > 0), in double: tilt
    // t = acos(z) under flatDeg -> (0, 0, 1) (as is a normal without azimuth); else the azimuth
    // snapped to rotation + k * 360 / directions (k rounded half away from zero, measured from
    // the rotation in [0, 360)) and the tilt to the centre of its ring of [flatDeg, 90).
    // directions 0: the input unchanged.
    std::array<double, 3> quantizeNormal(const Quantize& q, double x, double y, double z);
    // Every direction quantizeNormal can return: flat first, then ring by ring (innermost first),
    // azimuth by azimuth from the rotation counter-clockwise. directions 0: empty.
    std::vector<std::array<double, 3>> quantizePalette(const Quantize& q);

    // ---- problems (shared kinds: contract + section 11)

    // the pixel-art family's typed error: kind ("invalid_settings", "frames_mismatch", "no_alpha",
    // ...), field ("strength", "regions[1].tolerance", "frames.tile_size", "source", "" ...),
    // message and fix
    using Problem = pixelart::PixelProblem;

    // Every problem of the settings (never only the first): non-finite values, values out of
    // range ("strength must be between 0 and 8 (got 9)"), too many regions or colours
    std::vector<Problem> validateSettings(const Settings& settings);

    // ---- cells

    // A pixel rectangle of the image, top-left origin
    struct Cell {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool operator==(const Cell& o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
    };

    // The Texture Slicer / set_sprite_frames grid: tile size, or columns and rows; offset,
    // padding, count. Same math as the MCP sliceSheet (WorldTools.cpp): tile_size given ->
    // columns/rows = floor((avail + padding) / (tile + padding)); columns and rows given ->
    // tile = floor((avail - padding * (n - 1)) / n); both given: the tile size is used and
    // columns/rows > 0 cap the counts (sliceSheet); cells row-major, the last ones clipped to
    // the image; count (0 = all) caps them.
    struct Grid {
        int tileWidth = 0;
        int tileHeight = 0;
        int columns = 0;
        int rows = 0;
        int count = 0;
        int offsetX = 0;
        int offsetY = 0;
        int paddingX = 0;
        int paddingY = 0;
    };
    // False with `error` ("The grid is empty: the image is 30x30 pixels.") for an empty grid,
    // or more than kMaxCells cells
    bool sliceGrid(const Grid& grid, int imageWidth, int imageHeight, std::vector<Cell>& cells, std::string& error);
    inline constexpr int kMaxCells = 4096;

    // ---- generation

    struct Request {
        Image color;                  // RGBA8 (4 channels, 8-bit): an image, a sheet or an atlas
        std::vector<Cell> cells;      // empty: the whole image is one cell. Inside the image, not
                                      // overlapping (identical duplicates are processed once);
                                      // pixels outside every cell stay neutral
        bool sequence = false;        // the cells are frames of one animation: auto radius is one
                                      // value for all of them (section 3.6)
        Settings settings;
        int threads = 0;              // 0: hardware concurrency (at most 16); never changes the output
        bool shapeNormals = false;    // also Result::shape (the assessment's roughness and creases)
    };

    struct Progress {
        double fraction = 0.0;        // 0..1 (work items done / total)
        double elapsedSeconds = 0.0;
    };

    struct Control {
        // Called on a generating thread after work items, at most every progressIntervalSeconds
        std::function<void(const Progress&)> onProgress;
        double progressIntervalSeconds = 0.1;
        // Set from any thread: generate() returns Status::Cancelled after the work items running
        const std::atomic<bool>* cancel = nullptr;
    };

    enum class Status { Ok, InvalidRequest, Cancelled, Failed };
    const char* toString(Status status);   // "ok", "invalid_request", "cancelled", "failed"

    struct Result {
        Status status = Status::InvalidRequest;
        std::vector<Problem> errors;         // InvalidRequest / Failed: why
        std::vector<Problem> warnings;       // outline_not_found, region_unmatched (section 3.8)

        Image normal;                        // RGBA8, the colour image's size
        Image shape;                         // request.shapeNormals: the silhouette's share of the normals alone (no
                                             // painted relief, regions or quantization), encoded as `normal`; else empty
        Image height;                        // settings.height: 1-channel 16-bit (section 3.9); else empty
        float heightMin = 0.0f;              // the height range the 16-bit values span (px, after strength)
        float heightMax = 0.0f;

        // The radius used: one value (explicit, or auto for a sequence), or per island (auto on
        // a still image: radiusMin..radiusMax over its islands)
        bool radiusPerIsland = false;
        float radiusMin = 0.0f;
        float radiusMax = 0.0f;
        // The resolution scale k used (1 with resolution_scale off, version 1 or at <= 128 px):
        // over the islands (cells with wrap / extend), one value for a sequence
        float scaleMin = 1.0f;
        float scaleMax = 1.0f;
        // The profiles the islands used once auto is resolved (bits: 1 << int(Profile))
        uint32_t profilesUsed = 0;
        // The largest share of an island's pixels the outer band took (soft_edge auto; 0 without)
        float outerBandShare = 0.0f;

        std::optional<uint32_t> outlineColor;   // ignoreOutline: the colour detected (0xRRGGBB)
        int islands = 0;                     // processed (Transparent edges); cells otherwise
        int threads = 0;                     // used
        double seconds = 0.0;
    };

    // Validates (settings, image format and size, cells), then generates. Never throws:
    // problems come back with Status::InvalidRequest (kinds invalid_settings, invalid_source,
    // frames_mismatch, too_large), Failed when memory ran out. Blocking; any thread.
    Result generate(const Request& request, const Control& control = Control());

    // Limits (contract section 4)
    inline constexpr int64_t kMaxPixels = 16777216;            // 4096 x 4096, per image or atlas
}
