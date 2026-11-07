// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T1 R10, R24, R28: an op list on a scene's terrain as undo steps, shared by edit_terrain and the
// Terrain window (Import Raw..., Keep World Heights). Maps the ops make come first as structural
// steps, then one tile delta, then the hole map the last fill removed. The caller marks the
// history before and groups the steps after (CommandHistory::mark, groupSince).

#include "ecs/Entity.h"
#include "terrain/TerrainOps.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

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
