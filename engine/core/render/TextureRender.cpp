// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureRender.h"
#include "Engine.h"

using namespace jasperengine;

TextureRender::TextureRender(){
 }

TextureRender::TextureRender(const TextureRender& rhs) = default;

TextureRender& TextureRender::operator=(const TextureRender& rhs) = default;

TextureRender::~TextureRender(){
    //Cannot destroy because its a handle
}

bool TextureRender::createTexture(
                const std::string& label, int width, int height,
                ColorFormat colorFormat, TextureType type, int numFaces, void* data[], size_t size[], 
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV){
    if (Engine::isViewLoaded() && !isCreated())
        return backend.createTexture(label, width, height, colorFormat, type, numFaces, data, size, minFilter, magFilter, wrapU, wrapV);
    else
        return false;
}

bool TextureRender::createTexture3D(const std::string& label, int width, int height, int depth, const void* data, size_t size,
                                    TextureFilter filter, TextureWrap wrap){
    return backend.createTexture3D(label, width, height, depth, data, size, filter, wrap);
}

bool TextureRender::createTextureCubeWithMips(
                const std::string& label, int width,
                ColorFormat colorFormat, int numMipmaps, void* data[], size_t size[],
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV){
    if (Engine::isViewLoaded() && !isCreated())
        return backend.createTextureCubeWithMips(label, width, colorFormat, numMipmaps, data, size, minFilter, magFilter, wrapU, wrapV);
    else
        return false;
}

bool TextureRender::createFramebufferTexture(
                TextureType type, bool depth, bool shadowMap, int width, int height,
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV,
                ColorFormat colorFormat, int sampleCount, bool depthStencil, bool resolve, int numMipmaps){
    if (Engine::isViewLoaded() && !isCreated())
        return backend.createFramebufferTexture(type, depth, shadowMap, width, height, minFilter, magFilter, wrapU, wrapV, colorFormat,
                                                sampleCount, depthStencil, resolve, numMipmaps);
    else
        return false;
}

bool TextureRender::createDynamicTexture(const std::string& label, int width, int height){
    return Engine::isViewLoaded() && !isCreated() &&
        backend.createDynamicTexture(label, width, height);
}

bool TextureRender::createDynamicTexture(const std::string& label, int width, int height,
                                         ColorFormat format, TextureFilter filter){
    return Engine::isViewLoaded() && !isCreated() &&
        backend.createDynamicTexture(label, width, height, format, filter);
}

bool TextureRender::createDynamicColorTexture(const std::string& label, int width, int height){
    return Engine::isViewLoaded() && !isCreated() &&
        backend.createDynamicColorTexture(label, width, height);
}

void TextureRender::updateTexture(const void* data, size_t size){
    backend.updateTexture(data, size);
}

bool TextureRender::createAliasView(const TextureRender& source, const char* label){
    return backend.createAliasView(source.backend, label);
}

void TextureRender::destroyTexture(){
    backend.destroyTexture();
    transparent = false;
}

void TextureRender::setTransparent(bool transparent){
    this->transparent = transparent;
}

bool TextureRender::isTransparent() const{
    return transparent;
}

uint32_t TextureRender::getGLHandler() const{
    return backend.getGLHandler();
}

const void* TextureRender::getMetalHandler() const{
    return backend.getMetalHandler();
}

const void* TextureRender::getD3D11Handler() const{
    return backend.getD3D11Handler();
}

const void* TextureRender::getVulkanHandler() const{
    return backend.getVulkanHandler();
}

const void* TextureRender::getVulkanImageHandler() const{
    return backend.getVulkanImageHandler();
}

uint32_t TextureRender::getViewId() const{
    return backend.getViewId();
}

int TextureRender::getWidth() const{
    return backend.getWidth();
}

int TextureRender::getHeight() const{
    return backend.getHeight();
}

bool TextureRender::isViewValid(uint32_t viewId){
    return SokolTexture::isViewValid(viewId);
}

bool TextureRender::isCreated(){
    return backend.isCreated();
}
