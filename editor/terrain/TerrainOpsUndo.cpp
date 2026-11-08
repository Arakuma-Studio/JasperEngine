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
#include "texture/TextureData.h"

#include <algorithm>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using terrain::MapId;
using terrain::MapKind;

bool editor::makeTerrainBlankMap(Project* project, SceneProject* scene, Entity entity, MapId map, const terrain::MapFormat& format, TerrainMadeMap& made) {
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
    // a height map's texels are 16-bit
    const size_t texels = static_cast<size_t>(blank.width) * blank.height;
    blank.pixels.assign(texels * blank.channels * TextureData::getBytesPerChannel(blank.colorFormat), map.kind == MapKind::Hole ? 255 : 0);
    if (blank.channels == 4) {
        for (size_t i = 3; i < blank.pixels.size(); i += 4) blank.pixels[i] = 255;
    }
    // made at once so the edit has a map to write; the step that keeps it comes after it
    TerrainTextureEditCmd making(project, scene->id, entity, made.ref, made.before, made.blank);
    return making.execute();
}

bool editor::keepTerrainMadeMap(Project* project, SceneProject* scene, Entity entity, const TerrainMadeMap& made,
                                const terrain::TerrainMapStore& edited, const std::string& stepName, std::string& error) {
    TerrainMapSnapshot after = made.blank;
    if (!edited.readBytes(made.map, terrain::TexelRect{0, 0, after.width, after.height}, after.pixels)) {
        error = "A new map could not be read back.";
        return false;
    }
    Command* command = new TerrainTextureEditCmd(project, scene->id, entity, made.ref, made.before, std::move(after), stepName);
    command->setNoMerge();
    if (!CommandHandle::get(scene->id)->addCommand(command)) {
        error = "A new map could not be made.";
        return false;
    }
    return true;
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
        if (!makeTerrainBlankMap(project, scene, entity, map, format, made.back())) {
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
    for (const TerrainMadeMap& map : made) {
        if (!keepTerrainMadeMap(project, scene, entity, map, overlay, stepName, error)) return false;
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

std::shared_ptr<editor::TerrainOpsSnapshot> editor::snapshotTerrainMaps(Project* project, SceneProject* scene, Entity entity) {
    TerrainComponent& terrain = scene->scene->getComponent<TerrainComponent>(entity);
    EditorTerrainStore live(project, scene->id, scene->scene, entity, terrain);
    auto snapshot = std::make_shared<TerrainOpsSnapshot>();
    snapshot->store = std::make_unique<terrain::RawMapStore>(terrain.terrainSize, terrain.maxHeight);
    std::vector<MapId> maps = {MapId{MapKind::Height, 0}, MapId{MapKind::Hole, 0}};
    for (size_t i = 0; i < terrain.blendMaps.size(); i++) maps.push_back(MapId{MapKind::Blend, static_cast<int>(i)});
    for (size_t i = 0; i < terrain.foliageLayers.size(); i++) maps.push_back(MapId{MapKind::Density, static_cast<int>(i)});
    for (const MapId& map : maps) {
        const terrain::MapFormat format = live.format(map);
        if (!format.exists) continue;
        std::vector<uint8_t> bytes(static_cast<size_t>(format.bounds.area()) * format.channels * (format.bitsPerChannel >= 16 ? 2 : 1));
        if (!live.readBytes(map, format.bounds, bytes)) continue;
        snapshot->store->addMap(map, format.bounds.w, format.bounds.h, format.channels, format.bitsPerChannel, std::move(bytes));
        snapshot->hashes.emplace_back(map, terrain::mapHash(live, map));
    }
    snapshot->overlay = std::make_unique<terrain::OverlayMapStore>(*snapshot->store);
    return snapshot;
}

bool editor::commitTerrainOpsWithUndo(Project* project, SceneProject* scene, Entity entity, const TerrainOpsSnapshot& snapshot,
                                      const terrain::TerrainOpResult& result, const std::string& stepName, std::string& error) {
    Scene* engineScene = scene && scene->scene ? scene->scene : nullptr;
    if (!engineScene || !engineScene->findComponent<TerrainComponent>(entity)) {
        error = "The terrain was removed while the job ran: nothing was applied.";
        return false;
    }
    {
        EditorTerrainStore live(project, scene->id, engineScene, entity, engineScene->getComponent<TerrainComponent>(entity));
        for (const auto& [map, hash] : snapshot.hashes) {
            if (!live.format(map).exists || terrain::mapHash(live, map) != hash) {
                error = "The terrain changed while the job ran (" + std::string(map.kind == MapKind::Height ? "height" : map.kind == MapKind::Hole ? "hole" : map.kind == MapKind::Blend ? "blend" : "density") +
                        " map): nothing was applied. Call edit_terrain again.";
                return false;
            }
        }
    }
    CommandHistory* history = CommandHandle::get(scene->id);
    // a map the ops made is one step holding its final pixels, as applyTerrainOpsWithUndo does
    for (const MapId& map : result.createdMaps) {
        const terrain::MapFormat format = snapshot.overlay->format(map);
        TerrainMadeMap made;
        if (!makeTerrainBlankMap(project, scene, entity, map, format, made)) {
            error = "A new map could not be made.";
            return false;
        }
        if (!keepTerrainMadeMap(project, scene, entity, made, *snapshot.overlay, stepName, error)) return false;
    }
    // the live maps equal the copy, so the copy's before and after tiles are the live map's
    auto delta = std::make_unique<TerrainMapDeltaCmd>(project, scene->id, entity, entity, *snapshot.overlay, false, stepName, result.createdMaps);
    for (const auto& hook : result.undoHooks) delta->addUndoHook(hook);
    if (!delta->empty()) {
        Command* command = delta.release();
        command->setNoMerge();
        if (!history->addCommand(command)) {
            error = "The edit could not be applied.";
            return false;
        }
    }
    if (std::find(result.removedMaps.begin(), result.removedMaps.end(), MapId{MapKind::Hole, 0}) != result.removedMaps.end()) {
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
