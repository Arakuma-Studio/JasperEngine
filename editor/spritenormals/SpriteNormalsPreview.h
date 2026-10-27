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
    // lit 315° | lit 225°" (upper-left, upper-right, lower-right, lower-left), on a checkerboard
    struct SheetOptions {
        int maxWidth = 1024;
        int maxHeight = 1024;
        int maxFrames = 8;
        int maxScale = 8;
    };
    Image contactSheet(const Image& color, const Image& normal, const std::vector<Cell>& cells, bool directx,
                       float alphaThreshold, const SheetOptions& options = SheetOptions());
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
    };
    Metrics measure(const Image& normal, const std::vector<Cell>& cells, bool directx, float alphaThreshold);

    struct Assessment {
        std::vector<std::string> lines;   // "Shape: rounded (mean tilt 24°, 12% flat)", ...
        std::string hint;                 // "Looks fine." when nothing applies
        std::vector<Problem> warnings;    // kinds of section 7.3
    };
    Assessment assess(const Settings& settings, const Metrics& metrics, int cellCount);
}
