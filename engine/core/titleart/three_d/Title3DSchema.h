// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R1, R3, R4, R64, §6.1)
// - Purpose: S86's members of the `.titleart` schema: `style_3d` on Text, Shape, Vector and Image
//   layers, the document's `scene_3d`, Transform's `z` / `rotation_x` / `rotation_y`, CharAdjust's
//   `dz`, `tilt_x`, `tilt_y`, `face_3d`, `side_3d`, `bands_3d` and `rig`, and their kinds
//   (material_3d, band_3d, bevel_3d, ...). Registered once from TitleArtSchema's builder
//   (registerProgramMembers) when the 3D module is compiled in.
// - `style_3d` and `scene_3d` are nullable with a null default: a document without 3D writes
//   `null` for them, a 3D layer whose `style_3d` is null renders R2's derivation of its 2D style
//   (deriveStyle3D), and a null `scene_3d` is every default. The ops create them on first write.
// - Lighting overrides (a layer's `lighting`, a material's `environment`) are sparse kinds: only
//   the values set are written, an absent one inherits (part <- layer <- document).
// - Unknown material presets, bevel profiles and environment ids load as custom / round / studio
//   with one warning naming the id (R64).
// - Thread-safety: registration runs once inside the schema's first use.

#pragma once

#include "Export.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleArtSchema;

    // registers every S86 member and kind; called by registerProgramMembers
    JASPERENGINE_API void registerTitle3DMembers(TitleArtSchema& schema);

    // the enum values of the 3D members (MCP errors list them, the pickers show them)
    JASPERENGINE_API const std::vector<std::string>& title3DMaterialPresetNames();     // custom + the 22 presets
    JASPERENGINE_API const std::vector<std::string>& title3DEnvironmentNames();        // procedural, photo, S84's, custom, matcap
    JASPERENGINE_API const std::vector<std::string>& title3DToneMappingNames();        // none, aces_filmic, agx, neutral, punch

}
