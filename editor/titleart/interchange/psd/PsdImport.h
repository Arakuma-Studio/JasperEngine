// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R32, R33, R44, R57, §6.2, §6.9)
// - Purpose: Photoshop into Title Art.
//   - `create.from` of a layered PSD / PSB (kind "psd", sniffed by '8BPS'): RGB or Grayscale, 8
//     or 16 bits (other modes refused with psd_mode_unsupported; their styles still import);
//     pixel layers and smart objects become Image layers (pixels trimmed to
//     `<stem>_assets/<layer>.png`, placed at their bounds), text layers Text layers (the first
//     style run of EngineData: text, font matched by PostScript name to the project's fonts then
//     the system's by family, size, fill colour, tracking, leading, alignment), groups groups
//     (pass-through drawn Normal, noted), the 13 adjustment types Adjustment layers, solid and
//     gradient fill layers `fill` generators; blend modes, opacity, fill opacity, clipping,
//     visibility and user masks (an unlinked image mask covering the canvas, vector masks
//     rasterised with a note) carry over, and every layer's styles map as §6.9. The canvas is the
//     PSD's size, `dpi` from 0x03ED, `global_light` from 0x040D / 0x0419; an embedded profile
//     other than sRGB is ignored with `color_profile_ignored`.
//   - `create.from` of an `.asl` (kind "ps_styles") fails with ps_styles_not_a_document, whose fix
//     is the import_ps_styles op.
//   - ops `apply_ps_style {layer, file, style?, reference_px?}` (the style replaces the layer's
//     style and effects; px params scaled by the layer's size / the reference size) and
//     `import_ps_styles {file, styles?, scope?, reference_px?, overwrite?}` (one `.titlelook` per
//     style in the project's EditorSettings/TitleLibrary/Looks/, S84's look format).
// - Every report line is unique by content; the document's `import_notes` keep them.
// - Thread-safety: main thread (the registration); the import functions are pure apart from
//   reading the files they are given.

#pragma once

#include "PsStyleConvert.h"
#include "titleart/TitleArtOps.h"

#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    // R33: the document of a layered PSD
    bool importPsdDocument(const TitleCreateRequest& request, TitleCreateResult& out, std::string& code, std::string& error);

    // "Gold Bevel 2" -> "gold-bevel-2" ([a-z0-9-]{1,48}, "style" when nothing is left)
    std::string psLookSlug(const std::string& name);

    // the importers and ops above (from registerTitleInterchange)
    void registerPsdInterchange();

}
