// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R35, R45, §6.14.1)
// - Purpose: S84's three check_title checks (S80's registry, group "style", owner S84):
//   `style_lint` (document) reports StyleLint's findings, one result per finding with id
//   `style_lint.<rule>`, the element, the layer's library look (look.id) and the fix ops; one PASS
//   when there are none. Status: WARN for every rule (the lettering reads poorly but renders),
//   except noop_knockout with no rim at all, a FAIL (the letters render as nothing).
//   `color_scheme_match` (pixels): each layer recoloured by a Color Scheme (look.color_scheme)
//   is rendered alone without its rims and layer effects (the face's own part effects stay);
//   the mean OKLCH hue of that face ink (alpha >= 0.5, white cores excluded) must sit within
//   COLOR_MATCH_HUE_SLACK degrees of the hue span of the scheme's face ramp (paints.face.fill),
//   its hue at each lightness within the same slack of the ramp's on average (hueDeviation), and
//   its chroma above COLOR_MATCH_CHROMA_SHARE of the ramp's; FAIL otherwise, fixed by applying the
//   recorded scheme again. `metal_range` (pixels): each face that is metal (2D: its finish or
//   bevel on; 3D: metallic >= METAL_MIN_METALLIC), rendered the same way (3D: without its bands),
//   needs an L* range (p95 - p5 of its pixels) of at least METAL_MIN_L_RANGE, or the metal reads
//   as flat paint; FAIL otherwise, fixed by enabling the 2D finish or raising the 3D environment
//   intensity. Both SKIP when no layer applies, and (TitleArtChecks) without a render function.
// - The metrics are pure functions over a straight-alpha RGBA8 image (exposed for tests and for
//   R37's calibration).
// - Thread-safety: main thread (the registry); the metrics are pure.

#pragma once

#include "../TitleArtChecks.h"
#include "StyleLint.h"
#include "titleart/ColorScheme.h"

#include <string>

namespace jasperengine::editor::titlelibrary {

    // color_scheme_match: degrees the face's hue may sit outside the ramp's hue span
    constexpr double COLOR_MATCH_HUE_SLACK = 20.0;
    // ... and the share of the ramp's mean chroma the face must keep
    constexpr double COLOR_MATCH_CHROMA_SHARE = 0.5;
    // ink: alpha at least this (0..255); a white core: every channel at least this (0..1)
    constexpr int COLOR_MATCH_MIN_ALPHA = 128;
    constexpr double COLOR_MATCH_WHITE_CORE = 0.92;
    // OKLCH chroma below which a ramp sample has no hue worth judging (greys, near whites)
    constexpr double COLOR_MATCH_CHROMATIC = 0.03;
    // a ramp whose hues spread wider than this is a spectrum: any hue matches it
    constexpr double COLOR_MATCH_SPECTRUM_SPAN = 180.0;
    // metal_range: the L* p95 - p5 a metal face needs, and what makes a 3D material metal
    constexpr double METAL_MIN_L_RANGE = 9.0;
    constexpr double METAL_MIN_METALLIC = 0.5;
    // the factor the 3D fix raises the environment intensity by
    constexpr double METAL_ENVIRONMENT_RAISE = 1.5;

    struct Oklch {
        double L = 0.0, C = 0.0, h = 0.0;      // L 0..1, C, h degrees 0..360
    };
    Oklch toOklch(double r, double g, double b);                  // sRGB-encoded 0..1
    // OKLCH -> sRGB 0..1, chroma reduced (hue and lightness kept) until it fits the gamut
    titleart::TitleColor fromOklch(const Oklch& c);

    struct ColorMatchMetric {
        bool judged = false;                // false: no ink, or an achromatic ramp (nothing to compare)
        size_t pixels = 0;                  // ink pixels measured (white cores excluded)
        double faceHue = 0.0;               // mean OKLCH hue of the ink, degrees
        double faceChroma = 0.0;            // mean OKLCH chroma of the ink
        double rampHueFrom = 0.0, rampHueTo = 0.0;  // the ramp's hue span, clockwise from -> to
        bool rampSpectrum = false;          // the ramp covers every hue
        double rampChroma = 0.0;            // mean chroma of the ramp's samples (white cores excluded)
        double hueOutside = 0.0;            // degrees the mean hue sits outside the span (0 inside)
        // chroma-weighted mean of each chromatic ink pixel's hue minus the ramp's hue at the same
        // OKLab lightness, -180..180: the span alone misses a shift of a wide ramp (gold's runs
        // from brown to yellow, 37 to 103 deg: an evenly lit gold face moved +40 deg sits only
        // ~10 deg past it), while a shifted face strays from the ramp at every lightness
        double hueDeviation = 0.0;
        double chromaRatio = 1.0;           // faceChroma / rampChroma
        bool hueFails = false, chromaFails = false;
        std::string note;                   // why it was not judged
    };
    // the face ink of `face` against `schemeFaceRamp` (an S80 gradient object, or a "#rrggbb" colour)
    ColorMatchMetric measureColorSchemeMatch(const TitleCheckImage& face, const Json& schemeFaceRamp);
    // the face ramp of a scheme: paints.face.fill's gradient (its colour when solid), else the
    // face's finish ramp; null when it has neither
    Json schemeFaceRamp(const titleart::ColorScheme& scheme);

    // L* p95 - p5 of the pixels with alpha >= COLOR_MATCH_MIN_ALPHA; -1 when there are none
    double measureMetalRange(const TitleCheckImage& face);

    // a finding's check status (the per-rule choice above)
    TitleCheckStatus styleLintStatus(const StyleLintFinding& finding);

    // registers style_lint, color_scheme_match and metal_range; idempotent
    void registerLibraryChecks();

}
