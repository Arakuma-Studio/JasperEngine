// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMapDeltaCmd.h"
#include "component/TerrainComponent.h"

#include "Project.h"
#include "command/CommandNames.h"
#include "jobs/JobSystem.h"
#include "object/TerrainEdit.h"
#include "pool/TextureDataPool.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/TerrainBytes.h"
#include "terrainworld/TerrainWorldEditor.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // T1 R49: below this many tiles a stroke's commit packs them on the main thread
    constexpr size_t PARALLEL_DELTA_TILES = 32;

    TexelRect deltaTileRect(int x, int y, const TexelRect& bounds) {
        return intersect(TexelRect{x, y, OverlayMapStore::TILE_SIZE, OverlayMapStore::TILE_SIZE}, bounds);
    }

}

editor::TerrainMapDeltaCmd::TerrainMapDeltaCmd(Project* project, uint32_t sceneId, Entity entity, Entity nameEntity, const OverlayMapStore& overlay,
                                               bool alreadyApplied, std::string verb, const std::vector<MapId>& skipMaps)
    : project(project), sceneId(sceneId), entity(entity), nameEntity(nameEntity), verb(std::move(verb)), applied(alreadyApplied) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    TerrainComponent* terrain = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;

    // each tile's XOR, packed on the workers; the steps are put together in the overlay's order, so
    // the stored bytes do not depend on the thread count
    std::vector<OverlayMapStore::TileView> views = overlay.tileViews();
    std::erase_if(views, [&](const OverlayMapStore::TileView& view) { return std::find(skipMaps.begin(), skipMaps.end(), view.map) != skipMaps.end(); });
    std::vector<std::vector<uint8_t>> packed(views.size());
    auto pack = [&](size_t index) {
        const OverlayMapStore::TileView& view = views[index];
        thread_local std::vector<uint8_t> xored;
        xored.resize(view.before.size());
        bool changed = false;
        for (size_t i = 0; i < view.before.size(); i++) {
            xored[i] = view.before[i] ^ view.after[i];
            changed = changed || xored[i] != 0;
        }
        if (changed) packed[index] = packTerrainBytes(xored);
    };
    if (views.size() >= PARALLEL_DELTA_TILES) {
        jobs::parallelFor(views.size(), pack);
    } else {
        for (size_t i = 0; i < views.size(); i++) pack(i);
    }

    for (size_t index = 0; index < views.size(); index++) {
        // an empty pack is an unchanged tile: a changed one packs to at least one byte
        if (packed[index].empty()) continue;
        const MapId map = views[index].map;
        if (maps.empty() || maps.back().map != map) {
            MapDelta delta;
            delta.map = map;
            const MapFormat format = overlay.format(map);
            delta.bounds = format.bounds;
            delta.channels = format.channels;
            if (Texture* texture = terrain ? terrainMapTexture(*terrain, map) : nullptr) {
                delta.path = texture->getPath(0);
                delta.id = texture->getId();
                if (texture->hasData()) delta.colorFormat = texture->getData().getColorFormat();
                delta.generation = TextureDataPool::generation(delta.id);
            }
            maps.push_back(std::move(delta));
        }
        Tile tile;
        tile.x = views[index].rect.x;
        tile.y = views[index].rect.y;
        tile.bytes = std::move(packed[index]);
        maps.back().tiles.push_back(std::move(tile));
    }
    for (MapDelta& delta : maps) {
        delta.tiles.shrink_to_fit();
    }
}

void editor::TerrainMapDeltaCmd::addUndoHook(std::shared_ptr<TerrainStoreUndoHook> hook) {
    if (hook) undoHooks.push_back(std::move(hook));
}

void editor::TerrainMapDeltaCmd::addLargeEdit(std::shared_ptr<TerrainLargeEdit> edit) {
    if (edit) largeEdits.push_back(std::move(edit));
}

std::string editor::TerrainMapDeltaCmd::refusal() const {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    TerrainComponent* terrain = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    // T4 R30: a World's maps are its tiles, which only this history writes in the editor
    if (terrain && terrain->world) return std::string();
    for (const MapDelta& delta : maps) {
        Texture* texture = terrain ? terrainMapTexture(*terrain, delta.map) : nullptr;
        const std::string name = delta.path.empty() ? delta.id : delta.path;
        const std::string changed = "The map changed outside this history (" + name + "): this step no longer applies to it.";
        if (!texture || texture->getPath(0) != delta.path || texture->getId() != delta.id) return changed;
        if (TextureDataPool::generation(delta.id) != delta.generation) return changed;
        if (texture->empty() || texture->isFramebuffer() || !texture->hasData()) return changed;
        TextureData& data = texture->getData();
        if (data.getWidth() != delta.bounds.w || data.getHeight() != delta.bounds.h || data.getChannels() != delta.channels ||
            data.getColorFormat() != delta.colorFormat || !data.getData()) {
            return changed;
        }
    }
    return std::string();
}

std::string editor::TerrainMapDeltaCmd::undoRefusal() const {
    const std::string why = Command::undoRefusal();
    return why.empty() ? refusal() : why;
}

std::string editor::TerrainMapDeltaCmd::redoRefusal() const {
    const std::string why = Command::redoRefusal();
    return why.empty() ? refusal() : why;
}

void editor::TerrainMapDeltaCmd::applyXor(bool makesTiles) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    TerrainComponent* terrain = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    if (!terrain) return;

    EditorTerrainStore store(project, sceneId, sceneProject->scene, entity, *terrain);
    // T4 R29, R30: the first application of an overlay's bytes makes the World's tiles it reaches;
    // replaying them never does, as the store's hook puts back the ones it made
    std::optional<TerrainWorldAutoAddPause> pause;
    if (!makesTiles) pause.emplace(*terrain);
    std::vector<uint8_t> xored;
    std::vector<uint8_t> current;
    for (const MapDelta& delta : maps) {
        // (a World's maps have no texture: their stored texels say it)
        const int stride = terrain->world ? bytesPerTexel(store.format(delta.map))
                                          : delta.channels * TextureData::getBytesPerChannel(delta.colorFormat);
        for (const Tile& tile : delta.tiles) {
            const TexelRect rect = deltaTileRect(tile.x, tile.y, delta.bounds);
            const size_t bytes = static_cast<size_t>(rect.area()) * stride;
            xored.resize(bytes);
            current.resize(bytes);
            if (!unpackTerrainBytes(tile.bytes, xored) || !store.readBytes(delta.map, rect, current)) continue;
            for (size_t i = 0; i < bytes; i++) {
                current[i] ^= xored[i];
            }
            store.writeBytes(delta.map, rect, current);
        }
    }
    if (makesTiles) {
        if (std::unique_ptr<terrain::TerrainStoreUndoHook> hook = store.takeUndoHook()) undoHooks.push_back(std::move(hook));
    }
}

bool editor::TerrainMapDeltaCmd::execute() {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject) return false;
    wasModified = sceneProject->isModified;
    if (applied) {
        // the map already holds it: only the next redo has work to do
        applied = false;
    } else {
        for (const auto& edit : largeEdits) {
            edit->redo();
        }
        // the hooks of this step so far: one the first application adds has nothing to redo yet
        const std::vector<std::shared_ptr<terrain::TerrainStoreUndoHook>> hooks = undoHooks;
        applyXor(!replayed);
        for (const auto& hook : hooks) {
            hook->redo();
        }
    }
    replayed = true;
    // Painting writes the map's file, an asset like Unity's TerrainData: no override, and every
    // terrain on that file follows (R41)
    sceneProject->isModified = true;
    return true;
}

void editor::TerrainMapDeltaCmd::undo() {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject) return;
    for (auto it = undoHooks.rbegin(); it != undoHooks.rend(); ++it) {
        (*it)->undo();
    }
    applyXor(false);
    for (auto it = largeEdits.rbegin(); it != largeEdits.rend(); ++it) {
        (*it)->undo();
    }
    sceneProject->isModified = wasModified;
}

std::vector<editor::Command::Touched> editor::TerrainMapDeltaCmd::touchedPaths(bool) const {
    std::vector<Touched> touched;
    if (!project) return touched;
    for (const MapDelta& delta : maps) {
        if (!delta.path.empty()) touched.push_back({project->resolveAssetPath(delta.path), false});
    }
    return touched;
}

std::string editor::TerrainMapDeltaCmd::describe() const {
    std::string action = verb;
    if (action.empty()) {
        const MapKind kind = maps.empty() ? MapKind::Height : maps.front().map.kind;
        action = kind == MapKind::Blend ? "Paint Terrain" : (kind == MapKind::Density ? "Paint Details" : "Sculpt Terrain");
    }
    return action + " (" + CommandNames::entity(project, sceneId, nameEntity) + ")";
}

size_t editor::TerrainMapDeltaCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + verb.capacity() + maps.capacity() * sizeof(MapDelta);
    for (const MapDelta& delta : maps) {
        bytes += delta.path.capacity() + delta.id.capacity() + delta.tiles.capacity() * sizeof(Tile);
        for (const Tile& tile : delta.tiles) {
            bytes += tile.bytes.capacity();
        }
    }
    return bytes;
}

size_t editor::TerrainMapDeltaCmd::tileCount() const {
    size_t count = 0;
    for (const MapDelta& delta : maps) {
        count += delta.tiles.size();
    }
    return count;
}

size_t editor::TerrainMapDeltaCmd::rawTileCount() const {
    size_t count = 0;
    for (const MapDelta& delta : maps) {
        const int stride = delta.channels * TextureData::getBytesPerChannel(delta.colorFormat);
        for (const Tile& tile : delta.tiles) {
            if (tile.bytes.size() == static_cast<size_t>(deltaTileRect(tile.x, tile.y, delta.bounds).area()) * stride) count++;
        }
    }
    return count;
}
