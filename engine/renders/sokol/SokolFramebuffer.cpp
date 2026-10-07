// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolFramebuffer.h"
#include "Log.h"
#include "SokolCmdQueue.h"
#include "SokolPipelineCache.h"
#include "Engine.h"
#include "render/FramebufferRender.h"

using namespace jasperengine;

SokolFramebuffer::SokolFramebuffer(){
    for (int a = 0; a < MAX_COLOR_ATTACHMENTS; a++){
        for (int i = 0; i < 6; i++){
            colorAttachmentViews[a][i].id = SG_INVALID_ID;
        }
    }
    depthAttachmentView.id = SG_INVALID_ID;
    resolveAttachmentView.id = SG_INVALID_ID;
    numColorAttachments = 1;
    sampleCount = 1;
    ownsDepthAttachment = true;
}

SokolFramebuffer::SokolFramebuffer(const SokolFramebuffer& rhs) = default;

SokolFramebuffer& SokolFramebuffer::operator=(const SokolFramebuffer& rhs) = default;

bool SokolFramebuffer::createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap){
    if ((textureType != TextureType::TEXTURE_2D) && (textureType != TextureType::TEXTURE_CUBE)){
        Log::error("Framebuffer texture type must be 2D or CUBE");
        return false;
    }

    destroyFramebuffer();

    numColorAttachments = 1;

    bool colorCreated = colorTexture[0].createFramebufferTexture(textureType, false, shadowMap, width, height, minFilter, magFilter, wrapU, wrapV);
    bool depthCreated = depthTexture.createFramebufferTexture(TextureType::TEXTURE_2D, true, shadowMap, width, height, minFilter, magFilter, wrapU, wrapV);

    if (!colorCreated || !depthCreated) {
        destroyFramebuffer();
        return false;
    }

    size_t faces = (textureType == TextureType::TEXTURE_CUBE)? 6 : 1;

    for (int i = 0; i < faces; i++){
        sg_view_desc color_view_desc = {0};
        color_view_desc.color_attachment.image = colorTexture[0].backend.get();
        color_view_desc.color_attachment.slice = i;
        color_view_desc.label = "framebuffer-color-attachment-view";

        if (Engine::isAsyncThread()){
            colorAttachmentViews[0][i] = SokolCmdQueue::add_command_make_view(color_view_desc);
        }else{
            colorAttachmentViews[0][i] = sg_make_view(color_view_desc);
        }
    }

    sg_view_desc depth_view_desc = {0};
    depth_view_desc.depth_stencil_attachment.image = depthTexture.backend.get();
    depth_view_desc.label = "framebuffer-depth-attachment-view";

    if (Engine::isAsyncThread()){
        depthAttachmentView = SokolCmdQueue::add_command_make_view(depth_view_desc);
    }else{
        depthAttachmentView = sg_make_view(depth_view_desc);
    }

    bool created = isCreated();
    if (!created) {
        destroyFramebuffer();
    }else{
        initializeAttachments(faces);
    }

    return created;
}

bool SokolFramebuffer::createDepthOnlyFramebuffer(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap){
    destroyFramebuffer();

    numColorAttachments = 0;

    bool depthCreated = depthTexture.createFramebufferTexture(
        TextureType::TEXTURE_2D, true, shadowMap, width, height,
        minFilter, magFilter, wrapU, wrapV);
    if (!depthCreated){
        destroyFramebuffer();
        return false;
    }

    sg_view_desc depth_view_desc = {0};
    depth_view_desc.depth_stencil_attachment.image = depthTexture.backend.get();
    depth_view_desc.label = "framebuffer-depth-only-attachment-view";

    if (Engine::isAsyncThread()){
        depthAttachmentView = SokolCmdQueue::add_command_make_view(depth_view_desc);
    }else{
        depthAttachmentView = sg_make_view(depth_view_desc);
    }

    bool created = isCreated();
    if (!created){
        destroyFramebuffer();
    }else{
        initializeAttachments(1);
    }

    return created;
}

bool SokolFramebuffer::createFramebufferMRT(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, int numColor, const ColorFormat* formats){
    if (numColor < 1 || numColor > MAX_COLOR_ATTACHMENTS){
        Log::error("Framebuffer MRT color attachment count must be 1..%d", MAX_COLOR_ATTACHMENTS);
        return false;
    }

    destroyFramebuffer();

    numColorAttachments = numColor;

    bool depthCreated = depthTexture.createFramebufferTexture(TextureType::TEXTURE_2D, true, false, width, height, minFilter, magFilter, wrapU, wrapV);
    if (!depthCreated){
        destroyFramebuffer();
        return false;
    }

    for (int a = 0; a < numColorAttachments; a++){
        ColorFormat fmt = formats ? formats[a] : ColorFormat::RGBA;
        bool colorCreated = colorTexture[a].createFramebufferTexture(TextureType::TEXTURE_2D, false, false, width, height, minFilter, magFilter, wrapU, wrapV, fmt);
        if (!colorCreated){
            destroyFramebuffer();
            return false;
        }

        sg_view_desc color_view_desc = {0};
        color_view_desc.color_attachment.image = colorTexture[a].backend.get();
        color_view_desc.color_attachment.slice = 0;
        color_view_desc.label = "framebuffer-mrt-color-attachment-view";

        if (Engine::isAsyncThread()){
            colorAttachmentViews[a][0] = SokolCmdQueue::add_command_make_view(color_view_desc);
        }else{
            colorAttachmentViews[a][0] = sg_make_view(color_view_desc);
        }
    }

    sg_view_desc depth_view_desc = {0};
    depth_view_desc.depth_stencil_attachment.image = depthTexture.backend.get();
    depth_view_desc.label = "framebuffer-mrt-depth-attachment-view";

    if (Engine::isAsyncThread()){
        depthAttachmentView = SokolCmdQueue::add_command_make_view(depth_view_desc);
    }else{
        depthAttachmentView = sg_make_view(depth_view_desc);
    }

    bool created = isCreated();
    if (!created) {
        destroyFramebuffer();
    }else{
        initializeAttachments(1);
    }

    return created;
}

namespace {
    sg_view makeView(const sg_view_desc& desc){
        if (Engine::isAsyncThread())
            return SokolCmdQueue::add_command_make_view(desc);
        return sg_make_view(desc);
    }
}

bool SokolFramebuffer::createFramebuffer(const FramebufferDesc& desc){
    if (desc.width <= 0 || desc.height <= 0){
        Log::error("Framebuffer '%s' has an invalid size %i x %i", desc.label.c_str(), desc.width, desc.height);
        return false;
    }
    if (desc.color == ColorFormat::RED16){
        Log::error("Framebuffer '%s': RED16 is not a colour attachment format", desc.label.c_str());
        return false;
    }

    destroyFramebuffer();

    numColorAttachments = 1;
    sampleCount = (desc.sampleCount > 1) ? desc.sampleCount : 1;
    const bool msaa = sampleCount > 1;

    // colour: the sampled image is the resolve image under MSAA
    bool colorCreated = colorTexture[0].createFramebufferTexture(TextureType::TEXTURE_2D, false, false, desc.width, desc.height,
                                                                 desc.minFilter, desc.magFilter, desc.wrapU, desc.wrapV, desc.color,
                                                                 1, false, msaa);
    if (colorCreated && msaa){
        colorCreated = msaaColorTexture.createFramebufferTexture(TextureType::TEXTURE_2D, false, false, desc.width, desc.height,
                                                                 desc.minFilter, desc.magFilter, desc.wrapU, desc.wrapV, desc.color,
                                                                 sampleCount);
    }
    if (!colorCreated){
        destroyFramebuffer();
        return false;
    }

    sg_view_desc colorViewDesc = {};
    colorViewDesc.color_attachment.image = msaa ? msaaColorTexture.backend.get() : colorTexture[0].backend.get();
    colorViewDesc.label = "framebuffer-color-attachment-view";
    colorAttachmentViews[0][0] = makeView(colorViewDesc);

    if (msaa){
        sg_view_desc resolveViewDesc = {};
        resolveViewDesc.resolve_attachment.image = colorTexture[0].backend.get();
        resolveViewDesc.label = "framebuffer-resolve-attachment-view";
        resolveAttachmentView = makeView(resolveViewDesc);
    }

    if (desc.depth != DepthStencilFormat::NONE){
        const bool stencil = desc.depth == DepthStencilFormat::DEPTH_STENCIL;
        if (desc.shareDepthStencil){
            // the view, not only the image: GLES keeps an MSAA attachment's storage in its view
            const SokolFramebuffer& shared = desc.shareDepthStencil->backend;
            if (shared.depthAttachmentView.id == SG_INVALID_ID || shared.sampleCount != sampleCount){
                Log::error("Framebuffer '%s' cannot share a depth-stencil attachment of another sample count", desc.label.c_str());
                destroyFramebuffer();
                return false;
            }
            depthAttachmentView = shared.depthAttachmentView;
            ownsDepthAttachment = false;
        }else{
            if (!depthTexture.createFramebufferTexture(TextureType::TEXTURE_2D, true, false, desc.width, desc.height,
                                                       desc.minFilter, desc.magFilter, desc.wrapU, desc.wrapV, ColorFormat::RGBA,
                                                       sampleCount, stencil)){
                destroyFramebuffer();
                return false;
            }
            sg_view_desc depthViewDesc = {};
            depthViewDesc.depth_stencil_attachment.image = depthTexture.backend.get();
            depthViewDesc.label = "framebuffer-depth-attachment-view";
            depthAttachmentView = makeView(depthViewDesc);
        }
    }

    bool created = isCreated();
    if (!created){
        destroyFramebuffer();
    }else{
        initializeAttachments(1);
    }

    return created;
}

void SokolFramebuffer::initializeAttachments(size_t faces){
#if defined(SOKOL_VULKAN)
    // A camera texture can be sampled before its camera has rendered once.
    // Give new attachments defined contents and a shader-readable layout.
    sg_pass pass = {};
    for (int i = 0; i < numColorAttachments; i++){
        pass.action.colors[i].store_action = SG_STOREACTION_STORE;
    }
    pass.action.depth.store_action = SG_STOREACTION_STORE;
    pass.action.stencil.store_action = SG_STOREACTION_STORE;

    for (size_t face = 0; face < faces; face++){
        pass.attachments = get(face);
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_begin_pass(pass);
            SokolCmdQueue::add_command_end_pass();
        }else{
            sg_begin_pass(pass);
            sg_end_pass();
            SokolPipelineCache::invalidateApplied();
        }
    }
#else
    (void)faces;
#endif
}

void SokolFramebuffer::destroyFramebuffer(){
    if (sg_isvalid()){
        for (int a = 0; a < MAX_COLOR_ATTACHMENTS; a++){
            for (int i = 0; i < 6; i++){
                if (colorAttachmentViews[a][i].id == SG_INVALID_ID) {
                    continue;
                }

                if (Engine::isAsyncThread()){
                    SokolCmdQueue::add_command_destroy_view(colorAttachmentViews[a][i]);
                }else{
                    sg_destroy_view(colorAttachmentViews[a][i]);
                }
            }
        }

        // a shared depth-stencil view stays with its owner
        for (sg_view view : {ownsDepthAttachment ? depthAttachmentView : sg_view{SG_INVALID_ID}, resolveAttachmentView}){
            if (view.id == SG_INVALID_ID)
                continue;
            if (Engine::isAsyncThread()){
                SokolCmdQueue::add_command_destroy_view(view);
            }else{
                sg_destroy_view(view);
            }
        }
    }

    for (int a = 0; a < MAX_COLOR_ATTACHMENTS; a++){
        colorTexture[a].destroyTexture();
    }
    depthTexture.destroyTexture();
    msaaColorTexture.destroyTexture();

    for (int a = 0; a < MAX_COLOR_ATTACHMENTS; a++){
        for (int i = 0; i < 6; i++){
            colorAttachmentViews[a][i].id = SG_INVALID_ID;
        }
    }
    depthAttachmentView.id = SG_INVALID_ID;
    resolveAttachmentView.id = SG_INVALID_ID;
    numColorAttachments = 1;
    sampleCount = 1;
    ownsDepthAttachment = true;
}

bool SokolFramebuffer::isCreated(){
    if (!sg_isvalid()) {
        return false;
    }

    bool hasAttachment = false;
    for (int a = 0; a < MAX_COLOR_ATTACHMENTS; a++){
        for (int i = 0; i < 6; i++) {
            if (colorAttachmentViews[a][i].id == SG_INVALID_ID) {
                continue;
            }

            hasAttachment = true;
            if (sg_query_view_state(colorAttachmentViews[a][i]) != SG_RESOURCESTATE_VALID) {
                return false;
            }
        }
    }

    if (depthAttachmentView.id != SG_INVALID_ID) {
        hasAttachment = true;
        if (sg_query_view_state(depthAttachmentView) != SG_RESOURCESTATE_VALID) {
            return false;
        }
    }

    if (resolveAttachmentView.id != SG_INVALID_ID && sg_query_view_state(resolveAttachmentView) != SG_RESOURCESTATE_VALID) {
        return false;
    }

    return hasAttachment;
}

int SokolFramebuffer::getSampleCount() const{
    return sampleCount;
}

sg_view SokolFramebuffer::getDepthAttachmentView() const{
    return depthAttachmentView;
}

int SokolFramebuffer::getNumColorAttachments() const{
    return numColorAttachments;
}

TextureRender& SokolFramebuffer::getColorTexture(){
    return colorTexture[0];
}

TextureRender& SokolFramebuffer::getColorTexture(int index){
    if (index < 0 || index >= MAX_COLOR_ATTACHMENTS){
        index = 0;
    }
    return colorTexture[index];
}

TextureRender& SokolFramebuffer::getDepthTexture(){
    return depthTexture;
}

const void* SokolFramebuffer::getD3D11HandlerColorRTV() const{
    if (colorAttachmentViews[0][0].id != SG_INVALID_ID && sg_isvalid()){
        sg_d3d11_view_info info = sg_d3d11_query_view_info(colorAttachmentViews[0][0]);

        return info.rtv;
    }

    return nullptr;
}

const void* SokolFramebuffer::getD3D11HandlerDSV() const{
    if (depthAttachmentView.id != SG_INVALID_ID && sg_isvalid()){
        sg_d3d11_view_info info = sg_d3d11_query_view_info(depthAttachmentView);

        return info.dsv;
    }

    return nullptr;
}

sg_attachments SokolFramebuffer::get(size_t face){
    sg_attachments attachments = {};

    if (numColorAttachments > 1){
        // MRT framebuffers are 2D (single face); bind every color attachment.
        for (int a = 0; a < numColorAttachments; a++){
            attachments.colors[a] = colorAttachmentViews[a][0];
        }
    }else if (numColorAttachments == 1){
        attachments.colors[0] = colorAttachmentViews[0][face];
        // MSAA: every pass ends with a resolve into colorTexture[0]
        attachments.resolves[0] = resolveAttachmentView;
    }
    attachments.depth_stencil = depthAttachmentView;

    return attachments;
}
