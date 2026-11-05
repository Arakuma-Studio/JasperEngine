// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Models (S05 R11): parsed again on a worker. When the node hierarchy is the loaded one, the
// ModelComponents using the file rebuild their meshes, skins, morphs and animations on the
// GameObjects they have (node transforms, added components and submesh overrides kept). A
// hierarchy that changed is not rebuilt (it would drop the edits on its GameObjects): a
// warning and a problem say "Reimport", which sync_files {rebuild_models: true} does.

#include "ReloadHandlers.h"

#include "Project.h"
#include "component/ModelComponent.h"
#include "subsystem/MeshSystem.h"
#include "model/ModelCookQueue.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadModels(Context& ctx, const std::vector<fs::path>& files) {
    Project* project = &ctx.project;
    const bool rebuild = ctx.options.rebuildModels;
    for (const fs::path& path : files) {
        // its cooks are stale: requeued, and the reload below reads the source until they land
        // (S53 R13; a reload of what was cooked keeps the cook)
        if (ModelCookQueue::get().getProject() == project && project->isInsideAssetsPath(path)) {
            ModelCookQueue::get().sourceChanged(project->normalizeToAssetsRelative(path).generic_string());
        }
        // the file as its components name it (assets-relative)
        std::string filename;
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            Scene* scene = sceneProject->scene;
            if (!scene || !scene->isComponentRegistered<ModelComponent>()) continue;
            auto models = scene->getComponentArray<ModelComponent>();
            for (size_t i = 0; i < models->size() && filename.empty(); i++) {
                const ModelComponent& model = models->getComponentFromIndex(i);
                if (!model.filename.empty() && project->resolveAssetPath(model.filename).lexically_normal() == path) filename = model.filename;
            }
        }
        if (filename.empty()) continue;   // no GameObject uses it

        ctx.service.startJob("model", path, [filename, project, rebuild]() -> HotReloadService::AdoptFn {
            std::shared_ptr<MeshSystem::ModelReload> parsed = MeshSystem::parseModelForReload(filename);
            return [parsed, filename, project, rebuild](std::string& note) {
                std::string why;
                const bool sameHierarchy = MeshSystem::modelHierarchyMatches(*parsed, why);
                if (!sameHierarchy && !rebuild) {
                    note = "its hierarchy changed (" + why + "): Reimport it to rebuild its GameObjects (sync_files rebuild_models)";
                    return true;
                }
                MeshSystem::adoptModelReload(parsed);
                for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
                    if (!sceneProject->scene) continue;
                    std::shared_ptr<MeshSystem> meshes = sceneProject->scene->getSystem<MeshSystem>();
                    if (!meshes) continue;
                    const bool used = sameHierarchy ? meshes->reloadModelFile(filename) : meshes->rebuildModelFile(filename);
                    if (used) {
                        sceneProject->needUpdateRender = true;
                        if (!sameHierarchy) sceneProject->isModified = true;
                    }
                }
                return true;
            };
        });
    }
}
