// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R4, R5, §6.8)
// - Purpose: every asset reference a `.titleart` holds (fonts, fallback fonts, images, SVGs,
//   reflection maps, fill, mask and pattern images, Asset params of effects and generators, S85's
//   marker sounds),
//   found through the member schema and the registry, so a member or param a later spec adds
//   with an AssetRef type is visited without a change here. The editor's dependency index, its
//   relink on rename and move, Missing Assets and S81's export all walk documents with it.
// - Paths: `field` is the JSON path of the reference in the document ("layers[0].font",
//   "layers[1].style.rims[0].material.reflection.map", "layers[0].fallback_fonts[1]").
// - Thread-safety: pure functions over the document tree (the schema is read only).

#pragma once

#include "Export.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtDoc.h"

#include <functional>
#include <string>

namespace jasperengine::titleart {

    // `ref` is the AssetRef value in the tree ({path, guid} or null); a visitor may rewrite it in
    // place (relink). Return false to stop.
    using TitleRefVisitor = std::function<bool(Json& ref, const std::string& field, TitleAssetKind kind)>;

    // every AssetRef member and Asset param of `root` (a document root), null ones included
    JASPERENGINE_API void visitTitleArtRefs(Json& root, const TitleRefVisitor& visit);
    // the same over a const tree (the visitor gets a copy it cannot write back)
    JASPERENGINE_API void visitTitleArtRefs(const Json& root, const std::function<bool(const TitleAssetRef& ref, const std::string& field, TitleAssetKind kind)>& visit);

    JASPERENGINE_API const char* titleAssetKindName(TitleAssetKind kind);   // "font", "image", "vector", "environment"

}
