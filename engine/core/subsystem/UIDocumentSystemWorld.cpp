// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem World Space documents (S93 R36, D14): a WORLD-mode document lives in its own
// context and target as a TEXTURE-mode one does (UIDocumentSystemTexture.cpp); this file adds its
// quad. The quad is centred on the GameObject, textureWidth x textureHeight / Pixels Per Unit
// world units, in its XY plane (facing +Z), or turned to the camera (Billboard). RenderSystem
// queues it with the transparent objects (drawWorldDocument), and pointer input hits it through
// the same world dispatch as a texture on a mesh (raycastDocumentSurface), where the scene's
// meshes in front of it take the ray.

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/RmlRenderer.h"
#include "htmlui/UIWorldRaycast.h"
#include "math/Matrix4.h"
#include "picking/MeshRaycast.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core/ElementDocument.h>

#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    // texture pixels per world unit: an unset or broken value is the default
    float worldPixelsPerUnit(const UIDocumentComponent& document){
        return (std::isfinite(document.pixelsPerUnit) && document.pixelsPerUnit > 0.0f) ? document.pixelsPerUnit : 100.0f;
    }

}

bool UIDocumentSystem::hasWorldDocuments() const{
    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++){
        if (components->getComponentFromIndex(i).renderMode == UIDocumentRenderMode::WORLD)
            return true;
    }
    return false;
}

bool UIDocumentSystem::getWorldQuad(Entity entity, const CameraComponent* camera, Matrix4& worldFromQuad) const{
    const UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    Framebuffer* target = getTargetTexture(entity);
    if (!document || !transform || document->renderMode != UIDocumentRenderMode::WORLD || !target || !target->isCreated())
        return false;

    const float ppu = worldPixelsPerUnit(*document);
    const Matrix4 halfSize = Matrix4::scaleMatrix(Vector3(0.5f * (float)target->getWidth() / ppu, 0.5f * (float)target->getHeight() / ppu, 1.0f));
    if (document->billboard && camera){
        // the camera's orientation (its view's inverse, without the translation) at the
        // GameObject's position and scale
        const Matrix4 cameraWorld = camera->viewMatrix.inverse();
        const Vector3 eye = cameraWorld * Vector3(0.0f, 0.0f, 0.0f);
        const Matrix4 facing = Matrix4::translateMatrix(-eye) * cameraWorld;
        worldFromQuad = Matrix4::translateMatrix(transform->worldPosition) * facing * Matrix4::scaleMatrix(transform->worldScale) * halfSize;
    }else{
        worldFromQuad = transform->modelMatrix * halfSize;
    }
    return true;
}

void UIDocumentSystem::drawWorldDocument(Entity entity, CameraComponent& camera, const RenderTargetFormat& target, bool flipY){
    auto doc = documents.find(entity);
    HtmlUIContext* context = getTextureContext(entity);
    if (doc == documents.end() || !doc->second->IsVisible() || !context || !context->hasRenderedFrame())
        return;
    RmlRenderTarget* layers = context->getRenderTarget();
    FramebufferRender* layer = layers ? layers->getComposited() : nullptr;
    Matrix4 worldFromQuad;
    if (!layer || !getWorldQuad(entity, &camera, worldFromQuad))
        return;

    Matrix4 clipFromQuad = camera.viewProjectionMatrix * worldFromQuad;
    if (flipY)
        clipFromQuad = Matrix4::scaleMatrix(Vector3(1.0f, -1.0f, 1.0f)) * clipFromQuad;
    // both column-major
    Rml::Matrix4f matrix;
    std::memcpy(matrix.data(), (const float*)clipFromQuad, sizeof(float) * 16);
    context->getRenderer().drawWorldQuad(layer->getColorTexture(), target, matrix);
}

UIWorldHit UIDocumentSystem::raycastDocumentSurface(Entity entity, const UIDocumentComponent& document, const Ray& ray) const{
    if (document.renderMode != UIDocumentRenderMode::WORLD){
        const Entity surface = (document.inputSurface != NULL_ENTITY) ? document.inputSurface : entity;
        return raycastMeshUV(scene, surface, ray);
    }

    const Entity cameraEntity = scene->getCamera();
    const CameraComponent* camera = scene->findComponent<CameraComponent>(cameraEntity);
    Matrix4 worldFromQuad;
    if (!getWorldQuad(entity, camera, worldFromQuad))
        return UIWorldHit();
    UIWorldHit hit = raycastQuadUV(worldFromQuad, ray);
    if (!hit.hit)
        return hit;

    // depth tested as it is drawn: a mesh in front of the quad takes the ray
    MeshRaycastOptions options;
    options.ignore.push_back(entity);
    const MeshRaycastHit blocker = MeshRaycast::raycast(scene, ray, LAYER_MASK_DEFAULT_RAYCAST, options);
    const float rayLength = ray.getDirection().length();
    if (blocker.hit && rayLength > 0.0f && blocker.distance * rayLength < hit.distance)
        return UIWorldHit();
    return hit;
}
