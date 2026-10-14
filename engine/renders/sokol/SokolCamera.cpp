// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolCamera.h"

#include "System.h"
#include "SokolSystem.h"

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

void SokolCamera::setLabel(const char* label){
    pass.label = label;
}

// sg_begin_pass, and the pass's place in the frame stats' pass list
static void beginPass(const sg_pass& pass){
    sg_begin_pass(pass);
    SokolSystem::recordPass(pass);
}

void SokolCamera::setLoadActionLoad(){
    pass.action.colors[0].load_action = SG_LOADACTION_LOAD;
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

void SokolCamera::startRenderPass(int width, int height){
    pass.attachments = {};
    pass.swapchain = System::instance().getSokolSwapchain();
    pass.swapchain.width = width;
    pass.swapchain.height = height;
    beginPass(pass);
}

void SokolCamera::startRenderPass(){
    pass.attachments = {};
    pass.swapchain = System::instance().getSokolSwapchain();
    beginPass(pass);
}

void SokolCamera::startSplitRenderPass(FramebufferRender* framebuffer){
    sg_pass split = pass;
    split.swapchain = {};
    split.attachments = attachmentsOf(framebuffer, 0);
    // depth and stencil default to DONTCARE (invalidated on GLES and tilers)
    split.action.depth.store_action = SG_STOREACTION_STORE;
    split.action.stencil.store_action = SG_STOREACTION_STORE;
    beginPass(split);
}

void SokolCamera::resumeRenderPass(FramebufferRender* framebuffer){
    sg_pass resumed = pass;
    resumed.swapchain = {};
    resumed.attachments = attachmentsOf(framebuffer, 0);
    if (multisample){
        resumed.action.colors[0].store_action = SG_STOREACTION_DONTCARE;
    }
    resumed.action.colors[0].load_action = SG_LOADACTION_LOAD;
    resumed.action.depth.load_action = SG_LOADACTION_LOAD;
    resumed.action.stencil.load_action = SG_LOADACTION_LOAD;
    beginPass(resumed);
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
