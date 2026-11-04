// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMapDeltaCmd.h"

#include "Project.h"
#include "component/TerrainComponent.h"
#include "object/TerrainEdit.h"
#include "util/TerrainMapUtils.h"

using namespace jasperengine;

namespace {

    editor::TerrainMapRef refOf(terrain::MapId map) {
        switch (map.kind) {
            case terrain::MapKind::Blend: return editor::TerrainMapRef(editor::TerrainMapTarget::BlendMap, map.index);
            case terrain::MapKind::Density: return editor::TerrainMapRef(editor::TerrainMapTarget::DensityMap, map.index);
            default: return editor::TerrainMapRef(editor::TerrainMapTarget::HeightMap);
        }
    }

}

editor::TerrainMapDeltaCmd::TerrainMapDeltaCmd(Project* project, uint32_t sceneId, Entity entity, std::string label, Producer producer):
    project(project), sceneId(sceneId), entity(entity), label(std::move(label)), producer(std::move(producer)) {}

void editor::TerrainMapDeltaCmd::persist(const MapDelta& delta) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return;
    TerrainComponent* component = sceneProject->scene->findComponent<TerrainComponent>(entity);
    if (!component) return;
    const TerrainMapRef ref = refOf(delta.map);
    Texture* texture = TerrainMapUtils::findTexture(*component, ref);
    if (!texture || !TerrainMapUtils::hasLoadedData(*texture)) return;
    TextureData& data = texture->getData();
    const int bytesPerChannel = TextureData::getBytesPerChannel(data.getColorFormat());
    const size_t size = static_cast<size_t>(data.getWidth()) * static_cast<size_t>(data.getHeight()) *
                        static_cast<size_t>(data.getChannels()) * static_cast<size_t>(bytesPerChannel);
    const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
    if (!pixels || data.getSize() < size) return;
    TerrainMapUtils::writeFile(project, texture->getPath(0), data.getWidth(), data.getHeight(), data.getChannels(), bytesPerChannel,
                               std::vector<unsigned char>(pixels, pixels + size));
    TerrainMapUtils::refresh(sceneProject, entity, ref);
    TerrainMapUtils::refreshSharing(project, sceneId, entity, ref);
}

void editor::TerrainMapDeltaCmd::applyDeltas(bool restoreModified) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return;
    TerrainComponent* component = sceneProject->scene->findComponent<TerrainComponent>(entity);
    if (!component) return;
    TerrainComponentStore store(sceneProject->scene, entity, *component);
    std::vector<uint8_t> bytes;
    for (const MapDelta& delta : deltas) {
        // R25's guard: a delta applies only to the shape it was cut from
        const terrain::MapFormat format = store.format(delta.map);
        if (!format.exists || !format.bounds.contains(delta.rect)) continue;
        const size_t expected = static_cast<size_t>(delta.rect.area()) * static_cast<size_t>(format.channels) * static_cast<size_t>(format.bitsPerChannel / 8);
        if (expected != delta.xorBytes.size()) continue;
        bytes.resize(expected);
        if (!store.readBytes(delta.map, delta.rect, bytes)) continue;
        for (size_t i = 0; i < expected; i++) bytes[i] ^= delta.xorBytes[i];
        store.writeBytes(delta.map, delta.rect, bytes);
        persist(delta);
    }
    sceneProject->isModified = restoreModified ? wasModified : true;
}

bool editor::TerrainMapDeltaCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return false;
    if (producer) {
        wasModified = sceneProject->isModified;
        const Producer run = std::move(producer);
        producer = nullptr;
        if (!run(deltas)) return false;
        for (const MapDelta& delta : deltas) persist(delta);
        sceneProject->isModified = true;
        return true;
    }
    applyDeltas(false);
    return true;
}

void editor::TerrainMapDeltaCmd::undo() {
    applyDeltas(true);
}

std::vector<editor::Command::Touched> editor::TerrainMapDeltaCmd::touchedPaths(bool) const {
    std::vector<Touched> touched;
    SceneProject* sceneProject = project->getScene(sceneId);
    TerrainComponent* component = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    if (!component) return touched;
    for (const MapDelta& delta : deltas) {
        Texture* texture = TerrainMapUtils::findTexture(*component, refOf(delta.map));
        if (texture && !texture->getPath(0).empty()) touched.push_back({project->resolveAssetPath(texture->getPath(0)), false});
    }
    return touched;
}

size_t editor::TerrainMapDeltaCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + label.size();
    for (const MapDelta& delta : deltas) bytes += sizeof(MapDelta) + delta.xorBytes.capacity();
    return bytes;
}
