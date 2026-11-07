// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolFramebuffer.h"

#include <algorithm>
#include "Log.h"
#include "SokolCmdQueue.h"
#include "SokolSystem.h"
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
    for (sg_view& view : resolveAttachmentViews){
        view.id = SG_INVALID_ID;
    }
    numColorAttachments = 1;
    sampleCount = 1;
    ownsDepthAttachment = true;
    msaaColorImage.id = SG_INVALID_ID;
    msaaDepthImage.id = SG_INVALID_ID;
    msaaSamples = 1;
    nextResolveView = 0;
}

SokolFramebuffer::SokolFramebuffer(const SokolFramebuffer& rhs) = default;

SokolFramebuffer& SokolFramebuffer::operator=(const SokolFramebuffer& rhs) = default;

bool SokolFramebuffer::createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap, FramebufferDepth depth,
                                         ColorFormat colorFormat){
    if ((textureType != TextureType::TEXTURE_2D) && (textureType != TextureType::TEXTURE_CUBE)){
        Log::error("Framebuffer texture type must be 2D or CUBE");
        return false;
    }

    destroyFramebuffer();

    numColorAttachments = 1;

    bool colorCreated = colorTexture[0].createFramebufferTexture(textureType, false, shadowMap, width, height, minFilter, magFilter, wrapU, wrapV, colorFormat);
    const bool hasDepth = (depth != FramebufferDepth::NONE);
    bool depthCreated = !hasDepth || depthTexture.createFramebufferTexture(TextureType::TEXTURE_2D, true, shadowMap, width, height, minFilter, magFilter, wrapU, wrapV,
                                                                          ColorFormat::RGBA, 1, depth == FramebufferDepth::DEPTH_STENCIL);

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

    if (hasDepth){
        sg_view_desc depth_view_desc = {0};
        depth_view_desc.depth_stencil_attachment.image = depthTexture.backend.get();
        depth_view_desc.label = "framebuffer-depth-attachment-view";

        if (Engine::isAsyncThread()){
            depthAttachmentView = SokolCmdQueue::add_command_make_view(depth_view_desc);
        }else{
            depthAttachmentView = sg_make_view(depth_view_desc);
        }
    }

    bool created = isCreated();
    if (!created) {
        destroyFramebuffer();
    }else{
        initializeAttachments(faces);
    }

    return created;
}

namespace{
    sg_image makeImage(const sg_image_desc& desc){
        return Engine::isAsyncThread() ? SokolCmdQueue::add_command_make_image(desc) : sg_make_image(desc);
    }
    sg_view makeView(const sg_view_desc& desc){
        return Engine::isAsyncThread() ? SokolCmdQueue::add_command_make_view(desc) : sg_make_view(desc);
    }
    void destroyImage(sg_image image){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_image(image);
        }else{
            sg_destroy_image(image);
            SokolSystem::forgetAppliedBindings();
        }
    }
    void destroyView(sg_view view){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_view(view);
        }else{
            sg_destroy_view(view);
            SokolSystem::forgetAppliedBindings();
        }
    }
}

// defined in libs/sokol/sokol.cpp, where the backend's limits are
extern "C" int jasperengine_sg_max_msaa_samples(int requested);

int SokolFramebuffer::getMaxMsaaSamples(int requested){
    if (requested <= 1 || !sg_isvalid())
        return 1;
    if (!sg_query_pixelformat(SG_PIXELFORMAT_RGBA8).msaa || !sg_query_pixelformat(SG_PIXELFORMAT_DEPTH_STENCIL).msaa)
        return 1;
    return jasperengine_sg_max_msaa_samples(requested);
}

bool SokolFramebuffer::createMultisampleFramebuffer(int width, int height, int samples){
    destroyFramebuffer();
    if (samples < 2)
        return false;

    numColorAttachments = 1;
    msaaSamples = samples;

    // drawn and resolved within the pass: no sampling, so no texture view or sampler
    sg_image_desc color = {};
    color.usage.color_attachment = true;
    color.type = SG_IMAGETYPE_2D;
    color.width = width;
    color.height = height;
    color.sample_count = samples;
    color.pixel_format = SG_PIXELFORMAT_RGBA8;
    color.label = "msaa-color-image";
    msaaColorImage = makeImage(color);

    sg_image_desc depth = color;
    depth.usage.color_attachment = false;
    depth.usage.depth_stencil_attachment = true;
    depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    depth.label = "msaa-depth-stencil-image";
    msaaDepthImage = makeImage(depth);

    if (msaaColorImage.id == SG_INVALID_ID || msaaDepthImage.id == SG_INVALID_ID){
        destroyFramebuffer();
        return false;
    }

    sg_view_desc colorView = {};
    colorView.color_attachment.image = msaaColorImage;
    colorView.label = "msaa-color-attachment-view";
    colorAttachmentViews[0][0] = makeView(colorView);

    sg_view_desc depthView = {};
    depthView.depth_stencil_attachment.image = msaaDepthImage;
    depthView.label = "msaa-depth-attachment-view";
    depthAttachmentView = makeView(depthView);

    bool created = isCreated();
    if (!created){
        destroyFramebuffer();
    }
    return created;
}

int SokolFramebuffer::getMsaaSamples() const{
    return msaaSamples;
}

sg_attachments SokolFramebuffer::getResolved(SokolFramebuffer& target){
    sg_attachments attachments = {};
    attachments.colors[0] = colorAttachmentViews[0][0];
    attachments.depth_stencil = depthAttachmentView;

    const sg_image resolveImage = target.getColorTexture().backend.get();
    ResolveView* found = nullptr;
    for (ResolveView& entry : resolveViews){
        if (entry.image == resolveImage.id && entry.view.id != SG_INVALID_ID && sg_query_view_state(entry.view) == SG_RESOURCESTATE_VALID){
            found = &entry;
            break;
        }
    }
    if (!found){
        // the targets one camera resolves into are few (the scene target of each path): a
        // view is made once per target image and kept
        found = &resolveViews[nextResolveView];
        nextResolveView = (nextResolveView + 1) % RESOLVE_VIEWS;
        if (found->view.id != SG_INVALID_ID && sg_isvalid()){
            destroyView(found->view);
        }
        sg_view_desc resolveView = {};
        resolveView.resolve_attachment.image = resolveImage;
        resolveView.label = "msaa-resolve-attachment-view";
        found->image = resolveImage.id;
        found->view = makeView(resolveView);
    }
    attachments.resolves[0] = found->view;
    return attachments;
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

bool SokolFramebuffer::createFramebufferMRT(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, int numColor, const ColorFormat* formats, bool stencil){
    if (numColor < 1 || numColor > MAX_COLOR_ATTACHMENTS){
        Log::error("Framebuffer MRT color attachment count must be 1..%d", MAX_COLOR_ATTACHMENTS);
        return false;
    }

    destroyFramebuffer();

    numColorAttachments = numColor;

    bool depthCreated = depthTexture.createFramebufferTexture(TextureType::TEXTURE_2D, true, false, width, height, minFilter, magFilter, wrapU, wrapV, ColorFormat::RGBA, 1, stencil);
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

// (makeView: the helpers above)
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

    numColorAttachments = std::clamp(desc.colorCount, 1, MAX_COLOR_ATTACHMENTS);
    sampleCount = (desc.sampleCount > 1) ? desc.sampleCount : 1;
    const bool msaa = sampleCount > 1;

    for (int a = 0; a < numColorAttachments; a++){
        const ColorFormat format = (a == 0) ? desc.color : desc.moreColors[a - 1];
        if (format == ColorFormat::RED16){
            Log::error("Framebuffer '%s': RED16 is not a colour attachment format", desc.label.c_str());
            destroyFramebuffer();
            return false;
        }
        // colour: the sampled image is the resolve image under MSAA
        bool colorCreated = colorTexture[a].createFramebufferTexture(TextureType::TEXTURE_2D, false, false, desc.width, desc.height,
                                                                     desc.minFilter, desc.magFilter, desc.wrapU, desc.wrapV, format,
                                                                     1, false, msaa);
        if (colorCreated && msaa){
            colorCreated = msaaColorTexture[a].createFramebufferTexture(TextureType::TEXTURE_2D, false, false, desc.width, desc.height,
                                                                        desc.minFilter, desc.magFilter, desc.wrapU, desc.wrapV, format,
                                                                        sampleCount);
        }
        if (!colorCreated){
            destroyFramebuffer();
            return false;
        }

        sg_view_desc colorViewDesc = {};
        colorViewDesc.color_attachment.image = msaa ? msaaColorTexture[a].backend.get() : colorTexture[a].backend.get();
        colorViewDesc.label = "framebuffer-color-attachment-view";
        colorAttachmentViews[a][0] = makeView(colorViewDesc);

        if (msaa){
            sg_view_desc resolveViewDesc = {};
            resolveViewDesc.resolve_attachment.image = colorTexture[a].backend.get();
            resolveViewDesc.label = "framebuffer-resolve-attachment-view";
            resolveAttachmentViews[a] = makeView(resolveViewDesc);
        }
    }

    if (desc.depth != DepthStencilFormat::NONE){
        const bool stencil = desc.depth == DepthStencilFormat::DEPTH_STENCIL;
        if (desc.shareDepthStencil){
            // the view, not only the image: GLES keeps an MSAA attachment's storage in its view
            const SokolFramebuffer& shared = desc.shareDepthStencil->backend;
            if (shared.depthAttachmentView.id == SG_INVALID_ID || shared.getSampleCount() != sampleCount){
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
    // A borrowed depth-stencil keeps its owner's content.
    const bool loadDepthStencil = !ownsDepthAttachment;
    for (size_t face = 0; face < faces; face++){
        if (Engine::isAsyncThread()){
            sg_pass pass = {};
            for (int i = 0; i < numColorAttachments; i++){
                pass.action.colors[i].store_action = SG_STOREACTION_STORE;
            }
            pass.action.depth.store_action = SG_STOREACTION_STORE;
            pass.action.stencil.store_action = SG_STOREACTION_STORE;
            if (loadDepthStencil){
                pass.action.depth.load_action = SG_LOADACTION_LOAD;
                pass.action.stencil.load_action = SG_LOADACTION_LOAD;
            }
            pass.attachments = get(face);
            SokolCmdQueue::add_command_begin_pass(pass);
            SokolCmdQueue::add_command_end_pass();
        }else{
            // after the open pass when created inside one (sokol passes do not nest)
            SokolSystem::initializeAttachments(get(face), loadDepthStencil);
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
                    SokolSystem::forgetAppliedBindings();
                }
            }
        }

        // a shared depth-stencil view stays with its owner
        auto destroyAttachmentView = [](sg_view view){
            if (view.id == SG_INVALID_ID)
                return;
            if (Engine::isAsyncThread()){
                SokolCmdQueue::add_command_destroy_view(view);
            }else{
                sg_destroy_view(view);
                SokolSystem::forgetAppliedBindings();
            }
        };
        if (ownsDepthAttachment)
            destroyAttachmentView(depthAttachmentView);
        for (sg_view view : resolveAttachmentViews)
            destroyAttachmentView(view);
    }

    if (sg_isvalid()){
        for (ResolveView& entry : resolveViews){
            if (entry.view.id != SG_INVALID_ID){
                destroyView(entry.view);
            }
        }
        if (msaaColorImage.id != SG_INVALID_ID){
            destroyImage(msaaColorImage);
        }
        if (msaaDepthImage.id != SG_INVALID_ID){
            destroyImage(msaaDepthImage);
        }
    }
    for (ResolveView& entry : resolveViews){
        entry = ResolveView();
    }
    nextResolveView = 0;
    msaaColorImage.id = SG_INVALID_ID;
    msaaDepthImage.id = SG_INVALID_ID;
    msaaSamples = 1;

    for (int a = 0; a < MAX_COLOR_ATTACHMENTS; a++){
        colorTexture[a].destroyTexture();
    }
    depthTexture.destroyTexture();
    for (TextureRender& texture : msaaColorTexture){
        texture.destroyTexture();
    }

    for (int a = 0; a < MAX_COLOR_ATTACHMENTS; a++){
        for (int i = 0; i < 6; i++){
            colorAttachmentViews[a][i].id = SG_INVALID_ID;
        }
    }
    depthAttachmentView.id = SG_INVALID_ID;
    for (sg_view& view : resolveAttachmentViews){
        view.id = SG_INVALID_ID;
    }
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

    for (const sg_view& view : resolveAttachmentViews){
        if (view.id != SG_INVALID_ID && sg_query_view_state(view) != SG_RESOURCESTATE_VALID) {
            return false;
        }
    }

    return hasAttachment;
}

int SokolFramebuffer::getSampleCount() const{
    // a FramebufferDesc target's own samples, or a multisampled target's
    // (createMultisampleFramebuffer)
    return msaaSamples > 1 ? msaaSamples : sampleCount;
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
        // MRT framebuffers are 2D (single face); bind every color attachment (and, under MSAA,
        // the resolve of each)
        for (int a = 0; a < numColorAttachments; a++){
            attachments.colors[a] = colorAttachmentViews[a][0];
            attachments.resolves[a] = resolveAttachmentViews[a];
        }
    }else if (numColorAttachments == 1){
        attachments.colors[0] = colorAttachmentViews[0][face];
        // MSAA: every pass ends with a resolve into colorTexture[0]
        attachments.resolves[0] = resolveAttachmentViews[0];
    }
    attachments.depth_stencil = depthAttachmentView;

    return attachments;
}
