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

    // A blend, density or hole map an op makes, as its own structural step before the ops run: its
    // undo then removes the map, and the ops' delta has a map to apply to
    Command* terrainCreateMapStep(Project* project, SceneProject* scene, Entity entity, MapId map, const terrain::MapFormat& format) {
        const TerrainMapRef ref = TerrainMapUtils::refOfMap(map);
        TerrainComponent& terrain = scene->scene->getComponent<TerrainComponent>(entity);
        TerrainMapUtils::ensureMapSlot(terrain, ref);
        Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
        if (!texture) return nullptr;
        TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(project, *texture, true);
        TerrainMapSnapshot after;
        after.empty = false;
        after.path = TerrainMapUtils::makeEditableTexturePath(project, scene->id, entity, ref);
        after.id = after.path;
        after.colorFormat = TerrainMapUtils::expectedFormat(ref.target);
        after.channels = TerrainMapUtils::expectedChannels(ref.target);
        after.width = format.bounds.w;
        after.height = format.bounds.h;
        // what the engine's createMap fills: 0, opaque, holes solid
        const size_t texels = static_cast<size_t>(after.width) * after.height;
        after.pixels.assign(texels * after.channels, map.kind == MapKind::Hole ? 255 : 0);
        if (after.channels == 4) {
            for (size_t i = 3; i < after.pixels.size(); i += 4) after.pixels[i] = 255;
        }
        return new TerrainTextureEditCmd(project, scene->id, entity, ref, std::move(before), std::move(after));
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

    for (const MapId& map : preview.createdMaps) {
        EditorTerrainStore store(project, scene->id, engineScene, entity, engineScene->getComponent<TerrainComponent>(entity));
        terrain::MapFormat format = store.format(MapId{MapKind::Height, 0});
        if (options.newMapResolution > 0) format.bounds = terrain::TexelRect{0, 0, options.newMapResolution, options.newMapResolution};
        Command* command = terrainCreateMapStep(project, scene, entity, map, format);
        if (!command) {
            error = "The " + std::string(map.kind == MapKind::Hole ? "hole" : map.kind == MapKind::Blend ? "blend" : "density") + " map could not be made.";
            return false;
        }
        command->setNoMerge();
        if (!history->addCommand(command)) {
            error = "A new map could not be made.";
            return false;
        }
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

    auto delta = std::make_unique<TerrainMapDeltaCmd>(project, scene->id, entity, entity, overlay, false, stepName);
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
