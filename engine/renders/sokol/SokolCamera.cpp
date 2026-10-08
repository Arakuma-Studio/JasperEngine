// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolCamera.h"

#include "System.h"
#include "SokolPipelineCache.h"
#include "render/SystemRender.h"

#include "sokol_gfx.h"
#include <cmath>

using namespace jasperengine;

SokolCamera::SokolCamera(){
    pass = {0};
    pass.action.colors[0].load_action = SG_LOADACTION_LOAD;
}

SokolCamera::SokolCamera(const SokolCamera& rhs) = default;

SokolCamera& SokolCamera::operator=(const SokolCamera& rhs) = default;

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

void SokolCamera::setDepthStencilActions(LoadAction load, StoreAction store, float clearDepth, uint8_t clearStencil){
    // depth and stencil always alike: Metal takes the stencil store action from depth
    pass.action.depth.load_action = toLoadAction(load);
    pass.action.depth.store_action = toStoreAction(store);
    pass.action.depth.clear_value = clearDepth;
    pass.action.stencil.load_action = toLoadAction(load);
    pass.action.stencil.store_action = toStoreAction(store);
    pass.action.stencil.clear_value = clearStencil;
}

void SokolCamera::startRenderPass(FramebufferRender* framebuffer, size_t face){
    pass.swapchain = {};
    pass.attachments = framebuffer->backend.get(face);
    sg_begin_pass(pass);
    SokolPipelineCache::invalidateApplied();
}

void SokolCamera::startRenderPass(int width, int height){
    pass.attachments = {};
    pass.swapchain = System::instance().getSokolSwapchain();
    pass.swapchain.width = width;
    pass.swapchain.height = height;
    sg_begin_pass(pass);
    SokolPipelineCache::invalidateApplied();
}

void SokolCamera::startRenderPass(){
    pass.attachments = {};
    pass.swapchain = System::instance().getSokolSwapchain();
    sg_begin_pass(pass);
    SokolPipelineCache::invalidateApplied();
}

void SokolCamera::applyViewport(Rect rect){
    float x = rect.getX();
    float y = rect.getY();
    float flooredX = std::floor(x);
    float flooredY = std::floor(y);

    // Add truncated fractional parts to width and height
    float adjustedWidth = rect.getWidth() + (x - flooredX);
    float adjustedHeight = rect.getHeight() + (y - flooredY);
    sg_apply_viewport(flooredX, flooredY, std::ceil(adjustedWidth), std::ceil(adjustedHeight), false);
}

void SokolCamera::applyScissor(Rect rect){
    float x = rect.getX();
    float y = rect.getY();
    float flooredX = std::floor(x);
    float flooredY = std::floor(y);

    // Add truncated fractional parts to width and height
    float adjustedWidth = rect.getWidth() + (x - flooredX);
    float adjustedHeight = rect.getHeight() + (y - flooredY);
    sg_apply_scissor_rect(flooredX, flooredY, std::ceil(adjustedWidth), std::ceil(adjustedHeight), false);
}

// top-left rects as they are on every backend: GL's offscreen targets are rendered Y-flipped
// (RenderSystem::isRenderingFlipped), so its bottom-left origin is the image's top there
void SokolCamera::applyViewportTopLeft(int x, int y, int width, int height){
    sg_apply_viewport(x, y, width, height, SystemRender::getFeatures().originTopLeft);
}

void SokolCamera::applyScissorTopLeft(int x, int y, int width, int height){
    sg_apply_scissor_rect(x, y, width, height, SystemRender::getFeatures().originTopLeft);
}

void SokolCamera::endRenderPass(){
    sg_end_pass();
}
