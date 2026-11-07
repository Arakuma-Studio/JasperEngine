// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R1, R1a, R4, R6, §6.2)
// - Purpose: what the 3D mesher takes from a layer: its final outlines per glyph (text) or per
//   fill group (shapes, SVGs, traced pictures) in layer space, after S80's warp, distort and
//   weight, with each glyph's CharAdjust key, em-cell centre and span boxes, and whether it is
//   rigged (a rigged glyph's outline comes without its 2D adjustments: they become its matrix,
//   R4); plus the characters that get a material block of their own.
// - collectTitleSource reads them through S80's TitleContentProvider (TitleContent.h), so the
//   renderer, the editor's GLB writer and the tests share one path.
// - Thread-safety: main thread (the provider's rule); the result is a value.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/geometry/Path.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleMesh.h"

#include <map>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
    class TitleContentProvider;
}

namespace jasperengine::titleart::three_d {

    // one glyph (text) or one fill group (shape, SVG, traced picture) of a layer, layer space
    struct TitleGlyph3D {
        geometry::Path path;            // after warp, distort, weight (a rigged glyph: without its 2D adjustments)
        uint32_t charIndex = 0;         // the CharAdjust key (text)
        TitleVec2 pivot;                // the em-cell centre (text)
        TitleRect lineBox, charBox;     // the span boxes (text)
        bool rigged = false;
    };

    struct TitleSource3D {
        bool text = false;
        std::vector<TitleGlyph3D> glyphs;
        double size = 200.0;            // the layer's source size (the % unit x 100)
        double shear = 0.0;             // text shear (deg), added to a box band's slant
        bool orb = false;               // a Shape layer's `orb` in mode orb
    };

    // a character's material block: its first slot and, per band, whether it overrides that band
    struct CharBlock3D {
        uint16_t slot = 0;
        std::vector<bool> bands;
    };

    // the source of a Text, Shape, Vector or Image layer (`layer` normalized); `problems` gets the
    // provider's (glyph_missing ...). Rigged characters are laid out without their 2D adjustments.
    JASPERENGINE_API TitleSource3D collectTitleSource(const Json& layer, const TitleArtDoc& doc, TitleContentProvider& content,
                                                      const AssetResolver* assets, std::vector<TitleProblem>* problems);
    // the characters with a material block of their own, numbered after the layer's block
    // (at most TITLE_3D_MAX_SLOTS slots in all; later ones keep the layer's block)
    JASPERENGINE_API std::map<uint32_t, CharBlock3D> collectCharBlocks(const Json& layer, size_t bandCount);
    // whether layer `layer`'s character `charIndex` is rigged (CharAdjust, R4)
    JASPERENGINE_API bool layerCharRigged(const Json& layer, uint32_t charIndex);

    // the materials a mesh's slots show: the face (the bevel and side when they have none), the
    // bands' and their sides', and character blocks' overrides (face_3d, side_3d, bands_3d);
    // deduplicated by source, at most 16 (later ones show the first), with each slot's index
    struct TitleSlotMaterials {
        std::vector<TitleMaterial3D> materials;
        std::vector<uint8_t> slotMaterial;      // by mesh slot
    };
    JASPERENGINE_API TitleSlotMaterials resolveSlotMaterials(const Json& layer, const Title3D& style, const TitleMesh& mesh);

}
