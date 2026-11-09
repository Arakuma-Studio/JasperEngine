// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T1 R10, R24, R28: an op list on a scene's terrain as undo steps, shared by edit_terrain and the
// Terrain window (Import Raw..., Keep World Heights). Maps the ops make come first as structural
// steps, then one tile delta, then the hole map the last fill removed. The caller marks the
// history before and groups the steps after (CommandHistory::mark, groupSince).

#include "ecs/Entity.h"
#include "terrain/OverlayMapStore.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainOps.h"
#include "util/TerrainMapUtils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // A map an edit makes: its slot as it was, and the blank map the edit runs on (what the engine's
    // createMap fills: 0, opaque, holes solid)
    struct TerrainMadeMap {
        terraincore::MapId map;
        TerrainMapRef ref;
        TerrainMapSnapshot before;
        TerrainMapSnapshot blank;
    };

    // Makes the blank map at once, so the edit has a map to write; keepTerrainMadeMap adds the step that keeps it
    bool makeTerrainBlankMap(Project* project, SceneProject* scene, Entity entity, terraincore::MapId map, const terraincore::MapFormat& format,
                             TerrainMadeMap& made);
    // The structural step of a made map, holding its final pixels as `edited` has them: its undo takes the map
    // away, its redo brings it back whole, so the edit's tile delta skips it (TerrainMapDeltaCmd's skipMaps)
    bool keepTerrainMadeMap(Project* project, SceneProject* scene, Entity entity, const TerrainMadeMap& made,
                            const terraincore::TerrainMapStore& edited, const std::string& stepName, std::string& error);

    // The list as a dry run: what it would change, the maps it would make and remove
    terraincore::TerrainOpResult previewTerrainOps(Project* project, SceneProject* scene, Entity entity, const std::vector<terraincore::Op>& ops,
                                               const terraincore::OpOptions& options);

    // Runs the list for real. preview is previewTerrainOps' answer; reported gets its numbers
    // (they saw the whole transaction). False with error; the steps added so far stay for the
    // caller to undo.
    bool applyTerrainOpsWithUndo(Project* project, SceneProject* scene, Entity entity, const std::vector<terraincore::Op>& ops,
                                 const terraincore::OpOptions& options, const terraincore::TerrainOpResult& preview, const std::string& stepName,
                                 terraincore::TerrainOpResult& reported, std::string& error);

    // R69: a copy of a terrain's maps a job edits off the main thread, with the hash of each map
    // when it was taken (the commit refuses when the terrain changed meanwhile). The overlay over
    // the copy holds the job's changes.
    struct TerrainOpsSnapshot {
        std::unique_ptr<terraincore::RawMapStore> store;
        std::unique_ptr<terraincore::OverlayMapStore> overlay;
        std::vector<std::pair<terraincore::MapId, uint64_t>> hashes;
    };
    std::shared_ptr<TerrainOpsSnapshot> snapshotTerrainMaps(Project* project, SceneProject* scene, Entity entity);

    // The job's changes as the same undo steps applyTerrainOpsWithUndo makes; false with error
    // when the terrain is gone or a map changed since the snapshot (nothing is applied then)
    bool commitTerrainOpsWithUndo(Project* project, SceneProject* scene, Entity entity, const TerrainOpsSnapshot& snapshot,
                                  const terraincore::TerrainOpResult& result, const std::string& stepName, std::string& error);

}
