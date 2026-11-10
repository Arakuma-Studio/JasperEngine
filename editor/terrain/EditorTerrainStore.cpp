// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorTerrainStore.h"
#include "component/TerrainComponent.h"

#include "Project.h"
#include "terrain/TerrainMapPersistence.h"
#include "util/TerrainMapUtils.h"

#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::terraincore;

editor::EditorTerrainStore::EditorTerrainStore(Project* project, uint32_t sceneId, Scene* scene, Entity entity, TerrainComponent& terrain)
    : TerrainComponentStore(scene, entity, terrain), project(project), sceneId(sceneId) {}

editor::EditorTerrainStore::~EditorTerrainStore(){
    endStroke();
}

void editor::EditorTerrainStore::beginStroke(){
    if (strokeState){
        return;
    }
    beginTerrainStroke(component());
    strokeState = component().editState;
    TerrainMapPersistence::get().beginStroke();
}

void editor::EditorTerrainStore::endStroke(){
    if (!strokeState){
        return;
    }
    strokeState->strokes--;
    strokeState.reset();
    TerrainMapPersistence::get().endStroke();
}

const std::vector<editor::EditorTerrainStore::Sharer>& editor::EditorTerrainStore::sharersOf(MapId map){
    for (const Sharing& known : sharing){
        if (known.map == map){
            return known.others;
        }
    }
    Sharing found;
    found.map = map;
    Texture* texture = terrainMapTexture(component(), map);
    const std::string path = texture ? texture->getPath(0) : std::string();
    if (project && !path.empty()){
        for (SceneProject* other : project->getScenesAndPrefabStages()){
            if (!other->scene){
                continue;
            }
            auto terrains = other->scene->getComponentArray<TerrainComponent>();
            for (size_t i = 0; i < terrains->size(); i++){
                const Entity otherEntity = terrains->getEntity(i);
                if (other->id == sceneId && otherEntity == entity){
                    continue;
                }
                Texture* otherTexture = terrainMapTexture(terrains->getComponentFromIndex(i), map);
                if (otherTexture && otherTexture->getPath(0) == path){
                    found.others.push_back(Sharer{other->id, otherEntity});
                }
            }
        }
    }
    sharing.push_back(std::move(found));
    return sharing.back().others;
}

void editor::EditorTerrainStore::afterEdit(){
    std::vector<std::pair<MapId, TexelRect>> changed;
    TerrainComponentStore::takeDirty(changed);
    if (changed.empty()){
        return;
    }
    SceneProject* own = project ? project->getScene(sceneId) : nullptr;
    if (own){
        own->needUpdateRender = true;
    }
    for (const auto& [map, rect] : changed){
        bool merged = false;
        for (auto& [keptMap, keptRect] : edits){
            if (keptMap == map){
                keptRect = unite(keptRect, rect);
                merged = true;
            }
        }
        if (!merged){
            edits.emplace_back(map, rect);
        }

        Texture* texture = terrainMapTexture(component(), map);
        if (!texture || !project){
            continue;
        }
        TerrainMapPersistence::get().markDirty(project, *texture);

        for (const Sharer& sharer : sharersOf(map)){
            SceneProject* other = project->getScene(sharer.sceneId);
            TerrainComponent* otherTerrain = other && other->scene ? other->scene->findComponent<TerrainComponent>(sharer.entity) : nullptr;
            Texture* otherTexture = otherTerrain ? terrainMapTexture(*otherTerrain, map) : nullptr;
            if (!otherTexture || otherTexture->getPath(0) != texture->getPath(0)){
                continue;
            }
            // Only a Texture that lost the pool's entry needs telling which one it is: this copies
            // pointers, and the pixels were written once, in the entry they share
            if (!otherTexture->hasData() || otherTexture->getData().getData() != texture->getData().getData()){
                *otherTexture = *texture;
            }
            noteTerrainMapEdited(other->scene, sharer.entity, *otherTerrain, map, rect);
            other->needUpdateRender = true;
        }
    }
}

WriteResult editor::EditorTerrainStore::write(MapId map, TexelRect rect, std::span<const float> in){
    const WriteResult result = TerrainComponentStore::write(map, rect, in);
    afterEdit();
    return result;
}

WriteResult editor::EditorTerrainStore::writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in){
    const WriteResult result = TerrainComponentStore::writeBytes(map, rect, in);
    afterEdit();
    return result;
}

void editor::EditorTerrainStore::takeDirty(std::vector<std::pair<MapId, TexelRect>>& out){
    out.insert(out.end(), edits.begin(), edits.end());
    edits.clear();
}

WriteResult editor::EditorTerrainStore::createMap(MapId map, MapFormat newFormat, float fill){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    // T4: a World's maps are its tiles' sections, made by its store with no file of their own
    if (!sceneProject || project->getProjectPath().empty() || component().world){
        return TerrainComponentStore::createMap(map, newFormat, fill);
    }
    std::string why;
    if (!canCreateMap(map, &why)){
        return WriteResult{false, why, 0};
    }
    if (format(map).exists){
        return WriteResult{false, "map already exists", 0};
    }
    if (newFormat.bounds.empty() || newFormat.channels <= 0){
        return WriteResult{false, "empty map", 0};
    }

    TerrainComponent& terrain = component();
    const TerrainMapRef ref = TerrainMapUtils::refOfMap(map);
    TerrainMapUtils::ensureMapSlot(terrain, ref);
    Texture* texture = terrainMapTexture(terrain, map);
    if (!texture){
        return WriteResult{false, "no such map", 0};
    }

    const int step = newFormat.bitsPerChannel >= 16 ? 2 : 1;
    const size_t texels = static_cast<size_t>(newFormat.bounds.area());
    std::vector<unsigned char> pixels(texels * newFormat.channels * step);
    const uint32_t quantized = quantizeChannel(fill, newFormat.bitsPerChannel);
    const uint32_t opaque = newFormat.bitsPerChannel >= 16 ? 65535u : 255u;
    for (size_t i = 0; i < texels; i++){
        unsigned char* stored = pixels.data() + i * newFormat.channels * step;
        for (int c = 0; c < newFormat.channels; c++){
            encodeChannel(stored + c * step, c == 3 ? opaque : quantized, newFormat.bitsPerChannel);
        }
    }

    const ColorFormat colorFormat = newFormat.channels == 1 ? (step == 2 ? ColorFormat::RED16 : ColorFormat::RED) : ColorFormat::RGBA;
    const std::string path = TerrainMapUtils::makeEditableTexturePath(project, sceneId, entity, ref);
    if (!TerrainMapUtils::setFileBackedTextureData(project, *texture, path, newFormat.bounds.w, newFormat.bounds.h, colorFormat, newFormat.channels, pixels)){
        return WriteResult{false, "the new map could not be stored", 0};
    }
    TerrainMapUtils::refresh(sceneProject, entity, ref);
    TerrainEditEvents::publish(scene, entity, map, newFormat.bounds);
    return WriteResult{true, "", 0};
}
