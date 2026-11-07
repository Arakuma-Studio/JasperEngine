// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R36, §6.8)
// - Purpose: a title's 3D layers as a GLB (export_title {format: "glb"}), written with tinygltf
//   directly (GlbBuilder is FBX's). One node per 3D layer (its layer root as the node matrix,
//   translations in metres: `units_per_px`, 0.01 by default; glTF +Y up and +Z toward the viewer
//   match layer space) and one child node per rigged glyph (its rig matrix); one primitive per
//   material slot (bands optional).
// - Materials: metallic-roughness with KHR_materials_clearcoat, _iridescence, _transmission,
//   _volume (thickness 0.3 x size x units, attenuation colour = colour), _ior, _dispersion and
//   _emissive_strength (intensity above 1); opacity below 1 writes alphaMode BLEND. A ramp is exact:
//   TEXCOORD_0 = (the bent ramp t, 0.5) with the 256 x 1 sRGB ramp as baseColorTexture; TEXCOORD_1
//   is the wrap UV. The environment and specular multipliers have no glTF form: they go into
//   extras.jasperengine_title with the source hash. Patterns, base textures, surfaces and flakes
//   are dropped with a warning each.
// - Thread-safety: main thread (the mesher's caches lock; the content provider is the caller's).

#pragma once

#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
}

namespace jasperengine::editor {

    struct TitleGlbOptions {
        double unitsPerPx = 0.01;
        bool includeBands = true;
    };

    struct TitleGlbResult {
        bool ok = false;
        std::vector<uint8_t> bytes;
        std::vector<std::string> warnings;
        std::string error;
        uint32_t triangles = 0;
        uint32_t layers = 0;
    };

    TitleGlbResult writeTitleGlb(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets, const TitleGlbOptions& options);

}
