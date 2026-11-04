// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The palette edits (docs/specs/S43-palettes.md R10, R11, R18, R32, R39, D8): one vocabulary of
// operations for edit_palette's ops[] and the Palette window, applied to a palette in memory as
// one batch. Each op is a JSON object {op, ...}; the fields per op are listed in
// docs/mcp/guide/palettes.md. Sorting renumbers: names, locked indices, the transparent index,
// ramps and variants follow their colours.

#include "pixelart/PixelProblem.h"
#include "pixelart/Palette.h"

#include "json.hpp"

#include <string>
#include <vector>

namespace jasperengine::editor::pixelart {

    enum class PaletteSort : uint8_t { Hue, Saturation, Luma, Value, Reverse };   // Luma is OKLab L

    // "hue", "saturation", "luma", "value", "reverse"
    const char* paletteSortName(PaletteSort sort);
    bool parsePaletteSort(const std::string& name, PaletteSort& out);
    // the colours' new order: entry i is the old index of new colour i (stable)
    std::vector<int> paletteSortOrder(const jasperengine::pixelart::Palette& palette, PaletteSort sort);

    struct PaletteEditResult {
        bool ok = true;
        std::vector<int> changed;            // the indices whose colour is new or moved
        std::vector<PixelProblem> errors;    // ok false: nothing was applied
        std::vector<std::string> warnings;
    };

    // Applies every op in order; on any error (an op's field, or the R2 checks of the result) the
    // palette is left as it was. A read-only palette (a newer format version) refuses edits.
    PaletteEditResult applyPaletteOps(jasperengine::pixelart::Palette& palette, const nlohmann::json& ops);

    // Generate Ramp's Add to Palette (R10): the colours appended, an exact duplicate reusing its
    // index, and a ramp of them; replace names a ramp whose indices are rewritten in place instead
    PaletteEditResult addGeneratedRamp(jasperengine::pixelart::Palette& palette, const std::vector<jasperengine::pixelart::Rgba8>& colors,
                                       const std::string& rampName, const std::string& replace = std::string());

}
