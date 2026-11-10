// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "command/type/TreeEditCmd.h"

#include "Project.h"
#include "foliage/TreeDataFile.h"
#include "subsystem/FoliageSystem.h"

#include <algorithm>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::terraincore;

editor::TreeEditCmd::TreeEditCmd(Project* project, uint32_t sceneId, Entity terrain, std::string name, TreeDelta delta, uint64_t strokeId):
    project(project), sceneId(sceneId), terrain(terrain), name(std::move(name)), delta(std::move(delta)), strokeId(strokeId) {}

void editor::TreeEditCmd::apply(bool undo) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return;
    FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();
    std::shared_ptr<TreeStore> store = foliage->getOrCreateTreeStore(terrain);
    if (!store) return;
    store->applyDelta(delta, undo);
    TreeDelta touched = delta;
    foliage->treesChanged(terrain, touched);
    TreeDataFile::markDirty(project, sceneId, terrain);
    if (alongside) alongside(undo);
}

bool editor::TreeEditCmd::execute() {
    // the first time, a stroke's stamp has already gone into the store to answer its spacing
    if (firstExecute) {
        firstExecute = false;
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject) return false;
        FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();
        std::shared_ptr<TreeStore> store = foliage->getOrCreateTreeStore(terrain);
        if (!store) return false;
        for (const TreeRecord& record : delta.added) {
            if (!store->find(record.id)) {
                apply(false);
                return true;
            }
        }
        for (const TreeRecord& record : delta.removed) {
            if (store->find(record.id)) {
                apply(false);
                return true;
            }
        }
        foliage->treesChanged(terrain, delta);
        TreeDataFile::markDirty(project, sceneId, terrain);
        if (alongside) alongside(false);
        return true;
    }
    apply(false);
    return true;
}

void editor::TreeEditCmd::undo() {
    apply(true);
}

bool editor::TreeEditCmd::mergeWith(Command* other) {
    TreeEditCmd* earlier = dynamic_cast<TreeEditCmd*>(other);
    if (!earlier || strokeId == 0 || earlier->strokeId != strokeId || earlier->terrain != terrain) return false;

    // what this step removed of what the earlier added was never there; what it re-adds stays changed
    std::unordered_set<uint32_t> removedHere;
    for (const TreeRecord& record : delta.removed) removedHere.insert(record.id);
    std::unordered_set<uint32_t> addedEarlier;
    for (const TreeRecord& record : earlier->delta.added) addedEarlier.insert(record.id);

    TreeDelta merged;
    merged.nextIdBefore = earlier->delta.nextIdBefore;
    merged.nextIdAfter = delta.nextIdAfter;
    merged.removed = earlier->delta.removed;
    for (const TreeRecord& record : delta.removed) {
        if (addedEarlier.count(record.id) == 0) merged.removed.push_back(record);
    }
    for (const TreeRecord& record : earlier->delta.added) {
        if (removedHere.count(record.id) == 0) merged.added.push_back(record);
    }
    merged.added.insert(merged.added.end(), delta.added.begin(), delta.added.end());
    delta = std::move(merged);
    return true;
}
