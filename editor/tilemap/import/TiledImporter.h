// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TILED_IMPORTER_H
#define TILEMAP_TILED_IMPORTER_H

#include "tilemap/import/ImportPlan.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::tilemapimport {

    // Parses a Tiled map (.tmj JSON or .tmx XML; the format is told by its first character)
    // (docs/specs/S50-grid-tilemap.md R44, D11) into `out`: one Grid, its layers and tilesets.
    // External tilesets (.tsj, .tsx) are read through `readFile`, relative to the map's directory.
    // False with `problems` on malformed input (kind "parse", with a line or JSON path); `out` is
    // then partial and must be discarded.
    bool parseTiled(std::string_view text, const std::string& fileName, const FileReader& readFile, ImportPlan& out, std::vector<ImportProblem>& problems);

}

#endif // TILEMAP_TILED_IMPORTER_H
