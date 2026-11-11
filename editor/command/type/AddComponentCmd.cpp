// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AddComponentCmd.h"
#include "component/SplineComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/ActionComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "subsystem/TitleArtSystem.h"

#include <algorithm>

#include "Out.h"
#include "component/Light2DComponent.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"

using namespace jasperengine;

editor::AddComponentCmd::AddComponentCmd(Project* project, size_t sceneId, Entity entity, ComponentType componentType){
    this->project = project;
    this->sceneId = sceneId;
    this->entities.push_back(entity);
    this->componentType = componentType;

    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::AddComponentCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Scene* scene = sceneProject->scene;

        // a character controller and a rigidbody of its dimension never share a GameObject
        for (Entity entity : entities){
            const std::string conflict = Catalog::getComponentConflict(scene, entity, componentType);
            if (!conflict.empty()){
                Out::error("%s", conflict.c_str());
                return false;
            }
        }

        required.clear();
        if (componentType == ComponentType::Transform){
            trackedBefore = sceneProject->entities;
        }
        for (Entity& entity : entities){
            prefabUndo.capture(project, sceneId, entity);
            // a spline consumer reads its GameObject's Spline; an extrude draws with its Mesh
            if (componentType == ComponentType::SplineExtrudeComponent || componentType == ComponentType::SplineInstantiateComponent){
                std::vector<ComponentType> needs = {ComponentType::SplineComponent};
                if (componentType == ComponentType::SplineExtrudeComponent) needs.push_back(ComponentType::MeshComponent);
                const std::vector<ComponentType> has = Catalog::findComponents(scene, entity);
                for (ComponentType need : needs){
                    if (std::find(has.begin(), has.end(), need) == has.end()){
                        ProjectUtils::addEntityComponent(scene, entity, need, sceneProject->entities);
                        project->recordPrefabComponentAdded(sceneId, entity, need);
                        required.emplace_back(entity, need);
                    }
                }
            }
            // a Title Art Renderer draws through an Image (UI) or a Sprite (world): with neither, the
            // one the scene calls for comes in the same undo step (S81 R1)
            if (componentType == ComponentType::TitleArtRendererComponent && TitleArtSystem::spaceOf(scene, entity) == titleart::TitleArtSpace::None){
                bool ui = sceneProject->sceneType == SceneType::SCENE_UI;
                for (Entity parent = scene->findComponent<Transform>(entity) ? scene->getComponent<Transform>(entity).parent : NULL_ENTITY;
                     !ui && parent != NULL_ENTITY;
                     parent = scene->findComponent<Transform>(parent) ? scene->getComponent<Transform>(parent).parent : NULL_ENTITY){
                    ui = scene->findComponent<RectTransformComponent>(parent) != nullptr;
                }
                std::vector<ComponentType> needs;
                if (ui) needs = {ComponentType::RectTransformComponent, ComponentType::UIComponent, ComponentType::ImageComponent};
                else needs = {ComponentType::MeshComponent, ComponentType::SpriteComponent};
                const std::vector<ComponentType> has = Catalog::findComponents(scene, entity);
                for (ComponentType need : needs){
                    if (std::find(has.begin(), has.end(), need) != has.end()) continue;
                    ProjectUtils::addEntityComponent(scene, entity, need, sceneProject->entities);
                    project->recordPrefabComponentAdded(sceneId, entity, need);
                    required.emplace_back(entity, need);
                }
            }
            ProjectUtils::addEntityComponent(scene, entity, componentType, sceneProject->entities);
            // a 2D scene's splines lie in the XY plane (S18 R13)
            if (componentType == ComponentType::SplineComponent && sceneProject->sceneType == SceneType::SCENE_2D){
                scene->getComponent<SplineComponent>(entity).spline.setUpAxis(Vector3(0.0f, 0.0f, 1.0f));
            }
            for (const auto& [requiredEntity, requiredType] : required){
                if (requiredEntity == entity && requiredType == ComponentType::SplineComponent && sceneProject->sceneType == SceneType::SCENE_2D){
                    scene->getComponent<SplineComponent>(entity).spline.setUpAxis(Vector3(0.0f, 0.0f, 1.0f));
                }
            }

            // a Light 2D added in the editor (Add Component, add_component) starts at the
            // virtual height normal maps respond to, as a created one (CreateEntityCmd); the
            // struct default (0) stays the loader's and the scripts'. Redo adds it again here
            if (componentType == ComponentType::Light2DComponent){
                if (Light2DComponent* light = scene->findComponent<Light2DComponent>(entity)){
                    light->height = Light2DComponent::NEW_HEIGHT;
                }
            }

            // a Line or Trail Renderer added in the editor starts as a new one from the menu does
            // (a 2-point polyline with a width, a fading trail); the struct defaults stay the
            // loader's and the scripts'
            if (componentType == ComponentType::LineRendererComponent){
                if (LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity)){
                    ProjectUtils::setupNewLineRenderer(*lines, sceneProject->sceneType != SceneType::SCENE_3D);
                }
            }
            if (componentType == ComponentType::TrailRendererComponent){
                if (TrailRendererComponent* trail = scene->findComponent<TrailRendererComponent>(entity)){
                    ProjectUtils::setupNewTrailRenderer(*trail, sceneProject->sceneType != SceneType::SCENE_3D);
                }
            }

            // a Particle System added in the editor: Unity's default effect, in a 2D scene in its
            // pixel units (40 a unit, the presets' scale) under the 2D gravity
            if (componentType == ComponentType::ParticleSystemComponent){
                if (ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity)){
                    if (sceneProject->sceneType != SceneType::SCENE_3D && ps->effect.empty()){
                        particles::ParticleEffectDocument& d = ps->inlineEffect;
                        d.main.startSpeed = particles::MinMaxCurve(200.0f);
                        d.main.startSize = particles::MinMaxCurve(40.0f);
                        d.main.gravitySource = particles::GravitySource::Physics2D;
                        d.shape.radius = 40.0f;
                        ps->needReload = true;
                    }
                }
                if (ActionComponent* action = scene->findComponent<ActionComponent>(entity)){
                    if (action->target == NULL_ENTITY) action->target = entity;
                }
            }

            if (componentType == ComponentType::InstancedMeshComponent &&
                ProjectUtils::hasModelMeshChildrenWithoutRootGeometry(scene, entity)){
                Out::warning(
                    "Instanced Mesh on '%s' will not render: this model stores its geometry in child mesh entities. "
                    "Right-click the model in the Hierarchy and choose 'Merge static model'.",
                    scene->getEntityName(entity).c_str());
            }

            project->recordPrefabComponentAdded(sceneId, entity, componentType);
        }

        // textures that name this fluid or player ("fluid|<entity>", "video|<entity>") show its output now
        if (componentType == ComponentType::FluidSimulationComponent || componentType == ComponentType::VideoPlayerComponent){
            CameraTextureLink::resolve(scene);
        }

        if (componentType == ComponentType::AnimationPlayerComponent){
            if (!executed){
                for (Entity entity : entities){
                    PlayerModelLayout::ensureNodeHierarchyForPlayer(project, (uint32_t)sceneId, entity, layoutReloads);
                }
            }else{
                PlayerModelLayout::redoReloads(layoutReloads);
            }
        }
        executed = true;

        sceneProject->isModified = true;
    }

    return true;
}

void editor::AddComponentCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject) {
        Scene* scene = sceneProject->scene;

        // the model reloads ran after the component was added
        PlayerModelLayout::undoReloads(layoutReloads);

        for (Entity& entity : entities){
            // Undoing an add removes only the component introduced by this command. Any
            // ownership configured later belongs to later commands and must not broaden undo.
            ProjectUtils::removeEntityComponent(scene, entity, componentType, sceneProject->entities);
        }
        for (auto it = required.rbegin(); it != required.rend(); ++it){
            ProjectUtils::removeEntityComponent(scene, it->first, it->second, sceneProject->entities);
        }
        prefabUndo.restore(project, sceneId);

        ProjectUtils::reconcileTrackedEntities(scene, sceneProject->entities, project, sceneId);
        if (componentType == ComponentType::Transform){
            ProjectUtils::keepUntransformedPlaces(scene, trackedBefore, sceneProject->entities);
        }

        if (CameraTextureLink::isLinkSourceType(componentType)){
            CameraTextureLink::resolve(scene);
        }

        sceneProject->isModified = wasModified;
    }
}

bool editor::AddComponentCmd::mergeWith(Command* otherCommand){
    AddComponentCmd* otherCmd = dynamic_cast<AddComponentCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (project == otherCmd->project && sceneId == otherCmd->sceneId && componentType == otherCmd->componentType){

            for (Entity& otherEntity :  otherCmd->entities){
                entities.push_back(otherEntity);
            }
            prefabUndo.mergeOlder(otherCmd->prefabUndo);
            // the older step's list is the one from before both
            trackedBefore = otherCmd->trackedBefore;
            // the older command's reloads ran first
            PlayerModelLayout::Reloads reloads = std::move(otherCmd->layoutReloads);
            for (auto& reload : layoutReloads) reloads.push_back(std::move(reload));
            layoutReloads = std::move(reloads);

            wasModified = wasModified && otherCmd->wasModified;

            return true;
        }
    }

    return false;
}
