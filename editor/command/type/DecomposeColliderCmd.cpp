// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "DecomposeColliderCmd.h"

#include "EditorHost.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "physics/JoltShapeBuilder.h"
#include "util/MeshTriangles.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::vector<std::shared_ptr<ColliderDecomposition>>& runningDecompositions(){
        static std::vector<std::shared_ptr<ColliderDecomposition>> running;
        return running;
    }

    // the saved fields of a collider (the runtime ones left out): what tells a collider edited
    // while it was decomposed
    bool sameDecomposedCollider(const Collider3D& a, const Collider3D& b){
        return a.type == b.type && a.source == b.source && a.sourceEntity == b.sourceEntity && a.position == b.position &&
               a.rotation == b.rotation && a.vertices == b.vertices && a.indices == b.indices && a.material == b.material &&
               a.isTrigger == b.isTrigger && a.density == b.density && a.receiver == b.receiver;
    }

}

DecomposeColliderCmd::DecomposeColliderCmd(Project* project, uint32_t sceneId, Entity entity, std::vector<Collider3D> colliders)
    : project(project), sceneId(sceneId), entity(entity) {
    addPropertyCmd<std::vector<Collider3D>>(project, sceneId, entity, ComponentType::RigidbodyComponent, "colliders", std::move(colliders));
    setNoMerge();
}

std::string ColliderDecomposition::stage() const{
    std::lock_guard<std::mutex> lock(stageMutex);
    return stageName;
}

std::shared_ptr<ColliderDecomposition> ColliderDecomposition::find(uint32_t sceneId, Entity entity, size_t index){
    for (const std::shared_ptr<ColliderDecomposition>& running : runningDecompositions()){
        if (running->request.sceneId == sceneId && running->request.entity == entity && running->request.index == index){
            return running;
        }
    }
    return nullptr;
}

std::shared_ptr<ColliderDecomposition> ColliderDecomposition::start(Project* project, const Request& request, Finished finished, std::string& error){
    SceneProject* sceneProject = project ? project->getScene(request.sceneId) : nullptr;
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    if (!scene || !scene->isEntityCreated(request.entity)){
        error = "The GameObject is not in an open scene.";
        return nullptr;
    }
    const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(request.entity);
    if (!body){
        error = "\"" + scene->getEntityName(request.entity) + "\" has no Rigidbody.";
        return nullptr;
    }
    if (request.index >= body->colliders.size()){
        error = "decompose: index " + std::to_string(request.index) + " is out of range (the body has " + std::to_string(body->colliders.size()) + " collider(s)).";
        return nullptr;
    }
    const Collider3D& collider = body->colliders[request.index];
    if (collider.type != Collider3DType::MESH){
        error = "decompose: collider " + std::to_string(request.index) + " is not a Mesh collider (only a mesh is made into convex hulls).";
        return nullptr;
    }
    if (request.maxHulls < 1 || request.maxHulls > MAX_HULLS){
        error = "decompose: max_hulls must be 1 to " + std::to_string(MAX_HULLS) + ".";
        return nullptr;
    }
    if (!ConvexDecompositionCache::available()){
        error = "Convex decomposition is not available in this editor build.";
        return nullptr;
    }
    if (find(request.sceneId, request.entity, request.index)){
        error = "That collider is being decomposed already: wait for it to finish.";
        return nullptr;
    }

    auto decomposition = std::make_shared<ColliderDecomposition>();
    decomposition->project = project;
    decomposition->request = request;
    decomposition->source = collider;
    decomposition->finished = std::move(finished);

    // the triangles in the GameObject's own space, unscaled: what RAW_VERTICES hulls hold
    if (collider.source == Collider3DSource::RAW_MESH){
        decomposition->positions = collider.vertices;
        decomposition->indices = collider.indices;
    }else{
        const Entity meshEntity = collider.sourceEntity == NULL_ENTITY ? request.entity : collider.sourceEntity;
        if (MeshTriangles::isGeometryLoading(scene, meshEntity)){
            error = "The mesh of \"" + scene->getEntityName(meshEntity) + "\" is still loading: decompose again once it shows.";
            return nullptr;
        }
        if (!JoltShapeBuilder::appendTriangles(scene, request.entity, meshEntity, decomposition->positions, decomposition->indices)){
            error = "\"" + scene->getEntityName(meshEntity) + "\" has no mesh geometry to decompose.";
            return nullptr;
        }
        const Vector3 scale = JoltShapeBuilder::entityWorldScale(scene, request.entity);
        for (Vector3& position : decomposition->positions){
            position = Vector3(scale.x != 0.0f ? position.x / scale.x : position.x, scale.y != 0.0f ? position.y / scale.y : position.y,
                               scale.z != 0.0f ? position.z / scale.z : position.z);
        }
    }
    if (decomposition->indices.size() < 3){
        error = "The Mesh collider has no triangle to decompose.";
        return nullptr;
    }

    runningDecompositions().push_back(decomposition);
    if (jobs::hasSystem() && !jobs::system().isSingleThreaded()){
        jobs::submit([decomposition]() { decomposition->run(); }, {jobs::Lane::Background, jobs::GateClass::Default, "decompose-collider"});
    }else{
        decomposition->run();
    }
    return decomposition;
}

void ColliderDecomposition::run(){
    DecompositionParams params = DecompositionParams::fromQuality(request.quality);
    params.maxHulls = request.maxHulls;
    std::string error;
    succeeded = ConvexDecompositionCache::decompose(positions, indices, params, hulls, &cancelFlag, &error,
        [this](const char* name, double stageProgress){
            {
                std::lock_guard<std::mutex> lock(stageMutex);
                stageName = name ? name : "";
            }
            progressValue.store(std::clamp(stageProgress / 100.0, 0.0, 1.0));
        });
    failure = error;
    // the history and the scene are the main thread's
    std::shared_ptr<ColliderDecomposition> self = find(request.sceneId, request.entity, request.index);
    getEditorHost().enqueueMainThreadTask([self]() {
        if (self) self->land();
    });
}

void ColliderDecomposition::land(){
    std::vector<std::shared_ptr<ColliderDecomposition>>& running = runningDecompositions();
    std::shared_ptr<ColliderDecomposition> keep;
    for (auto it = running.begin(); it != running.end(); ++it){
        if (it->get() == this){
            keep = *it;
            running.erase(it);
            break;
        }
    }

    Outcome outcome;
    SceneProject* sceneProject = project->getScene(request.sceneId);
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    RigidbodyComponent* body = (scene && scene->isEntityCreated(request.entity)) ? scene->findComponent<RigidbodyComponent>(request.entity) : nullptr;
    if (cancelFlag.load()){
        outcome.cancelled = true;
        outcome.error = "Cancelled; the Mesh collider is unchanged.";
    }else if (!succeeded){
        outcome.error = "The mesh could not be decomposed: " + (failure.empty() ? std::string("V-HACD failed") : failure) + ".";
    }else if (hulls.empty()){
        outcome.error = "The mesh gave no convex hull.";
    }else if (!body || request.index >= body->colliders.size() || !sameDecomposedCollider(body->colliders[request.index], source)){
        outcome.error = "The collider changed or went while it was decomposed: decompose it again.";
    }else{
        // each hull a RAW_VERTICES Convex Hull in the Mesh collider's place, as it was posed and surfaced
        std::vector<Collider3D> colliders = body->colliders;
        std::vector<Collider3D> made;
        for (const ConvexHullPoints& hull : hulls){
            if (hull.points.size() < 4){
                continue;
            }
            Collider3D collider;
            collider.type = Collider3DType::CONVEX_HULL;
            collider.source = Collider3DSource::RAW_VERTICES;
            collider.vertices = hull.points;
            collider.position = source.position;
            collider.rotation = source.rotation;
            collider.material = source.material;
            collider.isTrigger = source.isTrigger;
            collider.density = source.density;
            collider.receiver = source.receiver;
            made.push_back(std::move(collider));
        }
        if (made.empty()){
            outcome.error = "The mesh gave no convex hull of 4 points or more.";
        }else{
            colliders.erase(colliders.begin() + (std::ptrdiff_t)request.index);
            colliders.insert(colliders.begin() + (std::ptrdiff_t)request.index, made.begin(), made.end());
            auto* command = new DecomposeColliderCmd(project, request.sceneId, request.entity, std::move(colliders));
            outcome.undoName = command->describe();
            outcome.hulls = made.size();
            if (!CommandHandle::get(request.sceneId)->addCommand(command)){
                outcome.error = "The editor rejected the collider edit.";
                outcome.hulls = 0;
            }
        }
    }
    if (finished){
        finished(outcome);
    }
}
