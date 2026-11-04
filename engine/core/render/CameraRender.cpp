// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CameraRender.h"

#include "sokol/SokolCamera.h"

using namespace jasperengine;

CameraRender::CameraRender(){ }

CameraRender::CameraRender(const CameraRender& rhs) = default;

CameraRender& CameraRender::operator=(const CameraRender& rhs) = default;

CameraRender::~CameraRender(){ }

void CameraRender::setClearColor(Vector4 clearColor){
    backend.setClearColor(clearColor);
}

void CameraRender::setClearDepth(float clearDepth){
    backend.setClearDepth(clearDepth);
}

void CameraRender::setLabel(const char* label){
    backend.setLabel(label);
}

void CameraRender::setMultisample(FramebufferRender* msaa){
    backend.setMultisample(msaa);
}

void CameraRender::setLoadActionLoad(){
    backend.setLoadActionLoad();
}

void CameraRender::setColorActions(LoadAction load, StoreAction store, Vector4 clearColor){
    backend.setColorActions(load, store, clearColor);
}

void CameraRender::setColorActions(int attachment, LoadAction load, StoreAction store, Vector4 clearColor){
    backend.setColorActions(attachment, load, store, clearColor);
}

void CameraRender::setDepthStencilActions(LoadAction load, StoreAction store, float clearDepth, uint8_t clearStencil){
    backend.setDepthStencilActions(load, store, clearDepth, clearStencil);
}

void CameraRender::startRenderPass(FramebufferRender* framebuffer, size_t face){
    backend.startRenderPass(framebuffer, face);
}

void CameraRender::startRenderPass(TextureRender* texture, size_t face, int mip){
    backend.startRenderPass(texture, face, mip);
}

void CameraRender::startRenderPass(int width, int height){
    backend.startRenderPass(width, height);
}

void CameraRender::startRenderPass(){
    backend.startRenderPass();
}

void CameraRender::startSplitRenderPass(FramebufferRender* framebuffer){
    backend.startSplitRenderPass(framebuffer);
}

void CameraRender::resumeRenderPass(FramebufferRender* framebuffer){
    backend.resumeRenderPass(framebuffer);
}

void CameraRender::applyViewport(Rect rect){
    backend.applyViewport(rect);
}

void CameraRender::applyScissor(Rect rect){
    backend.applyScissor(rect);
}

void CameraRender::applyScissorTopLeft(int x, int y, int width, int height){
    backend.applyScissorTopLeft(x, y, width, height);
}

void CameraRender::applyViewportTopLeft(int x, int y, int width, int height){
    backend.applyViewportTopLeft(x, y, width, height);
}

void CameraRender::endRenderPass(){
    backend.endRenderPass();
}
