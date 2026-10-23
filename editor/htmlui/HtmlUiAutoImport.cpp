// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUiAutoImport.h"

#include "HtmlUiImportMaintenance.h"
#include "ImportManifestWriter.h"

#include "AppSettings.h"
#include "Out.h"
#include "Project.h"
#include "component/UIDocumentComponent.h"
#include "subsystem/UIDocumentSystem.h"

#include "htmlui/compat/ImportManifest.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using jasperengine::htmlui::compat::ImportManifest;

HtmlUiAutoImport& HtmlUiAutoImport::get(){
    // never destroyed: shutdown() stops the worker while the editor still runs
    static HtmlUiAutoImport* instance = new HtmlUiAutoImport();
    return *instance;
}

ImportTarget HtmlUiAutoImport::targetFor(const Project* project){
    ImportTarget target;
    if (!project)
        return target;
    std::error_code ec;
    target.projectRoot = std::filesystem::absolute(project->getProjectPath(), ec).lexically_normal();
    target.assetsDir = std::filesystem::absolute(project->getAssetsPath(), ec).lexically_normal();
    if (!target.assetsDir.empty() && target.assetsDir.filename().empty())
        target.assetsDir = target.assetsDir.parent_path();
    return target;
}

void HtmlUiAutoImport::shutdown(){
    HttpClient::cancelAll();
    importQueue.stop();
}

size_t HtmlUiAutoImport::queueMissing(const ImportTarget& target, const std::string& documentPath, bool manual){
    size_t queued = 0;
    for (const RemoteResourceState& state : remoteResourceStates(target, documentPath)){
        if (state.imported)
            continue;
        ImportJob job;
        job.target = target;
        job.url = state.reference.url;
        job.kind = state.reference.kind;
        job.location = state.reference.location;
        job.documents = {documentPath};
        if (importQueue.enqueue(std::move(job), manual))
            queued++;
    }
    return queued;
}

size_t HtmlUiAutoImport::importDocument(Project* project, const std::string& documentPath, bool reimportAll){
    const ImportTarget target = targetFor(project);
    if (target.assetsDir.empty() || documentPath.empty())
        return 0;
    if (!reimportAll)
        return queueMissing(target, documentPath, true);

    size_t queued = 0;
    for (const DiscoveredReference& reference : discoverRemoteReferences(target, documentPath)){
        ImportJob job;
        job.target = target;
        job.url = reference.url;
        job.kind = reference.kind;
        job.location = reference.location;
        job.documents = {documentPath};
        if (importQueue.enqueue(std::move(job), true))
            queued++;
    }
    return queued;
}

bool HtmlUiAutoImport::update(Project* project){
    if (!project)
        return false;

    if (project->getProjectPath() != projectPath){
        projectPath = project->getProjectPath();
        seen.clear();
        importQueue.clearSession();
    }

    bool reloaded = false;
    ImportQueue::Batch batch;
    if (importQueue.takeFinishedBatch(batch)){
        applyBatch(project, batch);
        reloaded = true;
    }

    const bool automatic = AppSettings::getHtmlUiAutoImport();
    const ImportTarget target = targetFor(project);
    for (SceneProject* sceneProject : project->getHtmlUIScenes()){
        Scene* scene = sceneProject->scene;
        if (!scene || !scene->isComponentRegistered<UIDocumentComponent>())
            continue;
        UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
        if (!system)
            continue;
        auto components = scene->getComponentArray<UIDocumentComponent>();
        for (size_t i = 0; i < components->size(); i++){
            const Entity entity = components->getEntity(i);
            const std::string& filename = components->getComponentFromIndex(i).filename;
            if (filename.empty())
                continue;
            const uint32_t loads = system->getLoadCount(entity);
            auto& last = seen[{sceneProject->id, entity}];
            if (last.first == filename && last.second == loads)
                continue;
            last = {filename, loads};
            // a document that (re)loaded: what it references and the manifest lacks
            if (automatic && loads > 0 && !target.assetsDir.empty())
                queueMissing(target, filename, false);
        }
    }
    return reloaded;
}

void HtmlUiAutoImport::applyBatch(Project* project, const ImportQueue::Batch& batch){
    std::vector<std::string> files = {ImportManifest::kPath};
    size_t imported = 0;
    for (const ImportedResource& result : batch.results){
        if (!result.ok)
            continue;
        imported++;
        files.insert(files.end(), result.written.begin(), result.written.end());
    }
    if (imported == 0)
        return;

    // the runtime resolves through the new manifest (the Project window and the asset
    // database get the new files from the file watcher: they are the editor's writes)
    ImportManifest::reload();
    Out::info("[HTML UI] imported %zu remote resource%s; reloading the documents that use them", imported, imported == 1 ? "" : "s");

    for (SceneProject* sceneProject : project->getHtmlUIScenes()){
        Scene* scene = sceneProject->scene;
        if (!scene || !scene->isComponentRegistered<UIDocumentComponent>())
            continue;
        UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
        if (!system)
            continue;

        std::set<Entity> done;
        for (const UIDocumentSystem::Reloaded& entry : system->reloadChangedFiles(files))
            done.insert(entry.entity);

        // documents that asked for the imports but do not list the files yet (a remote URL is
        // no dependency until it resolves)
        auto components = scene->getComponentArray<UIDocumentComponent>();
        for (size_t i = 0; i < components->size(); i++){
            const Entity entity = components->getEntity(i);
            const std::string& filename = components->getComponentFromIndex(i).filename;
            if (done.count(entity) || filename.empty() ||
                std::find(batch.documents.begin(), batch.documents.end(), filename) == batch.documents.end())
                continue;
            system->reloadDocument(entity, true);
            done.insert(entity);
        }
        if (!done.empty())
            sceneProject->needUpdateRender = true;
    }
}
