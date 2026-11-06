// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pixel Perfect Camera's part of the renderer (docs/specs/S41-pixel-perfect-camera.md): which
// cameras draw through one, their integer layout (the Engine's view for the main scene's main
// camera), their clips and snapped view, the frame block's snap grid, the per-draw snap mode, the
// 90-degree steps, the blit's source rect and direct mode's bars. Nothing here runs for a scene
// without the component but one size test per frame.

#include "RenderSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "System.h"
#include "pool/ShaderPool.h"
#include "profiler/Profile.h"
#include "pixelart/PixelSnap.h"
#include "component/CameraComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    Vector2 smoothingShift(const PixelPerfectDrive& drive, const Vector2& residual){
        // R12: the image moves by whole screen pixels, s = round(f k), so every base pixel stays
        // k x k: |s / k| <= 0.5 base pixel
        if (!drive.smoothing)
            return Vector2(0.0f, 0.0f);
        const float k = (float)drive.layout.scale;
        return Vector2(std::round(residual.x * k) / k, std::round(residual.y * k) / k);
    }

}

float RenderSystem::pixelSnapBias = pixelart::kSnapBias;

bool RenderSystem::getPixelPerfectRequest(Scene* scene, Entity cameraEntity, PixelPerfectViewRequest& request){
    if (!scene || !scene->isComponentRegistered<PixelPerfectCameraComponent>())
        return false;
    const PixelPerfectCameraComponent* component = scene->findComponent<PixelPerfectCameraComponent>(cameraEntity);
    const CameraComponent* camera = scene->findComponent<CameraComponent>(cameraEntity);
    if (!component || !camera || !component->enabled || camera->type == CameraType::CAMERA_PERSPECTIVE)
        return false;
    request.active = true;
    request.referenceWidth = (int)component->referenceWidth;
    request.referenceHeight = (int)component->referenceHeight;
    request.mode = component->upscaleMode;
    request.smoothing = component->subpixelSmoothing;
    request.pixelsPerUnit = Engine::getPixelsPerUnit();
    return true;
}

bool RenderSystem::makePixelPerfectDrive(Entity cameraEntity, const CameraComponent& camera, const PixelPerfectCameraComponent& component,
                                         bool mainOfBaseLayer, int screenWidth, int screenHeight, PixelPerfectDrive& drive) const{
    // inert on a perspective camera and off with its GameObject (D8)
    if (!component.enabled || camera.type == CameraType::CAMERA_PERSPECTIVE || !scene->isEntityActiveInHierarchy(cameraEntity))
        return false;
    drive = PixelPerfectDrive();
    drive.cameraEntity = cameraEntity;
    drive.camera = &camera;
    drive.snapping = component.pixelSnapping;
    drive.rotation = component.rotationMode == PixelRotation::ON_GRID ? PixelRotation::ON_GRID : PixelRotation::STEPS_90;
    drive.mode = component.upscaleMode;
    drive.pixelsPerUnit = Engine::getPixelsPerUnit();
    drive.smoothing = component.subpixelSmoothing;
    drive.letterboxColor = component.letterboxColor;

    PixelPerfectViewRequest request;
    request.active = true;
    request.referenceWidth = (int)component.referenceWidth;
    request.referenceHeight = (int)component.referenceHeight;
    request.mode = component.upscaleMode;
    request.smoothing = component.subpixelSmoothing;
    request.pixelsPerUnit = drive.pixelsPerUnit;

    if (camera.renderToTexture){
        // R5: a render-to-texture camera snaps to its texture's pixels
        if (!camera.framebuffer || camera.framebuffer->getWidth() == 0 || camera.framebuffer->getHeight() == 0)
            return false;
        const int width = (int)camera.framebuffer->getWidth();
        const int height = (int)camera.framebuffer->getHeight();
        drive.layout = pixelart::computePixelPerfectLayout(width, height, width, height, PixelUpscaleMode::INTEGER, 1, false,
                                                           drive.pixelsPerUnit);
        drive.renderTexture = false;
    }else if (mainOfBaseLayer){
        drive.layout = Engine::computePixelPerfectView(request, screenWidth, screenHeight, Engine::getPixelPerfectForcedScale());
        drive.renderTexture = component.upscaleRenderTexture;
    }else{
        // R5: a layer scene takes the main scene's layout and draws directly on its grid
        if (Engine::getPixelPerfectView().active && Engine::getPixelPerfectLayout().baseWidth > 0)
            drive.layout = Engine::getPixelPerfectLayout();
        else
            drive.layout = Engine::computePixelPerfectView(request, screenWidth, screenHeight, Engine::getPixelPerfectForcedScale());
        drive.renderTexture = false;
    }
    if (drive.layout.baseWidth <= 0 || drive.layout.baseHeight <= 0)
        return false;
    if (!drive.renderTexture){
        // drawn directly: no target, no margin; Stretch Fill needs the target's blit, so it draws
        // as Integer then
        if (drive.mode == PixelUpscaleMode::STRETCH_FILL && !camera.renderToTexture){
            request.mode = PixelUpscaleMode::INTEGER;
            drive.layout = Engine::computePixelPerfectView(request, screenWidth, screenHeight, Engine::getPixelPerfectForcedScale());
            drive.mode = PixelUpscaleMode::INTEGER;
        }
        drive.layout.targetWidth = drive.layout.baseWidth;
        drive.layout.targetHeight = drive.layout.baseHeight;
    }
    // the smoothing's move of the image from the camera's last residual (snapPixelPerfectView
    // makes it again whenever the camera moves)
    drive.shift = smoothingShift(drive, component.subpixelOffset);
    return true;
}


void RenderSystem::applyPixelPerfectClips(CameraComponent& camera, const PixelPerfectDrive& drive) const{
    // the camera spans T / PPU world units; with the smoothing margin its origin is one base
    // pixel in (R3)
    const float ppu = (float)drive.pixelsPerUnit;
    const float margin = drive.renderTexture && drive.smoothing ? 1.0f : 0.0f;
    const float left = -margin / ppu;
    const float bottom = -margin / ppu;
    const float right = ((float)drive.layout.targetWidth - margin) / ppu;
    const float top = ((float)drive.layout.targetHeight - margin) / ppu;
    const float aspect = (float)drive.layout.targetWidth / (float)drive.layout.targetHeight;
    if (camera.leftClip != left || camera.rightClip != right || camera.bottomClip != bottom || camera.topClip != top ||
            camera.aspect != aspect){
        camera.leftClip = left;
        camera.rightClip = right;
        camera.bottomClip = bottom;
        camera.topClip = top;
        camera.aspect = aspect;
        camera.needUpdate = true;
    }
}

void RenderSystem::snapPixelPerfectView(CameraComponent& camera, const Transform& transform, PixelPerfectDrive& drive) const{
    // R8: the view at the snapped position (the target moves by the same delta); the Transform,
    // picking, audio and Lua keep the authored one
    const Vector3 world = transform.worldPosition;
    const Vector3 snapped = pixelart::snapWorld(world, drive.pixelsPerUnit);
    const Vector3 delta = world - snapped;
    if (delta.x != 0.0f || delta.y != 0.0f){
        camera.viewMatrix = camera.viewMatrix * Matrix4::translateMatrix(delta);
        camera.viewProjectionMatrix = camera.projectionMatrix * camera.viewMatrix;
        updateCameraFrustumPlanes(camera.viewProjectionMatrix, camera.frustumPlanes);
    }
    // the residual in base pixels of the image (y up: a UI camera's world is y down)
    const float ppu = (float)drive.pixelsPerUnit;
    const float residualX = delta.x * ppu;
    const float residualY = (camera.type == CameraType::CAMERA_UI ? -delta.y : delta.y) * ppu;
    drive.shift = smoothingShift(drive, Vector2(residualX, residualY));
    if (drive.smoothing){
        if (!drive.renderTexture && (drive.shift.x != 0.0f || drive.shift.y != 0.0f)){
            // direct mode: the projection moves the image (the target's blit does it otherwise)
            const Matrix4 move = Matrix4::translateMatrix(-2.0f * drive.shift.x / (float)drive.layout.baseWidth,
                                                          -2.0f * drive.shift.y / (float)drive.layout.baseHeight, 0.0f);
            camera.projectionMatrix = move * camera.projectionMatrix;
            camera.viewProjectionMatrix = camera.projectionMatrix * camera.viewMatrix;
            updateCameraFrustumPlanes(camera.viewProjectionMatrix, camera.frustumPlanes);
        }
    }
    // input follows the image the Engine shows (screen pixels, y down)
    if (mainPixelPerfect >= 0 && &drive == &pixelPerfectDrives[(size_t)mainPixelPerfect] && Engine::getMainScene() == scene){
        const float k = (float)drive.layout.scale;
        Engine::setViewShift(Vector2(-drive.shift.x * k, drive.shift.y * k));
    }
    if (PixelPerfectCameraComponent* component = scene->findComponent<PixelPerfectCameraComponent>(drive.cameraEntity)){
        component->snappedPosition = snapped;
        component->subpixelOffset = Vector2(residualX, residualY);
    }
}

void RenderSystem::updatePixelPerfect(Entity mainCameraEntity){
    // without the component, the price of the feature is this test (R14)
    const bool registered = scene->isComponentRegistered<PixelPerfectCameraComponent>();
    const size_t count = registered ? scene->getComponentArray<PixelPerfectCameraComponent>()->size() : 0;
    const bool engineMain = Engine::getMainScene() == scene;
    if (count == 0 && pixelPerfectDrives.empty() && !pixelSnapVariants){
        if (engineMain && Engine::getPixelPerfectView().active)
            Engine::setPixelPerfectView(PixelPerfectViewRequest());
        return;
    }

    // last frame's drives, to see what changed (this system's storage, kept between calls: no
    // allocation per frame)
    std::vector<PixelPerfectDrive>& previous = previousPixelPerfectDrives;
    previous.assign(pixelPerfectDrives.begin(), pixelPerfectDrives.end());
    pixelPerfectDrives.clear();
    mainPixelPerfect = -1;
    // a running scene other than the Engine's first is a layer (R5); a scene the editor edits
    // (not running) lays out as the game's main scene would
    const bool layerScene = Engine::isSceneRunning(scene) && !engineMain;
    const int screenWidth = System::instance().getScreenWidth();
    const int screenHeight = System::instance().getScreenHeight();

    // the main camera first: its request sets the Engine's view the others may follow
    PixelPerfectViewRequest mainRequest;
    if (engineMain && getPixelPerfectRequest(scene, mainCameraEntity, mainRequest) &&
            !scene->getComponent<CameraComponent>(mainCameraEntity).renderToTexture && scene->isEntityActiveInHierarchy(mainCameraEntity)){
        Engine::setPixelPerfectView(mainRequest);
    }else if (engineMain && Engine::getPixelPerfectView().active){
        Engine::setPixelPerfectView(PixelPerfectViewRequest());
    }

    bool snapping = false;
    if (count > 0){
        auto components = scene->getComponentArray<PixelPerfectCameraComponent>();
        for (size_t i = 0; i < components->size(); i++){
            PixelPerfectCameraComponent& component = components->getComponentFromIndex(i);
            const Entity entity = components->getEntity(i);
            component.active = false;
            CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
            if (!camera)
                continue;
            const bool main = entity == mainCameraEntity && !camera->renderToTexture;
            PixelPerfectDrive drive;
            if (!makePixelPerfectDrive(entity, *camera, component, main && !layerScene, screenWidth, screenHeight, drive))
                continue;
            // the cached matrices hold the policy, the snap and the shift: a change makes them again
            const PixelPerfectDrive* before = nullptr;
            for (const PixelPerfectDrive& old : previous){
                if (old.cameraEntity == entity)
                    before = &old;
            }
            if (!before || before->rotation != drive.rotation || before->snapping != drive.snapping || before->smoothing != drive.smoothing ||
                    before->renderTexture != drive.renderTexture || before->layout != drive.layout)
                camera->needUpdate = true;
            component.active = true;
            component.layout = drive.layout;
            applyPixelPerfectClips(*camera, drive);
            snapping = snapping || drive.snapping;
            if (main && !layerScene)
                mainPixelPerfect = (int)pixelPerfectDrives.size();
            pixelPerfectDrives.push_back(drive);
        }
    }
    // a camera whose drive went (the component disabled or removed, the camera made
    // perspective): its matrices drop the policy and the snap
    for (const PixelPerfectDrive& old : previous){
        bool kept = false;
        for (const PixelPerfectDrive& drive : pixelPerfectDrives)
            kept = kept || drive.cameraEntity == old.cameraEntity;
        if (!kept){
            if (CameraComponent* camera = scene->findComponent<CameraComponent>(old.cameraEntity))
                camera->needUpdate = true;
        }
    }

    // the objects' variants follow (a scene that starts or stops snapping reloads them once;
    // the particles' render key holds it)
    if (snapping != pixelSnapVariants){
        pixelSnapVariants = snapping;
        needReloadMeshVariants();
        needReloadUIs();
    }
}

void RenderSystem::refreshPixelPerfectCameras(){
    for (PixelPerfectDrive& drive : pixelPerfectDrives)
        drive.camera = scene->findComponent<CameraComponent>(drive.cameraEntity);
}

PixelPerfectDrive* RenderSystem::findPixelPerfectDrive(Entity cameraEntity){
    for (PixelPerfectDrive& drive : pixelPerfectDrives){
        if (drive.cameraEntity == cameraEntity)
            return &drive;
    }
    return nullptr;
}

const PixelPerfectDrive* RenderSystem::findPixelPerfectDrive(const CameraComponent& camera) const{
#ifdef JASPERENGINE_EDITOR
    // a view draws through its own copy of the camera
    if (rv().secondary && &camera == &rv().camera)
        return rv().pixelPerfectActive ? &rv().pixelPerfect : nullptr;
#endif
    for (const PixelPerfectDrive& drive : pixelPerfectDrives){
        if (drive.camera == &camera)
            return &drive;
    }
    return nullptr;
}

const PixelPerfectDrive* RenderSystem::viewPixelPerfect() const{
#ifdef JASPERENGINE_EDITOR
    if (rv().secondary)
        return rv().pixelPerfectActive ? &rv().pixelPerfect : nullptr;
#endif
    return mainPixelPerfect >= 0 ? &pixelPerfectDrives[(size_t)mainPixelPerfect] : nullptr;
}

const pixelart::PixelPerfectLayout* RenderSystem::getPixelPerfectLayout() const{
    return mainPixelPerfect >= 0 ? &pixelPerfectDrives[(size_t)mainPixelPerfect].layout : nullptr;
}

Matrix4 RenderSystem::pixelArtModel(Entity entity, const Matrix4& model, const PixelPerfectDrive* drive) const{
    // R10 Steps90: only a pixel-art drawable off the 90-degree grid is rebuilt
    if (!drive || !drive->snapping || !pixelart::hasOffGridRotation(model))
        return model;
    const Signature signature = scene->getSignature(entity);
    PixelRotation rotation = drive->rotation;
    if (signature.test(scene->getComponentId<SpriteComponent>())){
        const PixelRotation own = scene->getComponent<SpriteComponent>(entity).pixelRotation;
        if (own != PixelRotation::CAMERA)
            rotation = own;
    }else if (!signature.test(scene->getComponentId<TilemapComponent>()) && !signature.test(scene->getComponentId<UIComponent>())){
        return model;
    }
    return rotation == PixelRotation::STEPS_90 ? pixelart::roundRotation90(model) : model;
}

void RenderSystem::fillPassPixelSnap(frame_t& frame, const CameraComponent& camera){
    passPixelSnap = false;
    passPixelPerfect = nullptr;
    frame.pixelSnap = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    frame.pixelSnapOffset = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    if (!pixelSnapVariants)
        return;

    pixelart::SnapGrid grid;
    float scale = 1.0f;
    if (const PixelPerfectDrive* drive = findPixelPerfectDrive(camera)){
        if (!drive->snapping)
            return;
        passPixelPerfect = drive;
        grid.width = (float)drive->layout.targetWidth;
        grid.height = (float)drive->layout.targetHeight;
        scale = (float)drive->layout.scale;
        if (!drive->renderTexture){
            // R12 direct mode: the projection moved the image, the grid moves with it
            grid.offsetX = -drive->shift.x;
            grid.offsetY = -drive->shift.y;
        }
    }
#ifdef JASPERENGINE_EDITOR
    else if (rv().secondary && rv().snapPreview && &camera == &rv().camera){
        // the Scene view's Snap Preview: its own grid (R21)
        grid = rv().snapPreviewGrid;
        scale = grid.width > 0.0f ? rv().viewRect.getWidth() / grid.width : 1.0f;
    }
#endif
    if (!grid.isValid())
        return;
    grid.flipY = isRenderingFlipped(camera) ? -1.0f : 1.0f;
    passPixelSnap = true;
    passSnapGrid = grid;

    // a rolled camera's draws are off the grid: their pivots snap (D8)
    Matrix4 viewProjection = camera.viewProjectionMatrix;
    if (grid.flipY < 0.0f)
        viewProjection = Matrix4::scaleMatrix(Vector3(1.0f, -1.0f, 1.0f)) * viewProjection;
    const bool viewAligned = pixelart::isGridAligned(viewProjection, grid);
    // the terms the vertex stage reads, as pixelart::toPixels computes them (includes/pixel_snap.glsl)
    const pixelart::SnapTerms terms = pixelart::snapTerms(grid);
    frame.pixelSnap = Vector4(terms.halfWidth, terms.halfHeight, grid.flipY, scale);
    frame.pixelSnapOffset = Vector4(terms.centerX, terms.centerY, viewAligned ? 1.0f : 0.0f, pixelSnapBias);
}

float RenderSystem::pixelSnapDrawMode(const Matrix4& mvp) const{
    if (!passPixelSnap)
        return 0.0f;
    return pixelart::isGridAligned(mvp, passSnapGrid) ? 1.0f : 2.0f;
}

void RenderSystem::applyObjectBlock(ObjectRender& render, int slot, unsigned int reflectedSize, uint64_t meshProperties, const Matrix4* objectBlock){
    if (meshProperties & ShaderPool::getPixelSnapProperty(ShaderType::MESH)){
        float block[52];
        std::memcpy(block, objectBlock, sizeof(float) * 48);
        block[48] = pixelSnapDrawMode(objectBlock[2]);
        block[49] = 0.0f;
        block[50] = 0.0f;
        block[51] = 0.0f;
        render.applyUniformBlock(slot, uniformUploadSize(sizeof(block), reflectedSize), block);
    }else{
        render.applyUniformBlock(slot, uniformUploadSize(sizeof(float) * 48, reflectedSize), (void*)objectBlock);
    }
}

Vector4 RenderSystem::pixelPerfectInnerUV(const PixelPerfectDrive& drive) const{
    // the inner B of the T target (one margin pixel each side with smoothing), moved by the
    // smoothing shift: x right, and y up of the image, which is row 0 first in the texture
    const float targetWidth = (float)drive.layout.targetWidth;
    const float targetHeight = (float)drive.layout.targetHeight;
    const float margin = drive.smoothing ? 1.0f : 0.0f;
    return Vector4((float)drive.layout.baseWidth / targetWidth, (margin + drive.shift.x) / targetWidth,
                   (float)drive.layout.baseHeight / targetHeight, (margin - drive.shift.y) / targetHeight);
}

unsigned int RenderSystem::fixedResolutionWidth() const{
    if (const PixelPerfectDrive* drive = viewPixelPerfect()){
        if (drive->renderTexture)
            return (unsigned int)drive->layout.targetWidth;
    }
    return scene->getFixedResolutionWidth();
}

unsigned int RenderSystem::fixedResolutionHeight() const{
    if (const PixelPerfectDrive* drive = viewPixelPerfect()){
        if (drive->renderTexture)
            return (unsigned int)drive->layout.targetHeight;
    }
    return scene->getFixedResolutionHeight();
}

TextureFilter RenderSystem::fixedResolutionFilter() const{
    if (const PixelPerfectDrive* drive = viewPixelPerfect()){
        // Stretch Fill reads it bilinear (sharp bilinear), the integer modes nearest
        if (drive->renderTexture)
            return drive->mode == PixelUpscaleMode::STRETCH_FILL ? TextureFilter::LINEAR : TextureFilter::NEAREST;
    }
    return scene->getFixedResolutionFilter();
}

bool RenderSystem::renderPixelPerfectBars(const PixelPerfectDrive& drive, const Rect& viewport){
    // only bars of another colour than the background need the pass
    Framebuffer* target = targetFramebuffer();
    const float targetWidth = target ? (float)target->getWidth() : (float)System::instance().getScreenWidth();
    const float targetHeight = target ? (float)target->getHeight() : (float)System::instance().getScreenHeight();
    const bool bars = viewport.getX() > 0.0f || viewport.getY() > 0.0f || viewport.getX() + viewport.getWidth() < targetWidth ||
                      viewport.getY() + viewport.getHeight() < targetHeight;
    const Vector4 background = scene->getBackgroundColor();
    const Vector4& letterbox = drive.letterboxColor;
    if (!bars || (background.x == letterbox.x && background.y == letterbox.y && background.z == letterbox.z && background.w == letterbox.w))
        return false;
    loadBlit();
    if (!blitLoaded)
        return false;

    // the background as one texel, made again when it changes (rarely)
    auto byte = [](float v){ return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    const uint32_t packed = byte(background.x) | (byte(background.y) << 8) | (byte(background.z) << 16) | (byte(background.w) << 24);
    if (!pixelPerfectFillCreated || packed != pixelPerfectFillColor){
        if (pixelPerfectFillCreated)
            pixelPerfectFill.destroyTexture();
        static uint32_t texels[64];
        for (uint32_t& texel : texels)
            texel = packed;
        void* data[6] = {texels, texels, texels, texels, texels, texels};
        size_t sizes[6] = {sizeof(texels), sizeof(texels), sizeof(texels), sizeof(texels), sizeof(texels), sizeof(texels)};
        JE_GPU_ALLOC_SCOPE(Textures, "Pixel Perfect Camera background");
        pixelPerfectFill.createTexture("pixelperfect|background", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, sizes,
                                       TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
        pixelPerfectFillCreated = true;
        pixelPerfectFillColor = packed;
    }
    renderBlit(&pixelPerfectFill, viewport, &letterbox);
    return true;
}
