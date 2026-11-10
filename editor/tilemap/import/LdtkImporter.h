// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_LDTK_IMPORTER_H
#define TILEMAP_LDTK_IMPORTER_H

#include "tilemap/import/ImportPlan.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::tilemapimport {

    // Parses an LDtk project (.ldtk) (docs/specs/S50-grid-tilemap.md R45, D11) into `out`: one
    // Grid per level, its layers and the tilesets they use. External levels (.ldtkl) are read
    // through `readFile`, relative to the project's directory. False with `problems` on malformed
    // input (kind "parse", with a JSON path); `out` is then partial and must be discarded.
    bool parseLdtk(std::string_view text, const std::string& fileName, const FileReader& readFile, ImportPlan& out, std::vector<ImportProblem>& problems);

}

#endif // TILEMAP_LDTK_IMPORTER_H
