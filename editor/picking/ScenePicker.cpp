// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The click flow (decideClick) and the candidate list follow OpenEngine's
// SceneViewController::PickViaClickThrough (Apps/Editor/Source/SceneViewController.cpp) and
// BuildPickCandidates (Apps/Editor/Source/Picking/PickRootResolver.cpp), Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md).

#include "ScenePicker.h"
#include "component/AudioSourceComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "component/CameraComponent.h"
#include "IconPicker.h"

#include "component/LineRendererComponent.h"
#include "component/ModelComponent.h"
#include "component/PointsComponent.h"
#include "picking/SceneBvh.h"
#include "render/SceneRender.h"
#include "subsystem/SplineSystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the entry of a ray (origin + t * direction, t in [0, 1]) into a box from outside it
    bool rayEntersBox(const Ray& ray, const AABB& box, float& t){
        if (box.isNull() || box.isInfinite()){
            return false;
        }
        const Vector3 o = ray.getOrigin();
        const Vector3 d = ray.getDirection();
        if (box.contains(o)){
            return false;
        }
        float t0 = 0.0f, t1 = 1.0f;
        for (unsigned a = 0; a < 3; a++){
            if (std::fabs(d[a]) < 1e-30f){
                if (o[a] < box.getMinimum()[a] || o[a] > box.getMaximum()[a]) return false;
                continue;
            }
            float tn = (box.getMinimum()[a] - o[a]) / d[a];
            float tf = (box.getMaximum()[a] - o[a]) / d[a];
            if (tn > tf) std::swap(tn, tf);
            t0 = std::max(t0, tn);
            t1 = std::min(t1, tf);
            if (t0 > t1) return false;
        }
        t = t0;
        return true;
    }

    size_t transformIndex(Scene* scene, Entity entity){
        auto transforms = scene->getComponentArray<Transform>();
        return transforms->hasEntity(entity) ? transforms->getIndex(entity) : 0;
    }

    // bounds only from the icon the view draws (lights, cameras, audio sources)
    bool hasOnlyIconBounds(Scene* scene, Entity entity){
        const Signature signature = scene->getSignature(entity);
        if (signature.test(scene->getComponentId<MeshComponent>()) || signature.test(scene->getComponentId<UIComponent>())){
            return false;
        }
        return signature.test(scene->getComponentId<LightComponent>()) || signature.test(scene->getComponentId<Light2DComponent>()) ||
               signature.test(scene->getComponentId<CameraComponent>()) || signature.test(scene->getComponentId<AudioSourceComponent>());
    }

    bool isDrawn(Scene* scene, Entity entity){
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !transform->visible){
            return false;
        }
        return !scene->hasInactiveEntities() || scene->isEntityActiveInHierarchy(entity);
    }

}

ScenePicker::ScenePicker(Project* project): project(project){
}

bool ScenePicker::isInside(uint32_t sceneId, Entity entity, Entity ancestor) const{
    const SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || entity == NULL_ENTITY || ancestor == NULL_ENTITY){
        return false;
    }
    Scene* scene = sceneProject->scene;
    for (size_t depth = 0; entity != NULL_ENTITY && depth < 4096; depth++){
        if (entity == ancestor){
            return true;
        }
        const Transform* transform = scene->findComponent<Transform>(entity);
        entity = transform ? transform->parent : NULL_ENTITY;
    }
    return false;
}

Entity ScenePicker::pickRoot(uint32_t sceneId, Entity exact, const ScenePickRequest& request) const{
    const SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || exact == NULL_ENTITY){
        return exact;
    }
    Scene* scene = sceneProject->scene;
    // in Prefab Mode the prefab being edited is the whole stage, never a pick root
    const PrefabStage* stage = project->getPrefabStage(sceneId);
    const Entity stageRoot = stage ? stage->root : NULL_ENTITY;

    // O(depth): every prefab instance root (nested ones outward) and model root above it,
    // the outermost one kept
    Entity root = exact;
    Entity entity = exact;
    for (size_t depth = 0; entity != NULL_ENTITY && depth < 4096; depth++){
        if (entity != stageRoot && (project->isPrefabInstanceRoot(sceneId, entity) || scene->findComponent<ModelComponent>(entity))){
            root = entity;
        }
        const Transform* transform = scene->findComponent<Transform>(entity);
        entity = transform ? transform->parent : NULL_ENTITY;
    }

    // the Hierarchy row it is collapsed into, when that is further out
    if (request.collapsedRow){
        const Entity collapsed = request.collapsedRow(sceneId, exact);
        if (collapsed != NULL_ENTITY && collapsed != exact && collapsed != stageRoot && isInside(sceneId, root, collapsed)){
            root = collapsed;
        }
    }
    return root;
}

std::vector<Entity> ScenePicker::intermediateRoots(uint32_t sceneId, Entity root, Entity exact) const{
    std::vector<Entity> roots;
    const SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || root == exact){
        return roots;
    }
    Scene* scene = sceneProject->scene;
    const Transform* transform = scene->findComponent<Transform>(exact);
    Entity entity = transform ? transform->parent : NULL_ENTITY;
    for (size_t depth = 0; entity != NULL_ENTITY && entity != root && depth < 4096; depth++){
        if (project->isPrefabInstanceRoot(sceneId, entity)){
            roots.push_back(entity);
        }
        const Transform* parent = scene->findComponent<Transform>(entity);
        entity = parent ? parent->parent : NULL_ENTITY;
    }
    std::reverse(roots.begin(), roots.end());
    return roots;
}

PickFilter ScenePicker::makeFilter(const SceneProject* layerScene, const SceneProject* viewScene, bool waitForBvh) const{
    PickFilter filter;
    filter.kinds = PICK_ALL & ~PICK_ICON;
    filter.waitForBvh = waitForBvh;
    filter.excludeEditorFlags = EDITOR_HIDDEN | EDITOR_PICK_LOCKED;
    if (viewScene && layerScene == viewScene){
        // the view's own scene: its Layers filter (a hidden layer is not drawn either), and
        // its face culling toggle (every face drawn, every face hit)
        filter.layerMask = viewScene->viewState.layerFilter.pickable & viewScene->viewState.layerFilter.visible;
        filter.hitBackfaces = viewScene->displaySettings.disableFaceCulling;
    }
    auto pickable = std::make_shared<std::unordered_set<Entity>>();
    const std::vector<Entity> entities = project->getPickableEntities(layerScene);
    pickable->insert(entities.begin(), entities.end());
    // what a spline builds is picked as its GameObject (pickedEntity)
    std::shared_ptr<SplineSystem> splines = layerScene->scene ? layerScene->scene->findSystem<SplineSystem>() : nullptr;
    filter.accept = [pickable, splines](Entity entity){
        if (pickable->count(entity) > 0){
            return true;
        }
        const Entity owner = splines ? splines->getGeneratedOwner(entity) : NULL_ENTITY;
        return owner != NULL_ENTITY && pickable->count(owner) > 0;
    };
    return filter;
}

Entity ScenePicker::pickedEntity(Scene* scene, Entity hit){
    std::shared_ptr<SplineSystem> splines = scene ? scene->findSystem<SplineSystem>() : nullptr;
    const Entity owner = splines ? splines->getGeneratedOwner(hit) : NULL_ENTITY;
    return owner != NULL_ENTITY ? owner : hit;
}

void ScenePicker::pickLayer(const Project::SceneViewLayer& layer, SceneProject* viewScene, const Ray& ray, bool waitForBvh,
                            std::vector<ScenePickEntry>& out, bool& pending, ScenePickEntry& pendingHit) const{
    SceneProject* layerScene = layer.sceneProject;
    Scene* scene = layerScene->scene;
    const PickFilter filter = makeFilter(layerScene, layer.base ? viewScene : nullptr, waitForBvh);

    std::vector<PickHit> hits;
    scene->getPickBvh().raycast(ray, filter, hits, true);
    const float rayLength = ray.getDirection().length();

    // points and lines: handles of a constant screen size around each point (what the view
    // draws of them), not their bare bounds
    Scene* sizeScene = layer.base ? viewScene->scene : scene;
    auto testHandles = [&](auto array){
        for (size_t i = 0; i < array->size(); i++){
            const Entity entity = array->getEntity(i);
            if (!filter.accept(entity) || !isDrawn(scene, entity) || !(filter.layerMask & (1u << (scene->getEntityLayer(entity) & 31))) ||
                    (scene->getEntityEditorFlags(entity) & filter.excludeEditorFlags)){
                continue;
            }
            float t;
            if (!rayEntersBox(ray, project->getEntityWorldAABB(scene, entity, sizeScene), t)){
                continue;
            }
            PickHit hit;
            hit.entity = entity;
            hit.kind = PickKind::BOUNDS;
            hit.distance = t * rayLength;
            hit.point = ray.getPoint(t);
            hit.normal = hit.faceNormal = -(ray.getDirection() / rayLength);
            auto existing = std::find_if(hits.begin(), hits.end(), [entity](const PickHit& h){ return h.entity == entity; });
            if (existing == hits.end()){
                hits.push_back(hit);
            }else if (hit.distance < existing->distance){
                *existing = hit;
            }
        }
    };
    testHandles(scene->getComponentArray<PointsComponent>());
    testHandles(scene->getComponentArray<LineRendererComponent>());

    // 3D: the nearest first; 2D and UI: the last drawn first (a higher Transform index)
    if (layerScene->sceneType == SceneType::SCENE_3D){
        std::stable_sort(hits.begin(), hits.end(), [](const PickHit& a, const PickHit& b){ return a.distance < b.distance; });
    }else{
        std::stable_sort(hits.begin(), hits.end(), [scene](const PickHit& a, const PickHit& b){
            const size_t ia = transformIndex(scene, a.entity);
            const size_t ib = transformIndex(scene, b.entity);
            return ia != ib ? ia > ib : a.distance < b.distance;
        });
    }

    for (const PickHit& hit : hits){
        ScenePickEntry entry;
        entry.sceneId = layer.sceneId;
        entry.entity = pickedEntity(scene, hit.entity);
        entry.hit = hit;
        if (std::find(out.begin(), out.end(), entry) != out.end()){
            continue;
        }
        // a mesh still building in front of every exact hit: the click cannot be decided yet
        if (hit.pending && !pending && std::none_of(out.begin(), out.end(), [](const ScenePickEntry& e){ return e.hit.kind != PickKind::ICON && !e.hit.pending; })){
            pending = true;
            pendingHit = entry;
        }
        out.push_back(entry);
    }
}

ScenePickResult ScenePicker::pick(uint32_t viewSceneId, const ScenePickRequest& request){
    ScenePickResult result;
    SceneProject* viewScene = project->getScene(viewSceneId);
    if (!viewScene || !viewScene->scene || !viewScene->sceneRender){
        return result;
    }
    SceneRender* sceneRender = viewScene->sceneRender;
    const std::vector<Project::SceneViewLayer> layers = project->getSceneViewLayers(viewSceneId);

    // icons first: they are drawn over everything (the view's own scene draws them; not while
    // a game load has replaced it in the view)
    if (!project->getReplacingMainScene(viewSceneId)) {
        IconPickFilter iconFilter;
        iconFilter.lights = !(viewScene->sceneType == SceneType::SCENE_3D && viewScene->displaySettings.hideLightIcons);
        iconFilter.audio = !viewScene->displaySettings.hideSoundIcons;
        iconFilter.layerMask = viewScene->viewState.layerFilter.pickable & viewScene->viewState.layerFilter.visible;
        iconFilter.excludeEditorFlags = EDITOR_HIDDEN | EDITOR_PICK_LOCKED;
        auto pickable = std::make_shared<std::unordered_set<Entity>>();
        const std::vector<Entity> entities = project->getPickableEntities(viewScene);
        pickable->insert(entities.begin(), entities.end());
        iconFilter.accept = [pickable](Entity entity){ return pickable->count(entity) > 0; };
        std::vector<IconHit> icons;
        pickIcons(viewScene->scene, sceneRender->getCamera()->getViewProjectionMatrix(), (float)sceneRender->getViewWidth(),
                  (float)sceneRender->getViewHeight(), request.x, request.y, request.iconRadius, iconFilter, icons);
        const Ray ray = sceneRender->screenToRay(request.x, request.y);
        for (const IconHit& icon : icons){
            ScenePickEntry entry;
            entry.sceneId = viewSceneId;
            entry.entity = icon.entity;
            entry.hit.entity = icon.entity;
            entry.hit.kind = PickKind::ICON;
            if (const Transform* transform = viewScene->scene->findComponent<Transform>(icon.entity)){
                entry.hit.point = transform->worldPosition;
                entry.hit.distance = (transform->worldPosition - ray.getOrigin()).length();
            }
            result.hits.push_back(entry);
        }
        result.iconCount = result.hits.size();
    }

    // what is drawn on top first: the layers in reverse drawing order, the scene itself last
    for (size_t i = layers.size(); i-- > 0;){
        const Project::SceneViewLayer& layer = layers[i];
        if (!layer.sceneProject || !layer.sceneProject->scene){
            continue;
        }
        Ray ray;
        if (layer.base){
            ray = sceneRender->screenToRay(request.x, request.y);
        }else{
            if (!layer.view.contains(request.x, request.y)){
                continue;
            }
            ray = layer.view.ray(request.x, request.y);
        }
        pickLayer(layer, viewScene, ray, request.waitForBvh, result.hits, result.pending, result.pendingHit);
    }

    // the candidates: the icons, then the nearest hit's root, the prefab roots between it and
    // the hit, the hit, and every GameObject behind it
    result.candidates.assign(result.hits.begin(), result.hits.begin() + (std::ptrdiff_t)result.iconCount);
    if (result.hits.size() > result.iconCount){
        const ScenePickEntry& nearest = result.hits[result.iconCount];
        auto addCandidate = [&](uint32_t sceneId, Entity entity, const PickHit& hit){
            ScenePickEntry entry;
            entry.sceneId = sceneId;
            entry.entity = entity;
            entry.hit = hit;
            if (std::find(result.candidates.begin(), result.candidates.end(), entry) == result.candidates.end()){
                result.candidates.push_back(entry);
            }
        };
        if (!request.exactPick){
            const Entity root = pickRoot(nearest.sceneId, nearest.entity, request);
            addCandidate(nearest.sceneId, root, nearest.hit);
            for (Entity intermediate : intermediateRoots(nearest.sceneId, root, nearest.entity)){
                addCandidate(nearest.sceneId, intermediate, nearest.hit);
            }
        }
        for (size_t i = result.iconCount; i < result.hits.size(); i++){
            addCandidate(result.hits[i].sceneId, result.hits[i].entity, result.hits[i].hit);
        }
    }
    return result;
}

ScenePickClick ScenePicker::decideClick(const ScenePickResult& result, const ScenePickContext& context){
    ScenePickClick click;
    const std::vector<ScenePickEntry>& candidates = result.candidates;
    if (candidates.empty()){
        // a miss clears the selection; with a modifier it stays
        click.action = (context.shift || context.ctrl) ? ScenePickClick::Action::NONE : ScenePickClick::Action::CLEAR;
        return click;
    }

    const ScenePickEntry& front = candidates.front();
    const ScenePickEntry& nearestExact = result.hits.size() > result.iconCount ? result.hits[result.iconCount] : front;
    const bool drillable = result.iconCount == 0 && !context.exactPick;
    click.cycleCount = (int)candidates.size();
    auto indexOf = [&](const ScenePickEntry& entry){
        auto it = std::find(candidates.begin(), candidates.end(), entry);
        return it == candidates.end() ? -1 : (int)(it - candidates.begin());
    };
    auto insideFront = [&](const ScenePickEntry& entry){
        return entry.entity != NULL_ENTITY && entry.sceneId == front.sceneId && context.isInside &&
               context.isInside(front.sceneId, entry.entity, front.entity);
    };

    // Shift adds, Ctrl toggles; no click-through. What was drilled to stays the target: the
    // exact hit when it is selected, else the selected GameObject inside the instance
    if (context.shift || context.ctrl){
        ScenePickEntry target = front;
        if (drillable){
            if (context.isSelected && context.isSelected(nearestExact)){
                target = nearestExact;
            }else if (context.primary != front && insideFront(context.primary) && context.isSelected && context.isSelected(context.primary)){
                target = context.primary;
            }
        }
        click.action = context.ctrl ? ScenePickClick::Action::TOGGLE : ScenePickClick::Action::ADD;
        click.target = target;
        click.cycleIndex = indexOf(target);
        return click;
    }

    const int primaryIndex = context.primary.entity != NULL_ENTITY ? indexOf(context.primary) : -1;
    const bool sameSpot = std::fabs(context.x - context.lastX) <= SAME_SPOT_PIXELS && std::fabs(context.y - context.lastY) <= SAME_SPOT_PIXELS;

    click.action = ScenePickClick::Action::SELECT;
    if (primaryIndex >= 0 && context.depthAnchor){
        // the Ctrl+wheel step is confirmed
        click.target = context.primary;
        click.cycleIndex = primaryIndex;
    }else if (primaryIndex >= 0 && sameSpot){
        // the same spot again: the next one, wrapping around
        click.cycleIndex = (primaryIndex + 1) % (int)candidates.size();
        click.target = candidates[(size_t)click.cycleIndex];
    }else if (drillable && insideFront(context.primary)){
        // the selection is the instance or inside it: the GameObject under the pointer
        click.target = nearestExact;
        click.cycleIndex = indexOf(nearestExact);
    }else{
        click.target = front;
        click.cycleIndex = 0;
    }
    return click;
}

void ScenePicker::rectPlanes(const Matrix4& vp, Vector2 minNdc, Vector2 maxNdc, Plane planes[6]){
    auto row = [&](int r){ return Vector4(vp.get(0, r), vp.get(1, r), vp.get(2, r), vp.get(3, r)); };
    const Vector4 r0 = row(0), r1 = row(1), r3 = row(3);
    auto plane = [](const Vector4& v){ return Plane(Vector3(v.x, v.y, v.z), v.w); };
    planes[0] = plane(r0 - r3 * minNdc.x);   // x >= min
    planes[1] = plane(r3 * maxNdc.x - r0);   // x <= max
    planes[2] = plane(r1 - r3 * minNdc.y);
    planes[3] = plane(r3 * maxNdc.y - r1);
    // the corner rule reads x and y only: near and far always pass
    planes[4] = Plane(Vector3(0.0f, 0.0f, 0.0f), 1.0f);
    planes[5] = Plane(Vector3(0.0f, 0.0f, 0.0f), 1.0f);
}

Entity ScenePicker::bestOnRay(const std::vector<Entity>& entities, Scene* scene, const Ray& ray, Scene* sizeScene,
                              SceneType sceneType, float& distance, size_t& index, bool iconsDrawn) const{
    const float rayLength = ray.getDirection().length();
    if (!(rayLength > 0.0f) || !scene){
        return NULL_ENTITY;
    }
    auto accepted = std::make_shared<std::unordered_set<Entity>>(entities.begin(), entities.end());
    PickFilter filter;
    filter.kinds = PICK_ALL & ~PICK_ICON;
    std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>();
    filter.accept = [accepted, splines](Entity entity){
        if (accepted->count(entity) > 0){
            return true;
        }
        const Entity owner = splines ? splines->getGeneratedOwner(entity) : NULL_ENTITY;
        return owner != NULL_ENTITY && accepted->count(owner) > 0;
    };
    // the editor's incidental queries (what a press lands on, the physics snap's arming) never
    // build a mesh's tree on the main thread: one still building answers with its bounds
    filter.waitForBvh = false;
    std::vector<PickHit> hits;
    scene->getPickBvh().raycast(ray, filter, hits, true);

    Entity best = NULL_ENTITY;
    auto consider = [&](Entity entity, float fraction){
        if (fraction < 0.0f || fraction > 1.0f){
            return;
        }
        const size_t entityIndex = transformIndex(scene, entity);
        if (fraction < distance || (entityIndex >= index && sceneType != SceneType::SCENE_3D)){
            distance = fraction;
            index = entityIndex;
            best = entity;
        }
    };
    for (const PickHit& hit : hits){
        consider(pickedEntity(scene, hit.entity), hit.distance / rayLength);
    }
    // points, lines and (when drawn) icons: the view-sized boxes the old picker tested
    for (Entity entity : entities){
        if (!scene->isEntityCreated(entity) || !isDrawn(scene, entity)){
            continue;
        }
        const Signature signature = scene->getSignature(entity);
        const bool handles = signature.test(scene->getComponentId<PointsComponent>()) || signature.test(scene->getComponentId<LineRendererComponent>());
        const bool icon = iconsDrawn && hasOnlyIconBounds(scene, entity);
        if (!handles && !icon){
            continue;
        }
        float t;
        if (rayEntersBox(ray, project->getEntityWorldAABB(scene, entity, sizeScene), t)){
            consider(entity, t);
        }
    }
    return best;
}
