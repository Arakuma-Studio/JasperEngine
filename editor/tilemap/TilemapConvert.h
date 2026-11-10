// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// "Convert to Grid Tilemap" (docs/specs/S50-grid-tilemap.md R50, R72, AC9): a legacy Free Tilemap
// whose tiles all have one size and sit on that size's grid becomes a Grid and a Tilemap Layer on
// the same GameObject (same id, name, transform, children), with a texture-source .tileset holding
// one tile per legacy rect (rect names and indices kept as tile names and ids). The conversion is
// one undoable step: one undo restores the legacy component and the file set exactly.
//
// A rect's texture comes from its submesh's material, as the legacy mesh build reads it; each
// distinct texture is one source of the tileset. The Inspector shows check()'s result before
// converting (R72); edit_tilemap's `convert` op returns convert()'s.

#pragma once

#include "component/TilemapComponent.h"
#include "ecs/Entity.h"
#include "pixelart/PixelProblem.h"
#include "tilemap/Tileset.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::tilemap {

    using pixelart::PixelProblem;

    struct ConvertOffender {
        int tile = 0;                    // the index in TilemapComponent::tiles
        std::string name;
        float x = 0.0f, y = 0.0f;
        float width = 0.0f, height = 0.0f;
        std::string reason;
    };

    // what convert needs, once check found the map convertible
    struct ConvertSource {
        std::string texture;             // asset path
        int margin = 0;
        int spacing = 0;
    };

    struct ConvertRect {
        bool valid = false;              // false: no tile for this rect (it had no pixels, or could
                                         // not join the grid while no tile uses it; a warning says so)
        int source = 0;                  // index in ConvertCheck::sources
        int atX = 0;
        int atY = 0;
    };

    struct ConvertCell {
        CellCoord cell;
        int32_t tile = 0;
    };

    struct ConvertCheck {
        bool ok = false;
        // not_grid_aligned (or tileset_missing without a texture): what a refusal says
        PixelProblem problem;
        std::vector<ConvertOffender> offenders;   // up to 10
        size_t offenderCount = 0;                 // how many there are in all
        int tileWidth = 0;                        // pixels
        int tileHeight = 0;
        std::vector<std::string> warnings;        // flipY, textureScaleFactor...
        std::vector<ConvertSource> sources;
        std::vector<ConvertRect> rects;           // by legacy rect index
        std::vector<ConvertCell> cells;
    };

    // Reads only. `submeshTextures`: the base colour texture of each submesh of the component's
    // Mesh (tilemapmesh::submeshTextures), where its rects' textures are
    ConvertCheck check(const TilemapComponent& tilemap, const std::vector<std::string>& submeshTextures);

    struct ConvertResult {
        bool ok = false;
        PixelProblem problem;                    // when refused or a step failed; nothing changed
        ConvertCheck check;
        std::string tilesetPath;                 // asset path of the written .tileset
        std::vector<std::string> warnings;       // the check's and the conversion's
        std::vector<std::string> scriptsUsingTilemap;   // scripts on the GameObject that use `Tilemap`
    };

    // Runs the conversion as one undo step on the scene's history ("Convert to Grid Tilemap").
    ConvertResult convert(Project* project, uint32_t sceneId, Entity entity);

}
