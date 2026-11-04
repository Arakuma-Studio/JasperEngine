// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PixelPerfectCamera.h"

#include "component/CameraComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/Transform.h"
#include "pixelart/PixelSnap.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

PixelPerfectCamera::PixelPerfectCamera(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<PixelPerfectCameraComponent>(entity)){
        scene->addComponent<PixelPerfectCameraComponent>(entity, {});
    }
}

PixelPerfectCamera::~PixelPerfectCamera(){
}

PixelPerfectCamera::PixelPerfectCamera(const PixelPerfectCamera& rhs) = default;
PixelPerfectCamera& PixelPerfectCamera::operator=(const PixelPerfectCamera& rhs) = default;

void PixelPerfectCamera::setEnabled(bool enabled){
    getComponent<PixelPerfectCameraComponent>().enabled = enabled;
    markChanged<PixelPerfectCameraComponent>();
}

bool PixelPerfectCamera::isEnabled() const{
    return getComponent<PixelPerfectCameraComponent>().enabled;
}

void PixelPerfectCamera::setReferenceResolution(Vector2 resolution){
    PixelPerfectCameraComponent& component = getComponent<PixelPerfectCameraComponent>();
    component.referenceWidth = (unsigned int)std::max(0.0f, std::round(resolution.x));
    component.referenceHeight = (unsigned int)std::max(0.0f, std::round(resolution.y));
    markChanged<PixelPerfectCameraComponent>();
}

Vector2 PixelPerfectCamera::getReferenceResolution() const{
    const PixelPerfectCameraComponent& component = getComponent<PixelPerfectCameraComponent>();
    return Vector2((float)component.referenceWidth, (float)component.referenceHeight);
}

void PixelPerfectCamera::setUpscaleMode(PixelUpscaleMode mode){
    getComponent<PixelPerfectCameraComponent>().upscaleMode = mode;
    markChanged<PixelPerfectCameraComponent>();
}

PixelUpscaleMode PixelPerfectCamera::getUpscaleMode() const{
    return getComponent<PixelPerfectCameraComponent>().upscaleMode;
}

void PixelPerfectCamera::setUpscaleRenderTexture(bool upscaleRenderTexture){
    getComponent<PixelPerfectCameraComponent>().upscaleRenderTexture = upscaleRenderTexture;
    markChanged<PixelPerfectCameraComponent>();
}

bool PixelPerfectCamera::isUpscaleRenderTexture() const{
    return getComponent<PixelPerfectCameraComponent>().upscaleRenderTexture;
}

void PixelPerfectCamera::setPixelSnapping(bool pixelSnapping){
    getComponent<PixelPerfectCameraComponent>().pixelSnapping = pixelSnapping;
    markChanged<PixelPerfectCameraComponent>();
}

bool PixelPerfectCamera::isPixelSnapping() const{
    return getComponent<PixelPerfectCameraComponent>().pixelSnapping;
}

void PixelPerfectCamera::setRotationMode(PixelRotation mode){
    getComponent<PixelPerfectCameraComponent>().rotationMode = mode == PixelRotation::ON_GRID ? PixelRotation::ON_GRID : PixelRotation::STEPS_90;
    markChanged<PixelPerfectCameraComponent>();
}

PixelRotation PixelPerfectCamera::getRotationMode() const{
    const PixelRotation mode = getComponent<PixelPerfectCameraComponent>().rotationMode;
    return mode == PixelRotation::ON_GRID ? PixelRotation::ON_GRID : PixelRotation::STEPS_90;
}

void PixelPerfectCamera::setSubpixelSmoothing(bool subpixelSmoothing){
    getComponent<PixelPerfectCameraComponent>().subpixelSmoothing = subpixelSmoothing;
    markChanged<PixelPerfectCameraComponent>();
}

bool PixelPerfectCamera::isSubpixelSmoothing() const{
    return getComponent<PixelPerfectCameraComponent>().subpixelSmoothing;
}

void PixelPerfectCamera::setLetterboxColor(Vector4 color){
    getComponent<PixelPerfectCameraComponent>().letterboxColor = color;
    markChanged<PixelPerfectCameraComponent>();
}

Vector4 PixelPerfectCamera::getLetterboxColor() const{
    return getComponent<PixelPerfectCameraComponent>().letterboxColor;
}

int PixelPerfectCamera::getPixelRatio() const{
    return getComponent<PixelPerfectCameraComponent>().layout.scale;
}

Vector2 PixelPerfectCamera::getBaseResolution() const{
    const pixelart::PixelPerfectLayout& layout = getComponent<PixelPerfectCameraComponent>().layout;
    return Vector2((float)layout.baseWidth, (float)layout.baseHeight);
}

Vector2 PixelPerfectCamera::getTargetResolution() const{
    const pixelart::PixelPerfectLayout& layout = getComponent<PixelPerfectCameraComponent>().layout;
    return Vector2((float)layout.targetWidth, (float)layout.targetHeight);
}

Rect PixelPerfectCamera::getViewport() const{
    return getComponent<PixelPerfectCameraComponent>().layout.viewport;
}

Vector3 PixelPerfectCamera::getSnappedPosition() const{
    const PixelPerfectCameraComponent& component = getComponent<PixelPerfectCameraComponent>();
    return component.active ? component.snappedPosition : drawnOrigin();
}

Vector2 PixelPerfectCamera::getSubpixelOffset() const{
    return getComponent<PixelPerfectCameraComponent>().subpixelOffset;
}

bool PixelPerfectCamera::isCropped() const{
    return getComponent<PixelPerfectCameraComponent>().layout.cropped;
}

bool PixelPerfectCamera::isUICamera() const{
    const CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
    return camera && camera->type == CameraType::CAMERA_UI;
}

Vector3 PixelPerfectCamera::drawnOrigin() const{
    const PixelPerfectCameraComponent& component = getComponent<PixelPerfectCameraComponent>();
    if (component.active)
        return component.snappedPosition;
    const Transform* transform = scene->findComponent<Transform>(entity);
    return transform ? pixelart::snapWorld(transform->worldPosition, Engine::getPixelsPerUnit()) : Vector3(0, 0, 0);
}

float PixelPerfectCamera::baseHeight() const{
    const PixelPerfectCameraComponent& component = getComponent<PixelPerfectCameraComponent>();
    if (component.layout.baseHeight > 0)
        return (float)component.layout.baseHeight;
    if (component.referenceHeight > 0)
        return (float)component.referenceHeight;
    return (float)Engine::getPreferredCanvasHeight();
}

Vector2 PixelPerfectCamera::worldToPixel(Vector3 world) const{
    const Vector3 origin = drawnOrigin();
    const float ppu = (float)Engine::getPixelsPerUnit();
    const float x = (world.x - origin.x) * ppu;
    // a UI camera's world is y down from the image's top
    const float y = isUICamera() ? baseHeight() - (world.y - origin.y) * ppu : (world.y - origin.y) * ppu;
    return Vector2(x, y);
}

Vector3 PixelPerfectCamera::pixelToWorld(Vector2 pixel) const{
    const Vector3 origin = drawnOrigin();
    const float ppu = (float)Engine::getPixelsPerUnit();
    const float y = isUICamera() ? (baseHeight() - pixel.y) / ppu : pixel.y / ppu;
    return Vector3(origin.x + pixel.x / ppu, origin.y + y, origin.z);
}

Vector3 PixelPerfectCamera::roundToPixel(Vector3 world) const{
    return pixelart::snapWorld(world, Engine::getPixelsPerUnit());
}
