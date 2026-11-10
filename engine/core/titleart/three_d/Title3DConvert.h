// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R67)
// - Purpose: Pachislop's 3D title values as this program's: a Title3DProps (`style3d`) as a
//   layer's `style_3d` (§6.1) and a Material3D as a `material_3d`, for S84's look converter and
//   the parity goldens (AC21). Pachislop's model is src/model/types.ts (by SafEight, used with the
//   author's permission).
//   - names to snake_case; `metalness` -> `metallic`, `reflectivity` -> `env_multiplier`,
//     `specBoost` -> `specular_multiplier`, `emissive` -> `emission`, `emissiveIntensity` ->
//     `emission_intensity`, a band's `shape` -> `plate`, `skew {x, y}` -> `[x, y]`;
//   - presets camelCase -> snake_case (`punchyGold` -> `punchy_gold`), unknown ones `custom`;
//   - curve points as they are (smooth / corner points: S80's Curve keys); gradients as they are;
//   - lights' `x, y, z` -> `position`; tone mapping `aces` -> `aces_filmic`;
//   - `faceImage` / `edgeImage` -> `face_image` / `edge_image`; `shadow` dropped (no shadow
//     catcher here); unknown keys dropped. The result is normalized by the caller (TitleArtIO).
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

namespace jasperengine::titleart {

    JASPERENGINE_API Json title3DFromPachislop(const Json& title3dProps);
    JASPERENGINE_API Json material3DFromPachislop(const Json& material);

}
