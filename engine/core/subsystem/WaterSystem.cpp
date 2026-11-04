// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/BuoyancyComponent.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/WaterVolumeComponent.h"
#include "object/environment/Water.h"
#include "shader/ShaderManifest.h"
#include "texture/Material.h"
#include "util/WaterWaves.h"
#include "subsystem/SplineSystem.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#include "subsystem/PhysicsSystem.h"
#include "Jolt/Physics/Body/BodyLock.h"
#include "Jolt/Physics/Body/BodyLockInterface.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>

using namespace jasperengine;

namespace{

    std::atomic<uint64_t> waveEvaluations{0};

    WaterWaveSample evaluate(const WaterWaveParams& waves, float x, float z, float time){
        waveEvaluations.fetch_add(1, std::memory_order_relaxed);
        return waterSurfaceAt(waves, Vector2(x, z), time);
    }

}

uint64_t WaterSystem::getWaveEvaluations(){
    return waveEvaluations.load(std::memory_order_relaxed);
}

WaterSystem::WaterSystem(Scene* scene, PhysicsSystem* physics): scene(scene), physics(physics){
}

void WaterSystem::gatherVolumes() const{
    volumes.clear();
    auto components = scene->getComponentArray<WaterVolumeComponent>();
    if (components->size() == 0)
        return;
    time = Water::getTime(scene);
    for (size_t i = 0; i < components->size(); i++){
        const WaterVolumeComponent& component = components->getComponentFromIndex(i);
        const Entity entity = components->getEntity(i);
        if (!component.enabled || !scene->isEntityActiveInHierarchy(entity))
            continue;
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;

        Volume volume;
        volume.entity = entity;
        // the surface: the GameObject's world height at surfaceOffset
        volume.surface = (transform->modelMatrix * Vector3(0.0f, component.surfaceOffset, 0.0f)).y;
        volume.worldToLocal = transform->modelMatrix.inverse();
        volume.halfX = std::fabs(component.size.x) * 0.5f;
        volume.halfZ = std::fabs(component.size.z) * 0.5f;
        volume.depth = std::fabs(component.size.y * transform->worldScale.y);
        volume.infinite = component.infinite;
        volume.density = std::max(0.0f, component.density);
        volume.flow = component.flow;
        volume.linearDrag = std::max(0.0f, component.linearDrag);
        volume.angularDrag = std::max(0.0f, component.angularDrag);
        if (std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>()){
            volume.spline = splines->hasWaterSurface(entity);
        }

        // the waves are the material's (R30): never copied into the volume
        if (component.waves == WaterVolumeWaves::FROM_MATERIAL){
            if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity); mesh && mesh->numSubmeshes > 0){
                // library:water or a fork of it (@role water); without WAVES a flat surface
                WaterWaveParams waves;
                if (Water::readWaves(mesh->submeshes[0].material, waves) && waves.waves)
                    volume.waves = waves;
            }
        }
        volumes.push_back(volume);
    }
}

const WaterSystem::Volume* WaterSystem::findVolume(float x, float y, float z, float& height, WaterWaveSample& wave, Vector3& flow) const{
    const Volume* found = nullptr;
    std::shared_ptr<SplineSystem> splines;
    for (const Volume& volume : volumes){
        float surface = volume.surface;
        Vector3 surfaceFlow = volume.flow;
        if (volume.spline){
            // a spline river or lake: its surface where it has water, depth below it
            if (!splines){
                splines = scene->findSystem<SplineSystem>();
            }
            const SplineWaterSample water = splines ? splines->sampleWaterSurface(x, z, volume.entity) : SplineWaterSample();
            if (!water.hit || y < water.height - volume.depth)
                continue;
            surface = water.height;
            surfaceFlow = volume.flow + water.flow;
        }else if (!volume.infinite){
            const Vector3 local = volume.worldToLocal * Vector3(x, volume.surface, z);
            if (std::fabs(local.x) > volume.halfX || std::fabs(local.z) > volume.halfZ)
                continue;
            if (y < volume.surface - volume.depth)
                continue;
        }
        WaterWaveSample surfaceWave;
        if (volume.waves.waves && volume.waves.height != 0.0f){
            surfaceWave = evaluate(volume.waves, x, z, time);
            surface += surfaceWave.height;
        }
        // overlapping volumes: the highest surface wins
        if (!found || surface > height){
            found = &volume;
            height = surface;
            wave = surfaceWave;
            flow = surfaceFlow;
        }
    }
    return found;
}

WaterSample WaterSystem::sample(const Vector3& point) const{
    WaterSample result;
    gatherVolumes();
    float height = 0.0f;
    WaterWaveSample wave;
    Vector3 flow;
    if (const Volume* volume = findVolume(point.x, point.y, point.z, height, wave, flow)){
        result.volume = volume->entity;
        result.surfaceHeight = height;
        result.inWater = point.y <= height;
        result.normal = wave.normal;
        // the current plus the waves' motion (rising and falling; rolling with GERSTNER)
        result.flowVelocity = flow + wave.velocity;
        return result;
    }
    // a spline river or lake without a Water Volume (SplineSystem::sampleWaterSurface)
    if (std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>(); splines && splines->hasWaterSurfaces()){
        const SplineWaterSample water = splines->sampleWaterSurface(point.x, point.z);
        if (water.hit){
            result.volume = water.owner;
            result.surfaceHeight = water.height;
            result.inWater = point.y <= water.height;
            result.normal = water.normal;
            result.flowVelocity = water.flow;
            return result;
        }
    }
    // a library:water mesh without a Water Volume: its surface over its bounds (Water.h)
    const std::vector<WaterSurface> surfaces = Water::getSurfaces(scene);
    if (const WaterSurface* surface = Water::findSurface(surfaces, point)){
        result.volume = surface->entity;
        result.surfaceHeight = surface->level;
        if (surface->waves.waves){
            const WaterWaveSample wave = evaluate(surface->waves, point.x, point.z, Water::getTime(scene));
            result.surfaceHeight += wave.height;
            result.normal = wave.normal;
            result.flowVelocity = wave.velocity;
        }
        result.inWater = point.y <= result.surfaceHeight;
    }
    return result;
}

float WaterSystem::sampleHeight(float x, float z) const{
    const WaterSample result = sample(Vector3(x, std::numeric_limits<float>::infinity(), z));
    return result.hasWater() ? result.surfaceHeight : std::numeric_limits<float>::quiet_NaN();
}

void WaterSystem::applyBuoyancy(float dt){
#ifdef JASPERENGINE_PHYSICS_3D
    auto buoyancies = scene->getComponentArray<BuoyancyComponent>();
    // nothing floats, or nothing to float in: no work at all (R37)
    if (buoyancies->size() == 0 || scene->getComponentArray<WaterVolumeComponent>()->size() == 0)
        return;
    gatherVolumes();
    if (volumes.empty())
        return;

    const float gravity = physics->getGravity3D().length();
    JPH::BodyInterface& bodies = physics->getBodyInterface3D();
    // between steps, on the thread that steps: one body lock each, no mutex (R37's budget)
    const JPH::BodyLockInterface& locks = physics->world3D.GetBodyLockInterfaceNoLock();
    for (size_t i = 0; i < buoyancies->size(); i++){
        const BuoyancyComponent& buoyancy = buoyancies->getComponentFromIndex(i);
        const Entity entity = buoyancies->getEntity(i);
        if (!buoyancy.enabled || !scene->isEntityActiveInHierarchy(entity))
            continue;
        const RigidbodyComponent* rigidbody = scene->findComponent<RigidbodyComponent>(entity);
        if (!rigidbody || !rigidbody->enabled || rigidbody->type != BodyType::DYNAMIC || rigidbody->body.IsInvalid())
            continue;

        const JPH::BodyID id = rigidbody->body;
        JPH::BodyLockWrite lock(locks, id);
        if (!lock.Succeeded())
            continue;
        JPH::Body& body = lock.GetBody();
        // out of the world (disabled, inactive GameObject) it does not float
        if (!body.IsInBroadPhase())
            continue;
        const JPH::Shape* shape = body.GetShape();
        if (!shape)
            continue;
        // the shape's frame is the centre of mass's
        const JPH::RMat44 frame = body.GetCenterOfMassTransform();
        const JPH::Quat rotation = body.GetRotation();
        const JPH::AABox bounds = shape->GetLocalBounds();

        // The columns: AUTO grids sit on the face of the colliders' bounds that faces down now
        // (a body that rolls over keeps its columns under it) and run up the bounds along that
        // axis; CUSTOM points stand along the body's own up. A column's height is its length
        // seen vertically: how deep in it the water can reach.
        const JPH::Vec3 localDown = rotation.Conjugated() * JPH::Vec3(0.0f, -1.0f, 0.0f);
        int axis = 1;
        float sign = 1.0f;
        if (buoyancy.columns != BuoyancyColumns::CUSTOM){
            const JPH::Vec3 a = localDown.Abs();
            axis = a.GetX() > a.GetY() ? (a.GetX() > a.GetZ() ? 0 : 2) : (a.GetY() > a.GetZ() ? 1 : 2);
            sign = localDown[axis] > 0.0f ? -1.0f : 1.0f;   // the up of the column, along +axis or -axis
        }
        const float columnLength = bounds.mMax[axis] - bounds.mMin[axis];
        const float columnHeight = columnLength * std::fabs(localDown[axis]);
        if (!(columnHeight > 0.0f))
            continue;

        JPH::Vec3 bottoms[9];
        int columns = 0;
        if (buoyancy.columns == BuoyancyColumns::CUSTOM){
            const Transform* transform = scene->findComponent<Transform>(entity);
            const Vector3 scale = transform ? transform->worldScale : Vector3(1.0f, 1.0f, 1.0f);
            const size_t count = std::min<size_t>(buoyancy.customPoints.size(), 9);
            for (size_t c = 0; c < count; c++){
                const Vector3& p = buoyancy.customPoints[c];
                bottoms[columns++] = JPH::Vec3(p.x * scale.x, p.y * scale.y, p.z * scale.z);
            }
        }else{
            const int n = buoyancy.columns == BuoyancyColumns::AUTO_3x3 ? 3 : 2;
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            const float face = sign > 0.0f ? bounds.mMin[axis] : bounds.mMax[axis];
            for (int a = 0; a < n; a++){
                for (int b = 0; b < n; b++){
                    JPH::Vec3 point;
                    point.SetComponent(axis, face);
                    point.SetComponent(u, bounds.mMin[u] + (bounds.mMax[u] - bounds.mMin[u]) * (a + 0.5f) / n);
                    point.SetComponent(v, bounds.mMin[v] + (bounds.mMax[v] - bounds.mMin[v]) * (b + 0.5f) / n);
                    bottoms[columns++] = point;
                }
            }
        }
        if (columns == 0)
            continue;
        const float columnVolume = shape->GetVolume() * std::max(0.0f, buoyancy.volumeScale) / columns;

        // the columns' submerged parts first: a body none of whose columns is in the water gets nothing
        struct Column{
            JPH::RVec3 point;
            float depth;
            const Volume* volume;
            Vector3 waterVelocity;
        } work[9];
        int submerged = 0;
        bool movingWater = false;
        for (int c = 0; c < columns; c++){
            const JPH::RVec3 bottom = frame * bottoms[c];
            float height = 0.0f;
            WaterWaveSample wave;
            Vector3 flow;
            const Volume* volume = findVolume((float)bottom.GetX(), (float)bottom.GetY(), (float)bottom.GetZ(), height, wave, flow);
            if (!volume)
                continue;
            const float depth = std::clamp(height - (float)bottom.GetY(), 0.0f, columnHeight);
            if (depth <= 0.0f)
                continue;
            Column& column = work[submerged++];
            column.point = bottom + JPH::Vec3(0.0f, depth * 0.5f, 0.0f);
            column.depth = depth;
            column.volume = volume;
            column.waterVelocity = flow + wave.velocity;
            if (volume->waves.waves || flow.x != 0.0f || flow.y != 0.0f || flow.z != 0.0f)
                movingWater = true;
        }
        if (submerged == 0)
            continue;
        // a body asleep in still water stays asleep
        if (!movingWater && !body.IsActive())
            continue;

        const float dragScale = std::max(0.0f, buoyancy.dragScale);
        float submergedVolume = 0.0f;
        float angularDrag = 0.0f;
        for (int c = 0; c < submerged; c++){
            const Column& column = work[c];
            const float fraction = column.depth / columnHeight;
            const float rho = column.volume->density;
            // Archimedes: the weight of the water the column's submerged part displaces
            const JPH::Vec3 lift(0.0f, rho * gravity * columnVolume * fraction, 0.0f);
            const JPH::Vec3 pointVelocity = body.GetPointVelocity(column.point);
            const JPH::Vec3 relative = pointVelocity - JPH::Vec3(column.waterVelocity.x, column.waterVelocity.y, column.waterVelocity.z);
            const JPH::Vec3 drag = relative * (-column.volume->linearDrag * dragScale * rho * columnVolume * fraction);
            body.AddForce(lift + drag, column.point);
            submergedVolume += columnVolume * fraction;
            angularDrag += column.volume->angularDrag * rho * columnVolume * fraction;
        }
        // against the turning, by the submerged volume
        if (submergedVolume > 0.0f)
            body.AddTorque(body.GetAngularVelocity() * (-angularDrag * dragScale));
        // a sleeping body the water moves wakes (outside the lock)
        const bool wake = !body.IsActive();
        lock.ReleaseLock();
        if (wake)
            bodies.ActivateBody(id);
    }
    (void)dt;
#else
    (void)dt;
#endif
}


