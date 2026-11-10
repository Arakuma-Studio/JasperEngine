// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainTextureEditCmd.h"
#include "component/TerrainComponent.h"

#include "command/CommandNames.h"
#include "terrain/TerrainBytes.h"

#include <utility>

using namespace jasperengine;

editor::TerrainTextureEditCmd::PackedSnapshot editor::TerrainTextureEditCmd::pack(TerrainMapSnapshot snapshot){
    PackedSnapshot packed;
    packed.rawBytes = snapshot.pixels.size();
    if (!snapshot.pixels.empty()){
        packed.packed = packTerrainBytes(snapshot.pixels);
    }
    snapshot.pixels.clear();
    snapshot.pixels.shrink_to_fit();
    packed.head = std::move(snapshot);
    return packed;
}

editor::TerrainMapSnapshot editor::TerrainTextureEditCmd::unpack(const PackedSnapshot& snapshot){
    TerrainMapSnapshot result = snapshot.head;
    if (snapshot.rawBytes > 0){
        result.pixels.resize(snapshot.rawBytes);
        if (!unpackTerrainBytes(snapshot.packed, result.pixels)){
            result.pixels.clear();
        }
    }
    return result;
}

editor::TerrainTextureEditCmd::TerrainTextureEditCmd(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref,
                                                     TerrainMapSnapshot before, TerrainMapSnapshot after, std::string verb):
    project(project), sceneId(sceneId), entity(entity), ref(ref), beforePacked(pack(std::move(before))), afterPacked(pack(std::move(after))),
    verbOverride(std::move(verb)){}

void editor::TerrainTextureEditCmd::apply(const PackedSnapshot& snapshot, bool restoreModifiedState){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return;
    }
    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(entity);
    if (!terrain){
        return;
    }

    TerrainMapUtils::ensureMapSlot(*terrain, ref);
    Texture* texture = TerrainMapUtils::findTexture(*terrain, ref);
    if (!texture){
        return;
    }
    const std::string oldPath = texture->getPath(0);
    if (!restoreModifiedState){
        prefabUndo.capture(project, sceneId, entity);
    }
    TerrainMapUtils::applySnapshotToTexture(project, *texture, unpack(snapshot));
    TerrainMapUtils::refresh(sceneProject, entity, ref);

    if (restoreModifiedState){
        prefabUndo.restore(project, sceneId);
    }else if (texture->getPath(0) != oldPath){
        // Unity: another map bound to a prefab instance's terrain is an override of it.
        // The list carries its own length, which is what a newly created map needs
        std::vector<std::string> properties = {TerrainMapUtils::getPropertyName(ref)};
        const std::string container = TerrainMapUtils::getContainerPropertyName(ref);
        if (!container.empty()){
            properties.push_back(container);
        }
        project->recordPrefabEdit(sceneId, entity, ComponentType::TerrainComponent, properties);
    }
    // the content of the file is an asset's: the other terrains using it follow
    TerrainMapUtils::refreshSharing(project, sceneId, entity, ref);

    if (restoreModifiedState){
        sceneProject->isModified = wasModified;
    }else{
        sceneProject->isModified = true;
    }
}

bool editor::TerrainTextureEditCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return false;
    }
    wasModified = sceneProject->isModified;
    apply(afterPacked, false);
    return true;
}

void editor::TerrainTextureEditCmd::undo(){
    apply(beforePacked, true);
}

std::string editor::TerrainTextureEditCmd::describe() const{
    const char* verb = ref.target == TerrainMapTarget::HeightMap ? "Sculpt Terrain" :
        (ref.target == TerrainMapTarget::DensityMap ? "Paint Details" : "Paint Terrain");
    return (verbOverride.empty() ? std::string(verb) : verbOverride) + " (" + CommandNames::entity(project, sceneId, entity) + ")";
}

size_t editor::TerrainTextureEditCmd::memoryBytes() const{
    return sizeof(*this) + beforePacked.packed.capacity() + afterPacked.packed.capacity() +
        beforePacked.head.path.size() + afterPacked.head.path.size();
}

std::vector<editor::Command::Touched> editor::TerrainTextureEditCmd::touchedPaths(bool undo) const{
    const TerrainMapSnapshot& snapshot = undo ? beforePacked.head : afterPacked.head;
    if (snapshot.path.empty()) return {};
    return {{project->resolveAssetPath(snapshot.path), false}};
}
