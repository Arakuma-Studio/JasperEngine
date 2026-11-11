// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R2, R19, §6.2, §6.9)
// - Purpose: one of Pachislop's eleven colourways (src/presets/colourways.ts COLOURWAY_DEFS, by
//   SafEight, used with the author's permission) as a Color Scheme file (.titlecolors), from the
//   committed dump's evaluated `colourways` section (tools/titleart/pachislop-port/dump/): paints
//   with their fill / m3d thunks evaluated, LIGHT, COLOURWAY_INFO.
// - Mapping: Pachislop's names in ours (camelCase -> snake_case; fills `shapeBurst` ->
//   `shape_burst`; 3D materials through Title3DConvert's material3DFromPachislop: metalness ->
//   metallic, reflectivity -> env_multiplier ...); faceGlow -> face_glow, glowGradient ->
//   glow_gradient, bevel3d -> paints.bevel_rim_3d, side3d -> paints.side_3d, LIGHT[id] ->
//   light_ramp, patternGround -> pattern_ground, COLOURWAY_INFO[id].swatch -> swatch; `rainbow`
//   (Pachislop's `colourway === 'rainbow'` rule) gets `spectrum: true`. Names: en the def's
//   `name`, ja its `label` (白 青 緑 ...); descriptions, tags, synonyms and family hints are ours
//   (§6.6, §6.9). Provenance {source: pachislop, commit, file, line}.
// - Result: the canonical sparse JSON (writeColorScheme). `notes` names every Pachislop member
//   not carried (the UI-only `swatchPattern` CSS, unknown keys) and a colourway the dump lacks
//   (then the result is null).
// - Thread-safety: pure.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // the eleven ids in Pachislop's order
    const std::vector<std::string>& pachislopColourwayIds();

    titleart::Json colorSchemeFromPachislop(const std::string& colourwayId, const titleart::Json& dumpColourways, std::vector<std::string>& notes);

}
