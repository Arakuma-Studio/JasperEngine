// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ReplacePrefabCmd.h"

#include "Out.h"

#include <algorithm>

using namespace jasperengine;

editor::ReplacePrefabCmd::ReplacePrefabCmd(Project* project, uint32_t sceneId, std::vector<Entity> roots,
        const std::filesystem::path& newPath, const PrefabReplaceSettings& settings){
    this->project = project;
    this->sceneId = sceneId;
    this->roots = std::move(roots);
    this->newPath = newPath;
    this->settings = settings;

    SceneProject* sceneProject = project->getScene(sceneId);
    this->wasModified = sceneProject && sceneProject->isModified;
}

std::vector<Entity> editor::ReplacePrefabCmd::replaceableRoots(const Project* project, uint32_t sceneId, const std::vector<Entity>& targets){
    std::vector<Entity> result;
    for (Entity entity : targets) {
        if (project->canReplacePrefab(sceneId, entity) && std::find(result.begin(), result.end(), entity) == result.end()) {
            result.push_back(entity);
        }
    }
    return result;
}

bool editor::ReplacePrefabCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || project->isAnyScenePlaying()) {
        return false;
    }

    if (!prepared) {
        // The instances as they are and what they become, worked out once: a redo replays it
        for (Entity root : roots) {
            if (!project->canReplacePrefab(sceneId, root)) {
                continue;
            }
            Entry entry;
            entry.root = root;
            if (!project->capturePrefabInstanceState(sceneId, root, entry.oldNode, entry.oldMemberIds)) {
                continue;
            }
            PrefabReplaceReport report;
            if (!project->buildReplacedPrefabNode(sceneId, root, entry.oldNode, entry.oldMemberIds, newPath, settings,
                    entry.newNode, entry.newMemberIds, &report)) {
                continue;
            }
            const std::string name = sceneProject->scene->getEntityName(root);
            for (const std::string& dropped : report.dropped) {
                Out::warning("Replace Prefab on '%s': dropped %s", name.c_str(), dropped.c_str());
            }
            Out::info("Replace Prefab: '%s' is now an instance of '%s' (%zu override%s kept, %zu dropped)",
                name.c_str(), newPath.generic_string().c_str(), report.kept, report.kept == 1 ? "" : "s", report.dropped.size());
            entries.push_back(std::move(entry));
        }
        prepared = true;
    }
    if (entries.empty()) {
        return false;
    }

    for (const Entry& entry : entries) {
        project->rebuildPrefabInstance(sceneId, entry.root, entry.newNode, entry.newMemberIds);
    }
    sceneProject->isModified = true;
    return true;
}

void editor::ReplacePrefabCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        project->rebuildPrefabInstance(sceneId, it->root, it->oldNode, it->oldMemberIds);
    }
    sceneProject->isModified = wasModified;
}

bool editor::ReplacePrefabCmd::mergeWith(editor::Command* otherCommand){
    return false;
}

// ---- Unpack of a Missing Prefab ----

editor::UnpackMissingPrefabCmd::UnpackMissingPrefabCmd(Project* project, uint32_t sceneId, std::vector<Entity> roots){
    this->project = project;
    this->sceneId = sceneId;
    this->roots = std::move(roots);

    SceneProject* sceneProject = project->getScene(sceneId);
    this->wasModified = sceneProject && sceneProject->isModified;
}

bool editor::UnpackMissingPrefabCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || project->isAnyScenePlaying()) {
        return false;
    }
    Scene* scene = sceneProject->scene;

    entries.clear();
    for (Entity root : roots) {
        const MissingPrefabInstance* missing = project->getMissingPrefab(sceneId, root);
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(root);
        if (!missing || !component) {
            continue;
        }
        Entry entry;
        entry.root = root;
        entry.component = *component;
        entry.data = YAML::Clone(missing->data);
        entry.memberIds = missing->memberIds;
        entries.push_back(std::move(entry));

        project->clearMissingPrefab(sceneId, root);
        scene->removeComponent<PrefabComponent>(root);
        Out::info("Unpacked Missing Prefab '%s' (%s): it is a plain GameObject now", scene->getEntityName(root).c_str(),
            entries.back().component.path.c_str());
    }
    if (entries.empty()) {
        return false;
    }
    project->refreshScenePrefabList(sceneId);
    sceneProject->structureVersion++;
    sceneProject->needUpdateRender = true;
    sceneProject->isModified = true;
    return true;
}

void editor::UnpackMissingPrefabCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    Scene* scene = sceneProject->scene;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!scene->isEntityCreated(it->root)) {
            continue;
        }
        if (!scene->findComponent<PrefabComponent>(it->root)) {
            scene->addComponent<PrefabComponent>(it->root, it->component);
        }
        project->setMissingPrefab(sceneProject, it->root, it->data, &it->memberIds);
    }
    project->refreshScenePrefabList(sceneId);
    sceneProject->structureVersion++;
    sceneProject->needUpdateRender = true;
    sceneProject->isModified = wasModified;
}

bool editor::UnpackMissingPrefabCmd::mergeWith(editor::Command* otherCommand){
    return false;
}
