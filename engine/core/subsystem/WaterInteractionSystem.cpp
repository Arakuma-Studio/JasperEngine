// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterInteractionSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterWakeComponent.h"
#include "Log.h"
#include "object/environment/Water.h"
#include "subsystem/SplineSystem.h"
#include "util/MeshTriangles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

using namespace jasperengine;

namespace{

    // the model matrix of a GameObject's current world pose (the physics moves bodies between frames,
    // ahead of modelMatrix)
    Matrix4 waterPoseMatrix(const Transform& transform){
        return Matrix4::translateMatrix(transform.worldPosition) * transform.worldRotation.getRotationMatrix() *
               Matrix4::scaleMatrix(transform.worldScale);
    }

    // a mesh node into its model's space, through the local transforms between them
    Matrix4 waterMeshToEntity(Scene* scene, Entity entity, Entity mesh){
        Matrix4 matrix;
        Entity current = mesh;
        for (int hops = 0; current != NULL_ENTITY && current != entity && hops < 64; hops++){
            const Transform* transform = scene->findComponent<Transform>(current);
            if (!transform)
                break;
            matrix = transform->localMatrix * matrix;
            current = transform->parent;
        }
        return matrix;
    }

    uint64_t waterHashMix(uint64_t hash, uint64_t value){
        for (int i = 0; i < 8; i++){
            hash ^= (value >> (i * 8)) & 0xFF;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    uint64_t waterHashFloat(uint64_t hash, float value){
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return waterHashMix(hash, bits);
    }

    // the geometry the hull of an excluder is built from: its mesh sources, their place in it and
    // their vertex ranges (a rebuild into the same buffer still moves the bounds)
    uint64_t waterHullSignature(Scene* scene, Entity entity, const std::vector<Entity>& sources){
        uint64_t hash = 1469598103934665603ull;
        for (Entity source : sources){
            hash = waterHashMix(hash, source);
            const Matrix4 toEntity = waterMeshToEntity(scene, entity, source);
            for (int i = 0; i < 16; i++)
                hash = waterHashFloat(hash, static_cast<const float*>(toEntity)[i]);
            MeshComponent* mesh = scene->findComponent<MeshComponent>(source);
            for (const MeshTriangles::MeshSourceView& view : MeshTriangles::collectMeshSourceViews(mesh)){
                hash = waterHashMix(hash, (uint64_t)(uintptr_t)(view.vertexBuffer ? view.vertexBuffer->getData() : nullptr));
                hash = waterHashMix(hash, view.vertexAttr.getOffset());
                hash = waterHashMix(hash, view.vertexAttr.getCount());
            }
            for (int c = 0; c < 3; c++){
                hash = waterHashFloat(hash, mesh->verticesAABB.getMinimum()[c]);
                hash = waterHashFloat(hash, mesh->verticesAABB.getMaximum()[c]);
            }
        }
        return hash;
    }

    // a smoothed velocity reaches 63% of a new one in this many seconds
    constexpr float WAKE_VELOCITY_TAU = 0.15f;
    // history kept per emitter
    constexpr size_t WAKE_HISTORY_SAMPLES = 256;
    constexpr float WAKE_HISTORY_SECONDS = 60.0f;
    // seconds between two stamps of the wake crests
    constexpr float WAKE_CREST_PERIOD_MIN = 0.05f;
    constexpr float WAKE_CREST_PERIOD_MAX = 0.25f;

}

WaterInteractionSystem::WaterInteractionSystem(Scene* scene): SubSystem(scene){
    declareHookType(scene->getComponentId<WaterExcluderComponent>());
    declareHookType(scene->getComponentId<WaterWakeComponent>());
}

int WaterInteractionSystem::getWakeMapSize(int resolution){
    switch ((WaterWakeResolution)resolution){
        case WaterWakeResolution::LOW: return 128;
        case WaterWakeResolution::MEDIUM: return 256;
        case WaterWakeResolution::HIGH: return 512;
        case WaterWakeResolution::AUTO: break;
    }
    const Platform platform = Engine::getPlatform();
    return (platform == Platform::Android || platform == Platform::iOS || platform == Platform::Web) ? 128 : 256;
}

bool WaterInteractionSystem::isActive(Entity entity, bool componentEnabled) const{
    if (!componentEnabled || !scene->isEntityActiveInHierarchy(entity))
        return false;
    // hidden (Transform visible off, which its children inherit): keeps nothing out, leaves no trail
    const Transform* transform = scene->findComponent<Transform>(entity);
    return transform && transform->visible;
}

bool WaterInteractionSystem::updateHull(Entity entity, HullCache& cache){
    const std::vector<Entity> sources = MeshTriangles::meshSourceEntities(scene, entity);
    if (sources.empty()){
        const WaterHullStatus status = MeshTriangles::isGeometryLoading(scene, entity) ? WaterHullStatus::PENDING : WaterHullStatus::NO_MESH;
        const bool changed = cache.status != status || !cache.map.empty();
        cache = HullCache();
        cache.status = status;
        cache.seen = true;
        return changed;
    }
    const uint64_t signature = waterHullSignature(scene, entity, sources);
    if (signature == cache.signature && cache.status != WaterHullStatus::NONE && cache.status != WaterHullStatus::PENDING)
        return false;
    if (MeshTriangles::isGeometryLoading(scene, entity)){
        cache.status = WaterHullStatus::PENDING;
        return false;
    }

    std::vector<Vector3> points;
    for (Entity source : sources){
        const Matrix4 toEntity = waterMeshToEntity(scene, entity, source);
        std::vector<MeshTriangles::VertexRange> added;
        for (MeshTriangles::MeshSourceView& view : MeshTriangles::collectMeshSourceViews(scene->findComponent<MeshComponent>(source))){
            const MeshTriangles::VertexRange range = MeshTriangles::vertexRange(view);
            if (!view.vertexBuffer || std::find(added.begin(), added.end(), range) != added.end())
                continue;
            added.push_back(range);
            const unsigned int count = view.vertexAttr.getCount();
            for (unsigned int i = 0; i < count; i++)
                points.push_back(toEntity * view.vertexBuffer->getVector3(&view.vertexAttr, i));
        }
    }

    cache.signature = signature;
    cache.status = buildWaterHull(points, cache.hull);
    if (cache.status == WaterHullStatus::READY){
        buildWaterHullMap(cache.hull, cache.map);
    }else{
        cache.map.clear();
        if (cache.status == WaterHullStatus::NO_PHYSICS)
            Log::warn("Water Excluder of entity %u: this build has no 3D physics, its Convex Hull is the mesh's bounds box", (unsigned int)entity);
    }
    hullBuilds++;
    return true;
}

void WaterInteractionSystem::gatherExcluders(){
    excludersGathered = true;
    frameExcluders.clear();
    auto components = scene->getComponentArray<WaterExcluderComponent>();

    bool layersChanged = false;
    for (auto& entry : hulls)
        entry.second.seen = false;

    std::vector<Entity> layers;
    for (size_t i = 0; i < components->size(); i++){
        const WaterExcluderComponent& component = components->getComponentFromIndex(i);
        const Entity entity = components->getEntity(i);
        const Transform* transform = scene->findComponent<Transform>(entity);

        const WaterHull* hull = nullptr;
        bool hullMap = false;
        int layer = -1;
        if (component.shape == WaterExcluderShape::CONVEX_HULL){
            HullCache& cache = hulls[entity];
            cache.seen = true;
            if (updateHull(entity, cache))
                layersChanged = true;
            if (cache.status == WaterHullStatus::READY && !cache.map.empty()){
                hull = &cache.hull;
                hullMap = true;
                layer = (int)layers.size();
                layers.push_back(entity);
            }else if (cache.status == WaterHullStatus::NO_PHYSICS){
                hull = &cache.hull;
            }
            cache.layer = layer;
        }

        if (!transform || !isActive(entity, component.enabled))
            continue;
        if (component.shape == WaterExcluderShape::CONVEX_HULL && !hull)
            continue;

        Excluder excluder;
        excluder.entity = entity;
        excluder.shape = component.shape;
        excluder.hullMap = hullMap;
        excluder.localBox = getWaterExcluderLocalBox(component.shape, component.center, component.size, hull);
        excluder.modelMatrix = transform->modelMatrix;
        excluder.hullLayer = layer;
        if (!getWaterExcluderUnitSpace(excluder.localBox, hullMap, excluder.modelMatrix, excluder.worldToUnit))
            continue;
        excluder.worldBounds = excluder.modelMatrix * excluder.localBox;
        frameExcluders.push_back(excluder);
    }

    // hulls of removed excluders or of ones that are no longer hulls
    for (auto it = hulls.begin(); it != hulls.end();){
        if (!it->second.seen){
            it = hulls.erase(it);
            layersChanged = true;
        }else{
            ++it;
        }
    }
    if (layersChanged || layers != hullLayers){
        hullLayers = layers;
        hullMapsVersion++;
    }
}

void WaterInteractionSystem::releaseExcluders(){
    if (!hulls.empty() || !hullLayers.empty())
        hullMapsVersion++;
    hulls.clear();
    hullLayers.clear();
    frameExcluders.clear();
    excludersGathered = true;
}

const std::vector<WaterInteractionSystem::Excluder>& WaterInteractionSystem::getExcluders(){
    if (!excludersGathered)
        gatherExcluders();
    return frameExcluders;
}

Entity WaterInteractionSystem::getExcluderOwner(Scene* scene, Entity excluder){
    Entity current = excluder;
    for (int hops = 0; current != NULL_ENTITY && hops < 256; hops++){
        if (scene->findComponent<RigidbodyComponent>(current))
            return current;
        const Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return NULL_ENTITY;
}

AABB WaterInteractionSystem::getMeshSourceLocalBounds(Scene* scene, Entity entity){
    AABB bounds;
    for (Entity source : MeshTriangles::meshSourceEntities(scene, entity)){
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(source);
        if (mesh && !mesh->aabb.isNull())
            bounds.merge(waterMeshToEntity(scene, entity, source) * mesh->aabb);
    }
    return bounds;
}

Entity WaterInteractionSystem::findExcluder(const Vector3& point, Entity floatingBody){
    if (scene->getComponentArray<WaterExcluderComponent>()->size() == 0)
        return NULL_ENTITY;
    for (const Excluder& excluder : getExcluders()){
        const Transform* transform = scene->findComponent<Transform>(excluder.entity);
        if (!transform)
            continue;
        const Matrix4 model = waterPoseMatrix(*transform);
        const AABB bounds = model * excluder.localBox;
        if (!bounds.contains(point))
            continue;
        const auto found = hulls.find(excluder.entity);
        const WaterHull* hull = (found != hulls.end() && excluder.shape == WaterExcluderShape::CONVEX_HULL) ? &found->second.hull : nullptr;
        if (!isInsideWaterExcluder(excluder.shape, excluder.localBox, hull, model, point))
            continue;
        if (floatingBody != NULL_ENTITY && getExcluderOwner(scene, excluder.entity) == floatingBody)
            continue;
        return excluder.entity;
    }
    return NULL_ENTITY;
}

bool WaterInteractionSystem::contains(Entity excluder, const Vector3& point){
    const WaterExcluderComponent* component = scene->findComponent<WaterExcluderComponent>(excluder);
    const Transform* transform = scene->findComponent<Transform>(excluder);
    if (!component || !transform)
        return false;
    const WaterHull* hull = nullptr;
    if (component->shape == WaterExcluderShape::CONVEX_HULL){
        HullCache& cache = hulls[excluder];
        cache.seen = true;
        if (updateHull(excluder, cache))
            hullMapsVersion++;
        if (cache.status != WaterHullStatus::READY && cache.status != WaterHullStatus::NO_PHYSICS)
            return false;
        hull = &cache.hull;
    }
    const AABB localBox = getWaterExcluderLocalBox(component->shape, component->center, component->size, hull);
    return isInsideWaterExcluder(component->shape, localBox, hull, waterPoseMatrix(*transform), point);
}

WaterInteractionSystem::HullInfo WaterInteractionSystem::getHullInfo(Entity excluder) const{
    HullInfo info;
    const auto found = hulls.find(excluder);
    if (found == hulls.end())
        return info;
    info.status = found->second.status;
    info.faces = found->second.hull.planes.size();
    info.sourceVertices = found->second.hull.sourceVertices;
    info.localBounds = found->second.hull.bounds;
    return info;
}

const WaterHull* WaterInteractionSystem::getHull(Entity excluder) const{
    const auto found = hulls.find(excluder);
    if (found == hulls.end() || (found->second.status != WaterHullStatus::READY && found->second.status != WaterHullStatus::NO_PHYSICS))
        return nullptr;
    return &found->second.hull;
}

const std::vector<uint8_t>* WaterInteractionSystem::getHullMap(Entity excluder) const{
    const auto found = hulls.find(excluder);
    if (found == hulls.end() || found->second.map.empty())
        return nullptr;
    return &found->second.map;
}

WaterInteractionSystem::EmitterInfo WaterInteractionSystem::getEmitterInfo(Entity emitter) const{
    EmitterInfo info;
    const auto found = emitters.find(emitter);
    if (found == emitters.end())
        return info;
    info.emitting = found->second.emitting;
    info.speed = found->second.track.velocity.length();
    info.water = found->second.water;
    return info;
}

WaterInteractionSystem::WakeMapInfo WaterInteractionSystem::getWakeMapInfo() const{
    WakeMapInfo info;
    info.active = wakeActive;
    info.size = wakeMap.getSize();
    info.area = (float)wakeMap.getSize() * wakeMap.getTexel();
    info.corner = wakeMap.getCorner();
    return info;
}

uint8_t WaterInteractionSystem::getBodyTag(Entity waterMesh) const{
    if (!wakeActive)
        return 0;
    const auto body = meshBodies.find(waterMesh);
    const Entity key = body != meshBodies.end() ? body->second : waterMesh;
    const auto tag = bodyTags.find(key);
    return tag != bodyTags.end() ? tag->second : 0;
}

bool WaterInteractionSystem::sampleWake(float x, float z, Entity waterEntity, float& foam, float& crest) const{
    foam = 0.0f;
    crest = 0.0f;
    const uint8_t tag = getBodyTag(waterEntity);
    return tag != 0 && wakeMap.sample(x, z, tag, foam, crest);
}

uint8_t WaterInteractionSystem::tagOf(Entity key){
    const auto found = bodyTags.find(key);
    if (found != bodyTags.end())
        return found->second;
    if (bodyTags.size() >= 255){
        if (!bodyTagsWarned){
            bodyTagsWarned = true;
            Log::warn("Water Wakes: more than 255 waters hold wakes at once, the others show none");
        }
        return 0;
    }
    const uint8_t tag = (uint8_t)(bodyTags.size() + 1);
    bodyTags[key] = tag;
    return tag;
}

void WaterInteractionSystem::releaseWakes(){
    crestTimer = 0.0f;
    emitters.clear();
    wakeMap = WaterWakeMap();
    wakeActive = false;
    wakeDirty = false;
    bodyTags.clear();
    meshBodies.clear();
}

void WaterInteractionSystem::stepWakes(float dt){
    auto wakes = scene->getComponentArray<WaterWakeComponent>();
    wakeSteps++;
    const float time = Water::getTime(scene);
    wakeFade = scene->getWaterWakeFade();

    // the drawn waters and their bodies: library:water meshes at one level whose areas touch, and
    // the chunks of a spline's river or lake (its GameObject)
    std::vector<WaterSurface> surfaces = Water::getSurfaces(scene);
    std::vector<Entity> surfaceKeys(surfaces.size(), NULL_ENTITY);
    meshBodies.clear();
    for (size_t i = 0; i < surfaces.size(); i++){
        if (surfaceKeys[i] != NULL_ENTITY)
            continue;
        // the group of i: every surface reachable through touching areas at its level
        std::vector<size_t> group{i};
        surfaceKeys[i] = surfaces[i].entity;
        for (size_t g = 0; g < group.size(); g++){
            const WaterSurface& a = surfaces[group[g]];
            for (size_t j = 0; j < surfaces.size(); j++){
                if (surfaceKeys[j] != NULL_ENTITY)
                    continue;
                const WaterSurface& b = surfaces[j];
                const float gap = 0.01f;
                if (std::fabs(a.level - b.level) > gap || b.minXZ.x > a.maxXZ.x + gap || a.minXZ.x > b.maxXZ.x + gap ||
                        b.minXZ.y > a.maxXZ.y + gap || a.minXZ.y > b.maxXZ.y + gap)
                    continue;
                surfaceKeys[j] = surfaces[i].entity;
                group.push_back(j);
            }
        }
        Entity key = surfaces[group[0]].entity;
        for (size_t member : group)
            key = std::min(key, surfaces[member].entity);
        for (size_t member : group)
            surfaceKeys[member] = key;
    }
    for (size_t i = 0; i < surfaces.size(); i++)
        meshBodies[surfaces[i].entity] = surfaceKeys[i];
    std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>();
    if (splines && splines->hasWaterSurfaces()){
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            const Entity entity = meshes->getEntity(i);
            if (splines->isGenerated(entity))
                meshBodies[entity] = splines->getGeneratedOwner(entity);
        }
    }

    // the body over a world XZ whose surface lies within [bottom, top]
    auto findBody = [&](float x, float z, float bottom, float top) -> Entity {
        if (splines && splines->hasWaterSurfaces()){
            const SplineWaterSample water = splines->sampleWaterSurface(x, z);
            if (water.hit && water.height >= bottom && water.height <= top)
                return water.owner;
        }
        Entity found = NULL_ENTITY;
        float foundLevel = -std::numeric_limits<float>::infinity();
        for (size_t i = 0; i < surfaces.size(); i++){
            const WaterSurface& surface = surfaces[i];
            if (!surface.covers(x, z) || surface.level <= foundLevel)
                continue;
            const float height = surface.getHeight(x, z, time);
            if (height < bottom || height > top)
                continue;
            found = surfaceKeys[i];
            foundLevel = surface.level;
        }
        return found;
    };

    for (auto& entry : emitters)
        entry.second.seen = false;

    // the emitters: their motion and the bow positions they leave on a water
    bool crossing = false;
    struct Stamping{ WaterWakeTrack* track; WaterWakeFootprint footprint; float foam; float waves; };
    std::vector<Stamping> stampings;
    for (size_t i = 0; i < wakes->size(); i++){
        const WaterWakeComponent& wake = wakes->getComponentFromIndex(i);
        const Entity entity = wakes->getEntity(i);
        Emitter& emitter = emitters[entity];
        emitter.seen = true;
        emitter.emitting = false;
        WaterWakeTrack& track = emitter.track;
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !isActive(entity, wake.enabled) || (wake.foam <= 0.0f && wake.waves <= 0.0f)){
            // hidden, inactive or disabled: no trail from where it was to where it shows again
            track.reset();
            emitter.water = NULL_ENTITY;
            continue;
        }

        // its footprint: Width and Length, else its Water Excluder's box, else its meshes' bounds
        WaterWakeFootprint footprint;
        const Vector3 axisX = transform->worldRotation * Vector3(1.0f, 0.0f, 0.0f);
        const Vector3 axisZ = transform->worldRotation * Vector3(0.0f, 0.0f, 1.0f);
        footprint.axisX = Vector2(axisX.x, axisX.z).squaredLength() > 1.0e-6f ? Vector2(axisX.x, axisX.z).normalized() : Vector2(1.0f, 0.0f);
        footprint.axisZ = Vector2(axisZ.x, axisZ.z).squaredLength() > 1.0e-6f ? Vector2(axisZ.x, axisZ.z).normalized() : Vector2(0.0f, 1.0f);
        Vector3 center = transform->worldPosition;
        float bottom = center.y - 0.5f;
        float top = center.y + 0.5f;
        AABB localBox;
        if (const WaterExcluderComponent* excluder = scene->findComponent<WaterExcluderComponent>(entity)){
            const auto hull = hulls.find(entity);
            localBox = getWaterExcluderLocalBox(excluder->shape, excluder->center, excluder->size,
                                                hull != hulls.end() ? &hull->second.hull : nullptr);
        }
        if (localBox.isNull())
            localBox = getMeshSourceLocalBounds(scene, entity);
        float radiusX = 0.5f;
        float radiusZ = 0.5f;
        if (!localBox.isNull()){
            const Vector3 size = localBox.getSize();
            radiusX = 0.5f * size.x * std::fabs(transform->worldScale.x);
            radiusZ = 0.5f * size.z * std::fabs(transform->worldScale.z);
            const AABB world = transform->modelMatrix * localBox;
            center = world.getCenter();
            bottom = world.getMinimum().y;
            top = world.getMaximum().y;
        }
        if (wake.width > 0.0f)
            radiusX = 0.5f * wake.width;
        if (wake.length > 0.0f || wake.width > 0.0f)
            radiusZ = 0.5f * (wake.length > 0.0f ? wake.length : wake.width);
        footprint.radiusX = std::max(radiusX, 0.05f);
        footprint.radiusZ = std::max(radiusZ, 0.05f);

        if (!track.tracked){
            track.lastPosition = center;
            track.tracked = true;
            continue;
        }
        const Vector2 move(center.x - track.lastPosition.x, center.z - track.lastPosition.z);
        track.lastPosition = center;
        // a teleport would draw a wake across the water
        if (move.length() > std::max(5.0f, track.velocity.length() * dt * 4.0f + 1.0f)){
            track.reset();
            track.lastPosition = center;
            track.tracked = true;
            emitter.water = NULL_ENTITY;
            continue;
        }
        track.velocity = track.velocity + (move / dt - track.velocity) * (1.0f - std::exp(-dt / WAKE_VELOCITY_TAU));
        track.time += dt;

        emitter.water = findBody(center.x, center.z, bottom, top);
        track.body = emitter.water != NULL_ENTITY ? tagOf(emitter.water) : 0;
        const float speed = track.velocity.length();
        if (track.body != 0 && speed > 0.05f){
            crossing = true;
            emitter.emitting = true;
            const Vector2 direction = track.velocity / speed;
            float halfLength;
            float halfWidth;
            footprint.along(direction, halfLength, halfWidth);
            const Vector2 bow = Vector2(center.x, center.z) + direction * halfLength;
            // (twice Doriax's spacing: the step's cost is the history's stamps)
            if (track.history.empty() || (bow - track.history.back().position).length() >= std::max(0.5f, halfLength * 0.2f)){
                WaterWakeSample sample;
                sample.position = bow;
                sample.direction = direction;
                sample.speed = speed;
                sample.time = track.time;
                sample.body = track.body;
                track.history.push_back(sample);
            }
        }else{
            track.sternValid = false;
        }
        while (track.history.size() > WAKE_HISTORY_SAMPLES ||
               (!track.history.empty() && track.time - track.history.front().time > WAKE_HISTORY_SECONDS)){
            track.history.pop_front();
        }
        if (!track.history.empty())
            stampings.push_back({&track, footprint, wake.foam, wake.waves});
    }
    for (auto it = emitters.begin(); it != emitters.end();){
        it = it->second.seen ? std::next(it) : emitters.erase(it);
    }

    if (!crossing && !wakeActive && stampings.empty())
        return;

    // the map, around what the main camera looks at, never far from it
    const int size = getWakeMapSize((int)scene->getWaterWakeResolution());
    const float texel = scene->getWaterWakeArea() / (float)size;
    if (wakeMap.getSize() != size || wakeMap.getTexel() != texel){
        wakeMap.reset(size, texel);
        wakeMap.moveTo(0, 0);
    }
    Vector3 eye;
    Vector2 forward;
    if (const Transform* camera = scene->findComponent<Transform>(scene->getCamera())){
        eye = camera->worldPosition;
        if (const CameraComponent* component = scene->findComponent<CameraComponent>(scene->getCamera()))
            forward = Vector2(component->worldDirection.x, component->worldDirection.z);
    }
    const float reach = 0.25f * (float)size * texel;
    forward = forward.squaredLength() > 1.0e-6f ? forward.normalized() * reach : Vector2(0.0f, 0.0f);
    const int originX = (int)std::floor((eye.x + forward.x) / texel) - size / 2;
    const int originZ = (int)std::floor((eye.z + forward.y) / texel) - size / 2;
    wakeMap.moveTo(originX, originZ);

    // the crests are stamped again once they may have moved half a texel (they spread at about
    // 2 m/s: 16 times a second at 256 texels over 64 m, 8 at 128), the foam every step. An eighth
    // is left after the fade time, which shows as scattered foam
    crestTimer -= dt;
    const bool withCrests = crestTimer <= 0.0f;
    if (withCrests)
        crestTimer = std::clamp(texel * 0.25f, WAKE_CREST_PERIOD_MIN, WAKE_CREST_PERIOD_MAX);
    wakeMap.fade(std::exp(-2.0f * dt / wakeFade), withCrests);
    for (Stamping& stamping : stampings)
        wakeMap.stampTrack(*stamping.track, stamping.footprint, stamping.foam, stamping.waves, wakeFade, dt, withCrests);

    float strongest = 0.0f;
    wakeMap.encode(strongest);
    wakeActive = strongest > 0.004f || crossing;
    wakeDirty = true;
    if (!wakeActive){
        // faded out: the next wake starts on fresh tags
        bodyTags.clear();
        for (auto& entry : emitters)
            entry.second.track.history.clear();
    }
}

void WaterInteractionSystem::load(){
}

void WaterInteractionSystem::draw(){
}

void WaterInteractionSystem::destroy(){
    releaseExcluders();
    releaseWakes();
}

void WaterInteractionSystem::update(double dt){
    const bool anyExcluder = scene->getComponentArray<WaterExcluderComponent>()->size() > 0;
    const bool anyWake = scene->getComponentArray<WaterWakeComponent>()->size() > 0;

    if (anyExcluder){
        gatherExcluders();
    }else if (!hulls.empty() || !frameExcluders.empty() || !hullLayers.empty()){
        releaseExcluders();
    }

    // wakes advance with the scene's game time, only while it runs
    const bool running = anyWake && Engine::isSceneRunning(scene) && !paused && dt > 0.0;
    if (running){
        stepWakes((float)dt);
    }else if (!Engine::isSceneRunning(scene) || !anyWake){
        if (!emitters.empty() || wakeMap.isAllocated())
            releaseWakes();
    }
}

void WaterInteractionSystem::onComponentAdded(Entity entity, ComponentId componentId){
    (void)entity;
    // a query before the next update gathers again
    if (componentId == scene->getComponentId<WaterExcluderComponent>())
        excludersGathered = false;
}

void WaterInteractionSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId == scene->getComponentId<WaterExcluderComponent>()){
        if (hulls.erase(entity))
            hullMapsVersion++;
        excludersGathered = false;
    }else if (componentId == scene->getComponentId<WaterWakeComponent>()){
        emitters.erase(entity);
    }
}
