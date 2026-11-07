// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMapDeltaCmd.h"
#include "component/TerrainComponent.h"

#include "Project.h"
#include "command/CommandNames.h"
#include "object/TerrainEdit.h"
#include "pool/TextureDataPool.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/TerrainBytes.h"

#include <algorithm>
#include <cstring>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    TexelRect deltaTileRect(int x, int y, const TexelRect& bounds) {
        return intersect(TexelRect{x, y, OverlayMapStore::TILE_SIZE, OverlayMapStore::TILE_SIZE}, bounds);
    }

}

editor::TerrainMapDeltaCmd::TerrainMapDeltaCmd(Project* project, uint32_t sceneId, Entity entity, Entity nameEntity, const OverlayMapStore& overlay,
                                               bool alreadyApplied, std::string verb)
    : project(project), sceneId(sceneId), entity(entity), nameEntity(nameEntity), verb(std::move(verb)), applied(alreadyApplied) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    TerrainComponent* terrain = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;

    std::vector<uint8_t> xored;
    overlay.visitTiles([&](MapId map, TexelRect rect, std::span<const uint8_t> before, std::span<const uint8_t> after) {
        xored.resize(before.size());
        bool changed = false;
        for (size_t i = 0; i < before.size(); i++) {
            xored[i] = before[i] ^ after[i];
            changed = changed || xored[i] != 0;
        }
        if (!changed) return;

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
        tile.x = rect.x;
        tile.y = rect.y;
        tile.bytes = packTerrainBytes(xored);
        maps.back().tiles.push_back(std::move(tile));
    });
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

void editor::TerrainMapDeltaCmd::applyXor() {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    TerrainComponent* terrain = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    if (!terrain) return;

    EditorTerrainStore store(project, sceneId, sceneProject->scene, entity, *terrain);
    std::vector<uint8_t> xored;
    std::vector<uint8_t> current;
    for (const MapDelta& delta : maps) {
        const int stride = delta.channels * TextureData::getBytesPerChannel(delta.colorFormat);
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
        applyXor();
        for (const auto& hook : undoHooks) {
            hook->redo();
        }
    }
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
    applyXor();
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
