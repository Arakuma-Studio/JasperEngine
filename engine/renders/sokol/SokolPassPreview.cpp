// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolPassPreview.h"

#include "SokolPassRecorder.h"
#include "pool/ShaderPool.h"
#include "profiler/FrameTimes.h"
#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/TextureRender.h"
#include "sokol_gfx.h"
#include "sokol_gpu_timer.h"

#include <algorithm>
#include <cstring>
#include <memory>

using namespace jasperengine;

namespace {

    // the PASS_PREVIEW variants: the source's texture type
    constexpr int VARIANTS = 4;   // 2D, array ('Arr'), cube ('Cub'), 3D ('Tx3')

    struct Variant {
        std::shared_ptr<ShaderRender> shader;
        ObjectRender render;
        bool loaded = false;
        int paramsSlot = -1;
        std::pair<int, int> sourceSlot{-1, -1};
    };

    struct State {
        bool requested = false;
        bool blitting = false;
        PassPreviewRequest request;
        PassPreviewResult result;
        uint64_t countFrame = ~0ull;
        uint32_t occurrence = 0;
        Variant variants[VARIANTS];
        FramebufferRender target;
        CameraRender pass;
        TextureRender source;
    };

    State& state(){
        static State* s = new State();
        return *s;
    }

    struct PreviewParams {
        float params[4];
        float range[4];
    };

    bool loadVariant(Variant& variant, int index){
        if (variant.loaded)
            return true;
        const uint32_t properties = index == 0 ? 0u : (1u << (index - 1));
        variant.shader = ShaderPool::get(ShaderType::PASS_PREVIEW, properties);
        // built on demand by the editor: not there yet on the first frames
        if (!variant.shader || !variant.shader->isCreated())
            return false;
        variant.render.beginLoad(PrimitiveType::TRIANGLES);
        variant.render.setShader(variant.shader.get());
        if (!variant.render.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE))
            return false;
        ShaderData& data = variant.shader->shaderData;
        variant.paramsSlot = data.getUniformBlockIndexByName("u_fs_passPreview");
        variant.sourceSlot = data.getTextureIndexByName("u_previewSource");
        variant.loaded = true;
        return true;
    }

    void refuse(State& s, const PassRecord& pass, const std::string& reason){
        s.result.serial++;
        s.result.ready = false;
        s.result.unavailable = true;
        s.result.reason = reason;
        s.result.frame = FrameTimes::currentFrame();
        s.result.request = s.request;
        s.result.sourceWidth = pass.width;
        s.result.sourceHeight = pass.height;
        s.requested = false;
    }

    bool integerFormat(const char* format){
        const size_t length = std::strlen(format);
        return length >= 2 && (std::strcmp(format + length - 2, "UI") == 0 || std::strcmp(format + length - 2, "SI") == 0);
    }

    void blit(State& s, const PassRecord& pass){
        const PassPreviewRequest& request = s.request;
        if (pass.swapchain){
            refuse(s, pass, "swapchain pass: it draws into the window, which has no image to preview");
            return;
        }
        // the attachment, and the image it reads
        const PassAttachment* attachment = nullptr;
        bool resolved = false;
        if (request.attachment >= 0 && request.attachment < RENDER_STATS_MAX_COLORS){
            const PassAttachment& color = pass.colors[request.attachment];
            if (!color.used){
                refuse(s, pass, "the pass has no color" + std::to_string(request.attachment) + " attachment");
                return;
            }
            const PassAttachment& resolve = pass.resolves[request.attachment];
            if (resolve.used){
                attachment = &resolve;
                resolved = true;
            }else if (color.samples > 1){
                refuse(s, pass, "multisampled colour attachment without a resolve image");
                return;
            }else{
                attachment = &color;
            }
        }else{
            if (!pass.depth.used){
                refuse(s, pass, "the pass has no depth attachment");
                return;
            }
            if (pass.depth.samples > 1){
                refuse(s, pass, "multisampled depth attachment: it has no resolve image");
                return;
            }
            attachment = &pass.depth;
        }
        if (integerFormat(attachment->format)){
            refuse(s, pass, std::string("integer format ") + attachment->format + " cannot be sampled as colour");
            return;
        }
        const sg_image image = {attachment->image};
        if (sg_query_image_state(image) != SG_RESOURCESTATE_VALID){
            refuse(s, pass, "the attachment's image is gone");
            return;
        }
        const sg_image_type type = sg_query_image_type(image);
        const int variantIndex = type == SG_IMAGETYPE_ARRAY ? 1 : (type == SG_IMAGETYPE_CUBE ? 2 : (type == SG_IMAGETYPE_3D ? 3 : 0));
        Variant& variant = s.variants[variantIndex];
        if (!loadVariant(variant, variantIndex))
            return;   // the shader is still building: the request waits for the next match

        const int longest = std::max(attachment->width, attachment->height);
        const int maxSize = std::clamp(request.maxSize, 16, 1024);
        const float scale = longest > maxSize ? (float)maxSize / (float)longest : 1.0f;
        const int width = std::max(1, (int)((float)attachment->width * scale + 0.5f));
        const int height = std::max(1, (int)((float)attachment->height * scale + 0.5f));
        if (!s.target.isCreated() || s.target.getColorTexture().getWidth() != width || s.target.getColorTexture().getHeight() != height){
            s.target.destroyFramebuffer();
            if (!s.target.createFramebuffer(TextureType::TEXTURE_2D, width, height, TextureFilter::LINEAR, TextureFilter::LINEAR,
                                            TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false, FramebufferDepth::NONE)){
                refuse(s, pass, "the preview target could not be made");
                return;
            }
        }
        if (!s.source.backend.createImageAliasView(attachment->image, attachment->mip, "frame debugger source")){
            refuse(s, pass, "the attachment cannot be sampled");
            return;
        }

        PreviewParams params = {};
        params.params[0] = (float)std::clamp(request.mode, 0, 5);
        const int slices = type == SG_IMAGETYPE_3D ? std::max(sg_query_image_num_slices(image) >> attachment->mip, 1) : 1;
        params.params[1] = type == SG_IMAGETYPE_3D ? ((float)attachment->slice + 0.5f) / (float)slices : (float)attachment->slice;
        params.params[2] = 0.0f;
        const bool linear = request.farPlane > request.nearPlane && request.nearPlane > 0.0f;
        params.params[3] = linear ? 1.0f : 0.0f;
        params.range[0] = request.nearPlane;
        params.range[1] = request.farPlane;
        params.range[2] = request.depthMin;
        params.range[3] = request.depthMax;

        s.blitting = true;
        SokolPassRecorder::hideNextPass();
        s.pass.setLabel("frame debugger preview");
        s.pass.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        s.pass.startRenderPass(&s.target);
        PipelineState pipeline;
        pipeline.target.color = ColorFormat::RGBA;
        pipeline.target.depth = DepthStencilFormat::NONE;
        pipeline.blend = BlendMode::NONE;
        pipeline.depthCompare = CompareFunc::ALWAYS;
        if (variant.render.beginDraw(pipeline)){
            variant.render.addTexture(variant.sourceSlot, ShaderStageType::FRAGMENT, &s.source);
            variant.render.applyUniformBlock(variant.paramsSlot, sizeof(PreviewParams), &params);
            variant.render.draw(0, 3, 1);
        }
        s.pass.endRenderPass();
        s.blitting = false;

        s.result.serial++;
        s.result.ready = true;
        s.result.unavailable = false;
        s.result.reason.clear();
        s.result.frame = FrameTimes::currentFrame();
        s.result.request = request;
        s.result.width = width;
        s.result.height = height;
        s.result.sourceWidth = attachment->width;
        s.result.sourceHeight = attachment->height;
        s.result.format = attachment->format;
        s.result.resolved = resolved;
        s.requested = false;
    }

}

void SokolPassPreview::request(const PassPreviewRequest& request){
    State& s = state();
    s.request = request;
    s.requested = true;
}

void SokolPassPreview::cancel(){
    state().requested = false;
}

bool SokolPassPreview::isRequested(){
    return state().requested;
}

PassPreviewResult SokolPassPreview::result(){
    return state().result;
}

TextureRender* SokolPassPreview::texture(){
    State& s = state();
    return s.target.isCreated() ? &s.target.getColorTexture() : nullptr;
}

void SokolPassPreview::afterEndPass(){
    State& s = state();
    if (!s.requested || s.blitting)
        return;
    const PassRecord* pass = SokolPassRecorder::lastEndedPass();
    if (!pass || pass->hidden)
        return;
    // occurrences count from each frame's first pass of the view
    const uint64_t frame = FrameTimes::currentFrame();
    if (frame != s.countFrame){
        s.countFrame = frame;
        s.occurrence = 0;
    }
    if (pass->viewTag != s.request.viewTag || std::strncmp(pass->label, s.request.label.c_str(), RENDER_STATS_LABEL_LEN - 1) != 0)
        return;
    if (s.occurrence++ != s.request.occurrence)
        return;
    if (!pass->executed){
        refuse(s, *pass, "sokol skipped the pass (validation, or its attachments were gone)");
        return;
    }
    blit(s, *pass);
}

void SokolPassPreview::destroy(){
    State& s = state();
    s.requested = false;
    s.source.destroyTexture();
    if (s.target.isCreated())
        s.target.destroyFramebuffer();
    for (Variant& variant : s.variants){
        if (variant.loaded)
            variant.render.destroy();
        variant.loaded = false;
        variant.shader.reset();
    }
    s.result = PassPreviewResult();
}
