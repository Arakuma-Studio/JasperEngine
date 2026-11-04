// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "IconPicker.h"

#include "component/AudioSourceComponent.h"
#include "component/CameraComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/Transform.h"

#include <algorithm>
#include <cmath>
#include "spritenormals/SpriteNormalsField.h"

using namespace jasperengine;
using namespace jasperengine::editor;

bool editor::projectToView(const Matrix4& viewProjection, const Vector3& world, float width, float height, float& x, float& y, float* depth){
    const Vector4 clip = viewProjection * Vector4(world.x, world.y, world.z, 1.0f);
    if (!(clip.w > 1e-6f)){
        return false;
    }
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    x = (ndcX + 1.0f) * 0.5f * width;
    y = (1.0f - ndcY) * 0.5f * height;
    if (depth){
        *depth = clip.w;
    }
    return std::isfinite(x) && std::isfinite(y);
}

namespace {

    template<typename Component>
    void pickComponentIcons(Scene* scene, const Matrix4& viewProjection, float width, float height, float x, float y, float radius,
                            const IconPickFilter& filter, std::vector<IconHit>& out){
        auto array = scene->getComponentArray<Component>();
        const float radiusSquared = radius * radius;
        for (size_t i = 0; i < array->size(); i++){
            const Entity entity = array->getEntity(i);
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform || !transform->visible){
                continue;
            }
            if (scene->hasInactiveEntities() && !scene->isEntityActiveInHierarchy(entity)){
                continue;
            }
            if (!(filter.layerMask & (1u << (scene->getEntityLayer(entity) & 31)))){
                continue;
            }
            if (filter.excludeEditorFlags && (scene->getEntityEditorFlags(entity) & filter.excludeEditorFlags)){
                continue;
            }
            if (filter.accept && !filter.accept(entity)){
                continue;
            }
            float px, py, depth;
            if (!projectToView(viewProjection, transform->worldPosition, width, height, px, py, &depth)){
                continue;
            }
            const float dx = px - x;
            const float dy = py - y;
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > radiusSquared){
                continue;
            }
            // one icon per entity (a light with an audio source keeps the first)
            if (std::any_of(out.begin(), out.end(), [entity](const IconHit& hit){ return hit.entity == entity; })){
                continue;
            }
            out.push_back({entity, std::sqrt(distanceSquared), depth});
        }
    }

}

void editor::pickIcons(Scene* scene, const Matrix4& viewProjection, float width, float height, float x, float y, float radius,
                       const IconPickFilter& filter, std::vector<IconHit>& out){
    out.clear();
    if (!scene || width <= 0.0f || height <= 0.0f){
        return;
    }
    if (filter.lights){
        pickComponentIcons<LightComponent>(scene, viewProjection, width, height, x, y, radius, filter, out);
        pickComponentIcons<Light2DComponent>(scene, viewProjection, width, height, x, y, radius, filter, out);
    }
    pickComponentIcons<CameraComponent>(scene, viewProjection, width, height, x, y, radius, filter, out);
    if (filter.audio){
        pickComponentIcons<AudioSourceComponent>(scene, viewProjection, width, height, x, y, radius, filter, out);
    }
    pickComponentIcons<ReflectionProbeComponent>(scene, viewProjection, width, height, x, y, radius, filter, out);
    std::sort(out.begin(), out.end(), [](const IconHit& a, const IconHit& b){
        if (a.screenDistance != b.screenDistance) return a.screenDistance < b.screenDistance;
        return a.depth < b.depth;
    });
}
