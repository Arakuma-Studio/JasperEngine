// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R35, R45, §6.14.1)
// - Purpose: the style lint: the lettering mistakes that read as amateur (neighbouring rings of
//   one lightness, too many hues, rims too thin or too thick, a hard black drop shadow, a
//   layer-wide spectrum on several lines, switches that are on and change nothing), each finding
//   with what was measured, the threshold, the element and, where the repair is mechanical,
//   edit_title ops that remove it. LibraryChecks registers it as check_title's `style_lint`; the
//   Title Designer shows the same findings (S82).
// - Colours: each element (the face, a rim, a 3D band) is judged on 9 samples of its fill
//   gradient (one for a solid fill), pushed through an enabled 2D gradient-map finish by its
//   amount; 3D materials sample their gradient, else their colour. CIE L*a*b* (D65).
// - Rings: the face, then every enabled rim without an offset (2D; an offset rim is a dropped
//   base, shadow weight) or every enabled band from the first one not in the face's own material
//   (3D), inner to outer. Only text layers have rings.
// - Elements are named by their field path shorthands: "face", "rim#<i>", "band#<i>" (0-based).
// - Allowlists (a look's lint_allow) are the gate's: lintLayer and lintDocument report everything.
// - Thresholds are the constants below, the starting points calibration.json confirms (R37).
// - Thread-safety: pure functions over the document.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    using titleart::Json;

    // neighbouring rings closer than this in L* merge into one band
    constexpr double LINT_MIN_DELTA_L = 12.0;
    // both rings darker than this read as one thick keyline (fine)
    constexpr double LINT_DARK_L = 25.0;
    // rings whose samples span more L* than this are banded metals: they separate by their banding
    constexpr double LINT_BANDED_L = 30.0;
    // C* above which a sample is a hue rather than a neutral
    constexpr double LINT_CHROMATIC = 25.0;
    // hues within this many degrees (a*b* angle) are one family
    constexpr double LINT_HUE_FAMILY = 30.0;
    // chromatic samples further apart than this make a spectrum
    constexpr double LINT_SPECTRUM_SPREAD = 120.0;
    // this many hue families across face and rings is too many
    constexpr int LINT_MAX_HUE_FAMILIES = 3;
    // 2D rim total (% of size) below which the rims vanish on busy backgrounds; the fix's total
    constexpr double LINT_MIN_RIMS = 5.0;
    constexpr double LINT_THIN_FIX_TOTAL = 8.0;
    // rim total above the stem width of heavy display faces (% of size); the fix's total
    constexpr double LINT_STEM = 16.0;
    constexpr double LINT_THICK_FIX_TOTAL = 12.0;
    // a drop shadow is hard above this opacity, below this size (% of the layer size) and luma
    constexpr double LINT_HARD_SHADOW_OPACITY = 0.8;
    constexpr double LINT_HARD_SHADOW_SIZE = 4.0;
    constexpr double LINT_HARD_SHADOW_LUMA = 0.15;
    // the softened shadow of the fix
    constexpr double LINT_SOFT_SHADOW_OPACITY = 0.6;
    constexpr double LINT_SOFT_SHADOW_SIZE = 7.0;
    // the separator ring the contrast fix inserts (% of size; 3D: band grow)
    constexpr double LINT_SEPARATOR_WIDTH = 0.6;
    constexpr double LINT_SEPARATOR_GROW_3D = 0.4;
    // the rim the hollow-letters fix adds (% of size)
    constexpr double LINT_KNOCKOUT_RIM = 1.0;
    // erode's starting values (the schema's defaults)
    constexpr double LINT_ERODE_AMOUNT = 0.5;
    constexpr double LINT_ERODE_EDGE = 6.0;

    struct StyleLintFinding {
        std::string rule;           // contrast, hue_count, rims_thin, rims_thick, hard_shadow, spectrum_span, noop_knockout, noop_erode
        std::string element;        // "face", "rim#1", "band#0", "fx:drop_shadow" ("" for the layer)
        double measured = 0.0;
        double threshold = 0.0;
        std::string message;
        Json fix;                   // {ops: [...]} for edit_title, or null (advice only)
        std::string layer;          // the layer's id
    };

    // the rule ids, in the order a layer reports them
    const std::vector<std::string>& styleLintRules();
    // the practice a rule's findings break (our words), for the Designer's tooltip
    const char* styleLintRuleText(const std::string& rule);

    // the findings of one layer (an object of `doc`'s tree); none for layers without a style
    std::vector<StyleLintFinding> lintLayer(const titleart::TitleArtDoc& doc, const Json& layer);
    // every styled layer of the document at every depth, guide layers (and what is inside a guide
    // group) skipped
    std::vector<StyleLintFinding> lintDocument(const titleart::TitleArtDoc& doc);

}
