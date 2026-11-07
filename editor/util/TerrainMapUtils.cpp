// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMapUtils.h"
#include "object/TerrainEdit.h"
#include "component/TerrainComponent.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include "Out.h"
#include "util/TerrainMapFileWriter.h"

#include <cstring>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

Texture* editor::TerrainMapUtils::findTexture(TerrainComponent& terrain, const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::HeightMap){
        return &terrain.heightMap;
    }
    if (ref.target == TerrainMapTarget::BlendMap){
        if (ref.layer < 0 || ref.layer >= static_cast<int>(terrain.blendMaps.size())){
            return NULL;
        }
        return &terrain.blendMaps[ref.layer];
    }
    if (ref.layer < 0 || ref.layer >= static_cast<int>(terrain.foliageLayers.size())){
        return nullptr;
    }
    return &terrain.foliageLayers[ref.layer].densityMap;
}

std::string editor::TerrainMapUtils::getPropertyName(const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::HeightMap){
        return "heightMap";
    }
    if (ref.target == TerrainMapTarget::BlendMap){
        return "blendMaps[" + std::to_string(ref.layer) + "]";
    }
    return "foliageLayers[" + std::to_string(ref.layer) + "].densityMap";
}

std::string editor::TerrainMapUtils::getContainerPropertyName(const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::BlendMap){
        return "blendMaps";
    }
    if (ref.target == TerrainMapTarget::DensityMap){
        return "foliageLayers";
    }
    return std::string();
}

bool editor::TerrainMapUtils::hasLoadedData(Texture& texture){
    if (texture.empty() || texture.isFramebuffer()){
        return false;
    }

    texture.setReleaseDataAfterLoad(false);
    TextureLoadResult result = texture.load();
    return result.state == ResourceLoadState::Finished && result.data && texture.getData().getData();
}

bool editor::TerrainMapUtils::writeFile(Project* project, const std::string& relativePath, int width, int height, int channels, int bytesPerChannel, const std::vector<unsigned char>& pixels){
    const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
    if (!project || project->getProjectPath().empty() || relativePath.empty() || width <= 0 || height <= 0 || channels <= 0 || pixels.size() < expectedSize){
        return false;
    }

    fs::path outputPath = project->resolveAssetPath(relativePath);

    std::error_code ec;
    // (in a write scope, an MCP sculpt or its undo: beneath its root)
    FileUtils::createDirectories(outputPath.parent_path(), ec);
    if (ec){
        Out::warning("Failed to create terrain texture directory: %s", outputPath.parent_path().string().c_str());
        return false;
    }

    // Encode + write on the background worker: full-map PNG writes (especially
    // 16-bit heightmaps) are slow enough to hitch the editor on every stroke end.
    // The worker has no write scope of its own: the root of this thread's scope (an MCP call,
    // an MCP-made step) goes with the job, and the worker writes beneath it.
    TerrainMapFileWriter::get().enqueue(outputPath, width, height, channels, bytesPerChannel,
        std::vector<unsigned char>(pixels.begin(), pixels.begin() + expectedSize), safewrite::scopeRootFor(outputPath));
    return true;
}

void editor::TerrainMapUtils::refresh(SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, const TerrainMapRegion* region){
    if (!sceneProject){
        return;
    }
    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(entity);
    if (!terrain){
        return;
    }

    // T5 R25, R26: a known rectangle re-snaps or refills only the cells under it; without one
    // (a snapshot restored) every cell is rebuilt
    const bool regional = region && !region->empty();
    if (ref.target == TerrainMapTarget::HeightMap){
        terrain->heightMapLoaded = false;
        terrain->needUpdateTerrain = true;
        terrain->needUpdateTexture = true;
        // Instances sit on the surface, so a sculpt moves them.
        if (!regional) terrain->needUpdateFoliage = true;
    }else if (ref.target == TerrainMapTarget::BlendMap){
        terrain->needUpdateTexture = true;
    }else if (!regional){
        terrain->needUpdateFoliage = true;
    }
    if (regional){
        terrain::MapId map;
        map.kind = ref.target == TerrainMapTarget::HeightMap ? terrain::MapKind::Height
            : ref.target == TerrainMapTarget::BlendMap ? terrain::MapKind::Blend : terrain::MapKind::Density;
        map.index = ref.layer;
        TerrainEditEvents::publish(sceneProject->scene, entity, map,
            terrain::TexelRect{region->minX, region->minY, region->width(), region->height()});
    }

    if (Texture* texture = findTexture(*terrain, ref)){
        texture->invalidateRender();
    }
}

void editor::TerrainMapUtils::refreshSharing(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref){
    SceneProject* sceneProject = project->getScene(sceneId);
    TerrainComponent* terrain = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    Texture* texture = terrain ? findTexture(*terrain, ref) : nullptr;
    if (!texture || texture->getPath(0).empty()){
        return;
    }
    const std::string path = texture->getPath(0);
    for (SceneProject& other : project->getScenes()){
        if (!other.scene){
            continue;
        }
        auto terrains = other.scene->getComponentArray<TerrainComponent>();
        for (size_t i = 0; i < terrains->size(); i++){
            const Entity otherEntity = terrains->getEntity(i);
            if (other.id == sceneId && otherEntity == entity){
                continue;
            }
            Texture* otherTexture = findTexture(terrains->getComponentFromIndex(i), ref);
            if (otherTexture && otherTexture->getPath(0) == path){
                *otherTexture = *texture;
                refresh(&other, otherEntity, ref);
                other.needUpdateRender = true;
            }
        }
    }
}

std::vector<unsigned char> editor::TerrainMapUtils::copyRegion(const unsigned char* pixels, int mapWidth, int bytesPerTexel, const TerrainMapRegion& region){
    const size_t rowBytes = static_cast<size_t>(region.width()) * static_cast<size_t>(bytesPerTexel);
    std::vector<unsigned char> regionPixels(rowBytes * static_cast<size_t>(region.height()));

    for (int y = 0; y < region.height(); y++){
        const unsigned char* src = pixels + (static_cast<size_t>(region.minY + y) * mapWidth + region.minX) * bytesPerTexel;
        std::memcpy(regionPixels.data() + y * rowBytes, src, rowBytes);
    }

    return regionPixels;
}

bool editor::TerrainMapUtils::writeRegion(unsigned char* pixels, int mapWidth, int bytesPerTexel, const TerrainMapRegion& region, const std::vector<unsigned char>& regionPixels){
    const size_t rowBytes = static_cast<size_t>(region.width()) * static_cast<size_t>(bytesPerTexel);
    if (!pixels || regionPixels.size() < rowBytes * static_cast<size_t>(region.height())){
        return false;
    }

    for (int y = 0; y < region.height(); y++){
        unsigned char* dst = pixels + (static_cast<size_t>(region.minY + y) * mapWidth + region.minX) * bytesPerTexel;
        std::memcpy(dst, regionPixels.data() + y * rowBytes, rowBytes);
    }

    return true;
}
