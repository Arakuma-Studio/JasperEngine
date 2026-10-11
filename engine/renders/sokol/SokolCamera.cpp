// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolCamera.h"

#include "System.h"

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

void SokolCamera::startRenderPass(FramebufferRender* framebuffer, size_t face){
    pass.swapchain = {};
    pass.attachments = framebuffer->backend.get(face);
    sg_begin_pass(pass);
}

void SokolCamera::startRenderPass(int width, int height){
    pass.attachments = {};
    pass.swapchain = System::instance().getSokolSwapchain();
    pass.swapchain.width = width;
    pass.swapchain.height = height;
    sg_begin_pass(pass);
}

void SokolCamera::startRenderPass(){
    pass.attachments = {};
    pass.swapchain = System::instance().getSokolSwapchain();
    sg_begin_pass(pass);
}

void SokolCamera::startSplitRenderPass(FramebufferRender* framebuffer){
    sg_pass split = pass;
    split.swapchain = {};
    split.attachments = framebuffer->backend.get(0);
    // depth and stencil default to DONTCARE (invalidated on GLES and tilers)
    split.action.depth.store_action = SG_STOREACTION_STORE;
    split.action.stencil.store_action = SG_STOREACTION_STORE;
    sg_begin_pass(split);
}

void SokolCamera::resumeRenderPass(FramebufferRender* framebuffer){
    sg_pass resumed = pass;
    resumed.swapchain = {};
    resumed.attachments = framebuffer->backend.get(0);
    resumed.action.colors[0].load_action = SG_LOADACTION_LOAD;
    resumed.action.depth.load_action = SG_LOADACTION_LOAD;
    resumed.action.stencil.load_action = SG_LOADACTION_LOAD;
    sg_begin_pass(resumed);
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

void SokolCamera::endRenderPass(){
    sg_end_pass();
}
