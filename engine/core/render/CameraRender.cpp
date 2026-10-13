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

void CameraRender::setLoadActionLoad(){
    backend.setLoadActionLoad();
}

void CameraRender::startRenderPass(FramebufferRender* framebuffer, size_t face){
    backend.startRenderPass(framebuffer, face);
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

void CameraRender::endRenderPass(){
    backend.endRenderPass();
}
