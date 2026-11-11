// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/recipe.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// The cleanup recipe as JSON (docs/specs/S46-pixel-art-cleanup.md R28): every setting that turns
// a picture into fixed pixel art, hand edits included, so a person's tuning re-runs for an agent
// and an agent's run opens in the window. ritmo's keys in snake_case, plus S46's:
//
//   format "jasperengine.pixel-cleanup-recipe", version 1, pipeline_version, engine_version
//   grid            "auto" | {"kind": "cells", "cell_width" 1-128, "cell_height"?, "offset_x", "offset_y"}
//                   | {"kind": "size", "width" 1-4096, "height"?}
//   follow          "off" | "straight" | "bend"          outlines  "auto" | "on" | "off"
//   moved           [["x:3", 41.5], ...] (lines dragged, source pixels)
//   sample          {"inset" 0-0.45, "alpha_threshold" 0-255}
//   palette         absent or "default" (the project's Default Palette, else auto: the session
//                   resolves it) | "none" (own colours) | "auto" | {"auto": {"count" 2-256,
//                   "method" "dominant"|"balanced"|"distinct"}} | a preset name ("sweetie-16",
//                   "Sweetie 16") | a ".palette" asset path or a 32-hex GUID (resolved by the
//                   session) | ["#rrggbb", ...] | {"name", "colors", "locked", "asset": {"guid", "path"}}
//   metric          "oklab" | "ciede2000"                dither  "none" | "ordered"
//   dither_strength 0-1                                  cleanup {"size" 0-16, "strength" 0-1}
//   clean_keep      {"size": "WxH", "cells": [...]}       target_width null | 1-4096
//   reduce_mode     "outlines" | "common"                remove_background, crop: booleans
//   background      {"tolerance" 0-0.3, "enclosed", "specks", "shadow"}
//   lost_threshold  0-1                                  alpha_cutoff 0-255; seed 0-4294967295
//   edits           {"size": "WxH", "strokes": [[{"tool" "erase"|"restore"|"paint", "x", "y",
//                   "area", "color"?}, ...], ...]}
//   sheet           "auto" | "off" | {"frames": [{"box": {"x", "y", "width", "height"}, "include",
//                   "nudge": {"x", "y"}}], "animations": [{"name", "frames", "include", "fps" (0-60],
//                   "loop" "forward"|"pingpong"}], "anchor" "ground"|"feet"|"center"|"box", "padding"}
//                   (a sheet without "animations" takes its "fps" and "loop" for one animation of
//                   every frame, as ritmo's recipes from before animations do)
//   source          {"path", "sha256", "frame"?, "layer"?}     result_sha256: the output's pixels
//   output_format   null | "png" | "aseprite" (R28's `format`: `format` already names the recipe
//                   format, as every jasperengine recipe's does)     forced: boolean (R30)
//
// The decoder accepts partial recipes (missing keys take their defaults) and collects every
// error at once as {kind "bad_recipe", field, message, fix naming the valid values or range};
// unknown keys at any level fail naming the valid ones; a newer version fails "recipe_version".
// The encoder writes canonical JSON: sorted keys, doubles rounded to 6 decimals, no time.

#pragma once

#include "pixelart/PixelProblem.h"
#include "pixelart/cleanup/CleanupTypes.h"

#include "json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // true when the recipe decoded with no problem; `out` holds what could be read either way
    bool decodeRecipe(const nlohmann::json& json, Recipe& out, std::vector<PixelProblem>& problems);
    // the same from text (a problem "bad_recipe" when it is not JSON)
    bool decodeRecipeText(std::string_view text, Recipe& out, std::vector<PixelProblem>& problems);

    // canonical JSON (std::map keeps the keys sorted); decodeRecipe(encodeRecipe(r)) == r
    nlohmann::json encodeRecipe(const Recipe& recipe);
    // encodeRecipe's compact text: what the PNG chunk, the .meta and the JSON atlas carry
    std::string recipeText(const Recipe& recipe);

    // A recipe palette from a built-in palette's id or name, matched as ritmo's presetPalette does
    // (case, spaces, punctuation and "(16)" ignored: "Sweetie 16" is "sweetie-16")
    std::optional<RecipePalette> presetPalette(std::string_view name);

    // the top-level keys, for an error's fix and the guide
    const std::vector<std::string>& recipeKeys();

    // "oklab", ... (the names the recipe uses)
    const char* gridKindName(GridKind kind);
    const char* followName(Follow follow);
    const char* outlinesName(Outlines outlines);
    const char* reduceModeName(ReduceMode mode);
    const char* anchorName(Anchor anchor);
    const char* loopName(Loop loop);
    const char* outputFormatName(OutputFormat format);
    const char* proposeMethodName(ProposeMethod method);

}
