// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_FROM_ASEPRITE_H
#define TILEMAP_FROM_ASEPRITE_H

#include "tilemap/import/ImportPlan.h"

#include "pixelart/aseprite/AsepriteImport.h"

#include <string>
#include <vector>

namespace jasperengine::editor::tilemapimport {

    // "Create Tilemap from Aseprite" (docs/specs/S50-grid-tilemap.md R48): turns the import's
    // tilemap cels of `frame` into a plan like the other importers': an Aseprite-source tileset
    // per Aseprite tileset (our tile i is the Aseprite tile i + 1, its user data the tile's
    // properties), one Grid per tile size and a Tilemap Layer per tilemap layer.
    // `canvasHeight` (pixels) places the cels: y up, row 0 at the canvas' bottom. False with
    // `problems` when the file has no tilemap layers.
    bool tilemapFromAseprite(const pixelart::aseprite::AsepriteImport& import, const std::string& asepritePath, int canvasHeight, int frame, ImportPlan& out, std::vector<ImportProblem>& problems);

}

#endif // TILEMAP_FROM_ASEPRITE_H
