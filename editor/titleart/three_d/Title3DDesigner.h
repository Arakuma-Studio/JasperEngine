// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R50-R58; S82 §6.8, R35)
// - Purpose: S86's part of the Title Designer, registered through S82's hooks: the Dimension
//   section's subsections (Geometry, Bands, Lighting, Glint, Ambient Occlusion, Camera, Realtime
//   Preview, Status), the 3D Appearance rows (Face, Bevel, Side, Band 1..N) and the part pages'
//   material and band rows, the Document page's 3D Scene, the material, environment and bevel
//   profile pickers, the "Inherited" rows of the three lighting levels (sparse overrides: S82's
//   source dots), the Realtime tiers in Preview As and the Gizmo3DTarget of a 3D layer or of its
//   selected letters.
// - Every edit is an S80 / S86 op batch through TitleDesignerState (one named step: "Apply
//   Material Gold", "Move Band", "Rotate 3D Layer", "Tilt Letters", "Change Environment"); hovering
//   a picker tile previews its op (no step), leaving it or Esc restores.
// - Picker tiles: materials render a bevelled "Aa" through the window's preview queue (priority
//   Thumbnails); environments are drawn on the CPU from the environment itself (a chrome ball, a
//   gold disc and its panorama strip), only while the picker is open.
// - Thread-safety: main thread.

#pragma once

#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitleGizmo3D.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // once, with the Title Designer's other extensions (idempotent)
    void registerTitle3DDesigner();

    // S86 R56: the gizmo target of 3D layer `layerId` (letters null) or of those letters; false
    // for a layer that is not Dimension 3D, locked, or missing
    bool title3DGizmoTarget(const titleart::TitleArtDoc& doc, const std::string& layerId, const titledesigner::Gizmo3DLetters* letters,
                            titledesigner::Gizmo3DTarget& out);

    // the environment picker's tile of a preset (RGBA8, rows top first, `width` x `height`): a
    // chrome ball and a gold disc lit by it over its panorama strip
    std::vector<uint8_t> title3DEnvironmentTile(const std::string& preset, int width, int height);

}
