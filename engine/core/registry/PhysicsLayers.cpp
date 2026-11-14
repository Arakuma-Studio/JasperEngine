// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsLayers.h"
#include "Log.h"

#include <cmath>

using namespace jasperengine;

namespace{

    bool validLayer(int layer){
        return layer >= 0 && layer < MAX_LAYERS;
    }

    bool checkLayers(const char* function, int layer1, int layer2){
        if (!validLayer(layer1) || !validLayer(layer2)){
            Log::error("%s: layer numbers must be between 0 and %d (got %d and %d)", function, MAX_LAYERS - 1, layer1, layer2);
            return false;
        }
        return true;
    }

}

LayerCollisionMatrix::LayerCollisionMatrix(){
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        rows[layer].store(LAYER_MASK_EVERYTHING, std::memory_order_relaxed);
    }
    version.store(0, std::memory_order_relaxed);
    allEnabled.store(true, std::memory_order_relaxed);
}

void LayerCollisionMatrix::refreshAllEnabled(){
    bool all = true;
    for (int layer = 0; layer < MAX_LAYERS && all; layer++){
        all = rows[layer].load(std::memory_order_relaxed) == LAYER_MASK_EVERYTHING;
    }
    allEnabled.store(all, std::memory_order_relaxed);
}

bool LayerCollisionMatrix::setIgnore(int layer1, int layer2, bool ignore){
    if (!validLayer(layer1) || !validLayer(layer2)){
        return false;
    }
    const uint32_t bit1 = 1u << layer1;
    const uint32_t bit2 = 1u << layer2;
    if (ignore){
        rows[layer1].fetch_and(~bit2, std::memory_order_relaxed);
        rows[layer2].fetch_and(~bit1, std::memory_order_relaxed);
    }else{
        rows[layer1].fetch_or(bit2, std::memory_order_relaxed);
        rows[layer2].fetch_or(bit1, std::memory_order_relaxed);
    }
    refreshAllEnabled();
    version.fetch_add(1, std::memory_order_release);
    return true;
}

bool LayerCollisionMatrix::isIgnored(int layer1, int layer2) const{
    if (!validLayer(layer1) || !validLayer(layer2)){
        return false;
    }
    return !collides((unsigned int)layer1, (unsigned int)layer2);
}

uint32_t LayerCollisionMatrix::getMask(int layer) const{
    if (!validLayer(layer)){
        return LAYER_MASK_EVERYTHING;
    }
    return rows[layer].load(std::memory_order_relaxed);
}

bool LayerCollisionMatrix::setMask(int layer, uint32_t layerMask){
    if (!validLayer(layer)){
        return false;
    }
    const uint32_t bit = 1u << layer;
    rows[layer].store(layerMask, std::memory_order_relaxed);
    for (int other = 0; other < MAX_LAYERS; other++){
        if (other == layer){
            continue;
        }
        if ((layerMask >> other) & 1u){
            rows[other].fetch_or(bit, std::memory_order_relaxed);
        }else{
            rows[other].fetch_and(~bit, std::memory_order_relaxed);
        }
    }
    refreshAllEnabled();
    version.fetch_add(1, std::memory_order_release);
    return true;
}

void LayerCollisionMatrix::reset(){
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        rows[layer].store(LAYER_MASK_EVERYTHING, std::memory_order_relaxed);
    }
    allEnabled.store(true, std::memory_order_relaxed);
    version.fetch_add(1, std::memory_order_release);
}

void LayerCollisionMatrix::setRows(const uint32_t newRows[MAX_LAYERS]){
    bool changed = false;
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        uint32_t row = 0;
        for (int other = 0; other < MAX_LAYERS; other++){
            if (((newRows[layer] >> other) & 1u) && ((newRows[other] >> layer) & 1u)){
                row |= 1u << other;
            }
        }
        if (rows[layer].load(std::memory_order_relaxed) != row){
            rows[layer].store(row, std::memory_order_relaxed);
            changed = true;
        }
    }
    if (changed){
        refreshAllEnabled();
        version.fetch_add(1, std::memory_order_release);
    }
}

void LayerCollisionMatrix::getRows(uint32_t outRows[MAX_LAYERS]) const{
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        outRows[layer] = rows[layer].load(std::memory_order_relaxed);
    }
}

// Physics

LayerCollisionMatrix& Physics::getLayerCollisionMatrix(){
    // function-local, so a scene built during static initialization still sees it
    static LayerCollisionMatrix matrix;
    return matrix;
}

void Physics::ignoreLayerCollision(int layer1, int layer2, bool ignore){
    if (checkLayers("Physics.ignoreLayerCollision", layer1, layer2)){
        getLayerCollisionMatrix().setIgnore(layer1, layer2, ignore);
    }
}

bool Physics::getIgnoreLayerCollision(int layer1, int layer2){
    if (!checkLayers("Physics.getIgnoreLayerCollision", layer1, layer2)){
        return false;
    }
    return getLayerCollisionMatrix().isIgnored(layer1, layer2);
}

uint32_t Physics::getLayerCollisionMask(int layer){
    if (!validLayer(layer)){
        Log::error("Physics.getLayerCollisionMask: layer must be between 0 and %d (got %d)", MAX_LAYERS - 1, layer);
        return LAYER_MASK_NOTHING;
    }
    return getLayerCollisionMatrix().getMask(layer);
}

void Physics::setLayerCollisionMask(int layer, uint32_t layerMask){
    if (!getLayerCollisionMatrix().setMask(layer, layerMask)){
        Log::error("Physics.setLayerCollisionMask: layer must be between 0 and %d (got %d)", MAX_LAYERS - 1, layer);
    }
}

void Physics::resetLayerCollisionMatrix(){
    getLayerCollisionMatrix().reset();
}

// Physics2D

LayerCollisionMatrix& Physics2D::getLayerCollisionMatrix(){
    static LayerCollisionMatrix matrix;
    return matrix;
}

void Physics2D::ignoreLayerCollision(int layer1, int layer2, bool ignore){
    if (checkLayers("Physics2D.ignoreLayerCollision", layer1, layer2)){
        getLayerCollisionMatrix().setIgnore(layer1, layer2, ignore);
    }
}

bool Physics2D::getIgnoreLayerCollision(int layer1, int layer2){
    if (!checkLayers("Physics2D.getIgnoreLayerCollision", layer1, layer2)){
        return false;
    }
    return getLayerCollisionMatrix().isIgnored(layer1, layer2);
}

uint32_t Physics2D::getLayerCollisionMask(int layer){
    if (!validLayer(layer)){
        Log::error("Physics2D.getLayerCollisionMask: layer must be between 0 and %d (got %d)", MAX_LAYERS - 1, layer);
        return LAYER_MASK_NOTHING;
    }
    return getLayerCollisionMatrix().getMask(layer);
}

void Physics2D::setLayerCollisionMask(int layer, uint32_t layerMask){
    if (!getLayerCollisionMatrix().setMask(layer, layerMask)){
        Log::error("Physics2D.setLayerCollisionMask: layer must be between 0 and %d (got %d)", MAX_LAYERS - 1, layer);
    }
}

void Physics2D::resetLayerCollisionMatrix(){
    getLayerCollisionMatrix().reset();
}

// Queries

#if defined(JASPERENGINE_PHYSICS_2D) || defined(JASPERENGINE_PHYSICS_3D)
namespace{

    // A Unity-style origin/direction/distance cast as a Ray whose direction spans the
    // whole distance; the hit fraction is then turned back into a distance
    bool makeDistanceRay(const char* function, const Vector3& origin, Vector3 direction, float maxDistance, Ray& ray){
        const float length = direction.length();
        if (!(length > 0.0f) || !std::isfinite(length)){
            Log::error("%s: direction must be a non-zero vector", function);
            return false;
        }
        if (!(maxDistance > 0.0f) || !std::isfinite(maxDistance)){
            Log::error("%s: maxDistance must be a positive finite number", function);
            return false;
        }
        ray = Ray(origin, direction * (maxDistance / length));
        return true;
    }

    void toDistance(RayReturn& hit, float maxDistance){
        if (hit.hit){
            hit.distance *= maxDistance;
        }
    }

}
#endif

namespace {
    std::atomic<bool> queriesHitTriggers3D{true};
    std::atomic<bool> queriesHitBackfaces3D{false};
    std::atomic<bool> queriesHitTriggers2D{true};
    std::atomic<bool> queriesStartInColliders2D{false};

#if defined(JASPERENGINE_PHYSICS_2D) || defined(JASPERENGINE_PHYSICS_3D)
    QueryOptions layerMaskOptions(uint32_t layerMask){
        QueryOptions options;
        options.layerMask = layerMask;
        return options;
    }
#endif
}

void Physics::setQueriesHitTriggers(bool hitTriggers){
    queriesHitTriggers3D.store(hitTriggers, std::memory_order_relaxed);
}

bool Physics::getQueriesHitTriggers(){
    return queriesHitTriggers3D.load(std::memory_order_relaxed);
}

void Physics::setQueriesHitBackfaces(bool hitBackfaces){
    queriesHitBackfaces3D.store(hitBackfaces, std::memory_order_relaxed);
}

bool Physics::getQueriesHitBackfaces(){
    return queriesHitBackfaces3D.load(std::memory_order_relaxed);
}

bool Physics::hitsTriggers(const QueryOptions& options){
    return options.triggers == QueryTriggerInteraction::COLLIDE ||
           (options.triggers == QueryTriggerInteraction::USE_GLOBAL && getQueriesHitTriggers());
}

bool Physics::hitsBackfaces(const QueryOptions& options){
    return options.hitBackfaces.value_or(getQueriesHitBackfaces());
}

void Physics2D::setQueriesHitTriggers(bool hitTriggers){
    queriesHitTriggers2D.store(hitTriggers, std::memory_order_relaxed);
}

bool Physics2D::getQueriesHitTriggers(){
    return queriesHitTriggers2D.load(std::memory_order_relaxed);
}

void Physics2D::setQueriesStartInColliders(bool startInColliders){
    queriesStartInColliders2D.store(startInColliders, std::memory_order_relaxed);
}

bool Physics2D::getQueriesStartInColliders(){
    return queriesStartInColliders2D.load(std::memory_order_relaxed);
}

bool Physics2D::hitsTriggers(const QueryOptions& options){
    return options.triggers == QueryTriggerInteraction::COLLIDE ||
           (options.triggers == QueryTriggerInteraction::USE_GLOBAL && getQueriesHitTriggers());
}

bool Physics2D::startsInColliders(const QueryOptions& options){
    return options.startInColliders.value_or(getQueriesStartInColliders());
}

#ifdef JASPERENGINE_PHYSICS_3D
RayReturn Physics::raycast(Scene* scene, const Ray& ray, uint32_t layerMask){
    return raycast(scene, ray, layerMaskOptions(layerMask));
}

RayReturn Physics::raycast(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask){
    return raycast(scene, origin, direction, maxDistance, layerMaskOptions(layerMask));
}

RayReturn Physics::raycast(Scene* scene, const Ray& ray, const QueryOptions& options){
    return ray.cast(scene, RayFilter::BODY_3D, -1, false, (uint16_t)~0u, (uint16_t)~0u, options.ignore, options.layerMask, nullptr, &options);
}

RayReturn Physics::raycast(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, const QueryOptions& options){
    Ray ray;
    if (!makeDistanceRay("Physics.raycast", origin, direction, maxDistance, ray)){
        return Ray::NO_HIT;
    }
    RayReturn hit = raycast(scene, ray, options);
    toDistance(hit, maxDistance);
    return hit;
}

std::vector<RayReturn> Physics::raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask){
    return raycastAll(scene, ray, layerMaskOptions(layerMask));
}

std::vector<RayReturn> Physics::raycastAll(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask){
    return raycastAll(scene, origin, direction, maxDistance, layerMaskOptions(layerMask));
}

std::vector<RayReturn> Physics::raycastAll(Scene* scene, const Ray& ray, const QueryOptions& options){
    std::vector<RayReturn> hits;
    ray.cast(scene, RayFilter::BODY_3D, -1, false, (uint16_t)~0u, (uint16_t)~0u, options.ignore, options.layerMask, &hits, &options);
    if (hits.size() > options.maxHits){
        hits.resize(options.maxHits);
    }
    return hits;
}

std::vector<RayReturn> Physics::raycastAll(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, const QueryOptions& options){
    Ray ray;
    if (!makeDistanceRay("Physics.raycastAll", origin, direction, maxDistance, ray)){
        return {};
    }
    std::vector<RayReturn> hits = raycastAll(scene, ray, options);
    for (RayReturn& hit : hits){
        toDistance(hit, maxDistance);
    }
    return hits;
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
RayReturn Physics2D::raycast(Scene* scene, const Ray& ray, uint32_t layerMask){
    return raycast(scene, ray, layerMaskOptions(layerMask));
}

RayReturn Physics2D::raycast(Scene* scene, Vector2 origin, Vector2 direction, float distance, uint32_t layerMask){
    return raycast(scene, origin, direction, distance, layerMaskOptions(layerMask));
}

RayReturn Physics2D::raycast(Scene* scene, const Ray& ray, const QueryOptions& options){
    return ray.cast(scene, RayFilter::BODY_2D, -1, false, (uint16_t)~0u, (uint16_t)~0u, options.ignore, options.layerMask, nullptr, &options);
}

RayReturn Physics2D::raycast(Scene* scene, Vector2 origin, Vector2 direction, float distance, const QueryOptions& options){
    Ray ray;
    if (!makeDistanceRay("Physics2D.raycast", Vector3(origin.x, origin.y, 0), Vector3(direction.x, direction.y, 0), distance, ray)){
        return Ray::NO_HIT;
    }
    RayReturn hit = raycast(scene, ray, options);
    toDistance(hit, distance);
    return hit;
}

std::vector<RayReturn> Physics2D::raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask){
    return raycastAll(scene, ray, layerMaskOptions(layerMask));
}

std::vector<RayReturn> Physics2D::raycastAll(Scene* scene, Vector2 origin, Vector2 direction, float distance, uint32_t layerMask){
    return raycastAll(scene, origin, direction, distance, layerMaskOptions(layerMask));
}

std::vector<RayReturn> Physics2D::raycastAll(Scene* scene, const Ray& ray, const QueryOptions& options){
    std::vector<RayReturn> hits;
    ray.cast(scene, RayFilter::BODY_2D, -1, false, (uint16_t)~0u, (uint16_t)~0u, options.ignore, options.layerMask, &hits, &options);
    if (hits.size() > options.maxHits){
        hits.resize(options.maxHits);
    }
    return hits;
}

std::vector<RayReturn> Physics2D::raycastAll(Scene* scene, Vector2 origin, Vector2 direction, float distance, const QueryOptions& options){
    Ray ray;
    if (!makeDistanceRay("Physics2D.raycastAll", Vector3(origin.x, origin.y, 0), Vector3(direction.x, direction.y, 0), distance, ray)){
        return {};
    }
    std::vector<RayReturn> hits = raycastAll(scene, ray, options);
    for (RayReturn& hit : hits){
        toDistance(hit, distance);
    }
    return hits;
}
#endif

namespace {
    std::atomic<bool> physicsDebugDraw{false};
    std::atomic<bool> physicsDebugDrawContacts{false};
}

void Physics::setDebugDraw(bool debugDraw){
    physicsDebugDraw.store(debugDraw, std::memory_order_relaxed);
}

bool Physics::isDebugDraw(){
    return physicsDebugDraw.load(std::memory_order_relaxed);
}

void Physics::setDebugDrawContacts(bool debugDrawContacts){
    physicsDebugDrawContacts.store(debugDrawContacts, std::memory_order_relaxed);
}

bool Physics::isDebugDrawContacts(){
    return physicsDebugDrawContacts.load(std::memory_order_relaxed);
}
