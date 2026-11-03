// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SplineBakeObjects.h"

#include "Catalog.h"
#include "command/CommandHandle.h"
#include "command/CommandNames.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/ImportPrefabCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/PropertyCmd.h"
#include "subsystem/SplineSystem.h"

#include <filesystem>

using namespace jasperengine;

bool editor::SplineBakeObjects::bake(Project* project, uint32_t sceneId, Entity splineEntity, std::string& error){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->findComponent<SplineInstantiateComponent>(splineEntity)){
        error = "The GameObject has no Spline Instantiate.";
        return false;
    }
    Scene* scene = sceneProject->scene;
    std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>();
    if (!system){
        error = "The scene has no spline system.";
        return false;
    }
    system->rebuildNow(splineEntity);
    const std::vector<SplinePiecePlacement> pieces = system->getPiecePlacements(splineEntity);
    if (pieces.empty()){
        error = "There are no pieces to bake (give the pools a model or a prefab).";
        return false;
    }

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    const std::string owner = CommandNames::entity(project, sceneId, splineEntity);
    CreateEntityCmd* group = new CreateEntityCmd(project, sceneId, owner + " Pieces", EntityCreationType::EMPTY);
    history->addCommandNoMerge(group);
    const Entity parent = group->getEntity();
    for (const SplinePiecePlacement& piece : pieces){
        const std::string name = std::filesystem::path(piece.path).stem().string();
        if (piece.path.size() > 7 && piece.path.compare(piece.path.size() - 7, 7, ".prefab") == 0){
            ImportPrefabCmd* cmd = new ImportPrefabCmd(project, sceneId, project->getProjectPath() / piece.path, parent, true);
            cmd->setPlacement(piece.position, piece.rotation, piece.scale);
            cmd->setQuiet(true);
            history->addCommandNoMerge(cmd);
        }else{
            history->addCommandNoMerge(new ModelLoadCmd(project, sceneId, name.empty() ? "Piece" : name, parent, piece.position,
                                                        piece.rotation, piece.scale, piece.path));
        }
    }
    // the component's pieces are now the GameObjects: it stops placing its own
    auto* disable = new PropertyCmd<bool>(project, sceneId, splineEntity, ComponentType::SplineInstantiateComponent, "enabled", false);
    history->addCommandNoMerge(disable);
    history->groupSince(mark, "Bake Spline Pieces (" + owner + ")");
    return true;
}
