// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "foliage/FoliageMapPatchCmd.h"

#include "Project.h"
#include "component/InstancedMeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/TerrainEdit.h"
#include "util/TerrainMapUtils.h"

#include <map>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::terrain;

editor::FoliageMapPatchCmd::FoliageMapPatchCmd(Project* project, uint32_t sceneId, Entity terrain, MapId map, TexelRect rect,
                                               std::vector<uint8_t> before, std::vector<uint8_t> after, std::string name):
    project(project), sceneId(sceneId), terrain(terrain), map(map), rect(rect), before(std::move(before)), after(std::move(after)), name(std::move(name)) {}

void editor::FoliageMapPatchCmd::write(const std::vector<uint8_t>& bytes) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return;
    Scene* scene = sceneProject->scene;
    TerrainComponent* component = scene->findComponent<TerrainComponent>(terrain);
    if (!component) return;
    TerrainComponentStore store(scene, terrain, *component);
    store.writeBytes(map, rect, bytes);

    // the whole map to its painted file, as a stroke's end does; latest pixels win per path
    if (map.index >= 0 && map.index < static_cast<int>(component->foliageLayers.size())) {
        Texture& texture = component->foliageLayers[map.index].densityMap;
        if (texture.hasData() && !texture.getPath(0).empty()) {
            TextureData& data = texture.getData();
            const size_t size = static_cast<size_t>(data.getWidth()) * data.getHeight() * data.getChannels() * TextureData::getBytesPerChannel(data.getColorFormat());
            const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
            TerrainMapUtils::writeFile(project, texture.getPath(0), data.getWidth(), data.getHeight(), data.getChannels(),
                                       TextureData::getBytesPerChannel(data.getColorFormat()), std::vector<unsigned char>(pixels, pixels + size));
        }
    }
    sceneProject->isModified = true;
}

bool editor::FoliageMapPatchCmd::execute() {
    write(after);
    return true;
}

void editor::FoliageMapPatchCmd::undo() {
    write(before);
}

std::function<void(bool)> editor::FoliageMapPatchCmd::emptyHosts(Project* project, uint32_t sceneId, std::vector<std::string> hosts) {
    auto kept = std::make_shared<std::map<std::string, std::vector<InstanceData>>>();
    return [project, sceneId, hosts = std::move(hosts), kept](bool undone) {
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject) return;
        for (const std::string& name : hosts) {
            const Entity entity = static_cast<Entity>(std::stoul(name));
            InstancedMeshComponent* instmesh = sceneProject->scene->findComponent<InstancedMeshComponent>(entity);
            if (!instmesh) continue;
            if (undone) {
                instmesh->instances = (*kept)[name];
            } else {
                (*kept)[name] = instmesh->instances;
                instmesh->instances.clear();
            }
            instmesh->needUpdateInstances = true;
        }
    };
}
