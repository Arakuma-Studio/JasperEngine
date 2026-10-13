// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FramebufferRender.h"
#include "Engine.h"

using namespace jasperengine;

FramebufferRender::FramebufferRender(){ }

FramebufferRender::FramebufferRender(const FramebufferRender& rhs) = default;

FramebufferRender& FramebufferRender::operator=(const FramebufferRender& rhs) = default;

FramebufferRender::~FramebufferRender(){
    //Cannot destroy because its a handle
}

bool FramebufferRender::createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap){
    return createFramebuffer(textureType, width, height, minFilter, magFilter, wrapU, wrapV, shadowMap, !shadowMap);
}

bool FramebufferRender::createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap, bool stencil){
    return createFramebuffer(textureType, width, height, minFilter, magFilter, wrapU, wrapV, shadowMap,
                             stencil ? FramebufferDepth::DEPTH_STENCIL : FramebufferDepth::DEPTH);
}

bool FramebufferRender::createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap, FramebufferDepth depth,
                                          ColorFormat colorFormat){
    if (Engine::isViewLoaded() && !isCreated())
        return backend.createFramebuffer(textureType, width, height, minFilter, magFilter, wrapU, wrapV, shadowMap, depth, colorFormat);
    else
        return false;
}

bool FramebufferRender::createMultisampleFramebuffer(int width, int height, int samples){
    if (Engine::isViewLoaded() && !isCreated())
        return backend.createMultisampleFramebuffer(width, height, samples);
    else
        return false;
}

int FramebufferRender::getMsaaSamples() const{
    return backend.getMsaaSamples();
}

int FramebufferRender::getMaxMsaaSamples(int requested){
    return SokolFramebuffer::getMaxMsaaSamples(requested);
}

bool FramebufferRender::createDepthOnlyFramebuffer(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap){
    if (Engine::isViewLoaded() && !isCreated())
        return backend.createDepthOnlyFramebuffer(width, height, minFilter, magFilter, wrapU, wrapV, shadowMap);
    else
        return false;
}

bool FramebufferRender::createFramebufferMRT(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, int numColor, const ColorFormat* formats, bool stencil){
    if (Engine::isViewLoaded() && !isCreated())
        return backend.createFramebufferMRT(width, height, minFilter, magFilter, wrapU, wrapV, numColor, formats, stencil);
    else
        return false;
}

void FramebufferRender::destroyFramebuffer(){
    backend.destroyFramebuffer();
}

bool FramebufferRender::isCreated(){
    return backend.isCreated();
}

TextureRender& FramebufferRender::getColorTexture(){
    return backend.getColorTexture();
}

TextureRender& FramebufferRender::getColorAttachmentTexture(int index){
    return backend.getColorTexture(index);
}

int FramebufferRender::getNumColorAttachments() const{
    return backend.getNumColorAttachments();
}

TextureRender& FramebufferRender::getDepthTexture(){
    return backend.getDepthTexture();
}

const void* FramebufferRender::getD3D11HandlerColorRTV() const{
    return backend.getD3D11HandlerColorRTV();
}

const void* FramebufferRender::getD3D11HandlerDSV() const{
    return backend.getD3D11HandlerDSV();
}
