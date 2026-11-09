// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R28-R31, §6.8)
// - Purpose: a migrated (v11) Pachislop project as a `.titleart`: the structure (the flat layer
//   list with parentId as the tree, children bottom first; ids; groups; the comp as canvas,
//   background, global light and dpi), every layer kind (title2d / title3d by source as Text,
//   Shape, Vector or Image with `dimension`, background as S80's `fill` generator, group,
//   adjustment, generator), styles and masks through PachislopMembers (S84's member rules),
//   effects and generators through S83's PachislopFxMap / PachislopGeneratorMap.
// - Ids: every new id is derived from the Pachislop id (FNV-1a), so an import is the same
//   document every time; `follow`, layer masks, materialLinks and part keys follow their layers.
// - Nothing fails because one field cannot be mapped (R31): each unmapped, approximated or
//   dropped item is a note with the new layer path; a type the maps lack is kept as an S80
//   unknown effect or generator (`kept_unknown`).
// - 3D: with S86's module, `title3d` styles go through title3DFromPachislop; without it they import
//   as `dimension: 3d` with the face colour of the 3D face and the style kept in
//   `pachislop_style3d` (R30) for reimport_pachislop_3d.
// - The result is normalized with TitleArtOps' commit invariants (S80 validates it as it does an
//   op batch); clamps become `approximated` notes.
// - Thread-safety: pure apart from the callbacks.

#pragma once

#include "PachislopJson.h"
#include "titleart/interchange/TitleImport.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::pachislop {

    struct MapperAssets {
        // a Pachislop asset id -> AssetRef (null: unavailable, with its own note)
        std::function<Json(const std::string& assetId, const std::string& layerPath, const std::string& field)> asset;
        // a text layer's fontId -> AssetRef (null: the built-in face)
        std::function<Json(const std::string& fontId, const std::string& layerPath)> font;
        // a 2D reflection map name -> its environment's AssetRef (null: unavailable)
        std::function<Json(const std::string& mapName, const std::string& layerPath, const std::string& field)> environment;
    };

    struct MapResult {
        titleart::TitleArtDoc doc;
        std::vector<TitleImportNote> notes;
    };

    // `project` a v11 project (migrateProject's output)
    MapResult mapProject(const Json& project, const MapperAssets& assets);

    // the 3D module (S86) is in this build
    bool mapperHas3D();
    // R30's re-map of a layer's kept `pachislop_style3d` with S86's converter; false when the
    // layer has none or the 3D module is not in this build
    bool remapStyle3D(Json& layer);

}
