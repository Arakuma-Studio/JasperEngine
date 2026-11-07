// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/RenderBackendTime.h"
#include "SokolCamera.h"
#include "SokolObject.h"

#include "System.h"
#include "SokolPassRecorder.h"
#include "SokolSystem.h"
#include "render/SystemRender.h"

#include "sokol_gfx.h"
#include <cmath>

using namespace jasperengine;

SokolCamera::SokolCamera(){
    pass = {0};
    pass.action.colors[0].load_action = SG_LOADACTION_LOAD;
    // depth and stencil start every pass cleared (stencil masks never outlive their pass)
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    pass.action.depth.clear_value = 1.0f;
    pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
    pass.action.stencil.clear_value = 0;
    textureView.id = SG_INVALID_ID;
}

// a texture pass's view belongs to the pass that made it
SokolCamera::SokolCamera(const SokolCamera& rhs) : pass(rhs.pass), multisample(rhs.multisample) {
    textureView.id = SG_INVALID_ID;
}

SokolCamera& SokolCamera::operator=(const SokolCamera& rhs){
    pass = rhs.pass;
    multisample = rhs.multisample;
    return *this;
}

SokolCamera::~SokolCamera(){

}

void SokolCamera::setClearColor(Vector4 clearColor){
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
}

void SokolCamera::setClearDepth(float clearDepth){
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    // Depth defaults to DONTCARE in Sokol; the atlas is sampled later, so store it.
    pass.action.depth.store_action = SG_STOREACTION_STORE;
    pass.action.depth.clear_value = clearDepth;
}

void SokolCamera::setLabel(const char* label){
    pass.label = label;
}

// the pass (SokolSystem::beginPass: no pipeline applied, never nested), and its place in the
// frame stats' pass list
static void beginPass(const sg_pass& pass){
    SokolPassRecorder::listPass(pass);
    SokolSystem::beginPass(pass);
}

void SokolCamera::setLoadActionLoad(){
    pass.action.colors[0].load_action = SG_LOADACTION_LOAD;
}

namespace {
    sg_load_action toLoadAction(LoadAction action){
        switch (action){
        case LoadAction::CLEAR: return SG_LOADACTION_CLEAR;
        case LoadAction::LOAD: return SG_LOADACTION_LOAD;
        case LoadAction::DONTCARE: return SG_LOADACTION_DONTCARE;
        }
        return SG_LOADACTION_CLEAR;
    }

    sg_store_action toStoreAction(StoreAction action){
        return (action == StoreAction::STORE) ? SG_STOREACTION_STORE : SG_STOREACTION_DONTCARE;
    }
}

void SokolCamera::setColorActions(LoadAction load, StoreAction store, Vector4 clearColor){
    pass.action.colors[0].load_action = toLoadAction(load);
    pass.action.colors[0].store_action = toStoreAction(store);
    pass.action.colors[0].clear_value = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
}

void SokolCamera::setColorActions(int attachment, LoadAction load, StoreAction store, Vector4 clearColor){
    if (attachment < 0 || attachment >= SG_MAX_COLOR_ATTACHMENTS)
        return;
    pass.action.colors[attachment].load_action = toLoadAction(load);
    pass.action.colors[attachment].store_action = toStoreAction(store);
    pass.action.colors[attachment].clear_value = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
}

void SokolCamera::setDepthStencilActions(LoadAction load, StoreAction store, float clearDepth, uint8_t clearStencil){
    // depth and stencil always alike: Metal takes the stencil store action from depth
    pass.action.depth.load_action = toLoadAction(load);
    pass.action.depth.store_action = toStoreAction(store);
    pass.action.depth.clear_value = clearDepth;
    pass.action.stencil.load_action = toLoadAction(load);
    pass.action.stencil.store_action = toStoreAction(store);
    pass.action.stencil.clear_value = clearStencil;
}

void SokolCamera::setMultisample(FramebufferRender* msaa){
    multisample = msaa ? &msaa->backend : nullptr;
}

sg_attachments SokolCamera::attachmentsOf(FramebufferRender* framebuffer, size_t face){
    if (multisample)
        return multisample->getResolved(framebuffer->backend);
    return framebuffer->backend.get(face);
}

void SokolCamera::startRenderPass(FramebufferRender* framebuffer, size_t face){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    pass.swapchain = {};
    pass.attachments = attachmentsOf(framebuffer, face);
    if (multisample){
        // the samples resolve into the framebuffer at the end and are dropped
        sg_pass resolved = pass;
        resolved.action.colors[0].store_action = SG_STOREACTION_DONTCARE;
        beginPass(resolved);
        return;
    }
    beginPass(pass);
}

void SokolCamera::startRenderPass(TextureRender* texture, size_t face, int mip){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    sg_view_desc viewDesc = {};
    viewDesc.color_attachment.image = texture->backend.get();
    viewDesc.color_attachment.mip_level = mip;
    viewDesc.color_attachment.slice = (int)face;
    viewDesc.label = "texture-pass-color-view";
    textureView = sg_make_view(viewDesc);

    sg_pass texturePass = pass;
    texturePass.swapchain = {};
    texturePass.attachments = {};
    texturePass.attachments.colors[0] = textureView;
    // with no view the empty attachments would begin a swapchain pass
    texturePass.swapchain.invalid = (textureView.id == SG_INVALID_ID);
    beginPass(texturePass);
}

void SokolCamera::startRenderPass(int width, int height){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    pass.attachments = {};
    pass.swapchain = System::instance().getSokolSwapchain();
    pass.swapchain.width = width;
    pass.swapchain.height = height;
    beginPass(pass);
}

void SokolCamera::startRenderPass(){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    pass.attachments = {};
    pass.swapchain = System::instance().getSokolSwapchain();
    beginPass(pass);
}

void SokolCamera::startSplitRenderPass(FramebufferRender* framebuffer){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    sg_pass split = pass;
    split.swapchain = {};
    split.attachments = attachmentsOf(framebuffer, 0);
    // depth and stencil default to DONTCARE (invalidated on GLES and tilers)
    split.action.depth.store_action = SG_STOREACTION_STORE;
    split.action.stencil.store_action = SG_STOREACTION_STORE;
    beginPass(split);
}

void SokolCamera::resumeRenderPass(FramebufferRender* framebuffer, bool split, bool loadColor){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    sg_pass resumed = pass;
    resumed.swapchain = {};
    resumed.attachments = attachmentsOf(framebuffer, 0);
    if (split){
        // (DONTCARE invalidates them on GLES and tilers: the next resume would load nothing)
        resumed.action.depth.store_action = SG_STOREACTION_STORE;
        resumed.action.stencil.store_action = SG_STOREACTION_STORE;
    }else if (multisample){
        // the samples resolve into the framebuffer at the end and are dropped
        resumed.action.colors[0].store_action = SG_STOREACTION_DONTCARE;
    }
    resumed.action.colors[0].load_action = loadColor ? SG_LOADACTION_LOAD : SG_LOADACTION_DONTCARE;
    resumed.action.depth.load_action = SG_LOADACTION_LOAD;
    resumed.action.stencil.load_action = SG_LOADACTION_LOAD;
    beginPass(resumed);
}

void SokolCamera::applyViewport(Rect rect){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    float x = rect.getX();
    float y = rect.getY();
    float flooredX = std::floor(x);
    float flooredY = std::floor(y);

    // Add truncated fractional parts to width and height
    float adjustedWidth = rect.getWidth() + (x - flooredX);
    float adjustedHeight = rect.getHeight() + (y - flooredY);
    // the draws held for the rect before: the batcher's, then the instancing batch's
    SokolSystem::flushDeferred();
    SokolDrawBatch::flush();
    sg_apply_viewport(flooredX, flooredY, std::ceil(adjustedWidth), std::ceil(adjustedHeight), false);
}

void SokolCamera::applyScissor(Rect rect){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    float x = rect.getX();
    float y = rect.getY();
    float flooredX = std::floor(x);
    float flooredY = std::floor(y);

    // Add truncated fractional parts to width and height
    float adjustedWidth = rect.getWidth() + (x - flooredX);
    float adjustedHeight = rect.getHeight() + (y - flooredY);
    SokolSystem::applyScissor((int)flooredX, (int)flooredY, (int)std::ceil(adjustedWidth), (int)std::ceil(adjustedHeight), false);
}

// top-left rects as they are on every backend: GL's offscreen targets are rendered Y-flipped
// (RenderSystem::isRenderingFlipped), so its bottom-left origin is the image's top there
void SokolCamera::applyViewportTopLeft(int x, int y, int width, int height){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    // the draws held for the rect before: the batcher's, then the instancing batch's
    SokolSystem::flushDeferred();
    SokolDrawBatch::flush();
    sg_apply_viewport(x, y, width, height, SystemRender::getFeatures().originTopLeft);
}

void SokolCamera::applyScissorTopLeft(int x, int y, int width, int height){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    SokolSystem::applyScissor(x, y, width, height, SystemRender::getFeatures().originTopLeft);
}

void SokolCamera::endRenderPass(){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    // (the Frame Debugger's preview of this pass, when it asked for it)
    SokolSystem::endPass();

    if (textureView.id != SG_INVALID_ID){
        sg_destroy_view(textureView);
        textureView.id = SG_INVALID_ID;
    }
}
