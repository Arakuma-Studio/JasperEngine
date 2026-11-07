// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterEditorUtils.h"

#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "object/environment/Water.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/WaterInteractionSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;

namespace {

    constexpr float PI = 3.14159265358979f;

    // a collider's local half extents and volume (unscaled); false for a shape of unknown size
    bool colliderShape(const Collider3D& collider, Vector3& half, float& volume){
        switch (collider.type){
            case Collider3DType::BOX:
                half = Vector3(collider.width, collider.height, collider.depth) * 0.5f;
                volume = collider.width * collider.height * collider.depth;
                return true;
            case Collider3DType::SPHERE:
                half = Vector3(collider.radius, collider.radius, collider.radius);
                volume = 4.0f / 3.0f * PI * collider.radius * collider.radius * collider.radius;
                return true;
            case Collider3DType::CAPSULE:
                half = Vector3(collider.radius, collider.halfHeight + collider.radius, collider.radius);
                volume = PI * collider.radius * collider.radius * (2.0f * collider.halfHeight) +
                         4.0f / 3.0f * PI * collider.radius * collider.radius * collider.radius;
                return true;
            case Collider3DType::CYLINDER:
                half = Vector3(collider.radius, collider.halfHeight, collider.radius);
                volume = PI * collider.radius * collider.radius * 2.0f * collider.halfHeight;
                return true;
            case Collider3DType::TAPERED_CAPSULE:{
                const float r = std::max(collider.topRadius, collider.bottomRadius);
                half = Vector3(r, collider.halfHeight + r, r);
                // a frustum between the two spheres' centres plus their halves (an estimate)
                const float a = collider.topRadius, b = collider.bottomRadius;
                volume = PI * 2.0f * collider.halfHeight * (a * a + a * b + b * b) / 3.0f +
                         2.0f / 3.0f * PI * (a * a * a + b * b * b);
                return true;
            }
            default:
                return false;
        }
    }

    Vector3 worldScale(Scene* scene, Entity entity){
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            return Vector3(1.0f, 1.0f, 1.0f);
        return Vector3(std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.y), std::fabs(transform->worldScale.z));
    }

}

bool editor::WaterEditorUtils::colliderBounds(Scene* scene, Entity entity, Vector3& min, Vector3& max){
    const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
    if (!body)
        return false;
    bool any = false;
    const float inf = std::numeric_limits<float>::max();
    min = Vector3(inf, inf, inf);
    max = Vector3(-inf, -inf, -inf);
    for (size_t i = 0; i < body->colliders.size(); i++){
        Vector3 half;
        float volume = 0.0f;
        if (!colliderShape(body->colliders[i], half, volume))
            continue;
        const Vector3& p = body->colliders[i].position;
        min = Vector3(std::min(min.x, p.x - half.x), std::min(min.y, p.y - half.y), std::min(min.z, p.z - half.z));
        max = Vector3(std::max(max.x, p.x + half.x), std::max(max.y, p.y + half.y), std::max(max.z, p.z + half.z));
        any = true;
    }
    return any;
}

bool editor::WaterEditorUtils::colliderMass(Scene* scene, Entity entity, float& volume, float& mass){
    const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
    volume = 0.0f;
    mass = 0.0f;
    if (!body)
        return false;
    const Vector3 scale = worldScale(scene, entity);
    const float scaleVolume = scale.x * scale.y * scale.z;
    for (size_t i = 0; i < body->colliders.size(); i++){
        Vector3 half;
        float v = 0.0f;
        if (!colliderShape(body->colliders[i], half, v))
            continue;
        volume += v * scaleVolume;
        mass += v * scaleVolume * body->colliders[i].density;
    }
    return volume > 0.0f;
}

std::vector<Vector3> editor::WaterEditorUtils::columnBottoms(Scene* scene, Entity entity, const BuoyancyComponent& buoyancy,
                                                             Vector3& boundsMin, Vector3& boundsMax){
    std::vector<Vector3> bottoms;
    const bool bounded = colliderBounds(scene, entity, boundsMin, boundsMax);
    if (buoyancy.columns == BuoyancyColumns::CUSTOM){
        // WaterSystem uses at most 9
        for (size_t i = 0; i < buoyancy.customPoints.size() && i < 9; i++)
            bottoms.push_back(buoyancy.customPoints[i]);
        return bottoms;
    }
    if (!bounded)
        return bottoms;
    const int n = buoyancy.columns == BuoyancyColumns::AUTO_3x3 ? 3 : 2;
    for (int a = 0; a < n; a++){
        for (int b = 0; b < n; b++){
            const float u = (a + 0.5f) / n;
            const float v = (b + 0.5f) / n;
            bottoms.push_back(Vector3(boundsMin.x + (boundsMax.x - boundsMin.x) * u, boundsMin.y, boundsMin.z + (boundsMax.z - boundsMin.z) * v));
        }
    }
    return bottoms;
}

float editor::WaterEditorUtils::waterDensity(Scene* scene, Entity entity){
    auto volumes = scene->getComponentArray<WaterVolumeComponent>();
    if (volumes->size() == 0)
        return 0.0f;
    const Transform* transform = scene->findComponent<Transform>(entity);
    float best = std::numeric_limits<float>::max();
    float density = volumes->getComponentFromIndex(0).density;
    for (size_t i = 0; i < volumes->size(); i++){
        const Transform* volume = scene->findComponent<Transform>(volumes->getEntity(i));
        if (!transform || !volume)
            continue;
        const float distance = (volume->worldPosition - transform->worldPosition).length();
        if (distance < best){
            best = distance;
            density = volumes->getComponentFromIndex(i).density;
        }
    }
    return density;
}

float editor::WaterEditorUtils::meshVertexSpacing(Scene* scene, Entity entity){
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh || mesh->vertexCount == 0 || mesh->aabb.isNull())
        return 0.0f;
    const Vector3 scale = worldScale(scene, entity);
    const Vector3 size = mesh->aabb.getMaximum() - mesh->aabb.getMinimum();
    const float area = std::fabs(size.x * scale.x * size.z * scale.z);
    if (!(area > 0.0f))
        return 0.0f;
    return std::sqrt(area / (float)mesh->vertexCount);
}

float editor::WaterEditorUtils::materialWaveLength(Scene* scene, Entity entity){
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh)
        return 0.0f;
    float longest = 0.0f;
    for (unsigned int s = 0; s < mesh->numSubmeshes; s++){
        WaterWaveParams waves;
        if (Water::readWaves(mesh->submeshes[s].material, waves) && waves.waves && waves.height != 0.0f)
            longest = std::max(longest, waves.length);
    }
    return longest;
}

AABB editor::WaterEditorUtils::meshSourceLocalBounds(Scene* scene, Entity entity){
    return WaterInteractionSystem::getMeshSourceLocalBounds(scene, entity);
}

bool editor::WaterEditorUtils::sceneDrawsWaterInteraction(Scene* scene, bool wake){
    const uint32_t flag = wake ? ShaderRequire_WaterWake : ShaderRequire_WaterExclusion;
    const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++){
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const std::string& shader = RenderSystem::getSubmeshShader(mesh, mesh.submeshes[s], sceneMeshShader);
            if (shader.empty() || ShaderReference::isBuiltin(shader))
                continue;
            const std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
            if (manifest && (manifest->getPossibleRequire() & flag))
                return true;
        }
    }
    return false;
}

bool editor::WaterEditorUtils::excluderDriesItsOwnBodies(Scene* scene, Entity excluder){
    if (WaterInteractionSystem::getExcluderOwner(scene, excluder) != NULL_ENTITY)
        return false;
    auto bodies = scene->getComponentArray<RigidbodyComponent>();
    for (size_t i = 0; i < bodies->size(); i++){
        const RigidbodyComponent& body = bodies->getComponentFromIndex(i);
        const Entity entity = bodies->getEntity(i);
        if (body.type != BodyType::DYNAMIC || (body.buoyancy <= 0.0f && !scene->findComponent<BuoyancyComponent>(entity)))
            continue;
        // the body under the excluder's GameObject
        Entity current = entity;
        for (int hops = 0; current != NULL_ENTITY && hops < 256; hops++){
            if (current == excluder)
                return true;
            const Transform* transform = scene->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
    }
    return false;
}
