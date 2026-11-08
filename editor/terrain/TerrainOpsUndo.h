// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T1 R10, R24, R28: an op list on a scene's terrain as undo steps, shared by edit_terrain and the
// Terrain window (Import Raw..., Keep World Heights). Maps the ops make come first as structural
// steps, then one tile delta, then the hole map the last fill removed. The caller marks the
// history before and groups the steps after (CommandHistory::mark, groupSince).

#include "ecs/Entity.h"
#include "terrain/TerrainOps.h"
#include "util/TerrainMapUtils.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // A map an edit makes: its slot as it was, and the blank map the edit runs on (what the engine's
    // createMap fills: 0, opaque, holes solid)
    struct TerrainMadeMap {
        terrain::MapId map;
        TerrainMapRef ref;
        TerrainMapSnapshot before;
        TerrainMapSnapshot blank;
    };

    // Makes the blank map at once, so the edit has a map to write; keepTerrainMadeMap adds the step that keeps it
    bool makeTerrainBlankMap(Project* project, SceneProject* scene, Entity entity, terrain::MapId map, const terrain::MapFormat& format,
                             TerrainMadeMap& made);
    // The structural step of a made map, holding its final pixels as `edited` has them: its undo takes the map
    // away, its redo brings it back whole, so the edit's tile delta skips it (TerrainMapDeltaCmd's skipMaps)
    bool keepTerrainMadeMap(Project* project, SceneProject* scene, Entity entity, const TerrainMadeMap& made,
                            const terrain::TerrainMapStore& edited, const std::string& stepName, std::string& error);

    // The list as a dry run: what it would change, the maps it would make and remove
    terrain::TerrainOpResult previewTerrainOps(Project* project, SceneProject* scene, Entity entity, const std::vector<terrain::Op>& ops,
                                               const terrain::OpOptions& options);

    // Runs the list for real. preview is previewTerrainOps' answer; reported gets its numbers
    // (they saw the whole transaction). False with error; the steps added so far stay for the
    // caller to undo.
    bool applyTerrainOpsWithUndo(Project* project, SceneProject* scene, Entity entity, const std::vector<terrain::Op>& ops,
                                 const terrain::OpOptions& options, const terrain::TerrainOpResult& preview, const std::string& stepName,
                                 terrain::TerrainOpResult& reported, std::string& error);

}
