// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Framebuffer.h"

#include "Engine.h"
#include "Log.h"

using namespace jasperengine;

Framebuffer::Framebuffer(){
    this->width = 512;
    this->height = 512;
    this->minFilter = TextureFilter::LINEAR;
    this->magFilter = TextureFilter::LINEAR;
    this->wrapU = TextureWrap::REPEAT;
    this->wrapV = TextureWrap::REPEAT;
    this->numColorAttachments = 1;
    for (int i = 0; i < SokolFramebuffer::MAX_COLOR_ATTACHMENTS; i++){
        this->colorFormats[i] = ColorFormat::RGBA;
    }
    this->depth = FramebufferDepth::DEPTH_STENCIL;
    this->version = 0;
    this->hasDesc = false;
}

Framebuffer::~Framebuffer(){
    destroy();
}

Framebuffer::Framebuffer(const Framebuffer& rhs) = default;

Framebuffer& Framebuffer::operator=(const Framebuffer& rhs) = default;

void Framebuffer::create(){
    if (hasDesc){
        render.createFramebuffer(desc);
    }else if (numColorAttachments > 1){
        render.createFramebufferMRT(width, height, minFilter, magFilter, wrapU, wrapV, numColorAttachments, colorFormats, depth == FramebufferDepth::DEPTH_STENCIL);
    }else{
        render.createFramebuffer(TextureType::TEXTURE_2D, width, height, minFilter, magFilter, wrapU, wrapV, false, depth, colorFormats[0]);
    }
    version++;
}

bool Framebuffer::create(const FramebufferDesc& desc){
    this->desc = desc;
    this->hasDesc = true;
    this->width = (unsigned int)(desc.width > 0 ? desc.width : 0);
    this->height = (unsigned int)(desc.height > 0 ? desc.height : 0);
    this->minFilter = desc.minFilter;
    this->magFilter = desc.magFilter;
    this->wrapU = desc.wrapU;
    this->wrapV = desc.wrapV;
    this->numColorAttachments = 1;
    this->colorFormats[0] = desc.color;
    render.destroyFramebuffer();
    create();
    return isCreated();
}

void Framebuffer::setColorAttachments(int count, const ColorFormat* formats){
    if (count < 1) count = 1;
    if (count > SokolFramebuffer::MAX_COLOR_ATTACHMENTS) count = SokolFramebuffer::MAX_COLOR_ATTACHMENTS;
    this->numColorAttachments = count;
    for (int i = 0; i < SokolFramebuffer::MAX_COLOR_ATTACHMENTS; i++){
        this->colorFormats[i] = (formats && i < count) ? formats[i] : ColorFormat::RGBA;
    }
}

int Framebuffer::getNumColorAttachments() const{
    return this->numColorAttachments;
}

void Framebuffer::setDepthStencil(bool depthStencil){
    this->depth = depthStencil ? FramebufferDepth::DEPTH_STENCIL : FramebufferDepth::DEPTH;
}

bool Framebuffer::isDepthStencil() const{
    return this->depth == FramebufferDepth::DEPTH_STENCIL;
}

void Framebuffer::setDepthAttachment(FramebufferDepth depth){
    this->depth = depth;
}

FramebufferDepth Framebuffer::getDepthAttachment() const{
    return this->depth;
}

void Framebuffer::destroy(){
    render.destroyFramebuffer();
}

bool Framebuffer::isCreated(){
    return render.isCreated();
}

FramebufferRender& Framebuffer::getRender(){
    return render;
}

unsigned long Framebuffer::getVersion(){
    return this->version;
}

void Framebuffer::setWidth(unsigned int width){
    this->width = width;
}

unsigned int Framebuffer::getWidth() const{
    return this->width;
}

void Framebuffer::setHeight(unsigned int height){
    this->height = height;
}

unsigned int Framebuffer::getHeight() const{
    return this->height;
}

void Framebuffer::setMinFilter(TextureFilter filter){
    this->minFilter = filter;
}

TextureFilter Framebuffer::getMinFilter() const{
    return this->minFilter;
}

void Framebuffer::setMagFilter(TextureFilter filter){
    this->magFilter = filter;
}

TextureFilter Framebuffer::getMagFilter() const{
    return this->magFilter;
}

void Framebuffer::setWrapU(TextureWrap wrapU){
    this->wrapU = wrapU;
}

TextureWrap Framebuffer::getWrapU() const{
    return this->wrapU;
}

void Framebuffer::setWrapV(TextureWrap wrapV){
    this->wrapV = wrapV;
}

TextureWrap Framebuffer::getWrapV() const{
    return this->wrapV;
}