// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PIXELCOLOR_H
#define PIXELART_PIXELCOLOR_H

// The pixel-art family's colour maths (docs/specs/S43-palettes.md R12): sRGB, OKLab, CIELAB,
// CIEDE2000, HSV and OKLCh, the cached nearest-colour matcher, the ordered pair-mix dither,
// median cut and the ramp helpers. One library for the runtime and the editor (S46, S47, S48
// and S49 include it). Everything is deterministic and single-threaded: callers parallelise by
// rows. Powers and roots are fixed Newton iterations in double precision (no std::pow or cbrt,
// whose last bits differ per libc), and the sources build without floating-point contraction,
// so OKLab and the matcher give the same bits on every platform. CIEDE2000 and OKLCh hues use
// the C library's trigonometry.
//
// Two linear conventions meet here. The colour science (OKLab, CIELAB) decodes sRGB with the
// piecewise IEC 61966-2-1 curve. The engine's linear colours (materials, Lua's Color, the
// shaders' includes/srgb.glsl) are gamma 2.2; engineLinear*/fromEngineLinear convert in that
// convention, so a palette colour handed to the renderer shows as its exact byte.

#include "Export.h"
#include "math/Vector4.h"
#include "pixelart/Rgba8.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::pixelart {

    struct Lab { float l = 0.0f, a = 0.0f, b = 0.0f; };
    // OKLCh: lightness, chroma and the hue in degrees [0, 360)
    struct LCh { float l = 0.0f, c = 0.0f, h = 0.0f; };
    // hue in [0, 1) (0 is red), saturation and value in [0, 1] (value may pass 1 mid-computation)
    struct Hsv { float h = 0.0f, s = 0.0f, v = 0.0f; };

    enum class ColorMetric : uint8_t { OkLab, Ciede2000, Rgb };   // S46 R8 uses this name

    // "oklab", "ciede2000", "rgb"
    JASPERENGINE_API const char* colorMetricName(ColorMetric metric);
    JASPERENGINE_API bool parseColorMetric(std::string_view name, ColorMetric& out);

    // "#rrggbb", or "#rrggbbaa" when alpha is asked for (lower case)
    JASPERENGINE_API std::string toHex(Rgba8 color, bool alpha);
    // "#rrggbb" when the colour is opaque, else "#rrggbbaa"
    JASPERENGINE_API std::string toHex(Rgba8 color);
    // "#rrggbb" or "#rrggbbaa", the '#' optional, any case; alpha 255 without one
    JASPERENGINE_API bool parseHex(std::string_view text, Rgba8& out, bool* hasAlpha = nullptr);
    // 0xRRGGBB, the 24-bit key of a colour (alpha left out)
    inline uint32_t packRgb(Rgba8 color) { return (uint32_t(color.r) << 16) | (uint32_t(color.g) << 8) | uint32_t(color.b); }
    inline Rgba8 unpackRgb(uint32_t rgb, uint8_t alpha = 255) {
        return Rgba8{uint8_t((rgb >> 16) & 255u), uint8_t((rgb >> 8) & 255u), uint8_t(rgb & 255u), alpha};
    }

    // x^(1/n) for x >= 0 and n >= 2 (0 for anything else): a fixed Newton iteration
    JASPERENGINE_API double rootFixed(double x, int n);
    inline double cbrtFixed(double x) { return x < 0.0 ? -rootFixed(-x, 3) : rootFixed(x, 3); }

    // The IEC 61966-2-1 curve: bytes through tables, floats in [0, 1] through the curve itself
    JASPERENGINE_API float srgbToLinear(uint8_t c);
    JASPERENGINE_API uint8_t linearToSrgb(float linear);     // rounded to the nearest byte
    JASPERENGINE_API float srgbToLinearf(float c);
    JASPERENGINE_API float linearToSrgbf(float linear);

    // The engine's linear convention (gamma 2.2)
    JASPERENGINE_API float engineSrgbToLinear(float c);
    JASPERENGINE_API float engineLinearToSrgb(float linear);
    JASPERENGINE_API Vector4 toEngineLinear(Rgba8 color);
    JASPERENGINE_API Rgba8 fromEngineLinear(const Vector4& linear);   // clamped, rounded

    // OKLab (Ottosson, linear sRGB matrices)
    JASPERENGINE_API Lab toOklab(Rgba8 color);
    JASPERENGINE_API Lab linearToOklab(float r, float g, float b);
    JASPERENGINE_API Rgba8 fromOklab(const Lab& lab, uint8_t alpha = 255);   // clamped to sRGB
    JASPERENGINE_API void oklabToLinear(const Lab& lab, float rgb[3]);       // unclamped
    JASPERENGINE_API LCh toOklch(const Lab& lab);
    JASPERENGINE_API Lab fromOklch(const LCh& lch);

    // CIELAB under D65, the space CIEDE2000 is defined in
    JASPERENGINE_API Lab toCielab(Rgba8 color);
    // CIEDE2000 with k_L = k_C = k_H = 1
    JASPERENGINE_API float ciede2000(const Lab& x, const Lab& y);

    // HSV of sRGB-encoded channels in [0, 1]
    JASPERENGINE_API Hsv toHsv(float r, float g, float b);
    JASPERENGINE_API void fromHsv(const Hsv& hsv, float rgb[3]);
    JASPERENGINE_API Hsv toHsv(Rgba8 color);
    JASPERENGINE_API Rgba8 fromHsv(const Hsv& hsv, uint8_t alpha = 255);    // clamped, rounded

    // How far apart two colours look (alpha ignored). OkLab: Euclidean in OKLab; Ciede2000: the
    // CIEDE2000 difference; Rgb: Euclidean on the sRGB bytes / 255.
    JASPERENGINE_API float distance(Rgba8 x, Rgba8 y, ColorMetric metric);

    // The nearest palette colour, cached per 24-bit colour in an open-addressed table. Ties go
    // to the lower index. Not thread-safe: one per worker.
    class JASPERENGINE_API PaletteMatcher {
    public:
        // `only` restricts the candidates to those indices; empty means every colour whose alpha
        // is not 0 (a transparent slot is not a colour to match), or all of them when none is
        PaletteMatcher(std::span<const Rgba8> palette, ColorMetric metric, std::span<const uint16_t> only = {});

        int nearest(Rgba8 color);
        std::pair<int, float> nearestWithDistance(Rgba8 color);
        // the same answers, computed without the cache (tests, one-off lookups)
        std::pair<int, float> nearestUncached(Rgba8 color) const;

        // Yliluoma's positional dither (algorithm 1) over the 8 nearest colours: the pair (a, b)
        // and ratio r (in 1/steps, up to `strength`) whose mix minimises |mix(a, b, r) - c|^2 +
        // 0.1 r (1 - r) |a - b|^2 in the metric's space. A colour of the palette gives itself
        // with ratio 0. The ratio says how many of every `steps` dither cells show b: steps is
        // the cell count of the Bayer matrix that shows it (4, 16 or 64; a finer one could not
        // show the difference, a coarser one wastes 8 x 8's levels). ratio16 is in 1/steps.
        struct Mix { uint16_t a = 0, b = 0; uint8_t ratio16 = 0; };
        Mix ditherPair(Rgba8 color, float strength, int steps = 16) const;

        ColorMetric metric() const { return colorMetric; }
        int size() const { return (int)palette.size(); }
        const std::vector<uint16_t>& candidates() const { return candidateIndices; }

    private:
        struct Slot { uint32_t key = 0; uint16_t index = 0; float distance = 0.0f; };   // key: rgb + 1, 0 is empty

        Lab space(Rgba8 color) const;
        // the metric's space of a linear colour (a dither's mix: what the eye averages)
        Lab spaceOfLinear(const float rgb[3]) const;
        float metricDistance(const Lab& x, const Lab& y) const;

        std::vector<Rgba8> palette;
        std::vector<Lab> labs;
        std::vector<std::array<float, 3>> linears;
        std::vector<uint16_t> candidateIndices;
        ColorMetric colorMetric = ColorMetric::OkLab;
        std::vector<Slot> cache;
        size_t cacheUsed = 0;
    };

    // Median cut in OKLab: `count` colours for the pixels (alpha ignored; pass the opaque ones),
    // the `keep` colours first and counted in. Fewer come back when the image has fewer unique
    // colours. Deterministic.
    JASPERENGINE_API std::vector<Rgba8> medianCut(std::span<const Rgba8> pixels, int count, std::span<const Rgba8> keep);
    // Up to `iterations` k-means passes in OKLab over the pixels; the first `fixed` centres (the
    // kept colours) never move. Stops early when no pixel changes centre.
    JASPERENGINE_API std::vector<Rgba8> kMeansRefine(std::span<const Rgba8> pixels, std::vector<Rgba8> centres, int fixed, int iterations);

    // The family's ordered-dither threshold (the CPU twin of includes/bayer.glsl): the standard
    // recursive Bayer matrix of size 2, 4 or 8 at (x, y), as (m + 0.5) / size^2. A mix of ratio
    // k/16 shows its second colour where the threshold is below k/16 (k cells of 16 at size 4).
    JASPERENGINE_API float bayerThreshold(int size, int x, int y);

    // pixelfx's fit(): a ramp resampled to `steps` colours, keeping both ends (no blending)
    JASPERENGINE_API std::vector<Rgba8> fitRamp(std::span<const Rgba8> ramp, int steps);
    // pixelfx's at(): the step for t in [0, 1], never blended ({0, 0, 0, 0} for an empty ramp)
    JASPERENGINE_API Rgba8 rampAt(std::span<const Rgba8> ramp, float t);

}

#endif // PIXELART_PIXELCOLOR_H
