// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Textures (S05 R11): the pool entries reading a changed image decode again on a worker with
// their own settings (faces, SVG scale, alpha border) and are swapped in: every Texture of the
// id takes the new pixels through its generation (R13), the old GPU texture stays bound until
// then, and on a failure it stays.

#include "ReloadHandlers.h"

#include "Project.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureCookQueue.h"
#include "util/Util.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadTextures(Context& ctx, const std::vector<fs::path>& files) {
    Project* project = &ctx.project;
    for (const fs::path& path : files) {
        const std::string relative = rel(ctx.project, path);
        // its cooks are stale: requeued, and the reload below goes raw until they land (S14 R8)
        if (TextureCookQueue::get().getProject() == project) {
            const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
            TextureCookQueue::get().sourceChanged(assetPath);
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
        if (sources.empty()) {
            // nothing loaded reads it (yet): the next load takes the file as it is
            if (lut) ctx.service.recordDone("texture", path, 0.0);
            continue;
        }
        ctx.result.texturesReloaded++;

        watch::PathStat stat;
        const uint64_t estimate = watch::statPath(path, stat, false) ? stat.size * 4 : 0;
        ctx.service.startJob("texture", path, [sources, project]() -> HotReloadService::AdoptFn {
            std::vector<std::pair<std::string, std::shared_ptr<std::array<TextureData, 6>>>> decoded;
            for (const auto& [id, source] : sources) {
                decoded.emplace_back(id, std::make_shared<std::array<TextureData, 6>>(TextureDataPool::decode(id, source)));
            }
            return [decoded, project](std::string&) {
                for (const auto& [id, data] : decoded) {
                    TextureDataPool::replace(id, data);
                    // stale, rebuilt in place by the next getRender() with the new pixels
                    TexturePool::invalidate(id);
                }
                for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) sceneProject->needUpdateRender = true;
                return true;
            };
        }, estimate);
    }
}
