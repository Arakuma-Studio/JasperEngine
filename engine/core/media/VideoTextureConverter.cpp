// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoTextureConverter.h"

#include "IVideoSource.h"
#include "Log.h"
#include "VideoColorConvert.h"
#include "pool/ShaderPool.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "render/TextureRender.h"
#include "texture/Framebuffer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    std::atomic<int> liveConverters{0};

    // htmlui_post.vert / htmlui_post.frag's blocks
    struct PostVs {
        float uvTransform[4];
    };
    struct PostFs {
        float params[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        float texCoordMinMax[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        float colorMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };

    // one HTMLUI_POST variant as a vertexless fullscreen triangle
    struct Program {
        std::shared_ptr<ShaderRender> shader;
        ObjectRender object;
        bool loaded = false;
        int vsSlot = -1;
        int fsSlot = -1;
        std::pair<int, int> texture{-1, -1};
        std::pair<int, int> chromaU{-1, -1};
        std::pair<int, int> chromaV{-1, -1};

        bool load(HtmlUIPostVariant variant, TextureRender* placeholder){
            if (loaded)
                return true;
            // asked again until built: the pool hands a pending variant back to its builder
            if (!shader || !shader->isCreated())
                shader = ShaderPool::get(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties(variant));
            if (!shader || !shader->isCreated())
                return false;
            ShaderData& data = shader->shaderData;
            vsSlot = data.getUniformBlockIndexByName("u_vs_htmluiPost");
            fsSlot = data.getUniformBlockIndexByName("u_fs_htmluiPost");
            texture = data.getTextureIndexByName("u_htmluiPostTex");
            chromaU = data.getTextureIndexByName("u_htmluiYuvU");
            chromaV = data.getTextureIndexByName("u_htmluiYuvV");
            object.beginLoad(PrimitiveType::TRIANGLES);
            object.setShader(shader.get());
            object.addTexture(texture, ShaderStageType::FRAGMENT, placeholder);
            if (chromaU.first >= 0)
                object.addTexture(chromaU, ShaderStageType::FRAGMENT, placeholder);
            if (chromaV.first >= 0)
                object.addTexture(chromaV, ShaderStageType::FRAGMENT, placeholder);
            if (!object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)){
                Log::error("[Video] could not load the video conversion program");
                return false;
            }
            loaded = true;
            return true;
        }

        void destroy(){
            if (loaded)
                object.destroy();
            loaded = false;
            shader.reset();
        }
    };

}

struct VideoTextureConverter::Impl {
    // shared with the Textures linked to it (Texture::setFramebuffer keeps the owner): an Image
    // showing it outlives the player safely, its GPU side freed (the texture draws its fallback)
    std::shared_ptr<Framebuffer> target = std::make_shared<Framebuffer>();
    std::unique_ptr<TextureRender> planes[3];
    int planeSizes[3][2] = {};
    TextureRender placeholder;
    bool placeholderMade = false;
    Program yuv;
    Program blit;
    CameraRender pass;

    // the pending work
    VideoFrame frame;
    bool framePending = false;
    IVideoSource* source = nullptr;
    int targetWidth = 0, targetHeight = 0;
    VideoAspectRatio fit = VideoAspectRatio::FIT_INSIDE;

    bool picture = false;
    int width = 0, height = 0;
    uint64_t conversions = 0;

    bool ensurePlaceholder(){
        if (placeholderMade)
            return true;
        const uint8_t white[4] = {255, 255, 255, 255};
        void* data[1] = {const_cast<uint8_t*>(white)};
        size_t size[1] = {sizeof(white)};
        placeholderMade = placeholder.createTexture("video-placeholder", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                                    TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
        return placeholderMade;
    }

    bool ensureTarget(int w, int h){
        if (target->isCreated() && width == w && height == h)
            return true;
        FramebufferDesc desc;
        desc.width = w;
        desc.height = h;
        desc.color = ColorFormat::RGBA;
        desc.depth = DepthStencilFormat::NONE;
        desc.sampleCount = 1;
        desc.label = "video-player";
        if (target->isCreated())
            target->destroy();
        if (!target->create(desc)){
            Log::error("[Video] could not create the %d x %d video texture", w, h);
            return false;
        }
        width = w;
        height = h;
        picture = false;
        return true;
    }

    bool ensurePlane(int index, int w, int h){
        if (planes[index] && planeSizes[index][0] == w && planeSizes[index][1] == h)
            return true;
        static const char* labels[3] = {"video-y", "video-u", "video-v"};
        planes[index] = std::make_unique<TextureRender>();
        if (!planes[index]->createDynamicTexture(labels[index], w, h, ColorFormat::RED, TextureFilter::LINEAR)){
            planes[index].reset();
            Log::error("[Video] could not create the %d x %d video plane", w, h);
            return false;
        }
        planeSizes[index][0] = w;
        planeSizes[index][1] = h;
        return true;
    }

    PipelineState offscreenState(BlendMode blend) const{
        PipelineState state;
        state.target.swapchain = false;
        state.target.color = ColorFormat::RGBA;
        state.target.depth = DepthStencilFormat::NONE;
        state.target.sampleCount = 1;
        state.blend = blend;
        state.writeMask = ColorWriteMask::RGBA;
        return state;
    }
};

VideoTextureConverter::VideoTextureConverter(): impl(std::make_unique<Impl>()){
    liveConverters++;
}

VideoTextureConverter::~VideoTextureConverter(){
    release();
    liveConverters--;
}

int VideoTextureConverter::getLiveCount(){
    return liveConverters;
}

Framebuffer* VideoTextureConverter::getFramebuffer(){
    return impl->target.get();
}

void VideoTextureConverter::submit(VideoFrame frame, int targetWidth, int targetHeight, VideoAspectRatio fit){
    if (frame.y.empty() || frame.width <= 0 || frame.height <= 0)
        return;
    impl->frame = std::move(frame);
    impl->framePending = true;
    impl->source = nullptr;
    impl->targetWidth = targetWidth;
    impl->targetHeight = targetHeight;
    impl->fit = fit;
}

void VideoTextureConverter::submitSource(IVideoSource* source, int width, int height){
    if (!source || width <= 0 || height <= 0)
        return;
    impl->source = source;
    impl->framePending = false;
    impl->targetWidth = width;
    impl->targetHeight = height;
}

void VideoTextureConverter::forgetSource(IVideoSource* source){
    if (impl->source == source)
        impl->source = nullptr;
}

bool VideoTextureConverter::hasPending() const{
    return impl->framePending || impl->source;
}

bool VideoTextureConverter::hasPicture() const{
    return impl->picture;
}

int VideoTextureConverter::getWidth() const{
    return impl->width;
}

int VideoTextureConverter::getHeight() const{
    return impl->height;
}

uint64_t VideoTextureConverter::getConversionCount() const{
    return impl->conversions;
}

void VideoTextureConverter::fitRect(float srcW, float srcH, float dstW, float dstH, VideoAspectRatio fit, float out[4]){
    float w = dstW, h = dstH;
    if (srcW > 0.0f && srcH > 0.0f){
        const float sx = dstW / srcW, sy = dstH / srcH;
        switch (fit){
            case VideoAspectRatio::NO_SCALING: w = srcW; h = srcH; break;
            case VideoAspectRatio::FIT_VERTICALLY: w = srcW * sy; h = dstH; break;
            case VideoAspectRatio::FIT_HORIZONTALLY: w = dstW; h = srcH * sx; break;
            case VideoAspectRatio::FIT_INSIDE: { const float s = std::min(sx, sy); w = srcW * s; h = srcH * s; break; }
            case VideoAspectRatio::FIT_OUTSIDE: { const float s = std::max(sx, sy); w = srcW * s; h = srcH * s; break; }
            case VideoAspectRatio::STRETCH: break;
        }
    }
    out[0] = (dstW - w) * 0.5f;
    out[1] = (dstH - h) * 0.5f;
    out[2] = w;
    out[3] = h;
}

namespace {

    // the part of a fitted rectangle inside [0, dstW] x [0, dstH], and the texture coordinates
    // (scale.xy, offset.zw) that show the picture's matching part there
    bool visiblePart(const float fitted[4], float dstW, float dstH, float rect[4], float uv[4]){
        const float x0 = std::max(fitted[0], 0.0f), y0 = std::max(fitted[1], 0.0f);
        const float x1 = std::min(fitted[0] + fitted[2], dstW), y1 = std::min(fitted[1] + fitted[3], dstH);
        if (x1 <= x0 || y1 <= y0 || fitted[2] <= 0.0f || fitted[3] <= 0.0f)
            return false;
        rect[0] = x0;
        rect[1] = y0;
        rect[2] = x1 - x0;
        rect[3] = y1 - y0;
        uv[0] = rect[2] / fitted[2];
        uv[1] = rect[3] / fitted[3];
        uv[2] = (x0 - fitted[0]) / fitted[2];
        uv[3] = (y0 - fitted[1]) / fitted[3];
        return true;
    }

}

bool VideoTextureConverter::convert(){
    Impl& m = *impl;
    if (m.source){
        // the browser's element: drawn straight into an RGBA target of its size
        IVideoSource* source = m.source;
        if (!m.ensureTarget(m.targetWidth, m.targetHeight))
            return false;
        m.source = nullptr;
        if (!source->uploadTo(*m.target))
            return false;
        m.picture = true;
        m.conversions++;
        return true;
    }
    if (!m.framePending)
        return false;
    if (!m.ensurePlaceholder() || !m.yuv.load(HtmlUIPostVariant::YUV, &m.placeholder))
        return false;   // the shader is still building: the frame waits
    const VideoFrame& frame = m.frame;
    const int w = m.targetWidth > 0 && m.targetHeight > 0 ? m.targetWidth : frame.width;
    const int h = m.targetWidth > 0 && m.targetHeight > 0 ? m.targetHeight : frame.height;
    const bool chroma = frame.hasChroma();
    const int cw = chroma ? frame.chromaWidth() : 1, ch = chroma ? frame.chromaHeight() : 1;
    if (!m.ensureTarget(w, h) || !m.ensurePlane(0, frame.width, frame.height) || !m.ensurePlane(1, cw, ch) || !m.ensurePlane(2, cw, ch))
        return false;
    m.framePending = false;
    m.planes[0]->updateTexture(frame.y.data(), frame.y.size());
    if (chroma){
        m.planes[1]->updateTexture(frame.u.data(), frame.u.size());
        m.planes[2]->updateTexture(frame.v.data(), frame.v.size());
    }else{
        // monochrome: neutral chroma
        const uint8_t neutral = 128;
        m.planes[1]->updateTexture(&neutral, 1);
        m.planes[2]->updateTexture(&neutral, 1);
    }

    float fitted[4] = {0.0f, 0.0f, (float)w, (float)h};
    if (w != frame.width || h != frame.height)
        fitRect((float)frame.width, (float)frame.height, (float)w, (float)h, m.fit, fitted);
    float rect[4], uv[4];
    const bool visible = visiblePart(fitted, (float)w, (float)h, rect, uv);
    const bool covers = visible && rect[0] <= 0.0f && rect[1] <= 0.0f && rect[2] >= (float)w && rect[3] >= (float)h;

    // the bars of a letterboxed picture are transparent black
    m.pass.setColorActions(covers ? LoadAction::DONTCARE : LoadAction::CLEAR, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    m.pass.setLabel("video conversion");
    m.pass.startRenderPass(&m.target->getRender());
    if (visible){
        const int x = (int)std::floor(rect[0]), y = (int)std::floor(rect[1]);
        const int vw = std::max(1, (int)std::ceil(rect[0] + rect[2]) - x), vh = std::max(1, (int)std::ceil(rect[1] + rect[3]) - y);
        m.pass.applyViewportTopLeft(x, y, vw, vh);
        m.pass.applyScissorTopLeft(x, y, vw, vh);
        Program& program = m.yuv;
        program.object.addTexture(program.texture, ShaderStageType::FRAGMENT, m.planes[0].get());
        program.object.addTexture(program.chromaU, ShaderStageType::FRAGMENT, m.planes[1].get());
        program.object.addTexture(program.chromaV, ShaderStageType::FRAGMENT, m.planes[2].get());
        if (program.object.beginDraw(m.offscreenState(BlendMode::NONE))){
            PostVs vs = {{uv[0], uv[1], uv[2], uv[3]}};
            program.object.applyUniformBlock(program.vsSlot, sizeof(PostVs), &vs);
            PostFs fs;
            float scale[2];
            VideoColorConvert::chromaScale(frame, scale);
            fs.params[1] = scale[0];
            fs.params[2] = scale[1];
            VideoColorConvert::matrix(frame.color, frame.height, fs.colorMatrix);
            program.object.applyUniformBlock(program.fsSlot, sizeof(PostFs), &fs);
            program.object.draw(0, 3, 1);
        }
        // nothing keeps a plane bound
        program.object.addTexture(program.texture, ShaderStageType::FRAGMENT, &m.placeholder);
        program.object.addTexture(program.chromaU, ShaderStageType::FRAGMENT, &m.placeholder);
        program.object.addTexture(program.chromaV, ShaderStageType::FRAGMENT, &m.placeholder);
    }
    m.pass.endRenderPass();
    m.picture = true;
    m.conversions++;
    return true;
}

bool VideoTextureConverter::drawPlane(const RenderTargetFormat& format, CameraRender& pass, const Rect& viewport, VideoAspectRatio fit,
                                      float alpha, bool flipY){
    Impl& m = *impl;
    if (!m.picture || !m.target->isCreated() || alpha <= 0.0f)
        return false;
    if (!m.ensurePlaceholder() || !m.blit.load(HtmlUIPostVariant::PASSTHROUGH, &m.placeholder))
        return false;
    const float vw = viewport.getWidth(), vh = viewport.getHeight();
    float fitted[4], rect[4], uv[4];
    fitRect((float)m.width, (float)m.height, vw, vh, fit, fitted);
    if (!visiblePart(fitted, vw, vh, rect, uv))
        return false;
    if (flipY){
        // the destination's first row is its bottom one: the picture's last row goes there
        uv[3] = uv[3] + uv[1];
        uv[1] = -uv[1];
    }
    // (centred fits are symmetric: the same rectangle with either origin)
    const Rect area(viewport.getX() + rect[0], viewport.getY() + rect[1], rect[2], rect[3]);
    pass.applyViewport(area);
    pass.applyScissor(area);
    Program& program = m.blit;
    program.object.addTexture(program.texture, ShaderStageType::FRAGMENT, &m.target->getRender().getColorTexture());
    PipelineState state;
    state.target = format;
    state.blend = BlendMode::PREMULTIPLIED;
    state.depthCompare = CompareFunc::ALWAYS;
    state.depthWrite = false;
    state.writeMask = ColorWriteMask::RGBA;
    bool drawn = false;
    if (program.object.beginDraw(state)){
        PostVs vs = {{uv[0], uv[1], uv[2], uv[3]}};
        program.object.applyUniformBlock(program.vsSlot, sizeof(PostVs), &vs);
        PostFs fs;
        fs.params[0] = std::clamp(alpha, 0.0f, 1.0f);
        program.object.applyUniformBlock(program.fsSlot, sizeof(PostFs), &fs);
        program.object.draw(0, 3, 1);
        drawn = true;
    }
    program.object.addTexture(program.texture, ShaderStageType::FRAGMENT, &m.placeholder);
    pass.applyViewport(viewport);
    pass.applyScissor(viewport);
    return drawn;
}

void VideoTextureConverter::setPictureForTesting(int width, int height){
    impl->picture = true;
    impl->width = width;
    impl->height = height;
    impl->framePending = false;
    impl->source = nullptr;
}

void VideoTextureConverter::release(){
    Impl& m = *impl;
    m.yuv.destroy();
    m.blit.destroy();
    for (auto& plane : m.planes){
        if (plane)
            plane->destroyTexture();
        plane.reset();
    }
    std::memset(m.planeSizes, 0, sizeof(m.planeSizes));
    if (m.placeholderMade)
        m.placeholder.destroyTexture();
    m.placeholderMade = false;
    if (m.target->isCreated())
        m.target->destroy();
    m.picture = false;
    m.framePending = false;
    m.source = nullptr;
    m.width = m.height = 0;
}
