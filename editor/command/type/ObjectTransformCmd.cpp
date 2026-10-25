// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ObjectTransformCmd.h"
#include "animation/ClipRecordHooks.h"
#include "pose/PoseInspectorHooks.h"

#include <algorithm>
#include <cmath>
#include <map>

using namespace jasperengine;

namespace {
    // the command's value is the one the entity showed (a gizmo matrix decomposed again)
    bool sameShown(const Vector3& a, const Vector3& b) {
        for (unsigned i = 0; i < 3; i++) {
            const float scale = std::max(1.0f, std::fabs(b[i]));
            if (std::fabs(a[i] - b[i]) > 1e-4f * scale) return false;
        }
        return true;
    }

    bool sameShown(const Quaternion& a, const Quaternion& b) {
        return std::fabs(a.dot(b)) >= 1.0f - 1e-6f;
    }
}

editor::ObjectTransformCmd::ObjectTransformCmd(Project* project, size_t sceneId, Entity entity, Matrix4 localMatrix){
    this->project = project;
    this->sceneId = sceneId;
    
    TransformCmdValue& value = props[entity];
    localMatrix.decomposeStandard(value.newPosition, value.newScale, value.newRotation);
    // the matrix origin of a pivoted UI element is not its position
    if (RectTransformComponent* layout = project->getScene(sceneId)->scene->findComponent<RectTransformComponent>(entity)){
        value.newPosition -= getUIPivotShift(*layout, value.newRotation, value.newScale);
    }

    this->wasModified = project->getScene(sceneId)->isModified;
}

editor::ObjectTransformCmd::ObjectTransformCmd(Project* project, size_t sceneId, Entity entity, Vector3 position, Quaternion rotation, Vector3 scale){
    this->project = project;
    this->sceneId = sceneId;

    props[entity].newPosition = position;
    props[entity].newRotation = rotation;
    props[entity].newScale = scale;

    this->wasModified = project->getScene(sceneId)->isModified;
}

editor::ObjectTransformCmd::ObjectTransformCmd(Project* project, size_t sceneId){
    this->project = project;
    this->sceneId = sceneId;

    this->wasModified = project->getScene(sceneId) ? project->getScene(sceneId)->isModified : false;
}

void editor::ObjectTransformCmd::addEntity(Entity entity, Vector3 position, Quaternion rotation, Vector3 scale){
    TransformCmdValue& value = props[entity];
    value.newPosition = position;
    value.newRotation = rotation;
    value.newScale = scale;
    value.explicitOld = false;
}

void editor::ObjectTransformCmd::addEntity(Entity entity, Vector3 position, Quaternion rotation, Vector3 scale,
                                           Vector3 oldPosition, Quaternion oldRotation, Vector3 oldScale){
    TransformCmdValue& value = props[entity];
    value.newPosition = position;
    value.newRotation = rotation;
    value.newScale = scale;
    value.oldPosition = oldPosition;
    value.oldRotation = oldRotation;
    value.oldScale = oldScale;
    value.explicitOld = true;
}

bool editor::ObjectTransformCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return false;
    }
    // a posed bone: the old values recorded are its rest, and the edit applies to the rest
    if (poseInspectorHooks().beforeTransformWrite){
        for (const auto& entry : props) poseInspectorHooks().beforeTransformWrite(static_cast<uint32_t>(sceneId), entry.first);
    }
    // what the entities show now: a clip preview may be showing other values than their rest
    std::map<Entity, Transform> shown;
    for (const auto& entry : props){
        if (const Transform* transform = sceneProject->scene->findComponent<Transform>(entry.first)) shown[entry.first] = *transform;
    }
    // clip previews put a previewed Transform back to its rest (animation clips 9.R6)
    for (const auto& entry : props) ClipRecordHooks::beforeComponentWrite(static_cast<uint32_t>(sceneId), entry.first, ComponentType::Transform, "");
    for (auto& [entity, property] : props){
        if (Transform* transform = sceneProject->scene->findComponent<Transform>(entity)){
            // A channel the preview showed (now back at its rest) that the command leaves as shown
            // (a gizmo move carries the shown rotation and scale) is unchanged: it keeps its rest,
            // so neither the scene nor a later redo gets the previewed value
            auto it = shown.find(entity);
            if (it != shown.end()){
                const Transform& before = it->second;
                if (transform->position != before.position && sameShown(property.newPosition, before.position)) property.newPosition = transform->position;
                if (transform->rotation != before.rotation && sameShown(property.newRotation, before.rotation)) property.newRotation = transform->rotation;
                if (transform->scale != before.scale && sameShown(property.newScale, before.scale)) property.newScale = transform->scale;
            }
            if (!property.explicitOld){
                property.oldPosition = transform->position;
                property.oldRotation = transform->rotation;
                property.oldScale = transform->scale;
            }

            prefabUndo.capture(project, sceneId, entity);

            transform->position = property.newPosition;
            transform->rotation = property.newRotation;
            transform->scale = property.newScale;

            transform->needUpdate = true;

            // Unity: what moved on a prefab instance's GameObject is an override of it
            std::vector<std::string> changed;
            if (property.newPosition != property.oldPosition) changed.push_back("position");
            if (property.newRotation != property.oldRotation) changed.push_back("rotation");
            if (property.newScale != property.oldScale) changed.push_back("scale");
            if (!changed.empty()){
                project->recordPrefabEdit(sceneId, entity, ComponentType::Transform, changed);
            }
        }
    }

    sceneProject->isModified = true;

    for (const auto& entry : props) ClipRecordHooks::afterComponentWrite(static_cast<uint32_t>(sceneId), entry.first, ComponentType::Transform, "");

    return true;
}

void editor::ObjectTransformCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return;
    }
    for (const auto& entry : props) ClipRecordHooks::beforeComponentWrite(static_cast<uint32_t>(sceneId), entry.first, ComponentType::Transform, "");
    for (auto const& [entity, property] : props){
        if (Transform* transform = sceneProject->scene->findComponent<Transform>(entity)){
            transform->position = property.oldPosition;
            transform->rotation = property.oldRotation;
            transform->scale = property.oldScale;

            transform->needUpdate = true;
        }
    }
    prefabUndo.restore(project, sceneId);

    sceneProject->isModified = wasModified;

    for (const auto& entry : props) ClipRecordHooks::afterComponentWrite(static_cast<uint32_t>(sceneId), entry.first, ComponentType::Transform, "");
}

bool editor::ObjectTransformCmd::mergeWith(editor::Command* otherCommand){
    ObjectTransformCmd* otherCmd = dynamic_cast<ObjectTransformCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (sceneId == otherCmd->sceneId){

            for (auto const& [otherEntity, otherProperty] : otherCmd->props){
                if (props.find(otherEntity) != props.end()) {
                    props[otherEntity].oldPosition = otherProperty.oldPosition;
                    props[otherEntity].oldRotation = otherProperty.oldRotation;
                    props[otherEntity].oldScale = otherProperty.oldScale;
                }else{
                    props[otherEntity] = otherProperty;
                }
            }

            prefabUndo.mergeOlder(otherCmd->prefabUndo);
            wasModified = wasModified && otherCmd->wasModified;

            return true;
        }
    }

    return false;
}
