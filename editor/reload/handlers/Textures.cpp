// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Textures (S05 R11): the pool entries reading a changed image decode again on a worker with
// their own settings (faces, SVG scale, alpha border) and are swapped in: every Texture of the
// id takes the new pixels through its generation (R13), the old GPU texture stays bound until
// then, and on a failure it stays.

#include "sprite/SpriteAtlasBuilder.h"
#include "sprite/SpriteSheetBuilder.h"
#include "ReloadHandlers.h"

#include "Project.h"
#include "component/TerrainComponent.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureData.h"
#include "util/Util.h"

#include <stdexcept>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {
    // The terrains whose painted layers read the file: their array is made from the files, not
    // through the pools, so no pool entry of it is there to swap (RenderSystem::getTerrainDetailArray).
    // invalidate: their arrays are built again.
    bool terrainLayersRead(Project& project, const std::string& relative, bool invalidate) {
        bool found = false;
        for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            auto terrains = sceneProject->scene->getComponentArray<TerrainComponent>();
            for (size_t i = 0; i < terrains->size(); i++) {
                bool reads = false;
                for (TerrainSurfaceLayer& layer : terrains->getComponentFromIndex(i).surfaceLayers) {
                    forEachTerrainLayerTexture(layer, [&](Texture& texture) {
                        reads = reads || (!texture.empty() && reload::rel(project, project.resolveAssetPath(texture.getPath())) == relative);
                    });
                }
                if (!reads) continue;
                found = true;
                if (!invalidate) continue;
                if (std::shared_ptr<RenderSystem> render = sceneProject->scene->getSystem<RenderSystem>()) {
                    render->invalidateTerrainDetailArray(terrains->getEntity(i));
                }
                sceneProject->needUpdateRender = true;
            }
        }
        return found;
    }
}

void reload::reloadTextures(Context& ctx, const std::vector<fs::path>& files) {
    Project* project = &ctx.project;
    for (const fs::path& path : files) {
        const std::string relative = rel(ctx.project, path);
        // its cooks are stale: requeued, and the reload below goes raw until they land (S14 R8)
        if (TextureCookQueue::get().getProject() == project) {
            const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
            TextureCookQueue::get().sourceChanged(assetPath);
        }
        // S45: its texture sheet and the Sprite Atlases packing it, loaded or not
        if (sprite::SpriteAtlasBuilder::get().getProject() == project) {
            const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
            sprite::SpriteSheetBuilder::get().invalidate(assetPath);
            sprite::SpriteAtlasBuilder::get().memberChanged(assetPath);
        }
        const std::vector<std::string> ids = TextureDataPool::findIdsUsingFile(watch::pathToUtf8(path));

        // a scene's color grading LUT: the renderer keeps its strip by path
        bool lut = false;
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            const std::string& lutTexture = sceneProject->scene->getPostProcessing().lutTexture;
            if (lutTexture.empty() || rel(ctx.project, project->resolveAssetPath(lutTexture)) != relative) continue;
            if (std::shared_ptr<RenderSystem> render = sceneProject->scene->getSystem<RenderSystem>()) render->invalidatePostProcessTextures();
            sceneProject->needUpdateRender = true;
            lut = true;
        }

        std::vector<std::pair<std::string, TextureSource>> sources;
        for (const std::string& id : ids) {
            TextureSource source;
            if (TextureDataPool::getSource(id, source)) sources.emplace_back(id, source);
        }
        const bool terrainLayers = terrainLayersRead(ctx.project, relative, false);
        if (sources.empty() && !terrainLayers) {
            // nothing loaded reads it (yet): the next load takes the file as it is
            if (lut) ctx.service.recordDone("texture", path, 0.0);
            continue;
        }
        ctx.result.texturesReloaded++;

        watch::PathStat stat;
        const uint64_t estimate = watch::statPath(path, stat, false) ? stat.size * 4 : 0;
        ctx.service.startJob("texture", path, [sources, project, relative, terrainLayers, path]() -> HotReloadService::AdoptFn {
            std::vector<std::pair<std::string, std::shared_ptr<std::array<TextureData, 6>>>> decoded;
            for (const auto& [id, source] : sources) {
                decoded.emplace_back(id, TextureDataPool::shareDecoded(TextureDataPool::decode(id, source)));
            }
            // the array reads the file when it is built again: a broken one leaves it as it is
            if (terrainLayers && sources.empty()) {
                TextureData check;
                if (!check.loadTextureFromFile(watch::pathToUtf8(path).c_str())) {
                    throw std::runtime_error("Texture " + relative + ": could not be read");
                }
                check.setDataOwned(true);
            }
            return [decoded, project, relative, terrainLayers](std::string&) {
                for (const auto& [id, data] : decoded) {
                    TextureDataPool::replace(id, data);
                    // stale, rebuilt in place by the next getRender() with the new pixels
                    TexturePool::invalidate(id);
                }
                if (terrainLayers) terrainLayersRead(*project, relative, true);
                for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) sceneProject->needUpdateRender = true;
                return true;
            };
        }, estimate);
    }
}
