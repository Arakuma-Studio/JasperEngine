// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RagdollWizard.h"
#include "component/Transform.h"

#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/RemoveComponentCmd.h"

#include <unordered_set>

using namespace jasperengine;

namespace {

    // every top-level field of a component value as a property command (the list fields whole)
    void writeComponent(editor::MultiPropertyCmd& multi, editor::Project* project, uint32_t sceneId, Entity entity,
                        editor::ComponentType type, void* value){
        for (const auto& [name, property] : editor::Catalog::getProperties(type, value)){
            if (name.find('[') != std::string::npos || name.find('.') != std::string::npos || !property.ref){
                continue;
            }
            switch (property.type){
                case editor::PropertyType::Bool: multi.addPropertyCmd<bool>(project, sceneId, entity, type, name, *static_cast<bool*>(property.ref)); break;
                case editor::PropertyType::Float: multi.addPropertyCmd<float>(project, sceneId, entity, type, name, *static_cast<float*>(property.ref)); break;
                case editor::PropertyType::Int:
                case editor::PropertyType::Enum: multi.addPropertyCmd<int>(project, sceneId, entity, type, name, *static_cast<int*>(property.ref)); break;
                case editor::PropertyType::UInt: multi.addPropertyCmd<unsigned int>(project, sceneId, entity, type, name, *static_cast<unsigned int*>(property.ref)); break;
                case editor::PropertyType::String: multi.addPropertyCmd<std::string>(project, sceneId, entity, type, name, *static_cast<std::string*>(property.ref)); break;
                case editor::PropertyType::Vector3: multi.addPropertyCmd<Vector3>(project, sceneId, entity, type, name, *static_cast<Vector3*>(property.ref)); break;
                case editor::PropertyType::Quat: multi.addPropertyCmd<Quaternion>(project, sceneId, entity, type, name, *static_cast<Quaternion*>(property.ref)); break;
                case editor::PropertyType::Entity: multi.addPropertyCmd<Entity>(project, sceneId, entity, type, name, *static_cast<Entity*>(property.ref)); break;
                case editor::PropertyType::Custom:
                    if (type == editor::ComponentType::RigidbodyComponent && name == "colliders"){
                        multi.addPropertyCmd<std::vector<Collider3D>>(project, sceneId, entity, type, name, *static_cast<std::vector<Collider3D>*>(property.ref));
                    }else if (type == editor::ComponentType::Joint3DComponent && name == "pathPoints"){
                        multi.addPropertyCmd<std::vector<Vector3>>(project, sceneId, entity, type, name, *static_cast<std::vector<Vector3>*>(property.ref));
                    }
                    break;
                default:
                    break;
            }
        }
    }

}

bool editor::applyRagdollWizard(Project* project, uint32_t sceneId, Entity root, const RagdollBones& bones,
                                const RagdollBuildSettings& settings, RagdollBuildResult& result, std::string& error){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(root)){
        error = "no such scene or GameObject";
        return false;
    }
    Scene* scene = sceneProject->scene;
    result = RagdollBuilder::build(scene, root, bones, settings);
    if (!result.ok){
        error = result.error;
        return false;
    }

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t start = history->mark();
    auto add = [&](Command* command){
        command->setNoMerge();
        return history->addCommand(command);
    };
    auto rollback = [&](){
        while (history->position() > start && history->canUndo() && history->undo()) {}
    };

    // what a ragdoll on this root made before: the bodies and joints below it
    std::unordered_set<Entity> previous;
    if (scene->findComponent<RagdollComponent>(root)){
        auto transforms = scene->getComponentArray<Transform>();
        if (transforms->hasEntity(root)){
            const size_t first = transforms->getIndex(root);
            const size_t last = scene->findBranchLastIndex(root);
            for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
                const Entity entity = transforms->getEntity(i);
                if (scene->findComponent<RigidbodyComponent>(entity)) previous.insert(entity);
            }
        }
    }
    for (const RagdollBuiltPart& part : result.parts){
        previous.insert(part.entity);
    }
    for (Entity entity : previous){
        if (scene->findComponent<Joint3DComponent>(entity) && !add(new RemoveComponentCmd(project, sceneId, entity, ComponentType::Joint3DComponent))){
            rollback();
            error = "could not remove the Joint of " + scene->getEntityName(entity);
            return false;
        }
        if (scene->findComponent<RigidbodyComponent>(entity) && !add(new RemoveComponentCmd(project, sceneId, entity, ComponentType::RigidbodyComponent))){
            rollback();
            error = "could not remove the Rigidbody of " + scene->getEntityName(entity);
            return false;
        }
    }

    for (RagdollBuiltPart& part : result.parts){
        if (!add(new AddComponentCmd(project, sceneId, part.entity, ComponentType::RigidbodyComponent))){
            rollback();
            error = "could not add a Rigidbody to " + scene->getEntityName(part.entity);
            return false;
        }
        MultiPropertyCmd* values = new MultiPropertyCmd();
        writeComponent(*values, project, sceneId, part.entity, ComponentType::RigidbodyComponent, &part.body);
        add(values);
    }
    for (RagdollBuiltPart& part : result.parts){
        if (!part.hasJoint) continue;
        if (!add(new AddComponentCmd(project, sceneId, part.entity, ComponentType::Joint3DComponent))){
            rollback();
            error = "could not add a Joint to " + scene->getEntityName(part.entity);
            return false;
        }
        MultiPropertyCmd* values = new MultiPropertyCmd();
        writeComponent(*values, project, sceneId, part.entity, ComponentType::Joint3DComponent, &part.joint);
        add(values);
    }
    if (!scene->findComponent<RagdollComponent>(root) && !add(new AddComponentCmd(project, sceneId, root, ComponentType::RagdollComponent))){
        rollback();
        error = "could not add the Ragdoll to " + scene->getEntityName(root);
        return false;
    }
    MultiPropertyCmd* ragdollValues = new MultiPropertyCmd();
    writeComponent(*ragdollValues, project, sceneId, root, ComponentType::RagdollComponent, &result.ragdoll);
    add(ragdollValues);

    history->groupSince(start);
    return true;
}
