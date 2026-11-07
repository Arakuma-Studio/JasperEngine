// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainOpsUndo.h"
#include "component/TerrainComponent.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/TerrainMapDeltaCmd.h"
#include "command/type/TerrainTextureEditCmd.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/OverlayMapStore.h"
#include "util/TerrainMapUtils.h"

#include <algorithm>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using terrain::MapId;
using terrain::MapKind;

namespace {

    // A map an op makes: its slot as it was, and the blank map the ops run on (what the engine's
    // createMap fills: 0, opaque, holes solid)
    struct TerrainMadeMap {
        MapId map;
        TerrainMapRef ref;
        TerrainMapSnapshot before;
        TerrainMapSnapshot blank;
    };

    bool terrainMakeBlankMap(Project* project, SceneProject* scene, Entity entity, MapId map, const terrain::MapFormat& format, TerrainMadeMap& made) {
        made.map = map;
        made.ref = TerrainMapUtils::refOfMap(map);
        TerrainComponent& terrain = scene->scene->getComponent<TerrainComponent>(entity);
        TerrainMapUtils::ensureMapSlot(terrain, made.ref);
        Texture* texture = TerrainMapUtils::findTexture(terrain, made.ref);
        if (!texture) return false;
        made.before = TerrainMapUtils::captureSnapshot(project, *texture, true);
        TerrainMapSnapshot& blank = made.blank;
        blank.empty = false;
        blank.path = TerrainMapUtils::makeEditableTexturePath(project, scene->id, entity, made.ref);
        blank.id = blank.path;
        blank.colorFormat = TerrainMapUtils::expectedFormat(made.ref.target);
        blank.channels = TerrainMapUtils::expectedChannels(made.ref.target);
        blank.width = format.bounds.w;
        blank.height = format.bounds.h;
        const size_t texels = static_cast<size_t>(blank.width) * blank.height;
        blank.pixels.assign(texels * blank.channels, map.kind == MapKind::Hole ? 255 : 0);
        if (blank.channels == 4) {
            for (size_t i = 3; i < blank.pixels.size(); i += 4) blank.pixels[i] = 255;
        }
        // made at once so the ops have a map to write; the step that keeps it comes after them
        TerrainTextureEditCmd making(project, scene->id, entity, made.ref, made.before, made.blank);
        return making.execute();
    }

}

terrain::TerrainOpResult editor::previewTerrainOps(Project* project, SceneProject* scene, Entity entity, const std::vector<terrain::Op>& ops,
                                                   const terrain::OpOptions& options) {
    EditorTerrainStore store(project, scene->id, scene->scene, entity, scene->scene->getComponent<TerrainComponent>(entity));
    terrain::OpOptions dry = options;
    dry.dryRun = true;
    return terrain::applyOps(store, ops, dry);
}

bool editor::applyTerrainOpsWithUndo(Project* project, SceneProject* scene, Entity entity, const std::vector<terrain::Op>& ops,
                                     const terrain::OpOptions& options, const terrain::TerrainOpResult& preview, const std::string& stepName,
                                     terrain::TerrainOpResult& reported, std::string& error) {
    CommandHistory* history = CommandHandle::get(scene->id);
    Scene* engineScene = scene->scene;

    std::vector<TerrainMadeMap> made;
    std::vector<MapId> madeIds;
    for (const MapId& map : preview.createdMaps) {
        EditorTerrainStore store(project, scene->id, engineScene, entity, engineScene->getComponent<TerrainComponent>(entity));
        terrain::MapFormat format = store.format(MapId{MapKind::Height, 0});
        if (options.newMapResolution > 0) format.bounds = terrain::TexelRect{0, 0, options.newMapResolution, options.newMapResolution};
        made.emplace_back();
        if (!terrainMakeBlankMap(project, scene, entity, map, format, made.back())) {
            error = "The " + std::string(map.kind == MapKind::Hole ? "hole" : map.kind == MapKind::Blend ? "blend" : "density") + " map could not be made.";
            return false;
        }
        madeIds.push_back(map);
    }

    // the last fill keeps the hole map here; its removal is a structural step of its own
    const bool removesHoles = std::find(preview.removedMaps.begin(), preview.removedMaps.end(), MapId{MapKind::Hole, 0}) != preview.removedMaps.end();
    EditorTerrainStore writer(project, scene->id, engineScene, entity, engineScene->getComponent<TerrainComponent>(entity));
    writer.setKeepsMaps(true);
    terrain::OverlayMapStore overlay(writer);
    terrain::OpOptions inPlace = options;
    inPlace.inPlace = true;
    const terrain::TerrainOpResult result = terrain::applyOps(overlay, ops, inPlace);
    if (!result.ok) {
        error = result.error;
        return false;
    }
    reported = preview;
    reported.dryRun = false;
    reported.undoHooks = result.undoHooks;

    // a made map's step holds its final pixels: its undo takes the map away, its redo brings it back
    // whole, and the delta below carries only the maps that were there
    for (TerrainMadeMap& map : made) {
        TerrainMapSnapshot after = map.blank;
        if (!overlay.readBytes(map.map, terrain::TexelRect{0, 0, after.width, after.height}, after.pixels)) {
            error = "A new map could not be read back.";
            return false;
        }
        Command* command = new TerrainTextureEditCmd(project, scene->id, entity, map.ref, map.before, std::move(after), stepName);
        command->setNoMerge();
        if (!history->addCommand(command)) {
            error = "A new map could not be made.";
            return false;
        }
    }

    auto delta = std::make_unique<TerrainMapDeltaCmd>(project, scene->id, entity, entity, overlay, false, stepName, madeIds);
    for (const auto& hook : result.undoHooks) delta->addUndoHook(hook);
    if (!delta->empty()) {
        Command* command = delta.release();
        command->setNoMerge();
        if (!history->addCommand(command)) {
            error = "The edit could not be applied.";
            return false;
        }
    }
    overlay.discard();

    if (removesHoles) {
        Texture* holes = TerrainMapUtils::findTexture(engineScene->getComponent<TerrainComponent>(entity), TerrainMapRef(TerrainMapTarget::HoleMap));
        if (holes && !holes->empty()) {
            TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(project, *holes, true);
            Command* removal = new TerrainTextureEditCmd(project, scene->id, entity, TerrainMapRef(TerrainMapTarget::HoleMap), std::move(before),
                                                         TerrainMapSnapshot(), stepName);
            removal->setNoMerge();
            history->addCommand(removal);
        }
    }
    return true;
}
