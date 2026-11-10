// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// What turns an import plan into files and GameObjects (docs/specs/S50-grid-tilemap.md R46, R47,
// D11). Main thread only. Everything apply makes is one undo step: the .tileset files, the Grid
// GameObjects with a child per layer, or (with a prefab target) a prefab file instead of the
// GameObjects. A linked Grid keeps its source map (GridComponent::importSource) and re-imports in
// place when it changes, matching layers by GameObject name so their GameObjects, extra components
// and scripts stay.

#ifndef TILEMAP_IMPORT_APPLY_H
#define TILEMAP_IMPORT_APPLY_H

#include "tilemap/import/ImportPlan.h"

#include "ecs/Entity.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::tilemapimport {

    // Reads a map file of the project: .tmj or .tmx through parseTiled, .ldtk through parseLdtk.
    // External files are read beneath the project's assets (a map cannot reach outside it).
    // `filesRead`, when given, receives the map and its external files (absolute paths).
    bool loadPlan(Project* project, const std::filesystem::path& mapFile, ImportPlan& out, std::vector<ImportProblem>& problems,
                  std::vector<std::filesystem::path>* filesRead = nullptr);

    struct ImportTarget {
        Project* project = nullptr;
        uint32_t sceneId = 0;                   // the scene the GameObjects go in
        Entity parent = NULL_ENTITY;            // their parent; NULL_ENTITY: the scene's root
        std::filesystem::path mapFile;          // absolute: the source, beneath the assets
        std::filesystem::path tilesetsDir;      // absolute; empty: next to the map
        std::filesystem::path prefab;           // absolute; non-empty: write this prefab instead of leaving GameObjects
        std::string grid;                       // one ImportGrid by name (LDtk's level); empty: every grid
        bool linked = false;                    // the Grids follow the source file (R47)
    };

    // R46: what apply would make, reading only the disk (nothing written, no undo step)
    struct ImportReport {
        struct Grid {
            std::string name;
            std::vector<std::string> layers;
        };
        struct Tileset {
            std::string name;
            std::string file;                   // assets-relative: where apply would write it
        };
        ImportCounts counts;
        std::vector<Grid> grids;
        std::vector<Tileset> tilesets;
        std::vector<ImportSkipped> skipped;
        std::vector<std::string> warnings;
    };

    ImportReport previewReport(const ImportPlan& plan, const ImportTarget& target);

    struct ImportResult {
        bool ok = false;
        ImportProblem problem;                  // when !ok; kind "import_failed", "grid_not_found", or a parse problem of the source
        std::vector<Entity> grids;              // created (empty after a prefab import)
        std::vector<Entity> layers;
        std::vector<Entity> removedLayers;      // reimportLinked: layers no longer in the file
        std::vector<std::string> tilesetFiles;  // assets-relative
        std::string prefab;                     // project-relative, when one was written
        std::vector<std::string> warnings;
        // the textures the tilesets newly name, switched to the Sprite import in the same step
        // (tilemap::sourceImageImportCommands)
        std::vector<std::string> imageNotes;
    };

    // Makes the plan real, as one undo step; on any failure the step is rolled back
    ImportResult apply(const ImportPlan& plan, const ImportTarget& target);

    // R47: reads the Grid's source again and updates its layers in place, as one undo step (none
    // when nothing changed). Layers match by GameObject name; a layer no longer in the file is
    // deleted. A map with several grids (LDtk levels) finds its own by the GameObject's name. On a
    // parse failure nothing changes.
    ImportResult reimportLinked(Project* project, uint32_t sceneId, Entity gridEntity);

    // S05's hook: the linked Grids of the open scenes whose map or one of its external files is
    // among `changed` (absolute) re-import. A failure is logged and keeps the previous version.
    void linkedSourcesChanged(Project& project, const std::vector<std::filesystem::path>& changed);

}

#endif // TILEMAP_IMPORT_APPLY_H
