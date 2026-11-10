// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R8, R12, R39, §6.4, §6.5.2)
// - Purpose: recolouring a document by role with a Color Scheme (ColorScheme.h), the port of
//   Pachislop's colourways.ts. The one implementation the editor's op, the previews, MCP and
//   players (S81's setColorScheme) call. Pure CPU and deterministic: no GPU, platform or editor
//   dependency; it reads the schema and the effect registry.
// - Targets: an empty `layerId` recolours every layer of the document; a group recolours itself
//   (its effects) and every layer inside, recursively; any other layer recolours itself, the
//   generator layers its look created (`look.owner` = the layer) and the effects of its auto group
//   (a group with that owner holding it). Guide layers (and what is inside a guide group) and
//   Adjustment layers are skipped; a face whose fill is `source` (Image layers, SVG artwork) keeps
//   its pixels (its rims are recoloured). A target id that names no layer changes nothing.
// - What changes: colours by role (§6.5.2); structure stays (ring and band counts, widths, joins,
//   offsets, bevels, textures, gloss, depths, effects, non-colour generator params, alpha
//   profiles). Every recoloured layer records `look.color_scheme` = the scheme's id. When the
//   document exposes a `color_scheme` parameter whose `options` hold the id (or are empty), the
//   scheme's data is refreshed in `color_schemes`.
// - Idempotent: applying the same scheme twice changes nothing the second time. A ring's role
//   comes from its colour (luma 0.18); on a layer that already records this scheme, rings that
//   the scheme's paints already produced keep the roles that produced them.
// - Changed paths: one entry per changed leaf value, "<layer id>:<field path>" in the layer
//   ("a1b2c3d4:style.rims[1].material.fill.color", "a1b2c3d4:chars.3.face.bevel.shadow_color",
//   "a1b2c3d4:part_effects.rim:0f0f0f0f[0].params.color", "a1b2c3d4:generator.params.gradient.stops",
//   "a1b2c3d4:look.color_scheme"); the layer id is the text before the first ':'. A list whose
//   length changed (gradient stops) is one entry for the list. Document members have no prefix
//   ("color_schemes.gold"). Each path once, in the order recoloured (document order for the
//   document or a group; a layer, then what its look owns).
// - Thread-safety: pure (the document is the caller's).

#pragma once

#include "Export.h"
#include "titleart/ColorScheme.h"
#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    struct SchemeTarget {
        std::string layerId;    // empty: the whole document
    };

    JASPERENGINE_API std::vector<std::string> applyColorScheme(TitleArtDoc& doc, const ColorScheme& s, const SchemeTarget& t);

    // The schemes of `schemes` that the document's exposed `color_scheme` parameters offer (every
    // one of them for a parameter with empty `options`) written into `color_schemes`; returns the
    // changed paths ("color_schemes.<id>")
    JASPERENGINE_API std::vector<std::string> embedColorSchemes(TitleArtDoc& doc, const std::vector<ColorScheme>& schemes);

    // The scheme `id` embedded in the document's `color_schemes` (what a player switches to);
    // false when there is none or it does not read (warnings appended)
    JASPERENGINE_API bool embeddedColorScheme(const TitleArtDoc& doc, const std::string& id, ColorScheme& out,
                                              std::vector<std::string>* warnings = nullptr);

}
