// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabRender.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "component/LightComponent.h"

#include "Configs.h"
#include "Project.h"
#include "Engine.h"
#include "util/GraphicUtils.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

editor::PrefabRender::~PrefabRender(){
    clear();
}

void editor::PrefabRender::clear(){
    delete camera;
    delete light;
    delete scene;
    camera = nullptr;
    light = nullptr;
    scene = nullptr;
    root = NULL_ENTITY;
    flat = false;
}

bool editor::PrefabRender::build(Project* project, const std::filesystem::path& prefabPath){
    clear();

    scene = new Scene();
    // the preview's own camera and light take system ids: the prefab's GameObjects get theirs
    scene->setDefaultEntityPool(EntityPool::System);
    camera = new Camera(scene);
    light = new Light(scene);
    scene->setDefaultEntityPool(EntityPool::User);

    scene->setBackgroundColor(0.0, 0.0, 0.0, 0.0);
    scene->setCamera(camera);

    light->setDirection(-0.4, -0.5, -0.5);
    light->setIntensity(6.0);
    light->setType(LightType::DIRECTIONAL);

    root = project->instantiatePrefabPreview(scene, prefabPath);
    if (root == NULL_ENTITY){
        clear();
        return false;
    }
    // the prefab's cameras must not take over the view
    scene->setCamera(camera);

    // 2D and UI prefabs (sprites, tilemaps, UI) are shown flat, like their Scene view
    bool hasFlat = false;
    bool has3D = false;
    for (Entity entity : scene->getEntityList()){
        if (entity == camera->getEntity() || entity == light->getEntity()){
            continue;
        }
        Signature signature = scene->getSignature(entity);
        const bool flatComponent = signature.test(scene->getComponentId<SpriteComponent>()) ||
            signature.test(scene->getComponentId<TilemapComponent>()) ||
            signature.test(scene->getComponentId<MeshPolygonComponent>()) ||
            signature.test(scene->getComponentId<UIComponent>()) ||
            signature.test(scene->getComponentId<RectTransformComponent>());
        if (flatComponent){
            hasFlat = true;
        }else if (signature.test(scene->getComponentId<MeshComponent>()) ||
                  signature.test(scene->getComponentId<ModelComponent>()) ||
                  signature.test(scene->getComponentId<TerrainComponent>())){
            has3D = true;
        }
    }
    flat = hasFlat && !has3D;

    camera->setFramebufferSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
    camera->setRenderToTexture(true);
    if (flat){
        camera->setType(CameraType::CAMERA_ORTHO);
        camera->setTransparentSort(false);
        camera->setDepthTest(false);
    }else{
        camera->setType(CameraType::CAMERA_PERSPECTIVE);
        camera->setAspect(1.0f);
    }
    frame();
    return true;
}

void editor::PrefabRender::frame(){
    if (!scene || !camera){
        return;
    }

    AABB bounds;
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++){
        Entity entity = transforms->getEntity(i);
        if (entity == camera->getEntity() || entity == light->getEntity()){
            continue;
        }
        Transform& transform = transforms->getComponentFromIndex(i);
        Signature signature = scene->getSignature(entity);
        AABB box;
        if (signature.test(scene->getComponentId<MeshComponent>())){
            box = scene->getComponent<MeshComponent>(entity).worldAABB;
        }else if (signature.test(scene->getComponentId<UIComponent>())){
            box = scene->getComponent<UIComponent>(entity).worldAABB;
            if (signature.test(scene->getComponentId<RectTransformComponent>()) && !signature.test(scene->getComponentId<PolygonComponent>())){
                RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
                if (layout.width > 0 && layout.height > 0){
                    Vector2 center = GraphicUtils::getRectTransformCenter(scene, entity, layout);
                    box = transform.modelMatrix * AABB(-center.x, -center.y, 0, layout.width - center.x, layout.height - center.y, 0);
                }
            }
        }
        if (!box.isNull() && !box.isInfinite()){
            bounds.merge(box);
        }else if (bounds.isNull()){
            bounds.merge(transform.worldPosition);
        }
    }
    if (bounds.isNull() || bounds.isInfinite()){
        bounds = AABB(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
    }

    const Vector3 center = bounds.getCenter();
    const Vector3 size = bounds.getSize();

    if (flat){
        float half = std::max(size.x, size.y) * 0.5f * 1.08f;
        if (half <= 0.001f){
            half = 1.0f;
        }
        camera->setPosition(center.x, center.y, center.z + 1.0f);
        camera->setTarget(center.x, center.y, center.z);
        camera->setOrtho(-half, half, -half, half, -100000.0f, 100000.0f);
        return;
    }

    float maxDimension = std::max({size.x, size.y, size.z});
    if (maxDimension <= 0.001f){
        maxDimension = 1.0f;
    }
    // ModelRender's framing: the bounding size at a 45 degree field of view, from above front right
    float distance = (maxDimension / 2.0f) / std::tan(22.5f * (float)M_PI / 180.0f);
    distance = std::max(distance, maxDimension * 1.5f);
    Vector3 offset(0.5f, 0.3f, 0.6f);
    offset.normalize();
    const Vector3 position = center + offset * distance;
    const float radius = std::max(0.5f, maxDimension * 0.5f);

    camera->setPosition(position.x, position.y, position.z);
    camera->setTarget(center.x, center.y, center.z);
    camera->setNearClip(std::max(0.01f, distance - radius * 3.0f));
    camera->setFarClip(std::max(distance + radius * 6.0f, 1000.0f));
}

Framebuffer* editor::PrefabRender::getFramebuffer(){
    return camera ? camera->getFramebuffer() : nullptr;
}

Scene* editor::PrefabRender::getScene(){
    return scene;
}
