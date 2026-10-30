// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a sprite normal map looks like, without a GPU: the sprite lit by 2D point lights with
// exactly the engine's formula (shaderlib includes/lighting2d.glsl getLight2DIntensity on the
// unlit 2D path of mesh.frag: linear albedo x (ambient + sum of colour x intensity x attenuation
// x N.L), gamma 2.2 back to sRGB), a contact sheet for agents and tests, quality metrics and the
// plain-language assessment shared by the window, the MCP tool and the CLI.

#include "SpriteNormals.h"

#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    struct PaintOverlay;   // SpriteNormalsPaint.h

    // A point light over the image. Image pixel coordinates (x right, y DOWN, origin top-left
    // of the image given to renderLit); height and range in pixels (world units of a 1:1 sprite)
    struct PreviewLight {
        float x = 0.0f;
        float y = 0.0f;
        float height = 64.0f;          // > 0 (the engine default for new Light 2Ds)
        float range = 200.0f;
        float falloff = 1.0f;          // pow(1 - d/range, falloff)
        float red = 1.0f, green = 1.0f, blue = 1.0f;   // linear
        float intensity = 1.0f;
    };

    struct LightSetup {
        std::vector<PreviewLight> lights;
        float ambientRed = 1.0f, ambientGreen = 1.0f, ambientBlue = 1.0f;   // linear
        float ambientIntensity = 0.15f;
        bool directx = false;          // the map's convention (green flipped back before lighting)
        float alphaThreshold = 0.03f;  // pixels at or below are left transparent
    };

    // One light at `angleDegrees` around the image centre (0 = right, 90 = up the image,
    // counter-clockwise), at distance 0.75 * max(w, h), height 0.5 * max(w, h), range 3 * max(w, h)
    LightSetup orbitLight(int width, int height, float angleDegrees, bool directx = false, float alphaThreshold = 0.03f);

    // RGBA8 of `color`'s size: lit colour, the colour's alpha. normal and color the same size.
    Image renderLit(const Image& normal, const Image& color, const LightSetup& setup);

    // The contact sheet (section 7.2): per frame a row "colour | normal | lit 135° | lit 45° |
    // lit 315° | lit 225°" (upper-left, upper-right, lower-right, lower-left), on a checkerboard.
    // `paint` (Phase 5, optional, the colour image's size): a 1 px magenta outline around the
    // painted visible pixels, in the normal column only (the lit columns stay clean).
    struct SheetOptions {
        int maxWidth = 1024;
        int maxHeight = 1024;
        int maxFrames = 8;
        int maxScale = 8;
    };
    Image contactSheet(const Image& color, const Image& normal, const std::vector<Cell>& cells, bool directx,
                       float alphaThreshold, const SheetOptions& options = SheetOptions(), const PaintOverlay* paint = nullptr);
    // Which frames a sheet shows: all when <= maxFrames, else round(i * (n - 1) / (maxFrames - 1))
    std::vector<int> sheetFrames(int frameCount, int maxFrames);

    // ---- quality

    struct Metrics {
        double coveragePct = 0.0;      // opaque pixels (above the cutoff) / all pixels
        double flatPct = 0.0;          // opaque pixels tilted under 2 degrees from +Z
        double steepPct = 0.0;         // tilted over 60 degrees
        double meanTiltDeg = 0.0;      // mean tilt of opaque pixels
        double leanDeg = 0.0;          // angle between the mean opaque normal and +Z (NormalMapTilt's measure)
        double noisePct = 0.0;         // opaque pixels with >= 3 opaque 8-neighbours whose normal is over
                                       // 35 degrees from those neighbours' mean normal
        double softEdgePct = 0.0;      // opaque pixels with alpha < 250
        int framesTouchingEdges = 0;   // cells whose opaque pixels touch the cell border (cells given)
        bool hasTransparency = false;  // some pixel at or below the cutoff
        int opaquePixels = 0;
        int paintedPixels = 0;         // opaque pixels the paint layer covers (Phase 5; `paint` given)
        double paintedPct = 0.0;       // ... / opaque pixels
        // High-resolution smoothness (hires design spec, rule 6), over the "inner" pixels: more
        // than 8k px inside the alpha-0.5 silhouette of their cell, k = max(1, S / 128) with S the
        // median over the cells of the largest island's shorter side (`scale`). Measured only
        // when k > 1 (above 128 px); 0 and innerPixels 0 otherwise.
        double scale = 1.0;
        int innerPixels = 0;
        double roughnessDeg = 0.0;     // mean angle between each normal and its sigma = 4k px blur (of `shape` when given)
        double creasePct = 0.0;        // inner pixels on a crease: over kCreaseDeg from that blur
    };
    // The deviation from the blurred normal that marks a crease pixel (the medial-axis ridge of
    // the rounded profile above 128 px deviates 10-25 degrees along it; inflate stays under 3)
    inline constexpr double kCreaseDeg = 8.0;
    // `paint` (optional, the normal's size): the painted visible pixels counted. The metrics are
    // of `normal` as given: the composited result when there is paint. `shape` (optional,
    // Result::shape): the roughness and creases are measured on it (the silhouette's share: the
    // painted relief is wanted detail, not a defect), else on `normal`.
    Metrics measure(const Image& normal, const std::vector<Cell>& cells, bool directx, float alphaThreshold,
                    const PaintOverlay* paint = nullptr, const Image* shape = nullptr);

    struct Assessment {
        std::vector<std::string> lines;   // "Shape: rounded (mean tilt 24°, 12% flat)", ..., "Painted: 214 px (8.1%)"
        std::string hint;                 // "Looks fine." when nothing applies
        std::vector<Problem> warnings;    // kinds of section 7.3
    };
    // `result` (optional): the profile auto resolved to in the Shape line
    Assessment assess(const Settings& settings, const Metrics& metrics, int cellCount, const Result* result = nullptr);
    // The warning thresholds of assess() (contract table; hires design spec for the last two)
    // (calibrated on the hires evaluation set: the new defaults measure 0.3-0.8 degrees and 0%,
    // the props preset on flat-faced art up to 1.3 degrees and 1.2%; the rounded profile above
    // 128 px 1.3-9.5 degrees and 4.6-68%)
    inline constexpr double kCreasedPct = 2.0;      // crease_pct above: creased
    inline constexpr double kStreakyDeg = 1.5;      // roughness_deg above (and not creased): streaky
}
