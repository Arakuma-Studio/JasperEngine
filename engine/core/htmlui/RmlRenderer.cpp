// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlUi's render interface on the engine render abstraction: a port of the semantics of
// RmlUi's GL3 renderer (Backends/RmlUi_Renderer_GL3.cpp) onto sokol passes. Phase-1 spec
// §3.2: every image is top-left origin on every backend, the context's draws go straight into
// its RmlRenderTarget, and a sokol pass is opened only when a draw needs another target.

#include "RmlRenderer.h"
#include "RmlRendererInternal.h"

#include "HtmlUI.h"
#include "HtmlUITexture.h"
#include "RmlFilterContext.h"
#include "RmlFontEngine.h"

#include "Engine.h"
#include "HtmlUIDependencies.h"
#include "Log.h"
#include "pool/ShaderPool.h"
#include "render/FramebufferRender.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "texture/Framebuffer.h"
#include "texture/TextureData.h"
#include "util/GlyphAtlas.h"
#include "compat/VirtualFiles.h"
#include "effects/EffectRenderer.h"
#include "effects/EffectRuntime.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>

using namespace jasperengine;

namespace {

    static_assert(sizeof(Rml::Vertex) == 20, "HTMLUI vertex layout expects 20-byte Rml::Vertex");
    static_assert(sizeof(Rml::Vector2f) == 8 && sizeof(Rml::ColourbPremultiplied) == 4, "HTMLUI vertex layout");

    const size_t minimumBufferBytes = 4096;
    // shared geometry pages; a geometry above a quarter of it gets a dedicated buffer
    const size_t pageBytes = 64 * 1024;
    const size_t geometryAlignment = 16;
    // empty pages and dedicated buffers kept for reuse; beyond it a free one is destroyed
    const size_t maximumFreeBytes = 16 * 1024 * 1024;

    static_assert(sizeof(float) * 28 == 112, "u_fs_htmluiClip is 112 bytes");

    // u_fs_htmluiGradient
    struct GradientParams {
        float funcAndCount[4];
        float pv[4];
        float stopColors[RmlCompiledShader::MAX_STOPS][4];
        float stopPositions[RmlCompiledShader::MAX_STOPS / 4][4];
    };
    static_assert(sizeof(GradientParams) == 352, "u_fs_htmluiGradient is 352 bytes");

    // u_vs_htmluiPost
    struct PostVsParams {
        float uvTransform[4];
    };


    Rml::Rectanglei fullRect(int width, int height){
        return Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(width, 1), std::max(height, 1)));
    }

    bool isEmpty(Rml::Rectanglei rect){
        return !rect.Valid() || rect.Width() <= 0 || rect.Height() <= 0;
    }

    // a ∪ b as the bounding rect (empty rects ignored)
    Rml::Rectanglei unite(Rml::Rectanglei a, Rml::Rectanglei b){
        if (isEmpty(a))
            return isEmpty(b) ? Rml::Rectanglei::MakeInvalid() : b;
        return isEmpty(b) ? a : a.Join(b);
    }

    Rml::Rectanglei intersect(Rml::Rectanglei a, Rml::Rectanglei b){
        if (isEmpty(a) || isEmpty(b))
            return Rml::Rectanglei::MakeInvalid();
        const Rml::Rectanglei r = a.Intersect(b);
        return isEmpty(r) ? Rml::Rectanglei::MakeInvalid() : r;
    }

    Rml::Colourf toColourf(Rml::ColourbPremultiplied colour){
        Rml::Colourf result;
        for (int i = 0; i < 4; i++)
            result[i] = (1.0f / 255.0f) * float(colour[i]);
        return result;
    }

    const PipelineType compositePipelines = (PipelineType)(PIP_DEFAULT_NODEPTH | PIP_RTT_NODEPTH);

}

//-------------------------------------------------------------------------------------------
// RmlRenderTarget
//-------------------------------------------------------------------------------------------

RmlRenderTarget::RmlRenderTarget(){
}

RmlRenderTarget::~RmlRenderTarget(){
    destroy();
}

bool RmlRenderTarget::ensure(int width, int height, int sampleCount){
    width = std::max(width, 1);
    height = std::max(height, 1);
    sampleCount = std::max(sampleCount, 1);

    if (isCreated() && width == this->width && height == this->height && sampleCount == this->sampleCount)
        return false;

    destroy();
    this->width = width;
    this->height = height;
    this->sampleCount = sampleCount;
    ensureLayer(0);
    return true;
}

bool RmlRenderTarget::isCreated() const{
    return !layers.empty() && layers[0] && layers[0]->isCreated();
}

void RmlRenderTarget::destroy(){
    // shared depth-stencil: the layers that borrow layer 0's go first
    for (size_t i = layers.size(); i-- > 0;){
        layers[i].reset();
    }
    layers.clear();
    layerChecked.clear();
    for (std::unique_ptr<Framebuffer>& target : postprocess){
        target.reset();
    }
    for (uint64_t& checked : postprocessChecked)
        checked = 0;
}

void RmlRenderTarget::newFrame(){
    frame++;
}

bool RmlRenderTarget::checkedThisFrame(const Framebuffer& framebuffer) const{
    for (size_t i = 0; i < layers.size(); i++){
        if (layers[i].get() == &framebuffer)
            return i < layerChecked.size() && layerChecked[i] == frame;
    }
    for (int i = 0; i < POSTPROCESS_COUNT; i++){
        if (postprocess[i].get() == &framebuffer)
            return postprocessChecked[i] == frame;
    }
    return false;
}

int RmlRenderTarget::getWidth() const{
    return width;
}

int RmlRenderTarget::getHeight() const{
    return height;
}

int RmlRenderTarget::getSampleCount() const{
    return sampleCount;
}

FramebufferRender* RmlRenderTarget::getComposited(){
    return isCreated() ? &layers[0]->getRender() : nullptr;
}

Framebuffer* RmlRenderTarget::getLayer(int index) const{
    if (index < 0 || index >= (int)layers.size())
        return nullptr;
    return layers[index].get();
}

int RmlRenderTarget::getLayerCount() const{
    return (int)layers.size();
}

int RmlRenderTarget::getPostprocessCount() const{
    int count = 0;
    for (const std::unique_ptr<Framebuffer>& target : postprocess){
        if (target)
            count++;
    }
    return count;
}

uint64_t RmlRenderTarget::getCreateCount() const{
    return createCount;
}

namespace {
    std::atomic<uint64_t> targetGenerations{0};
}

uint64_t RmlRenderTarget::getGeneration() const{
    return generation;
}

Framebuffer* RmlRenderTarget::ensureLayer(int index){
    if (index < 0)
        return nullptr;
    if ((int)layerChecked.size() < (int)layers.size())
        layerChecked.resize(layers.size(), 0);
    if (index < (int)layers.size() && layers[index] && (layerChecked[index] == frame || layers[index]->isCreated())){
        layerChecked[index] = frame;
        return layers[index].get();
    }
    // layer 0 first: the others share its depth-stencil
    if (index > 0 && !ensureLayer(0))
        return nullptr;
    if ((int)layers.size() <= index)
        layers.resize(index + 1);

    FramebufferDesc desc;
    desc.width = width;
    desc.height = height;
    desc.color = ColorFormat::RGBA;
    desc.depth = DepthStencilFormat::DEPTH_STENCIL;
    desc.sampleCount = sampleCount;
    desc.shareDepthStencil = (index > 0) ? &layers[0]->getRender() : nullptr;
    desc.label = "htmlui-layer";

    std::unique_ptr<Framebuffer> layer = std::make_unique<Framebuffer>();
    createCount++;
    generation = ++targetGenerations;
    if (!layer->create(desc)){
        Log::error("[HTML UI] could not create the %d x %d layer %d (%d samples)", width, height, index, sampleCount);
        return nullptr;
    }
    layers[index] = std::move(layer);
    layerChecked.resize(layers.size(), 0);
    layerChecked[index] = frame;
    return layers[index].get();
}

Framebuffer* RmlRenderTarget::ensurePostprocess(int index){
    if (index < 0 || index >= POSTPROCESS_COUNT)
        return nullptr;
    if (postprocess[index] && (postprocessChecked[index] == frame || postprocess[index]->isCreated())){
        postprocessChecked[index] = frame;
        return postprocess[index].get();
    }

    FramebufferDesc desc;
    desc.width = width;
    desc.height = height;
    desc.color = ColorFormat::RGBA;
    desc.depth = DepthStencilFormat::NONE;
    desc.sampleCount = 1;
    desc.label = "htmlui-postprocess";

    std::unique_ptr<Framebuffer> target = std::make_unique<Framebuffer>();
    createCount++;
    generation = ++targetGenerations;
    if (!target->create(desc)){
        Log::error("[HTML UI] could not create the %d x %d post-process target %d", width, height, index);
        return nullptr;
    }
    postprocess[index] = std::move(target);
    postprocessChecked[index] = frame;
    return postprocess[index].get();
}

//-------------------------------------------------------------------------------------------
// RmlFilterContext
//-------------------------------------------------------------------------------------------

RmlFilterContext::RmlFilterContext(RmlRenderer& renderer, Framebuffer& sourceLayer): renderer(renderer), sourceLayer(&sourceLayer){
    pickFreeTargets();
}

void RmlFilterContext::pickFreeTargets(){
    int found = 0;
    for (int i = 0; i < 3 && found < 2; i++){
        if (i == primaryIndex)
            continue;
        if (found == 0)
            secondaryIndex = i;
        else
            tertiaryIndex = i;
        found++;
    }
}

Framebuffer& RmlFilterContext::primary(){
    return (primaryIndex < 0) ? *sourceLayer : *renderer.postTarget(primaryIndex);
}

Framebuffer& RmlFilterContext::secondary(){
    return *renderer.postTarget(secondaryIndex);
}

Framebuffer& RmlFilterContext::tertiary(){
    return *renderer.postTarget(tertiaryIndex);
}

void RmlFilterContext::swap(){
    primaryIndex = secondaryIndex;
    pickFreeTargets();
}

bool RmlFilterContext::isPrimaryLayer() const{
    return primaryIndex < 0;
}

RmlRenderer& RmlFilterContext::getRenderer(){
    return renderer;
}

void RmlFilterContext::blit(Framebuffer& src, Framebuffer& dst){
    RmlRenderer::PostDraw draw;
    draw.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
    draw.source = &src.getRender().getColorTexture();
    draw.scissor = getScissor();
    renderer.drawPost(dst, LoadAction::CLEAR, draw, postState(BlendMode::NONE));
}

void RmlFilterContext::blurInPlace(Framebuffer& target, Framebuffer& temp, float sigmaDevicePx){
    renderer.blurInto(target, target, temp, sigmaDevicePx, getScissor());
}

Rml::Rectanglei RmlFilterContext::getScissor() const{
    return renderer.currentScissorTarget();
}

Rml::Vector2i RmlFilterContext::getLayerSize() const{
    return renderer.targetSize;
}

PipelineState RmlFilterContext::postState(BlendMode blend) const{
    return renderer.postTargetState(blend);
}

void RmlFilterContext::drawFullscreen(Framebuffer& dst, ObjectRender& program, const PipelineState& state,
                                      const std::function<void(ObjectRender&)>& applyUniforms, LoadAction load){
    if (!renderer.bindTarget(dst, load))
        return;
    renderer.applyPassViewport(fullRect((int)dst.getWidth(), (int)dst.getHeight()));
    const Rml::Rectanglei scissor = getScissor();
    if (isEmpty(scissor))
        return;
    renderer.applyPassScissor(scissor);
    renderer.drawFullscreenProgram(program, state, applyUniforms);
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: geometry and textures
//-------------------------------------------------------------------------------------------

RmlRenderer::RmlRenderer(){
    projection = Rml::Matrix4f::Identity();
    transform = projection;
    effects = std::make_unique<htmlui::EffectRenderer>(*this);
}

RmlRenderer::~RmlRenderer(){
    releaseGpuResources();

    // RmlUi gave every handle back before (HtmlUIContext::destroy); whatever is left would leak
    for (RmlGeometry* geometry : liveGeometry){
        delete geometry;
    }
    liveGeometry.clear();
    for (RmlGeometry* geometry : releasedGeometry){
        delete geometry;
    }
    releasedGeometry.clear();
    for (HtmlUITexture* texture : liveTextures){
        delete texture;
    }
    liveTextures.clear();
    atlasTextures.clear();
}

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices){
    RmlGeometry* geometry = nullptr;
    if (!releasedGeometry.empty()){
        geometry = releasedGeometry.back();
        releasedGeometry.pop_back();
        *geometry = RmlGeometry();
    }else{
        geometry = new RmlGeometry();
    }
    static uint64_t generations = 0;
    geometry->alive = true;
    geometry->generation = ++generations;
    geometry->vertices = vertices;
    geometry->indices = indices;
    liveGeometry.insert(geometry);
    stats.geometryLive = (uint32_t)liveGeometry.size();
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle){
    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
    // recorded commands may draw it (the recording does not read the geometry it records): released
    // after the replay. RmlUi frees the mesh now, so data not in persistent storage yet is kept
    if (recording && geometry && geometry->alive){
        if (!geometry->vertexPage || !geometry->indexPage){
            if (keptMeshesUsed == keptMeshes.size())
                keptMeshes.emplace_back();
            KeptMesh& kept = keptMeshes[keptMeshesUsed++];
            kept.vertices.assign(geometry->vertices.begin(), geometry->vertices.end());
            kept.indices.assign(geometry->indices.begin(), geometry->indices.end());
            geometry->vertices = Rml::Span<const Rml::Vertex>(kept.vertices.data(), kept.vertices.size());
            geometry->indices = Rml::Span<const int>(kept.indices.data(), kept.indices.size());
        }
        deferredReleases.push_back(DeferredRelease{ReleaseKind::GEOMETRY, handle});
        return;
    }
    releaseGeometryNow(handle);
}

void RmlRenderer::releaseGeometryNow(Rml::CompiledGeometryHandle handle){
    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
    if (liveGeometry.erase(geometry) == 0)
        return;

    if (geometry->vertexPage)
        releaseRange(geometry->vertexPage, geometry->vertexBytes);
    if (geometry->indexPage)
        releaseRange(geometry->indexPage, geometry->indexBytes);
    // kept for reuse, so a handle stays readable: draws check its flag instead of looking it up
    // in liveGeometry (a hash lookup per draw)
    *geometry = RmlGeometry();
    releasedGeometry.push_back(geometry);
    stats.geometryLive = (uint32_t)liveGeometry.size();
}

bool RmlRenderer::appendTransient(const void* data, size_t bytes, bool index, BufferRender*& buffer, size_t& offset){
    const uint64_t frame = SystemRender::getFrameCount();
    // sokol advances the append position by the size rounded up to 4
    const size_t step = (bytes + 3) / 4 * 4;

    RmlTransientBuffer* chosen = nullptr;
    size_t largest = 0;
    for (std::unique_ptr<RmlTransientBuffer>& candidate : transientBuffers){
        if (candidate->index != index)
            continue;
        if (candidate->frame != frame){
            candidate->frame = frame;
            candidate->used = 0;
        }
        largest = std::max(largest, candidate->capacity);
        if (!chosen && candidate->used + step <= candidate->capacity)
            chosen = candidate.get();
    }
    if (!chosen){
        std::unique_ptr<RmlTransientBuffer> created = std::make_unique<RmlTransientBuffer>();
        created->capacity = std::max(pageBytes, largest * 2);
        while (created->capacity < step)
            created->capacity *= 2;
        created->index = index;
        created->frame = frame;
        gpuCreateCount++;
        if (!created->buffer.createBuffer((unsigned int)created->capacity, nullptr, index ? BufferType::INDEX_BUFFER : BufferType::VERTEX_BUFFER, BufferUsage::STREAM)){
            Log::error("[HTML UI] could not create a %u-byte geometry buffer", (unsigned int)created->capacity);
            return false;
        }
        chosen = created.get();
        transientBuffers.push_back(std::move(created));
    }

    const int appended = chosen->buffer.appendBuffer((unsigned int)bytes, data);
    if (appended < 0)
        return false;
    buffer = &chosen->buffer;
    offset = (size_t)appended;
    chosen->used = (size_t)appended + step;
    return true;
}

bool RmlRenderer::allocateRange(const void* data, size_t bytes, bool index, RmlGeometryPage*& page, size_t& offset){
    const uint64_t frame = SystemRender::getFrameCount();
    const size_t aligned = (bytes + geometryAlignment - 1) / geometryAlignment * geometryAlignment;
    page = nullptr;
    offset = 0;

    if (aligned > pageBytes / 4){
        // a dedicated buffer: a free one of the size class, released and updated before this frame
        size_t capacity = minimumBufferBytes;
        while (capacity < aligned)
            capacity *= 2;
        for (std::unique_ptr<RmlGeometryPage>& candidate : pages){
            if (candidate->dedicated && candidate->index == index && candidate->capacity == capacity && candidate->live == 0 &&
                candidate->releaseFrame != frame && candidate->updateFrame != frame){
                page = candidate.get();
                break;
            }
        }
        if (!page){
            std::unique_ptr<RmlGeometryPage> created = std::make_unique<RmlGeometryPage>();
            created->capacity = capacity;
            created->index = index;
            created->dedicated = true;
            gpuCreateCount++;
            if (!created->buffer.createBuffer((unsigned int)capacity, nullptr, index ? BufferType::INDEX_BUFFER : BufferType::VERTEX_BUFFER, BufferUsage::STREAM)){
                Log::error("[HTML UI] could not create a %u-byte geometry buffer", (unsigned int)capacity);
                return false;
            }
            page = created.get();
            pages.push_back(std::move(created));
        }
        page->buffer.updateBuffer((unsigned int)bytes, const_cast<void*>(data));
        page->updateFrame = frame;
        page->releaseFrame = UINT64_MAX;
        page->live = 1;
        page->top = aligned;
        page->uploadedTop = aligned;
        return true;
    }

    // a page with room that was not uploaded this frame (the newest first), or an empty one
    for (size_t i = pages.size(); i-- > 0;){
        RmlGeometryPage* candidate = pages[i].get();
        if (candidate->dedicated || candidate->index != index || candidate->updateFrame == frame)
            continue;
        if (candidate->live == 0){
            // released this frame: draws of this frame may still read it
            if (candidate->releaseFrame == frame)
                continue;
            candidate->top = 0;
            candidate->uploadedTop = 0;
        }
        if (candidate->top + aligned <= candidate->capacity){
            page = candidate;
            break;
        }
    }
    if (!page){
        std::unique_ptr<RmlGeometryPage> created = std::make_unique<RmlGeometryPage>();
        created->capacity = pageBytes;
        created->index = index;
        created->shadow.resize(pageBytes);
        gpuCreateCount++;
        if (!created->buffer.createBuffer((unsigned int)pageBytes, nullptr, index ? BufferType::INDEX_BUFFER : BufferType::VERTEX_BUFFER, BufferUsage::STREAM)){
            Log::error("[HTML UI] could not create a %u-byte geometry page", (unsigned int)pageBytes);
            return false;
        }
        page = created.get();
        pages.push_back(std::move(created));
    }

    offset = page->top;
    std::memcpy(page->shadow.data() + offset, data, bytes);
    page->top += aligned;
    page->live++;
    page->releaseFrame = UINT64_MAX;
    return true;
}

void RmlRenderer::persistGeometry(){
    // the geometry drawn from transient buffers this frame moves to persistent storage (which
    // this frame's draws did not use, so updating it now is safe), each page uploaded once
    std::vector<RmlGeometry*> pending;
    pending.swap(pendingGeometry);
    for (RmlGeometry* geometry : pending){
        if (!geometry->alive || (geometry->vertexPage && geometry->indexPage))
            continue;
        const size_t vertexBytes = geometry->vertices.size() * sizeof(Rml::Vertex);
        const size_t indexBytes = geometry->indices.size() * sizeof(int);
        RmlGeometryPage* vertexPage = nullptr;
        RmlGeometryPage* indexPage = nullptr;
        size_t vertexOffset = 0, indexOffset = 0;
        if (!allocateRange(geometry->vertices.data(), vertexBytes, false, vertexPage, vertexOffset))
            continue;
        if (!allocateRange(geometry->indices.data(), indexBytes, true, indexPage, indexOffset)){
            releaseRange(vertexPage, 0);
            continue;
        }
        geometry->vertexPage = vertexPage;
        geometry->indexPage = indexPage;
        geometry->vertexOffset = vertexOffset;
        geometry->indexOffset = indexOffset;
        geometry->vertexBytes = (vertexBytes + geometryAlignment - 1) / geometryAlignment * geometryAlignment;
        geometry->indexBytes = (indexBytes + geometryAlignment - 1) / geometryAlignment * geometryAlignment;
        stats.geometryBytes += (uint32_t)(geometry->vertexBytes + geometry->indexBytes);
    }

    const uint64_t frame = SystemRender::getFrameCount();
    for (std::unique_ptr<RmlGeometryPage>& page : pages){
        if (page->dedicated || page->uploadedTop >= page->top || page->updateFrame == frame)
            continue;
        page->buffer.updateBuffer((unsigned int)page->top, page->shadow.data());
        page->updateFrame = frame;
        page->uploadedTop = page->top;
    }
}

void RmlRenderer::releaseRange(RmlGeometryPage* page, size_t bytes){
    stats.geometryBytes -= (uint32_t)std::min<size_t>(bytes, stats.geometryBytes);
    if (page->live > 0)
        page->live--;
    if (page->live > 0)
        return;
    page->releaseFrame = SystemRender::getFrameCount();

    // a bounded amount of free storage is kept for reuse
    size_t freeBytes = 0;
    for (const std::unique_ptr<RmlGeometryPage>& candidate : pages){
        if (candidate->live == 0)
            freeBytes += candidate->capacity;
    }
    if (freeBytes > maximumFreeBytes){
        for (size_t i = 0; i < pages.size(); i++){
            if (pages[i].get() == page){
                page->buffer.destroyBuffer();
                pages.erase(pages.begin() + (long)i);
                break;
            }
        }
    }
}

void RmlRenderer::destroyGeometryBuffers(){
    for (RmlGeometry* geometry : liveGeometry){
        geometry->vertexPage = nullptr;
        geometry->indexPage = nullptr;
        geometry->transientFrame = UINT64_MAX;
        geometry->transientVertices = nullptr;
        geometry->transientIndices = nullptr;
    }
    for (std::unique_ptr<RmlGeometryPage>& page : pages){
        page->buffer.destroyBuffer();
    }
    pages.clear();
    for (std::unique_ptr<RmlTransientBuffer>& buffer : transientBuffers){
        buffer->buffer.destroyBuffer();
    }
    transientBuffers.clear();
    pendingGeometry.clear();
    stats.geometryBytes = 0;
}

bool RmlRenderer::bindGeometry(Rml::CompiledGeometryHandle handle, ObjectRender& program, uint32_t& indexCount){
    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
    if (!geometry || !geometry->alive || geometry->vertices.empty() || geometry->indices.empty())
        return false;

    BufferRender* vertices = nullptr;
    BufferRender* indices = nullptr;
    size_t vertexOffset = 0, indexOffset = 0;
    if (geometry->vertexPage && geometry->indexPage){
        vertices = &geometry->vertexPage->buffer;
        indices = &geometry->indexPage->buffer;
        vertexOffset = geometry->vertexOffset;
        indexOffset = geometry->indexOffset;
    }else{
        // first drawn this frame: from the transient buffers (once a frame), persistent at its end
        const uint64_t frame = SystemRender::getFrameCount();
        if (geometry->transientFrame != frame){
            // RmlUi's int indices are the UINT32 index buffer as they are
            if (!appendTransient(geometry->vertices.data(), geometry->vertices.size() * sizeof(Rml::Vertex), false,
                                 geometry->transientVertices, geometry->transientVertexOffset) ||
                !appendTransient(geometry->indices.data(), geometry->indices.size() * sizeof(int), true,
                                 geometry->transientIndices, geometry->transientIndexOffset)){
                return false;
            }
            geometry->transientFrame = frame;
            pendingGeometry.push_back(geometry);
        }
        vertices = geometry->transientVertices;
        indices = geometry->transientIndices;
        vertexOffset = geometry->transientVertexOffset;
        indexOffset = geometry->transientIndexOffset;
    }

    program.replaceVertexBuffer(&compositeVertexBuffer, vertices, vertexOffset);
    program.setIndexBuffer(indices, indexOffset);
    indexCount = (uint32_t)geometry->indices.size();
    return true;
}

HtmlUITexture* RmlRenderer::createStaticTexture(const uint8_t* rgba, int width, int height, const char* label, TextureFilter filter,
                                                TextureWrap wrapU, TextureWrap wrapV, ColorFormat format){
    std::unique_ptr<HtmlUITexture> texture = std::make_unique<HtmlUITexture>();
    texture->kind = HtmlUITexture::Kind::STATIC;
    texture->ownedRender = std::make_unique<TextureRender>();
    texture->width = width;
    texture->height = height;

    // no GPU yet (scripts laid a document out before the platform loaded the view): the size
    // only, RmlUi makes the texture again once the view exists
    if (!Engine::isViewLoaded()){
        texture->dead = true;
        texturesWithoutView = true;
        HtmlUITexture* result = texture.release();
        liveTextures.insert(result);
        return result;
    }

    void* data[1] = {const_cast<uint8_t*>(rgba)};
    size_t size[1] = {(size_t)width * (size_t)height * (format == ColorFormat::RED ? 1 : 4)};
    gpuCreateCount++;
    // LINEAR (or NEAREST) min filter: no mipmaps
    if (!texture->ownedRender->createTexture(label, width, height, format, TextureType::TEXTURE_2D, 1, data, size,
                                             filter, filter, wrapU, wrapV)){
        texture->ownedRender->destroyTexture();
        texture->dead = true;
        return nullptr;
    }
    texture->render = texture->ownedRender.get();

    HtmlUITexture* result = texture.release();
    liveTextures.insert(result);
    return result;
}

Rml::TextureHandle RmlRenderer::createAtlasTexture(int width, int height, const AtlasTexture& desc){
    const bool coverage = desc.sample == HtmlUITexture::Sample::COVERAGE;
    HtmlUITexture* texture = nullptr;
    if (!Engine::isViewLoaded()){
        // no GPU yet: RmlUi makes it again once the view exists (createStaticTexture's way)
        texture = new HtmlUITexture();
        texture->kind = HtmlUITexture::Kind::DYNAMIC;
        texture->width = width;
        texture->height = height;
        texture->dead = true;
        texturesWithoutView = true;
    }else{
        // dynamic: glyphs added to the page are uploaded into the same texture
        gpuCreateCount++;
        std::unique_ptr<HtmlUITexture> created = HtmlUITexture::createDynamic(coverage ? "htmlui-glyph-atlas" : "htmlui-glyph-atlas-rgba",
                                                                              width, height, coverage ? ColorFormat::RED : ColorFormat::RGBA);
        if (!created)
            return 0;
        texture = created.release();
    }
    liveTextures.insert(texture);
    atlasTextures.push_back(texture);
    texture->atlas = desc.atlas;
    texture->atlasGeneration = desc.generation;
    texture->sample = desc.sample;
    texture->distanceRange = desc.distanceRange;
    texture->drawFrame = desc.drawFrame;
    if (desc.whiteX >= 0 && desc.whiteY >= 0 && width > 0 && height > 0){
        // the centre of the 4x4 block: every bilinear tap reads full coverage
        texture->hasWhiteTexel = true;
        texture->whiteU = ((float)desc.whiteX + 2.0f) / (float)width;
        texture->whiteV = ((float)desc.whiteY + 2.0f) / (float)height;
    }
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

Rml::TextureHandle RmlRenderer::createTextureVariant(Rml::TextureHandle baseHandle, const float textEffect[3], std::shared_ptr<uint64_t> drawFrame){
    HtmlUITexture* base = toTexture(baseHandle);
    if (!base)
        return 0;
    HtmlUITexture* texture = new HtmlUITexture();
    texture->kind = HtmlUITexture::Kind::EXTERNAL;
    texture->sample = base->sample;
    texture->render = base->render;
    texture->width = base->width;
    texture->height = base->height;
    texture->dead = base->dead;
    texture->distanceRange = base->distanceRange;
    for (int i = 0; i < 3; i++)
        texture->textEffect[i] = textEffect[i];
    texture->drawFrame = std::move(drawFrame);
    liveTextures.insert(texture);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

void RmlRenderer::noteDrawn(HtmlUITexture* texture){
    if (!texture)
        return;
    if (texture->drawFrame)
        *texture->drawFrame = SystemRender::getFrameCount();
    // drawing at once, an atlas page not uploaded since glyphs were added (the replay uploads them all first)
    if (texture->atlas)
        uploadAtlasTexture(*texture);
}

void RmlRenderer::uploadAtlasTexture(HtmlUITexture& texture){
    if (texture.dead || !texture.atlas || !texture.atlasGeneration || *texture.atlasGeneration == texture.uploadedGeneration)
        return;
    const std::vector<unsigned char>& pixels = texture.atlas->getPixels();
    const size_t bytes = (size_t)texture.width * (size_t)texture.height * (texture.sample == HtmlUITexture::Sample::COVERAGE ? 1u : 4u);
    // (a page parked by the font engine has no texels: its texture goes with its source)
    if (pixels.size() != bytes)
        return;
    if (texture.update(pixels.data(), bytes))
        texture.uploadedGeneration = *texture.atlasGeneration;
}

void RmlRenderer::updateAtlasTextures(){
    for (HtmlUITexture* texture : atlasTextures)
        uploadAtlasTexture(*texture);
}

void RmlRenderer::addTextStats(uint32_t glyphsRasterized, uint32_t textRegenerations, uint32_t textEffectFallbacks, uint32_t atlasEvictions){
    stats.glyphsRasterized += glyphsRasterized;
    stats.textRegenerations += textRegenerations;
    stats.textEffectFallbacks += textEffectFallbacks;
    stats.atlasEvictions += atlasEvictions;
}

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& rmlSource){
    // phase-2 hook H7: "?htmlui-filter=nearest" (image-rendering: pixelated) asks for nearest
    // sampling and is not part of the file; "@htmlui/..." paths are compat's virtual files
    bool nearest = false, repeatX = false, repeatY = false;
    const std::string source = stripTextureFilter(rmlSource, nearest, repeatX, repeatY);
    const TextureFilter filter = nearest ? TextureFilter::NEAREST : TextureFilter::LINEAR;

    HtmlUIFileLog::recordLazyFile(this, source);   // hot reload: images are loaded at draw time
    TextureData data;
    bool loaded = false;
    if (htmlui::compat::VirtualFiles::isVirtual(source)){
        // a "?svgScale=" suffix stays meaningful for virtual SVGs
        float svgScale = 1.0f;
        const std::string path = TextureData::parseSvgScalePath(source, &svgScale);
        const std::shared_ptr<const std::string> content = htmlui::compat::VirtualFiles::find(path);
        if (content && !content->empty()){
            data.setSVGScale(svgScale);
            loaded = data.loadTextureFromMemory(reinterpret_cast<unsigned char*>(const_cast<char*>(content->data())), (unsigned int)content->size());
        }
    }else{
        loaded = !source.empty() && data.loadTextureFromFile(source.c_str());
    }
    if (!loaded){
        Log::error("[HTML UI] could not load image '%s'", source.c_str());
        return 0;
    }

    const int width = data.getWidth();
    const int height = data.getHeight();
    const int channels = data.getChannels();
    const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
    if (data.getColorFormat() == ColorFormat::RED16 || !pixels || width <= 0 || height <= 0 || channels < 1 || channels > 4){
        Log::error("[HTML UI] unsupported image format '%s'", source.c_str());
        return 0;
    }

    // RGBA8 with premultiplied alpha, row 0 at the top (RmlUi's texture coordinates).
    // The engine's TextureAlphaBorder fix is never applied: premultiplied colors need none
    const size_t count = (size_t)width * (size_t)height;
    std::vector<uint8_t> rgba(count * 4);
    for (size_t i = 0; i < count; i++){
        const unsigned char* p = pixels + i * channels;
        uint8_t r, g, b, a;
        if (channels == 1){
            r = g = b = p[0]; a = 255;
        }else if (channels == 2){
            r = g = b = p[0]; a = p[1];
        }else if (channels == 3){
            r = p[0]; g = p[1]; b = p[2]; a = 255;
        }else{
            r = p[0]; g = p[1]; b = p[2]; a = p[3];
        }
        uint8_t* out = &rgba[i * 4];
        out[0] = (uint8_t)((r * a + 127) / 255);
        out[1] = (uint8_t)((g * a + 127) / 255);
        out[2] = (uint8_t)((b * a + 127) / 255);
        out[3] = a;
    }

    HtmlUITexture* texture = createStaticTexture(rgba.data(), width, height, "htmlui-image", filter,
                                                 repeatX ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE,
                                                 repeatY ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE);
    if (!texture){
        Log::error("[HTML UI] could not create the texture of image '%s'", source.c_str());
        return 0;
    }

    texture_dimensions = Rml::Vector2i(width, height);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

std::string RmlRenderer::stripTextureFilter(const std::string& source, bool& nearest){
    bool repeatX = false, repeatY = false;
    return stripTextureFilter(source, nearest, repeatX, repeatY);
}

std::string RmlRenderer::stripTextureFilter(const std::string& source, bool& nearest, bool& repeatX, bool& repeatY){
    static const std::string parameter = "htmlui-filter=nearest";
    static const std::string wrap = "rmlui-wrap=";
    nearest = false;
    repeatX = repeatY = false;
    const size_t query = source.find('?');
    if (query == std::string::npos)
        return source;
    // the query's parameters without htmlui-filter (others, like svgScale, stay)
    std::string kept;
    size_t start = query + 1;
    while (start <= source.size()){
        size_t end = source.find('&', start);
        if (end == std::string::npos)
            end = source.size();
        const std::string item = source.substr(start, end - start);
        if (item == parameter)
            nearest = true;
        else if (item.compare(0, wrap.size(), wrap) == 0){
            const std::string axes = item.substr(wrap.size());
            repeatX = (axes == "repeat" || axes == "repeat-x");
            repeatY = (axes == "repeat" || axes == "repeat-y");
        }else if (!item.empty())
            kept += (kept.empty() ? "" : "&") + item;
        start = end + 1;
    }
    return source.substr(0, query) + (kept.empty() ? "" : "?" + kept);
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions){
    return GenerateTextureWithWrap(source, source_dimensions, false, false);
}

Rml::TextureHandle RmlRenderer::GenerateTextureWithWrap(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions, bool repeatX,
                                                        bool repeatY){
    // already premultiplied RGBA8 (font pages, RmlUi callback textures)
    if (source_dimensions.x <= 0 || source_dimensions.y <= 0 ||
        source.size() != (size_t)source_dimensions.x * (size_t)source_dimensions.y * 4){
        Log::error("[HTML UI] GenerateTexture: %d x %d does not match %u bytes of RGBA8", source_dimensions.x, source_dimensions.y, (unsigned int)source.size());
        return 0;
    }

    HtmlUITexture* texture = createStaticTexture(source.data(), source_dimensions.x, source_dimensions.y, "htmlui-texture", TextureFilter::LINEAR,
                                                 repeatX ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE,
                                                 repeatY ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE);
    if (!texture){
        Log::error("[HTML UI] could not create a %d x %d texture", source_dimensions.x, source_dimensions.y);
        return 0;
    }
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

HtmlUITexture* RmlRenderer::toTexture(Rml::TextureHandle handle){
    if (!handle)
        return nullptr;
    HtmlUITexture* texture = reinterpret_cast<HtmlUITexture*>(handle);
    // a texture handed to RmlUi through CallbackTextureInterface::SetTextureHandle (Lottie,
    // effects) is this renderer's from its first draw: view loss and ReleaseTexture treat it
    // like the renderer's own
    if (liveTextures.count(texture) == 0)
        liveTextures.insert(texture);
    return texture;
}

void RmlRenderer::destroyTexture(HtmlUITexture* texture){
    if (liveTextures.erase(texture) > 0 && texture->savedLayer && stats.boxShadowTextures > 0)
        stats.boxShadowTextures--;
    if (texture->atlas)
        atlasTextures.erase(std::remove(atlasTextures.begin(), atlasTextures.end(), texture), atlasTextures.end());
    cancelVideoConversion(texture);
    // the destructor destroys what it owns, without GPU calls when dead
    delete texture;
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle handle){
    HtmlUITexture* texture = reinterpret_cast<HtmlUITexture*>(handle);
    if (!texture)
        return;
    if (std::find(pendingReleases.begin(), pendingReleases.end(), texture) != pendingReleases.end())
        return;

    // draws earlier in this frame may still use it (a font page replaced while the context
    // renders): it goes after the frame
    if (frameState == FrameState::ACTIVE){
        pendingReleases.push_back(texture);
        return;
    }

    destroyTexture(texture);
}

void RmlRenderer::flushPendingReleases(){
    std::vector<HtmlUITexture*> releases;
    releases.swap(pendingReleases);
    for (HtmlUITexture* texture : releases){
        destroyTexture(texture);
    }
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: state
//-------------------------------------------------------------------------------------------

// While recording, the scissor and the clip mask are also set at once: the record-time checks
// (SaveLayer*) and the state getters read them; the replay sets them again from beginFrame's
void RmlRenderer::EnableScissorRegion(bool enable){
    scissorEnabled = enable;
    if (recording)
        recordCommand(CommandType::SCISSOR_ENABLE).flag = enable ? 1 : 0;
}

void RmlRenderer::SetScissorRegion(Rml::Rectanglei region){
    scissorRegion = region;
    // in target px once, not at every draw
    scissorTarget = toTargetRect(region, contextSize, targetSize);
    if (recording){
        // the draws read the target rectangle; the document one (the stateful passes') goes to the pool
        Command& command = recordCommand(CommandType::SCISSOR_REGION);
        command.i[0] = scissorTarget.Left();
        command.i[1] = scissorTarget.Top();
        command.i[2] = scissorTarget.Right();
        command.i[3] = scissorTarget.Bottom();
        command.a = commandRects.size();
        commandRects.push_back(region);
    }
}

void RmlRenderer::SetTransform(const Rml::Matrix4f* newTransform){
    if (recording){
        // RmlUi's matrix lives only during the call
        Command& command = recordCommand(CommandType::TRANSFORM);
        if (newTransform){
            command.flag = 1;
            command.i[0] = (int32_t)commandMatrices.size();
            commandMatrices.push_back(*newTransform);
        }
        return;
    }
    transform = newTransform ? (projection * (*newTransform)) : projection;
}

void RmlRenderer::EnableClipMask(bool enable){
    clipMaskEnabled = enable;
    if (recording)
        recordCommand(CommandType::CLIP_ENABLE).flag = enable ? 1 : 0;
}

const Rml::Matrix4f& RmlRenderer::getCurrentTransform() const{
    return transform;
}

Rml::Rectanglei RmlRenderer::getCurrentScissor() const{
    if (!scissorEnabled && activeClipScope < 0)
        return Rml::Rectanglei::MakeInvalid();
    return currentScissorTarget();
}

bool RmlRenderer::isClipMaskEnabled() const{
    return clipMaskEnabled;
}

uint8_t RmlRenderer::getStencilRef() const{
    return stencilRef;
}

Rml::Vector2f RmlRenderer::getPixelScale() const{
    const float sx = (contextSize.x > 0) ? (float)targetSize.x / (float)contextSize.x : 1.0f;
    const float sy = (contextSize.y > 0) ? (float)targetSize.y / (float)contextSize.y : 1.0f;
    return Rml::Vector2f(sx, sy);
}

Rml::Rectanglei RmlRenderer::currentScissorTarget() const{
    return effectiveScissorTarget();
}

PipelineState RmlRenderer::layerState(BlendMode blend, ColorWriteMask writeMask, const StencilState& stencil) const{
    PipelineState state;
    state.target.swapchain = false;
    state.target.color = ColorFormat::RGBA;
    state.target.depth = DepthStencilFormat::DEPTH_STENCIL;
    state.target.sampleCount = target ? target->getSampleCount() : 1;
    state.blend = blend;
    state.writeMask = writeMask;
    state.stencil = stencil;
    return state;
}

PipelineState RmlRenderer::getGeometryState(BlendMode blend) const{
    StencilState stencil;
    if (clipMaskEnabled){
        stencil.enabled = true;
        stencil.compare = CompareFunc::EQUAL;
        stencil.failOp = StencilOp::KEEP;
        stencil.depthFailOp = StencilOp::KEEP;
        stencil.passOp = StencilOp::KEEP;
        stencil.readMask = 0xFF;
        stencil.writeMask = 0x00;
        stencil.ref = stencilRef;
    }
    return layerState(blend, ColorWriteMask::RGBA, stencil);
}

PipelineState RmlRenderer::postTargetState(BlendMode blend) const{
    PipelineState state;
    state.target.swapchain = false;
    state.target.color = ColorFormat::RGBA;
    state.target.depth = DepthStencilFormat::NONE;
    state.target.sampleCount = 1;
    state.blend = blend;
    state.writeMask = ColorWriteMask::RGBA;
    return state;
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: passes
//-------------------------------------------------------------------------------------------

void RmlRenderer::endPass(bool resolve){
    if (activeKind == TargetKind::NONE)
        return;
    // sokol's GL backend resolves MSAA with glBlitFramebuffer at the end of the pass, and the blit
    // honours the scissor box: what may differ is resolved (the result of a full resolve), or
    // nothing when the layer is not read yet (a later pass resolves it). The other backends
    // resolve the whole render area at every pass end (nothing is tracked there).
    if (activeFramebuffer && Engine::isOpenGL() && activeSamples > 1){
        TargetRegions& r = regionsOf(*activeFramebuffer);
        const Rml::Rectanglei full = activeFull;
        const Rml::Rectanglei region = resolve ? intersect(r.stale, full) : Rml::Rectanglei::MakeInvalid();
        applyPassScissor(isEmpty(region) ? Rml::Rectanglei::FromSize(Rml::Vector2i(0, 0)) : region);
        if (resolve)
            r.stale = Rml::Rectanglei::MakeInvalid();
    }
    pass.endRenderPass();
    activeRegions = nullptr;
    activeKind = TargetKind::NONE;
    activeIndex = -1;
    activeFramebuffer = nullptr;
}

bool RmlRenderer::beginPass(Framebuffer& framebuffer, TargetKind kind, int index, LoadAction colorLoad, LoadAction depthStencilLoad){
    flushDeferredComposite();
    endPass();
    // a layer or post-process target found created this frame is (see RmlRenderTarget::newFrame)
    if (!(target && target->checkedThisFrame(framebuffer)) && !framebuffer.isCreated())
        return false;
    pass.setColorActions(colorLoad, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    // ignored by a pass without a depth-stencil attachment
    pass.setDepthStencilActions(depthStencilLoad, StoreAction::STORE, 1.0f, 0);
    // its name in the frame stats' pass list (JASPERENGINE_FRAME_STATS)
    pass.setLabel(kind == TargetKind::LAYER ? "html ui layer" : (kind == TargetKind::POSTPROCESS ? "html ui post" : "html ui"));
    pass.startRenderPass(&framebuffer.getRender());
    activeKind = kind;
    activeIndex = index;
    activeFramebuffer = &framebuffer;
    activeSamples = samplesOf(framebuffer, kind);
    // an MSAA layer on GL: what its resolve image may lack is tracked (the other backends resolve
    // the whole render area at every pass end)
    activeRegions = (Engine::isOpenGL() && kind == TargetKind::LAYER && activeSamples > 1) ? &regionsOf(framebuffer) : nullptr;
    stats.passes++;

    // the viewport and the scissor, nothing else: pipelines are applied per draw (the layers and
    // post-process targets have the render target's size)
    const Rml::Rectanglei full = ((kind == TargetKind::LAYER || kind == TargetKind::POSTPROCESS) && target)
                                 ? fullRect(target->getWidth(), target->getHeight())
                                 : fullRect((int)framebuffer.getWidth(), (int)framebuffer.getHeight());
    activeFull = full;
    // sg_begin_pass sets both to the whole framebuffer on every backend (sokol_gfx.h: "sg_begin_pass()
    // will reset both the viewport and scissor rectangles"): applying them again was two backend
    // calls a pass, at once overridden by the first draw's
    appliedViewport = full;
    appliedScissor = full;

    // a clear changes the samples where they held something: the resolve image differs there
    if (activeRegions && colorLoad == LoadAction::CLEAR){
        activeRegions->stale = unite(activeRegions->stale, activeRegions->content);
        activeRegions->content = Rml::Rectanglei::MakeInvalid();
    }
    return true;
}

RmlRenderer::TargetRegions& RmlRenderer::regionsOf(const Framebuffer& framebuffer){
    // the last two looked up (a pass's end and the next one's start ask for them): no hash lookup
    // on memory the driver has evicted (references into the map stay valid until it is cleared)
    for (int i = 0; i < 2; i++){
        if (regionsCache[i].framebuffer == &framebuffer)
            return *regionsCache[i].regions;
    }
    auto it = regions.find(&framebuffer);
    if (it == regions.end()){
        // not seen since it was made: anything may be in it
        TargetRegions unknown;
        unknown.content = fullRect((int)framebuffer.getWidth(), (int)framebuffer.getHeight());
        unknown.stale = unknown.content;
        it = regions.emplace(&framebuffer, unknown).first;
    }
    regionsCache[1] = regionsCache[0];
    regionsCache[0] = RegionsCacheEntry{&framebuffer, &it->second};
    return it->second;
}

void RmlRenderer::noteColorWrite(){
    if (!activeRegions)
        return;
    Rml::Rectanglei written = intersect(appliedViewport, appliedScissor);
    if (isEmpty(written))
        return;
    // A pixel of margin: Mesa's MSAA rasterizer covers samples of the row above and the column left
    // of a scissor (MsaaScissor.HtmlUIClipEdge); a resolve limited to the scissor would drop them and
    // differ from a full one. The resolve is clamped to the target (endPass).
    written = written.Extend(1);
    activeRegions->content = unite(activeRegions->content, written);
    activeRegions->stale = unite(activeRegions->stale, written);
}

int RmlRenderer::samplesOf(Framebuffer& framebuffer, TargetKind kind) const{
    if (kind == TargetKind::LAYER && target)
        return target->getSampleCount();
    if (kind == TargetKind::POSTPROCESS)
        return 1;
    return framebuffer.getRender().getSampleCount();
}

void RmlRenderer::ensureResolved(Framebuffer& layer){
    if (activeFramebuffer == &layer){
        if (activeSamples > 1)
            endPass(true);
        return;
    }
    int index = -1;
    const TargetKind kind = kindOf(layer, index);
    if (samplesOf(layer, kind) <= 1)
        return;
    if (!Engine::isOpenGL() || isEmpty(regionsOf(layer).stale))
        return;
    // a pass that only resolves
    if (beginPass(layer, kind, index, LoadAction::LOAD, LoadAction::LOAD))
        endPass(true);
}

namespace {
    std::atomic<bool> compositeFusion{true};
}

void RmlRenderer::setCompositeFusionForTesting(bool enabled){
    compositeFusion = enabled;
}

void RmlRenderer::flushDeferredComposite(){
    if (deferred.layer < 0 || fusingDeferred)
        return;
    const DeferredComposite d = deferred;
    deferred = DeferredComposite();
    Framebuffer* layer = target ? target->getLayer(d.layer) : nullptr;
    Framebuffer* post = target ? target->postprocess[d.post].get() : nullptr;
    if (!layer || !post || d.layer >= layersSize)
        return;
    // what CompositeLayers would have drawn
    PostDraw draw;
    draw.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
    draw.source = &post->getRender().getColorTexture();
    draw.scissor = d.scissor;
    drawPost(*layer, LoadAction::LOAD, draw, d.state);
}

bool RmlRenderer::bindLayer(int index){
    flushDeferredComposite();
    if (!target || index < 0 || index >= layersSize)
        return false;
    // drawing on into the open pass (every draw comes here): the layer exists
    if (activeKind == TargetKind::LAYER && activeIndex == index && activeFramebuffer && activeFramebuffer == target->getLayer(index))
        return true;
    Framebuffer* layer = target->ensureLayer(index);
    if (!layer)
        return false;
    if ((int)layerClearPending.size() <= index)
        layerClearPending.resize(index + 1, true);

    if (activeKind == TargetKind::LAYER && activeIndex == index && activeFramebuffer == layer)
        return true;

    // first bind in this frame (or since pushed): cleared; layer 0 also clears the shared
    // stencil, the others keep it
    const bool clear = layerClearPending[index];
    const LoadAction colorLoad = clear ? LoadAction::CLEAR : LoadAction::LOAD;
    const LoadAction depthStencilLoad = (clear && index == 0) ? LoadAction::CLEAR : LoadAction::LOAD;
    layerClearPending[index] = false;
    return beginPass(*layer, TargetKind::LAYER, index, colorLoad, depthStencilLoad);
}

bool RmlRenderer::bindTopLayer(){
    return bindLayer(layersSize - 1);
}

bool RmlRenderer::bindDrawLayer(bool stencilOnly){
    // a clip mask drawn while the top layer is a deferred composite (a backdrop with a rounded
    // border box): every layer shares layer 0's depth-stencil, so the layer under it takes the
    // stencil draw, the one the composite goes to next
    if (stencilOnly && deferred.layer >= 1 && deferred.layer == layersSize - 1){
        fusingDeferred = true;
        const bool bound = bindLayer(deferred.layer - 1);
        fusingDeferred = false;
        return bound;
    }
    return bindTopLayer();
}

void RmlRenderer::ensureLayerCleared(int index){
    if (index >= 0 && index < (int)layerClearPending.size() && layerClearPending[index])
        bindLayer(index);
}

RmlRenderer::TargetKind RmlRenderer::kindOf(const Framebuffer& framebuffer, int& index) const{
    index = -1;
    if (target){
        for (int i = 0; i < (int)target->layers.size(); i++){
            if (target->layers[i].get() == &framebuffer){
                index = i;
                return TargetKind::LAYER;
            }
        }
        for (int i = 0; i < RmlRenderTarget::POSTPROCESS_COUNT; i++){
            if (target->postprocess[i].get() == &framebuffer){
                index = i;
                return TargetKind::POSTPROCESS;
            }
        }
    }
    return TargetKind::OTHER;
}

bool RmlRenderer::bindTarget(Framebuffer& framebuffer, LoadAction load){
    flushDeferredComposite();
    int index = -1;
    const TargetKind kind = kindOf(framebuffer, index);
    if (kind == TargetKind::LAYER)
        return bindLayer(index);
    // drawing on into the open pass
    if (activeFramebuffer == &framebuffer && load == LoadAction::LOAD)
        return true;
    return beginPass(framebuffer, kind, index, load, LoadAction::DONTCARE);
}

void RmlRenderer::applyPassViewport(Rml::Rectanglei rect){
    if (rect == appliedViewport)
        return;
    pass.applyViewportTopLeft(rect.Left(), rect.Top(), rect.Width(), rect.Height());
    appliedViewport = rect;
}

void RmlRenderer::applyPassScissor(Rml::Rectanglei rect){
    if (rect == appliedScissor)
        return;
    pass.applyScissorTopLeft(rect.Left(), rect.Top(), rect.Width(), rect.Height());
    appliedScissor = rect;
}

Framebuffer* RmlRenderer::postTarget(int index){
    return target ? target->ensurePostprocess(index) : nullptr;
}

void RmlRenderer::drawFullscreenProgram(ObjectRender& program, const PipelineState& state, const std::function<void(ObjectRender&)>& applyUniforms){
    if (!program.beginDraw(state))
        return;
    if (applyUniforms)
        applyUniforms(program);
    program.draw(0, 3, 1);
    stats.drawCalls++;
    if (state.writeMask != ColorWriteMask::NONE)
        noteColorWrite();
}

bool RmlRenderer::drawPost(Framebuffer& dst, LoadAction load, const PostDraw& draw, const PipelineState& state){
    if (!draw.source || draw.variant < 0 || draw.variant >= HTMLUI_POST_VARIANT_COUNT)
        return false;
    if (!bindTarget(dst, load))
        return false;

    const Rml::Rectanglei full = fullRect((int)dst.getWidth(), (int)dst.getHeight());
    const Rml::Rectanglei viewport = draw.viewport.Valid() ? draw.viewport : full;
    const Rml::Rectanglei scissor = draw.scissor.Valid() ? draw.scissor.Intersect(full) : full;
    if (isEmpty(viewport) || isEmpty(scissor))
        return false;
    applyPassViewport(viewport);
    applyPassScissor(scissor);

    PostProgram& program = postPrograms[draw.variant];
    program.object.addTexture(program.textureSlot, ShaderStageType::FRAGMENT, draw.source);
    stats.textureBinds++;
    if (draw.mask){
        program.object.addTexture(program.maskSlot, ShaderStageType::FRAGMENT, draw.mask);
        stats.textureBinds++;
    }

    drawFullscreenProgram(program.object, state, [&](ObjectRender& object){
        PostVsParams vs;
        std::memcpy(vs.uvTransform, draw.uvTransform, sizeof(vs.uvTransform));
        object.applyUniformBlock(program.vsSlot, sizeof(PostVsParams), &vs);
        PostParams fs = draw.params;
        object.applyUniformBlock(program.fsSlot, sizeof(PostParams), &fs);
    });
    return true;
}

void RmlRenderer::stencilClear(uint8_t value){
    if (!bindDrawLayer(true))
        return;
    const Rml::Rectanglei scissor = currentScissorTarget();
    if (isEmpty(scissor))
        return;
    applyPassViewport(fullRect(targetSize.x, targetSize.y));
    applyPassScissor(scissor);

    // sokol has no clear inside a pass: a fullscreen triangle writes the stencil only,
    // limited by the scissor like GL3's scissored glClear
    StencilState stencil;
    stencil.enabled = true;
    stencil.compare = CompareFunc::ALWAYS;
    stencil.passOp = StencilOp::REPLACE;
    stencil.ref = value;
    PostProgram& program = postPrograms[(int)HtmlUIPostVariant::PASSTHROUGH];
    program.object.addTexture(program.textureSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
    drawFullscreenProgram(program.object, layerState(BlendMode::NONE, ColorWriteMask::NONE, stencil), [&](ObjectRender& object){
        PostVsParams vs = {{1.0f, 1.0f, 0.0f, 0.0f}};
        object.applyUniformBlock(program.vsSlot, sizeof(PostVsParams), &vs);
        PostParams fs;
        object.applyUniformBlock(program.fsSlot, sizeof(PostParams), &fs);
    });
    stats.stencilDraws++;
}

bool RmlRenderer::drawGeometry(RmlGeometry* geometry, Rml::Vector2f translation, HtmlUITexture* texture, const PipelineState& state){
    if (!bindDrawLayer(state.writeMask == ColorWriteMask::NONE))
        return false;
    // clipped away: sokol widens an empty scissor to 1x1 on Metal/Vulkan/WebGPU
    const Rml::Rectanglei scissor = currentScissorTarget();
    if (isEmpty(scissor))
        return false;

    uint32_t indexCount = 0;
    if (!bindGeometry(reinterpret_cast<Rml::CompiledGeometryHandle>(geometry), geometryObject, indexCount))
        return false;

    applyPassViewport(fullRect(targetSize.x, targetSize.y));
    applyPassScissor(scissor);

    // one program for everything: untextured geometry samples white
    geometryObject.addTexture(geometryTextureSlot, ShaderStageType::FRAGMENT, texture ? texture->render : &placeholderWhite);
    stats.textureBinds++;
    noteDrawn(texture);
    if (!geometryObject.beginDraw(state))
        return false;
    HtmlUIParams params = makeParams(transform, translation);
    geometryObject.applyUniformBlock(geometryParamsSlot, sizeof(HtmlUIParams), &params);
    // a stencil-only draw (a clip mask) is not clipped analytically
    const ClipParams& clipParams = (state.writeMask == ColorWriteMask::NONE)
                                   ? noClip : currentClipParams(texture);
    geometryObject.applyUniformBlock(geometryClipSlot, sizeof(ClipParams), const_cast<ClipParams*>(&clipParams));
    geometryObject.draw(0, indexCount, 1);
    stats.drawCalls++;
    if (state.writeMask == ColorWriteMask::NONE)
        stats.stencilDraws++;
    else
        noteColorWrite();
    return true;
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: geometry, clip masks
//-------------------------------------------------------------------------------------------

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle textureHandle){
    if (frameState == FrameState::ACTIVE)
        stats.geometrySubmitted++;
    if (recording){
        // the texture is checked now (it cannot change before the replay, which draws it as it
        // is); the geometry is read at replay only (an empty one draws nothing there)
        HtmlUITexture* texture = toTexture(textureHandle);
        if (texture && (texture->dead || !texture->render || !texture->render->isCreated()))
            return;
        Command& command = recordCommand(CommandType::GEOMETRY);
        command.a = handle;
        command.b = reinterpret_cast<uintptr_t>(texture);
        command.x = translation.x;
        command.y = translation.y;
        return;
    }
    renderGeometryNow(handle, translation, textureHandle);
}

void RmlRenderer::drawRecordedGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, HtmlUITexture* texture){
    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
    if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
        return;
    drawGeometry(geometry, translation, texture, getGeometryState(BlendMode::PREMULTIPLIED));
}

void RmlRenderer::renderGeometryNow(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle textureHandle){
    if (frameState != FrameState::ACTIVE){
        if (frameState == FrameState::NONE && !loggedOutsideFrame){
            Log::error("[HTML UI] geometry rendered outside a frame is ignored");
            loggedOutsideFrame = true;
        }
        return;
    }

    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
    if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
        return;

    HtmlUITexture* texture = toTexture(textureHandle);
    // a texture lost with the view (or never created) is skipped
    if (texture && (texture->dead || !texture->render || !texture->render->isCreated()))
        return;

    drawGeometry(geometry, translation, texture, getGeometryState(BlendMode::PREMULTIPLIED));
}

void RmlRenderer::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation){
    if (recording){
        RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
        if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
            return;
        // the reference the replay will test against (getStencilRef during the traversal)
        switch (operation){
        case Rml::ClipMaskOperation::Set:
        case Rml::ClipMaskOperation::SetInverse:
            stencilRef = 1;
            break;
        case Rml::ClipMaskOperation::Intersect:
            stencilRef = (uint8_t)std::min(255, stencilRef + 1);
            break;
        }
        Command& command = recordCommand(CommandType::CLIP_MASK);
        command.flag = (uint8_t)operation;
        command.a = handle;
        command.x = translation.x;
        command.y = translation.y;
        return;
    }
    renderToClipMaskNow(operation, handle, translation);
}

void RmlRenderer::RenderToClipMaskShape(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                                        const Rml::ClipShape& shape){
    if (!recording){
        renderToClipMaskNow(operation, handle, translation);
        return;
    }
    const size_t before = commands.size();
    RenderToClipMask(operation, handle, translation);
    if (commands.size() == before)
        return;
    // the replay may clip by the shape instead of drawing the mask (RmlRendererBatch.cpp)
    ClipShapeRecord record;
    record.rect = shape.rect;
    for (int i = 0; i < 4; i++)
        record.radii[i] = shape.radii[i];
    commandClipShapes.push_back(record);
    commands.back().i[0] = (int32_t)commandClipShapes.size();
}

void RmlRenderer::renderToClipMaskNow(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation){
    if (frameState != FrameState::ACTIVE)
        return;
    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
    if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
        return;

    StencilState write;
    write.enabled = true;
    write.compare = CompareFunc::ALWAYS;
    write.readMask = 0xFF;
    write.writeMask = 0xFF;
    switch (operation){
    case Rml::ClipMaskOperation::Set:
        stencilClear(0);
        write.passOp = StencilOp::REPLACE;
        write.ref = 1;
        stencilRef = 1;
        break;
    case Rml::ClipMaskOperation::SetInverse:
        stencilClear(1);
        write.passOp = StencilOp::REPLACE;
        write.ref = 0;
        stencilRef = 1;
        break;
    case Rml::ClipMaskOperation::Intersect:
        write.passOp = StencilOp::INCR_CLAMP;
        write.ref = 0;
        stencilRef = (uint8_t)std::min(255, stencilRef + 1);
        break;
    }

    drawGeometry(geometry, translation, nullptr, layerState(BlendMode::NONE, ColorWriteMask::NONE, write));
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: layers and filters
//-------------------------------------------------------------------------------------------

Rml::LayerHandle RmlRenderer::PushLayer(){
    if (recording){
        // the index the replay's push will have: the layers are a stack
        const int index = recordLayers++;
        recordCommand(CommandType::PUSH_LAYER).i[0] = index;
        return (Rml::LayerHandle)index;
    }
    return pushLayerNow();
}

Rml::LayerHandle RmlRenderer::pushLayerNow(){
    if (frameState != FrameState::ACTIVE)
        return 0;

    const int index = layersSize;
    layersSize++;
    if ((int)layerClearPending.size() <= index)
        layerClearPending.resize(index + 1, true);
    layerClearPending[index] = true;
    // the same index popped and pushed again while still the open pass: cleared on next bind
    if (activeKind == TargetKind::LAYER && activeIndex == index)
        endPass();
    if (target && !target->ensureLayer(index))
        Log::error("[HTML UI] layer %d is not available: its content is not drawn", index);
    stats.layersPushed++;
    return (Rml::LayerHandle)index;
}

void RmlRenderer::PopLayer(){
    if (recording){
        if (recordLayers > 1)
            recordLayers--;
        recordCommand(CommandType::POP_LAYER);
        return;
    }
    popLayerNow();
}

void RmlRenderer::popLayerNow(){
    if (frameState != FrameState::ACTIVE)
        return;
    if (layersSize <= 1){
        Log::error("[HTML UI] PopLayer without a pushed layer");
        return;
    }
    // the next draw switches back to the new top layer; a deferred composite into the popped
    // layer is dropped with it (its content is never read again)
    layersSize--;
    if (deferred.layer >= layersSize)
        deferred = DeferredComposite();
}

void RmlRenderer::CompositeLayers(Rml::LayerHandle sourceHandle, Rml::LayerHandle destinationHandle, Rml::BlendMode blendMode,
                                  Rml::Span<const Rml::CompiledFilterHandle> filters){
    if (recording){
        Command& command = recordCommand(CommandType::COMPOSITE);
        command.i[0] = (int32_t)sourceHandle;
        command.i[1] = (int32_t)destinationHandle;
        command.i[2] = (int32_t)commandFilters.size();
        command.i[3] = (int32_t)filters.size();
        command.flag = (uint8_t)blendMode;
        for (Rml::CompiledFilterHandle handle : filters){
            commandFilters.push_back(handle);
            // an effect filter reads its element now, the pass is drawn at replay
            if (handle && htmlui::EffectRenderer::isEffectHandle(handle))
                effects->prepareFilter(handle);
        }
        return;
    }
    compositeLayersNow(sourceHandle, destinationHandle, blendMode, filters);
}

void RmlRenderer::compositeLayersNow(Rml::LayerHandle sourceHandle, Rml::LayerHandle destinationHandle, Rml::BlendMode blendMode,
                                     Rml::Span<const Rml::CompiledFilterHandle> filters){
    if (frameState != FrameState::ACTIVE || !target)
        return;

    const int source = (int)sourceHandle;
    const int destination = (int)destinationHandle;
    if (source < 0 || source >= layersSize || destination < 0 || destination >= layersSize || source == destination){
        Log::error("[HTML UI] CompositeLayers from layer %d to %d of %d", source, destination, layersSize);
        return;
    }

    // the second composite of a backdrop: the filter output straight into the destination,
    // within both composites' scissors (the layer is transparent beyond the first's)
    if (deferred.layer == source && filters.empty() && blendMode == Rml::BlendMode::Blend){
        Framebuffer* post = postTarget(deferred.post);
        Framebuffer* destinationLayer = target->ensureLayer(destination);
        const Rml::Rectanglei scissor = intersect(deferred.scissor, currentScissorTarget());
        if (post && destinationLayer){
            if (!isEmpty(scissor)){
                PostDraw draw;
                draw.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
                draw.source = &post->getRender().getColorTexture();
                draw.scissor = scissor;
                fusingDeferred = true;
                drawPost(*destinationLayer, LoadAction::LOAD, draw, getGeometryState(BlendMode::PREMULTIPLIED));
                fusingDeferred = false;
            }
            stats.compositesFused++;
            return;
        }
    }
    flushDeferredComposite();

    // a layer nothing was drawn into is transparent: blending or adding it changes nothing
    if (blendMode != Rml::BlendMode::Replace && source < (int)layerClearPending.size() && layerClearPending[source])
        return;
    ensureLayerCleared(source);

    Framebuffer* sourceLayer = target->getLayer(source);
    if (!sourceLayer)
        return;
    // resolved, so it can be read
    ensureResolved(*sourceLayer);

    RmlFilterContext context(*this, *sourceLayer);
    if (!filters.empty()){
        // the filter chain's targets, created on first use
        const bool targets = postTarget(0) && postTarget(1) && postTarget(2);
        for (Rml::CompiledFilterHandle handle : filters){
            if (!handle)
                continue;
            if (!targets)
                continue;
            // bit 0 set: a phase-5 effect filter
            if (htmlui::EffectRenderer::isEffectHandle(handle)){
                if (effects->applyFilter(handle, context))
                    stats.filtersApplied++;
                continue;
            }
            applyFilter(context, *reinterpret_cast<const RmlCompiledFilter*>(handle));
            stats.filtersApplied++;
        }
    }

    Framebuffer* destinationLayer = target->ensureLayer(destination);
    if (!destinationLayer)
        return;
    // a filtered composite into a just pushed layer (a backdrop's): deferred, see DeferredComposite
    if (compositeFusion && blendMode == Rml::BlendMode::Blend && !clipMaskEnabled && !context.isPrimaryLayer() && destination == layersSize - 1 &&
        destination > 0 && destination < (int)layerClearPending.size() && layerClearPending[destination] && !isEmpty(currentScissorTarget())){
        deferred.layer = destination;
        deferred.post = context.primaryIndex;
        deferred.scissor = currentScissorTarget();
        deferred.state = getGeometryState(BlendMode::PREMULTIPLIED);
        return;
    }
    PostDraw draw;
    draw.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
    draw.source = &context.primary().getRender().getColorTexture();
    draw.scissor = currentScissorTarget();
    // Plus (the fork's cross-fade of background images): the premultiplied colours added
    const BlendMode blend = (blendMode == Rml::BlendMode::Replace) ? BlendMode::NONE
                            : (blendMode == Rml::BlendMode::Plus) ? BlendMode::ADDITIVE_PREMULTIPLIED
                                                                    : BlendMode::PREMULTIPLIED;
    drawPost(*destinationLayer, LoadAction::LOAD, draw, getGeometryState(blend));
}

void RmlRenderer::applyFilter(RmlFilterContext& context, const RmlCompiledFilter& filter){
    const Rml::Vector2f scale = getPixelScale();
    const Rml::Rectanglei window = context.getScissor();

    switch (filter.type){
    case RmlCompiledFilter::Type::PASSTHROUGH: {
        // opacity multiplies all four premultiplied channels (GL3 uses the blend colour,
        // which sokol bakes into the pipeline)
        PostDraw draw;
        draw.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
        draw.source = &context.primary().getRender().getColorTexture();
        draw.scissor = window;
        draw.params.params[0] = filter.blendFactor;
        drawPost(context.secondary(), LoadAction::CLEAR, draw, postTargetState(BlendMode::NONE));
        context.swap();
        break;
    }
    case RmlCompiledFilter::Type::COLOR_MATRIX: {
        PostDraw draw;
        draw.variant = (int)HtmlUIPostVariant::COLOR_MATRIX;
        draw.source = &context.primary().getRender().getColorTexture();
        draw.scissor = window;
        std::memcpy(draw.params.colorMatrix, filter.colorMatrix.data(), sizeof(draw.params.colorMatrix));
        drawPost(context.secondary(), LoadAction::CLEAR, draw, postTargetState(BlendMode::NONE));
        context.swap();
        break;
    }
    case RmlCompiledFilter::Type::MASK_IMAGE: {
        Framebuffer* mask = postTarget(RmlRenderTarget::BLEND_MASK);
        if (!mask)
            break;
        PostDraw draw;
        draw.variant = (int)HtmlUIPostVariant::BLEND_MASK;
        draw.source = &context.primary().getRender().getColorTexture();
        draw.mask = &mask->getRender().getColorTexture();
        draw.scissor = window;
        drawPost(context.secondary(), LoadAction::CLEAR, draw, postTargetState(BlendMode::NONE));
        context.swap();
        break;
    }
    case RmlCompiledFilter::Type::BLUR: {
        blurInto(context.primary(), context.secondary(), context.tertiary(), filter.sigma * scale.x, window);
        context.swap();
        break;
    }
    case RmlCompiledFilter::Type::DROP_SHADOW: {
        // the shadow: the source's alpha times the colour, offset, into a free target
        PostDraw shadow;
        shadow.variant = (int)HtmlUIPostVariant::DROP_SHADOW;
        shadow.source = &context.primary().getRender().getColorTexture();
        shadow.scissor = window;
        const Rml::Colourf colour = toColourf(filter.color);
        for (int i = 0; i < 4; i++)
            shadow.params.color[i] = colour[i];
        // texel centres of the window: nothing outside it is read
        shadow.params.texCoordMinMax[0] = ((float)window.Left() + 0.5f) / (float)targetSize.x;
        shadow.params.texCoordMinMax[1] = ((float)window.Top() + 0.5f) / (float)targetSize.y;
        shadow.params.texCoordMinMax[2] = ((float)window.Right() - 0.5f) / (float)targetSize.x;
        shadow.params.texCoordMinMax[3] = ((float)window.Bottom() - 0.5f) / (float)targetSize.y;
        // a pixel shows the source at its position minus the offset (top-down, so no flip of y)
        shadow.uvTransform[2] = -filter.offset.x * scale.x / (float)targetSize.x;
        shadow.uvTransform[3] = -filter.offset.y * scale.y / (float)targetSize.y;
        drawPost(context.secondary(), LoadAction::CLEAR, shadow, postTargetState(BlendMode::NONE));

        const float sigma = filter.sigma * scale.x;
        if (sigma >= 0.5f)
            blurInto(context.secondary(), context.secondary(), context.tertiary(), sigma, window);

        // the source over its shadow
        PostDraw over;
        over.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
        over.source = &context.primary().getRender().getColorTexture();
        over.scissor = window;
        drawPost(context.secondary(), LoadAction::LOAD, over, postTargetState(BlendMode::PREMULTIPLIED));
        context.swap();
        break;
    }
    case RmlCompiledFilter::Type::INVALID:
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unhandled render filter %d.", (int)filter.type);
        break;
    }
}

void RmlRenderer::blurInto(Framebuffer& source, Framebuffer& destination, Framebuffer& temp, float desiredSigma, Rml::Rectanglei window){
    // RmlUi GL3 RenderBlur, with a read-only source: the first pass reads source, every later
    // one ping-pongs between destination and temp; the result lands in destination
    window = window.Valid() ? window.Intersect(fullRect(targetSize.x, targetSize.y)) : fullRect(targetSize.x, targetSize.y);
    if (isEmpty(window))
        return;

    int passLevel = 0;
    float sigma = 0.0f;
    sigmaToParameters(desiredSigma, passLevel, sigma);

    const int width = targetSize.x;
    const int height = targetSize.y;
    const Rml::Rectanglei full = fullRect(width, height);
    Rml::Rectanglei scissor = window;

    // downscale by halves with bilinear filtering; with an odd size the UVs are scaled so the
    // fetches fall between texels (a 50% blend of neighbours)
    const Rml::Rectanglei half = Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(width / 2, 1), std::max(height / 2, 1)));
    const float uvScaleX = (width % 2 == 1) ? (1.0f - 1.0f / (float)width) : 1.0f;
    const float uvScaleY = (height % 2 == 1) ? (1.0f - 1.0f / (float)height) : 1.0f;

    Framebuffer* from = &source;
    for (int i = 0; i < passLevel; i++){
        scissor.p0 = (scissor.p0 + Rml::Vector2i(1)) / 2;
        scissor.p1 = Rml::Math::Max(scissor.p1 / 2, scissor.p0);
        Framebuffer* to = (i % 2 == 0) ? &temp : &destination;
        PostDraw draw;
        draw.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
        draw.source = &from->getRender().getColorTexture();
        draw.viewport = half;
        draw.scissor = scissor;
        draw.uvTransform[0] = uvScaleX;
        draw.uvTransform[1] = uvScaleY;
        drawPost(*to, LoadAction::CLEAR, draw, postTargetState(BlendMode::NONE));
        from = to;
    }

    // the data goes to temp, wherever the last downscale left it
    if (passLevel % 2 == 0){
        PostDraw copy;
        copy.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
        copy.source = &from->getRender().getColorTexture();
        copy.viewport = full;
        copy.scissor = scissor;
        drawPost(temp, LoadAction::CLEAR, copy, postTargetState(BlendMode::NONE));
    }
    if (isEmpty(scissor))
        return;

    PostDraw blur;
    blur.variant = (int)HtmlUIPostVariant::BLUR;
    blur.viewport = full;
    // the taps are mirrored at the data's edges (Chromium's backdrop blur); the two passes also write one texel
    // around the data, mirrored too, so the linear upscale below blends the edge texels with them, not with the
    // transparent clear (the downscaled data sits in the top-left of full-size targets: the texel beyond it is free)
    blur.scissor = Rml::Rectanglei::FromCorners(scissor.p0 - Rml::Vector2i(1), scissor.p1 + Rml::Vector2i(1)).Intersect(full);
    blurWeights(sigma, blur.params.weights);
    blur.params.texCoordMinMax[0] = ((float)scissor.Left() + 0.5f) / (float)width;
    blur.params.texCoordMinMax[1] = ((float)scissor.Top() + 0.5f) / (float)height;
    blur.params.texCoordMinMax[2] = ((float)scissor.Right() - 0.5f) / (float)width;
    blur.params.texCoordMinMax[3] = ((float)scissor.Bottom() - 0.5f) / (float)height;

    // vertical: temp -> destination
    blur.source = &temp.getRender().getColorTexture();
    blur.params.params[1] = 0.0f;
    blur.params.params[2] = 1.0f / (float)height;
    drawPost(destination, LoadAction::CLEAR, blur, postTargetState(BlendMode::NONE));

    // horizontal: destination -> temp; the clear leaves the region beyond the extended edge a
    // transparent border (GL3's padded clear)
    blur.source = &destination.getRender().getColorTexture();
    blur.params.params[1] = 1.0f / (float)width;
    blur.params.params[2] = 0.0f;
    drawPost(temp, LoadAction::CLEAR, blur, postTargetState(BlendMode::NONE));

    // upscale into the window, then (more exact for large pass levels) by a power of two
    PostDraw upscale;
    upscale.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
    upscale.source = &temp.getRender().getColorTexture();
    upscale.scissor = window;
    upscale.uvTransform[0] = (float)scissor.Width() / (float)width;
    upscale.uvTransform[1] = (float)scissor.Height() / (float)height;
    upscale.uvTransform[2] = (float)scissor.Left() / (float)width;
    upscale.uvTransform[3] = (float)scissor.Top() / (float)height;
    upscale.viewport = window;
    drawPost(destination, LoadAction::CLEAR, upscale, postTargetState(BlendMode::NONE));

    const Rml::Rectanglei exact = Rml::Rectanglei::FromCorners(scissor.p0 * (1 << passLevel), scissor.p1 * (1 << passLevel));
    if (exact != window && !isEmpty(exact)){
        upscale.viewport = exact;
        drawPost(destination, LoadAction::LOAD, upscale, postTargetState(BlendMode::NONE));
    }
}

Framebuffer* RmlRenderer::readableTopLayer(){
    const int top = layersSize - 1;
    ensureLayerCleared(top);
    Framebuffer* layer = target->getLayer(top);
    if (!layer)
        return nullptr;
    // resolved, so it can be read
    ensureResolved(*layer);
    return layer;
}

HtmlUITexture* RmlRenderer::createLayerTexture(int width, int height){
    std::unique_ptr<HtmlUITexture> texture = std::make_unique<HtmlUITexture>();
    texture->kind = HtmlUITexture::Kind::RENDER_TARGET;
    texture->ownedTarget = std::make_unique<Framebuffer>();
    FramebufferDesc desc;
    desc.width = width;
    desc.height = height;
    desc.color = ColorFormat::RGBA;
    desc.depth = DepthStencilFormat::NONE;
    desc.sampleCount = 1;
    desc.label = "htmlui-layer-texture";
    gpuCreateCount++;
    if (!texture->ownedTarget->create(desc)){
        Log::error("[HTML UI] could not create a %d x %d layer texture", desc.width, desc.height);
        return nullptr;
    }
    texture->render = &texture->ownedTarget->getRender().getColorTexture();
    texture->width = desc.width;
    texture->height = desc.height;
    texture->savedLayer = true;
    HtmlUITexture* result = texture.release();
    liveTextures.insert(result);
    stats.boxShadowTextures++;
    return result;
}

void RmlRenderer::copyLayerInto(Framebuffer& layer, HtmlUITexture& texture, Rml::Rectanglei bounds, bool tiled, Rml::Vector2i offset,
                                LoadAction load){
    if (!texture.ownedTarget)
        return;
    // the scissor region of the top layer (at the offset when tiled), no flip: every image is
    // top-left origin
    PostDraw copy;
    copy.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
    copy.source = &layer.getRender().getColorTexture();
    copy.uvTransform[0] = (float)bounds.Width() / (float)targetSize.x;
    copy.uvTransform[1] = (float)bounds.Height() / (float)targetSize.y;
    copy.uvTransform[2] = (float)bounds.Left() / (float)targetSize.x;
    copy.uvTransform[3] = (float)bounds.Top() / (float)targetSize.y;
    if (tiled){
        copy.viewport = Rml::Rectanglei::FromPositionSize(offset, bounds.Size());
        copy.scissor = copy.viewport;
    }
    drawPost(*texture.ownedTarget, load, copy, postTargetState(BlendMode::NONE));
    endPass();
}

void RmlRenderer::copyLayerIntoMask(Framebuffer& layer, Framebuffer& mask){
    PostDraw copy;
    copy.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
    copy.source = &layer.getRender().getColorTexture();
    copy.scissor = currentScissorTarget();
    drawPost(mask, LoadAction::CLEAR, copy, postTargetState(BlendMode::NONE));
}

// SAVE_TEXTURE's flag
namespace {
    constexpr uint8_t SAVE_TILED = 1;
    constexpr uint8_t SAVE_LOAD = 2;
}

Rml::TextureHandle RmlRenderer::SaveLayerAsTexture(){
    if (frameState != FrameState::ACTIVE || !target)
        return 0;
    if (!scissorEnabled){
        Rml::Log::Message(Rml::Log::LT_WARNING, "SaveLayerAsTexture needs a scissor region; nothing is saved.");
        return 0;
    }
    const Rml::Rectanglei bounds = currentScissorTarget();
    if (isEmpty(bounds))
        return 0;

    if (recording){
        // the texture now (RmlUi draws with its handle next), its content at replay
        HtmlUITexture* texture = createLayerTexture(bounds.Width(), bounds.Height());
        if (!texture)
            return 0;
        recordCommand(CommandType::SAVE_TEXTURE).a = reinterpret_cast<uintptr_t>(texture);
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    Framebuffer* layer = readableTopLayer();
    if (!layer)
        return 0;
    HtmlUITexture* texture = createLayerTexture(bounds.Width(), bounds.Height());
    if (!texture)
        return 0;
    copyLayerInto(*layer, *texture, bounds, false, Rml::Vector2i(0, 0), LoadAction::CLEAR);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

Rml::TextureHandle RmlRenderer::SaveLayerIntoTexture(Rml::TextureHandle textureHandle, Rml::Vector2i textureDimensions,
                                                     Rml::Vector2i destinationOffset){
    if (frameState != FrameState::ACTIVE || !target || !scissorEnabled)
        return 0;
    const Rml::Rectanglei bounds = currentScissorTarget();
    if (isEmpty(bounds) || textureDimensions.x <= 0 || textureDimensions.y <= 0)
        return 0;

    Framebuffer* layer = nullptr;
    if (!recording){
        layer = readableTopLayer();
        if (!layer)
            return 0;
    }

    // textureDimensions and destinationOffset are in document px; the layer holds target pixels.
    // The texture is made at the target's scale, as SaveLayerAsTexture makes its texture the size
    // of the scissor region in target pixels (a scaled canvas: the document drawn at screen
    // resolution), and is drawn over textureDimensions document px like that one.
    const float sx = (contextSize.x > 0) ? (float)targetSize.x / (float)contextSize.x : 1.0f;
    const float sy = (contextSize.y > 0) ? (float)targetSize.y / (float)contextSize.y : 1.0f;
    const Rml::Vector2i scaledDimensions(std::max(1, (int)std::ceil((float)textureDimensions.x * sx)),
                                         std::max(1, (int)std::ceil((float)textureDimensions.y * sy)));
    const Rml::Vector2i scaledOffset((int)std::lround((float)destinationOffset.x * sx), (int)std::lround((float)destinationOffset.y * sy));

    HtmlUITexture* texture = reinterpret_cast<HtmlUITexture*>(textureHandle);
    LoadAction load = LoadAction::LOAD;
    if (!texture){
        texture = createLayerTexture(scaledDimensions.x, scaledDimensions.y);
        if (!texture)
            return 0;
        // the first tile clears the whole texture
        load = LoadAction::CLEAR;
    }else if (texture->kind != HtmlUITexture::Kind::RENDER_TARGET || !texture->ownedTarget ||
               texture->width != scaledDimensions.x || texture->height != scaledDimensions.y){
        return 0;
    }

    if (recording){
        Command& command = recordCommand(CommandType::SAVE_TEXTURE);
        command.a = reinterpret_cast<uintptr_t>(texture);
        command.flag = SAVE_TILED | (load == LoadAction::LOAD ? SAVE_LOAD : 0);
        command.i[0] = scaledOffset.x;
        command.i[1] = scaledOffset.y;
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }
    copyLayerInto(*layer, *texture, bounds, true, scaledOffset, load);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

Rml::CompiledFilterHandle RmlRenderer::SaveLayerAsMaskImage(){
    if (frameState != FrameState::ACTIVE || !target)
        return 0;

    if (recording){
        // the filter now (RmlUi composites with it next), the blend mask's content at replay
        recordCommand(CommandType::SAVE_MASK);
    }else{
        Framebuffer* layer = readableTopLayer();
        Framebuffer* mask = postTarget(RmlRenderTarget::BLEND_MASK);
        if (!layer || !mask)
            return 0;
        copyLayerIntoMask(*layer, *mask);
    }

    RmlCompiledFilter* filter = new RmlCompiledFilter();
    filter->type = RmlCompiledFilter::Type::MASK_IMAGE;
    return reinterpret_cast<Rml::CompiledFilterHandle>(filter);
}

Rml::CompiledFilterHandle RmlRenderer::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters){
    // phase 5: an effect's filter form (the handle is the instance, bit 0 set)
    if (name == "jfx")
        return effects->compileFilter(parameters);

    RmlCompiledFilter filter;

    if (name == "opacity"){
        filter.type = RmlCompiledFilter::Type::PASSTHROUGH;
        filter.blendFactor = Rml::Get(parameters, "value", 1.0f);
    }else if (name == "blur"){
        filter.type = RmlCompiledFilter::Type::BLUR;
        filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
    }else if (name == "drop-shadow"){
        filter.type = RmlCompiledFilter::Type::DROP_SHADOW;
        filter.sigma = Rml::Get(parameters, "sigma", 0.0f);
        filter.color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        filter.offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
    }else if (name == "brightness"){
        filter.type = RmlCompiledFilter::Type::COLOR_MATRIX;
        const float value = Rml::Get(parameters, "value", 1.0f);
        filter.colorMatrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
    }else if (name == "contrast"){
        filter.type = RmlCompiledFilter::Type::COLOR_MATRIX;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float grayness = 0.5f - 0.5f * value;
        filter.colorMatrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
        filter.colorMatrix.SetColumn(3, Rml::Vector4f(grayness, grayness, grayness, 1.0f));
    }else if (name == "invert"){
        filter.type = RmlCompiledFilter::Type::COLOR_MATRIX;
        const float value = Rml::Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.0f, 1.0f);
        const float inverted = 1.0f - 2.0f * value;
        filter.colorMatrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.0f);
        filter.colorMatrix.SetColumn(3, Rml::Vector4f(value, value, value, 1.0f));
    }else if (name == "grayscale"){
        filter.type = RmlCompiledFilter::Type::COLOR_MATRIX;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev = 1.0f - value;
        const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        filter.colorMatrix = Rml::Matrix4f::FromRows(
            {gray.x + rev, gray.y,       gray.z,       0.0f},
            {gray.x,       gray.y + rev, gray.z,       0.0f},
            {gray.x,       gray.y,       gray.z + rev, 0.0f},
            {0.0f,         0.0f,         0.0f,         1.0f});
    }else if (name == "sepia"){
        filter.type = RmlCompiledFilter::Type::COLOR_MATRIX;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev = 1.0f - value;
        const Rml::Vector3f r = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
        const Rml::Vector3f g = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
        const Rml::Vector3f b = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
        filter.colorMatrix = Rml::Matrix4f::FromRows(
            {r.x + rev, r.y,       r.z,       0.0f},
            {g.x,       g.y + rev, g.z,       0.0f},
            {b.x,       b.y,       b.z + rev, 0.0f},
            {0.0f,      0.0f,      0.0f,      1.0f});
    }else if (name == "hue-rotate"){
        // https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
        filter.type = RmlCompiledFilter::Type::COLOR_MATRIX;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float s = Rml::Math::Sin(value);
        const float c = Rml::Math::Cos(value);
        filter.colorMatrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0.0f},
            {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0.0f},
            {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0.0f},
            {0.0f,                             0.0f,                             0.0f,                             1.0f});
    }else if (name == "saturate"){
        filter.type = RmlCompiledFilter::Type::COLOR_MATRIX;
        const float value = Rml::Get(parameters, "value", 1.0f);
        filter.colorMatrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * value, 0.715f - 0.715f * value, 0.072f - 0.072f * value, 0.0f},
            {0.213f - 0.213f * value, 0.715f + 0.285f * value, 0.072f - 0.072f * value, 0.0f},
            {0.213f - 0.213f * value, 0.715f - 0.715f * value, 0.072f + 0.928f * value, 0.0f},
            {0.0f,                    0.0f,                    0.0f,                    1.0f});
    }

    if (filter.type != RmlCompiledFilter::Type::INVALID)
        return reinterpret_cast<Rml::CompiledFilterHandle>(new RmlCompiledFilter(filter));

    // routed to the Console with the [HTML UI] prefix (RmlSystem::LogMessage)
    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter '%s': supported are opacity, blur, drop-shadow, brightness, contrast, "
                      "invert, grayscale, sepia, hue-rotate and saturate.", name.c_str());
    return 0;
}

void RmlRenderer::ReleaseFilter(Rml::CompiledFilterHandle filter){
    if (!filter)
        return;
    // recorded composites may use it (a box shadow's blur goes right after its composite)
    if (recording){
        deferredReleases.push_back(DeferredRelease{ReleaseKind::FILTER, filter});
        return;
    }
    releaseFilterNow(filter);
}

void RmlRenderer::releaseFilterNow(Rml::CompiledFilterHandle filter){
    if (htmlui::EffectRenderer::isEffectHandle(filter)){
        effects->releaseFilter(filter);
        return;
    }
    delete reinterpret_cast<RmlCompiledFilter*>(filter);
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: shaders (gradients)
//-------------------------------------------------------------------------------------------

Rml::CompiledShaderHandle RmlRenderer::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters){
    // phase 5: an effect's paint form (the handle is the instance, bit 0 set)
    if (name == "jfx")
        return effects->compileShader(parameters);

    auto applyColorStopList = [&name](RmlCompiledShader& shader, const Rml::Dictionary& shaderParameters){
        auto it = shaderParameters.find("color_stop_list");
        if (it == shaderParameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST)
            return;
        const Rml::ColorStopList& list = it->second.GetReference<Rml::ColorStopList>();
        if ((int)list.size() > RmlCompiledShader::MAX_STOPS){
            Rml::Log::Message(Rml::Log::LT_WARNING, "%s has %d color stops; only the first %d are drawn.", name.c_str(),
                              (int)list.size(), RmlCompiledShader::MAX_STOPS);
        }
        const int count = std::min((int)list.size(), RmlCompiledShader::MAX_STOPS);
        shader.stopPositions.resize(count);
        shader.stopColors.resize(count);
        for (int i = 0; i < count; i++){
            const Rml::ColorStop& stop = list[i];
            shader.stopPositions[i] = (stop.position.unit == Rml::Unit::NUMBER) ? stop.position.number : 0.0f;
            shader.stopColors[i] = toColourf(stop.color);
        }
    };

    RmlCompiledShader shader;
    bool known = true;

    if (name == "linear-gradient"){
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader.function = repeating ? RmlCompiledShader::Function::REPEATING_LINEAR : RmlCompiledShader::Function::LINEAR;
        shader.p = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
        shader.v = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f)) - shader.p;
        applyColorStopList(shader, parameters);
    }else if (name == "radial-gradient"){
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader.function = repeating ? RmlCompiledShader::Function::REPEATING_RADIAL : RmlCompiledShader::Function::RADIAL;
        shader.p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        shader.v = Rml::Vector2f(1.0f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
        applyColorStopList(shader, parameters);
    }else if (name == "conic-gradient"){
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader.function = repeating ? RmlCompiledShader::Function::REPEATING_CONIC : RmlCompiledShader::Function::CONIC;
        shader.p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const float angle = Rml::Get(parameters, "angle", 0.0f);
        shader.v = Rml::Vector2f(Rml::Math::Cos(angle), Rml::Math::Sin(angle));
        applyColorStopList(shader, parameters);
    }else{
        known = false;
    }

    if (known)
        return reinterpret_cast<Rml::CompiledShaderHandle>(new RmlCompiledShader(shader));

    // decorator: shader("...") included; phase-5 effects claim their names before this
    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader '%s': supported are linear-gradient, radial-gradient and "
                      "conic-gradient (also repeating).", name.c_str());
    return 0;
}

void RmlRenderer::RenderShader(Rml::CompiledShaderHandle shaderHandle, Rml::CompiledGeometryHandle geometryHandle, Rml::Vector2f translation,
                               Rml::TextureHandle /*texture*/){
    if (recording){
        if (!shaderHandle || !geometryHandle)
            return;
        // an effect reads its element now; one that draws nothing this frame is not recorded
        if (htmlui::EffectRenderer::isEffectHandle(shaderHandle) && !effects->prepareShader(shaderHandle))
            return;
        Command& command = recordCommand(CommandType::SHADER);
        command.a = shaderHandle;
        command.b = geometryHandle;
        command.x = translation.x;
        command.y = translation.y;
        return;
    }
    renderShaderNow(shaderHandle, geometryHandle, translation);
}

void RmlRenderer::renderShaderNow(Rml::CompiledShaderHandle shaderHandle, Rml::CompiledGeometryHandle geometryHandle, Rml::Vector2f translation){
    if (frameState != FrameState::ACTIVE || !shaderHandle || !geometryHandle)
        return;
    if (htmlui::EffectRenderer::isEffectHandle(shaderHandle)){
        effects->renderShader(shaderHandle, geometryHandle, translation);
        return;
    }
    const RmlCompiledShader& shader = *reinterpret_cast<const RmlCompiledShader*>(shaderHandle);

    if (!bindTopLayer())
        return;
    const Rml::Rectanglei scissor = currentScissorTarget();
    if (isEmpty(scissor))
        return;
    uint32_t indexCount = 0;
    if (!bindGeometry(geometryHandle, gradientObject, indexCount))
        return;
    applyPassViewport(fullRect(targetSize.x, targetSize.y));
    applyPassScissor(scissor);

    if (!gradientObject.beginDraw(getGeometryState(BlendMode::PREMULTIPLIED)))
        return;

    GradientParams gradient = {};
    const int stops = (int)shader.stopPositions.size();
    gradient.funcAndCount[0] = (float)(int)shader.function;
    gradient.funcAndCount[1] = (float)stops;
    gradient.pv[0] = shader.p.x;
    gradient.pv[1] = shader.p.y;
    gradient.pv[2] = shader.v.x;
    gradient.pv[3] = shader.v.y;
    for (int i = 0; i < stops; i++){
        for (int c = 0; c < 4; c++)
            gradient.stopColors[i][c] = shader.stopColors[i][c];
        gradient.stopPositions[i / 4][i % 4] = shader.stopPositions[i];
    }

    HtmlUIParams params = makeParams(transform, translation);
    gradientObject.applyUniformBlock(gradientParamsSlot, sizeof(HtmlUIParams), &params);
    gradientObject.applyUniformBlock(gradientFsSlot, sizeof(GradientParams), &gradient);
    gradientObject.draw(0, indexCount, 1);
    stats.drawCalls++;
    noteColorWrite();
}

void RmlRenderer::ReleaseShader(Rml::CompiledShaderHandle shader){
    if (!shader)
        return;
    if (recording){
        deferredReleases.push_back(DeferredRelease{ReleaseKind::SHADER, shader});
        return;
    }
    releaseShaderNow(shader);
}

void RmlRenderer::releaseShaderNow(Rml::CompiledShaderHandle shader){
    if (htmlui::EffectRenderer::isEffectHandle(shader)){
        effects->releaseShader(shader);
        return;
    }
    delete reinterpret_cast<RmlCompiledShader*>(shader);
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: frames and programs
//-------------------------------------------------------------------------------------------

namespace {

    bool loadGeometryObject(ObjectRender& object, ShaderRender* shader, BufferRender* vertices, BufferRender* indices, TextureRender* white){
        ShaderData& shaderData = shader->shaderData;
        const unsigned int stride = sizeof(Rml::Vertex);

        object.beginLoad(PrimitiveType::TRIANGLES);
        object.setShader(shader);
        // Rml::Vertex: position (2 floats), colour (4 bytes), tex_coord (2 floats). An
        // attribute the variant does not use has slot -1 and is skipped
        object.addAttribute(shaderData.getAttrIndex(AttributeType::POSITION), vertices, 2, AttributeDataType::FLOAT, stride, 0, false, false);
        object.addAttribute(shaderData.getAttrIndex(AttributeType::COLOR), vertices, 4, AttributeDataType::UNSIGNED_BYTE, stride, 8, true, false);
        object.addAttribute(shaderData.getAttrIndex(AttributeType::TEXCOORD1), vertices, 2, AttributeDataType::FLOAT, stride, 12, false, false);
        object.setIndex(indices, AttributeDataType::UNSIGNED_INT, 0);
        const std::pair<int, int> textureSlot = shaderData.getTextureIndexByName("u_htmluiTexture");
        if (textureSlot.first >= 0)
            object.addTexture(textureSlot, ShaderStageType::FRAGMENT, white);
        return object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::UINT32);
    }

}

bool RmlRenderer::loadPrograms(){
    geometryShader = ShaderPool::get(ShaderType::HTMLUI, ShaderPool::getHtmlUIProperties(true));
    gradientShader = ShaderPool::get(ShaderType::HTMLUI_GRADIENT, 0);
    primShader = ShaderPool::get(ShaderType::HTMLUI_PRIM, 0);
    bool ready = geometryShader && geometryShader->isCreated() && gradientShader && gradientShader->isCreated() && primShader &&
                 primShader->isCreated();
    static_assert(POST_PROGRAM_COUNT == HTMLUI_POST_VARIANT_COUNT, "one post program per HTMLUI_POST variant");
    for (int i = 0; i < HTMLUI_POST_VARIANT_COUNT; i++){
        PostProgram& program = postPrograms[i];
        // asked every time until created: the pool hands a pending variant to its builder again
        if (!program.shader || !program.shader->isCreated())
            program.shader = ShaderPool::get(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties((HtmlUIPostVariant)i));
        ready = ready && program.shader && program.shader->isCreated();
    }
    return ready;
}

bool RmlRenderer::loadPrimObject(){
    // the unit quad (two triangles), and one zero instance whose layout the instance buffers share
    const float corners[8] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const uint32_t cornerIndices[6] = {0, 1, 2, 0, 2, 3};
    HtmlUIPrimitive zero;
    std::memset(&zero, 0, sizeof(zero));
    gpuCreateCount += 3;
    if (!primQuadBuffer.createBuffer(sizeof(corners), (void*)corners, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE) ||
        !primQuadIndexBuffer.createBuffer(sizeof(cornerIndices), (void*)cornerIndices, BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE) ||
        !primInstanceTemplate.createBuffer(sizeof(zero), &zero, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE))
        return false;

    ShaderData& data = primShader->shaderData;
    primParamsSlot = data.getUniformBlockIndexByName("u_vs_htmluiPrim");
    primClipSlot = data.getUniformBlockIndexByName("u_fs_htmluiClip");
    primTextureSlot = data.getTextureIndexByName("u_htmluiTexture");
    primObject.beginLoad(PrimitiveType::TRIANGLES);
    primObject.setShader(primShader.get());
    primObject.addAttribute(data.getAttrIndex(AttributeType::POSITION), &primQuadBuffer, 2, AttributeDataType::FLOAT, sizeof(float) * 2, 0, false, false);
    // every instance attribute in one bind slot (more of them than sokol has vertex buffer slots)
    struct InstanceAttribute {
        const char* name;
        size_t offset;
        bool color;
    };
    const InstanceAttribute attributes[] = {
        {"i_a", offsetof(HtmlUIPrimitive, a), false},
        {"i_b", offsetof(HtmlUIPrimitive, b), false},
        {"i_c", offsetof(HtmlUIPrimitive, c), false},
        {"i_d", offsetof(HtmlUIPrimitive, d), false},
        {"i_e", offsetof(HtmlUIPrimitive, e), false},
        {"i_f", offsetof(HtmlUIPrimitive, f), false},
        {"i_params", offsetof(HtmlUIPrimitive, params), false},
        {"i_color", offsetof(HtmlUIPrimitive, color), true},
        {"i_border0", offsetof(HtmlUIPrimitive, border), true},
        {"i_border1", offsetof(HtmlUIPrimitive, border) + 4, true},
        {"i_border2", offsetof(HtmlUIPrimitive, border) + 8, true},
        {"i_border3", offsetof(HtmlUIPrimitive, border) + 12, true},
    };
    for (const InstanceAttribute& attribute : attributes){
        const int slot = data.getAttrIndexByName(attribute.name);
        if (slot < 0){
            Log::error("[HTML UI] the primitive program has no attribute %s", attribute.name);
            return false;
        }
        primObject.addInterleavedAttribute(slot, &primInstanceTemplate, 4, attribute.color ? AttributeDataType::UNSIGNED_BYTE : AttributeDataType::FLOAT,
                                           sizeof(HtmlUIPrimitive), attribute.offset, attribute.color, true);
    }
    primObject.setIndex(&primQuadIndexBuffer, AttributeDataType::UNSIGNED_INT, 0);
    if (primTextureSlot.first >= 0)
        primObject.addTexture(primTextureSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
    return primObject.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::UINT32);
}

bool RmlRenderer::loadPipelines(){
    if (pipelinesLoaded)
        return true;
    if (!Engine::isViewLoaded())
        return false;

    // every variant is asked for at once, so they compile together; still compiling (or
    // failed, which ShaderPool logged): asked again next frame
    if (!loadPrograms())
        return false;

    const uint8_t white[4] = {255, 255, 255, 255};
    void* whiteData[1] = {const_cast<uint8_t*>(white)};
    size_t whiteSize[1] = {sizeof(white)};
    gpuCreateCount++;
    if (!placeholderWhite.createTexture("htmlui-white", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, whiteData, whiteSize,
                                        TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)){
        Log::error("[HTML UI] could not create the placeholder texture");
        destroyPipelineObjects();
        return false;
    }

    // the composite: the document target as a quad in normalized device coordinates; its
    // buffers are also the layout template of the dynamic geometry objects
    const Rml::ColourbPremultiplied white4(255, 255, 255, 255);
    const Rml::Vertex quad[4] = {
        {Rml::Vector2f(-1.0f,  1.0f), white4, Rml::Vector2f(0.0f, 0.0f)},
        {Rml::Vector2f( 1.0f,  1.0f), white4, Rml::Vector2f(1.0f, 0.0f)},
        {Rml::Vector2f( 1.0f, -1.0f), white4, Rml::Vector2f(1.0f, 1.0f)},
        {Rml::Vector2f(-1.0f, -1.0f), white4, Rml::Vector2f(0.0f, 1.0f)},
    };
    const uint32_t quadIndices[6] = {0, 1, 2, 0, 2, 3};
    gpuCreateCount += 2;
    if (!compositeVertexBuffer.createBuffer(sizeof(quad), (void*)quad, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE) ||
        !compositeIndexBuffer.createBuffer(sizeof(quadIndices), (void*)quadIndices, BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE)){
        Log::error("[HTML UI] could not create the HTML UI composite");
        destroyPipelineObjects();
        return false;
    }

    ShaderData& geometryData = geometryShader->shaderData;
    geometryParamsSlot = geometryData.getUniformBlockIndexByName("u_vs_htmluiParams");
    geometryClipSlot = geometryData.getUniformBlockIndexByName("u_fs_htmluiClip");
    geometryTextureSlot = geometryData.getTextureIndexByName("u_htmluiTexture");
    gradientParamsSlot = gradientShader->shaderData.getUniformBlockIndexByName("u_vs_htmluiParams");
    gradientFsSlot = gradientShader->shaderData.getUniformBlockIndexByName("u_fs_htmluiGradient");

    // the composite draws inside the camera pass with its fixed pipeline types
    compositeObject.beginLoad(PrimitiveType::TRIANGLES);
    compositeObject.setShader(geometryShader.get());
    compositeObject.addAttribute(geometryData.getAttrIndex(AttributeType::POSITION), &compositeVertexBuffer, 2, AttributeDataType::FLOAT, sizeof(Rml::Vertex), 0, false, false);
    compositeObject.addAttribute(geometryData.getAttrIndex(AttributeType::COLOR), &compositeVertexBuffer, 4, AttributeDataType::UNSIGNED_BYTE, sizeof(Rml::Vertex), 8, true, false);
    compositeObject.addAttribute(geometryData.getAttrIndex(AttributeType::TEXCOORD1), &compositeVertexBuffer, 2, AttributeDataType::FLOAT, sizeof(Rml::Vertex), 12, false, false);
    compositeObject.setIndex(&compositeIndexBuffer, AttributeDataType::UNSIGNED_INT, 0);
    compositeObject.addTexture(geometryTextureSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
    compositeObject.setBlendState(BlendState::premultipliedAlpha());
    // no culling: the quad can be mirrored by the camera
    if (!compositeObject.endLoad(compositePipelines, false, false, CullingMode::BACK, WindingOrder::CCW)){
        Log::error("[HTML UI] could not create the HTML UI composite pipelines");
        destroyPipelineObjects();
        return false;
    }

    if (!loadGeometryObject(compositeMsaaObject, geometryShader.get(), &compositeVertexBuffer, &compositeIndexBuffer, &placeholderWhite) ||
        !loadGeometryObject(geometryObject, geometryShader.get(), &compositeVertexBuffer, &compositeIndexBuffer, &placeholderWhite) ||
        !loadGeometryObject(gradientObject, gradientShader.get(), &compositeVertexBuffer, &compositeIndexBuffer, &placeholderWhite)){
        Log::error("[HTML UI] could not load the HTML UI geometry programs");
        destroyPipelineObjects();
        return false;
    }

    if (!loadPrimObject()){
        Log::error("[HTML UI] could not load the HTML UI primitive program");
        destroyPipelineObjects();
        return false;
    }

    for (PostProgram& program : postPrograms){
        ShaderData& data = program.shader->shaderData;
        program.vsSlot = data.getUniformBlockIndexByName("u_vs_htmluiPost");
        program.fsSlot = data.getUniformBlockIndexByName("u_fs_htmluiPost");
        program.textureSlot = data.getTextureIndexByName("u_htmluiPostTex");
        program.maskSlot = data.getTextureIndexByName("u_htmluiMaskTex");
        program.chromaUSlot = data.getTextureIndexByName("u_htmluiYuvU");
        program.chromaVSlot = data.getTextureIndexByName("u_htmluiYuvV");
        // vertexless: a triangle from gl_VertexIndex, no vertex or index buffer
        program.object.beginLoad(PrimitiveType::TRIANGLES);
        program.object.setShader(program.shader.get());
        program.object.addTexture(program.textureSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
        if (program.maskSlot.first >= 0)
            program.object.addTexture(program.maskSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
        if (program.chromaUSlot.first >= 0)
            program.object.addTexture(program.chromaUSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
        if (program.chromaVSlot.first >= 0)
            program.object.addTexture(program.chromaVSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
        if (!program.object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)){
            Log::error("[HTML UI] could not load the HTML UI post-process programs");
            destroyPipelineObjects();
            return false;
        }
    }

    pipelinesLoaded = true;
    return true;
}

bool RmlRenderer::isReady() const{
    return pipelinesLoaded;
}

void RmlRenderer::abortFrame(){
    endPass();
    frameState = FrameState::NONE;
    target = nullptr;
    // the recorded frame is dropped, what it held released
    recording = false;
    finishRecording();
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: command recording (phase-1 spec 3.2.10)
//-------------------------------------------------------------------------------------------

namespace {
    std::atomic<bool> commandRecording{true};
}

void RmlRenderer::setCommandRecordingForTesting(bool enabled){
    commandRecording = enabled;
}

bool RmlRenderer::isCommandRecording(){
    return commandRecording;
}

size_t RmlRenderer::getLastFrameCommandCount() const{
    return lastFrameCommandCount;
}

RmlRenderer::Command& RmlRenderer::recordCommand(CommandType type){
    commands.emplace_back();
    Command& command = commands.back();
    command.type = type;
    command.flag = 0;
    command.i[0] = command.i[1] = command.i[2] = command.i[3] = 0;
    command.x = command.y = 0.0f;
    command.a = command.b = 0;
    return command;
}

void RmlRenderer::replayCommands(){
    // the state as beginFrame left it
    transform = projection;
    scissorEnabled = false;
    scissorRegion = frameStartScissor;
    scissorTarget = toTargetRect(scissorRegion, contextSize, targetSize);
    clipMaskEnabled = false;
    stencilRef = 0;
    layersSize = 1;
    activeClipScope = -1;

    // glyphs the recorded frame added to atlas pages, before anything draws
    updateAtlasTextures();
    if (batching){
        replayBatched();
        return;
    }
    // unbatched with SDF primitives: rounded clips the shaders can do are analytic (S16 R6)
    if (primitivesFrame){
        planClipScopes();
        replayScoped();
        return;
    }
    for (const Command& c : commands)
        replayCommand(c);
}

void RmlRenderer::replayCommand(const Command& c){
    switch (c.type){
    case CommandType::GEOMETRY:
        drawRecordedGeometry(c.a, Rml::Vector2f(c.x, c.y), reinterpret_cast<HtmlUITexture*>(c.b));
        break;
    case CommandType::CLIP_MASK:
        renderToClipMaskNow((Rml::ClipMaskOperation)c.flag, c.a, Rml::Vector2f(c.x, c.y));
        break;
    case CommandType::SCISSOR_ENABLE:
        scissorEnabled = c.flag != 0;
        break;
    case CommandType::SCISSOR_REGION:
        scissorTarget = Rml::Rectanglei::FromCorners(Rml::Vector2i(c.i[0], c.i[1]), Rml::Vector2i(c.i[2], c.i[3]));
        scissorRegion = commandRects[c.a];
        break;
    case CommandType::CLIP_ENABLE:
        clipMaskEnabled = c.flag != 0;
        break;
    case CommandType::TRANSFORM:
        transform = c.flag ? (projection * commandMatrices[(size_t)c.i[0]]) : projection;
        break;
    case CommandType::PUSH_LAYER: {
        const int index = (int)pushLayerNow();
        if (index != c.i[0])
            Log::error("[HTML UI] the replay pushed layer %d where the recording returned %d", index, c.i[0]);
        break;
    }
    case CommandType::POP_LAYER:
        popLayerNow();
        break;
    case CommandType::COMPOSITE:
        compositeLayersNow((Rml::LayerHandle)c.i[0], (Rml::LayerHandle)c.i[1], (Rml::BlendMode)c.flag,
                           Rml::Span<const Rml::CompiledFilterHandle>(commandFilters.data() + c.i[2], (size_t)c.i[3]));
        break;
    case CommandType::SAVE_TEXTURE: {
        // the texture exists since the recording; it stays unfilled when the layer cannot be made
        HtmlUITexture* texture = reinterpret_cast<HtmlUITexture*>(c.a);
        if (Framebuffer* layer = readableTopLayer())
            copyLayerInto(*layer, *texture, currentScissorTarget(), (c.flag & SAVE_TILED) != 0, Rml::Vector2i(c.i[0], c.i[1]),
                          (c.flag & SAVE_LOAD) ? LoadAction::LOAD : LoadAction::CLEAR);
        break;
    }
    case CommandType::SAVE_MASK: {
        Framebuffer* layer = readableTopLayer();
        Framebuffer* mask = postTarget(RmlRenderTarget::BLEND_MASK);
        if (layer && mask)
            copyLayerIntoMask(*layer, *mask);
        break;
    }
    case CommandType::SHADER:
        renderShaderNow(c.a, c.b, Rml::Vector2f(c.x, c.y));
        break;
    case CommandType::PRIMITIVES:
        drawPrimitivesNow(commandPrimitives.data() + c.i[0], (size_t)c.i[1], Rml::Vector2f(c.x, c.y));
        break;
    }
}

void RmlRenderer::finishRecording(){
    commands.clear();
    commandMatrices.clear();
    commandFilters.clear();
    commandRects.clear();
    commandClipShapes.clear();
    commandPrimitives.clear();
    activeClipScope = -1;
    // releases deferred while recording, in their order
    for (size_t i = 0; i < deferredReleases.size(); i++){
        const DeferredRelease release = deferredReleases[i];
        switch (release.kind){
        case ReleaseKind::GEOMETRY:
            releaseGeometryNow(release.handle);
            break;
        case ReleaseKind::SHADER:
            releaseShaderNow(release.handle);
            break;
        case ReleaseKind::FILTER:
            releaseFilterNow(release.handle);
            break;
        }
    }
    deferredReleases.clear();
    // the kept meshes' geometry is released: their buffers wait for the next frame's
    keptMeshesUsed = 0;
}

bool RmlRenderer::beginFrame(RmlRenderTarget& renderTarget, Rml::Vector2i size){
    if (frameState != FrameState::NONE){
        Log::error("[HTML UI] beginFrame inside a frame: the previous frame is dropped");
        abortFrame();
    }

    contextSize = Rml::Vector2i(std::max(size.x, 1), std::max(size.y, 1));
    framed = true;
    frameNumber = SystemRender::getFrameCount();
    // the effects' per-frame state (phase 5; once per sokol frame) and what their draws read once
    htmlui::EffectRuntime::get().beginFrame();
    effects->beginFrame();

    // per-frame statistics; the geometry counts are current
    const uint32_t geometryLive = stats.geometryLive;
    const uint32_t geometryBytes = stats.geometryBytes;
    const uint32_t savedLayers = stats.boxShadowTextures;
    stats = HtmlUIRenderStats();
    stats.geometryLive = geometryLive;
    stats.geometryBytes = geometryBytes;
    stats.boxShadowTextures = savedLayers;
    lastFrameDrawCount = 0;

    // shaders still compiling, or no target: the frame is dropped
    frameState = FrameState::DROPPED;
    if (!isReady() && !loadPipelines())
        return false;
    if (!renderTarget.isCreated())
        return false;

    target = &renderTarget;
    target->newFrame();
    targetSize = Rml::Vector2i(renderTarget.getWidth(), renderTarget.getHeight());
    // the tracked regions hold for these framebuffers only
    activeRegions = nullptr;
    deferred = DeferredComposite();
    if (regionsGeneration != target->getGeneration()){
        regions.clear();
        regionsCache[0] = regionsCache[1] = RegionsCacheEntry();
        regionsGeneration = target->getGeneration();
    }

    // document pixels to clip space; the viewport maps them onto the target's pixels, which
    // is the resolution scaling. GL renders offscreen Y-flipped, so the target's row 0 is
    // the top of the document on every backend (RenderSystem::isRenderingFlipped)
    projection = Rml::Matrix4f::ProjectOrtho(0.0f, (float)contextSize.x, (float)contextSize.y, 0.0f, -10000.0f, 10000.0f);
    if (Engine::isOpenGL()){
        projection = Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) * projection;
    }
    transform = projection;

    scissorEnabled = false;
    // the region kept from the last frame, at this frame's sizes (EnableScissorRegion may come alone)
    scissorTarget = toTargetRect(scissorRegion, contextSize, targetSize);
    clipMaskEnabled = false;
    stencilRef = 0;
    layersSize = 1;
    layerClearPending.assign(std::max<size_t>(1, target->layers.size()), false);
    layerClearPending[0] = true;
    activeKind = TargetKind::NONE;
    activeIndex = -1;
    activeFramebuffer = nullptr;

    frameMissesStart = SystemRender::getPipelineCacheMisses();
    frameAppliesStart = SystemRender::getPipelineApplies();
    frameState = FrameState::ACTIVE;

    // RmlUi's calls are recorded until endFrame replays them
    frameStartScissor = scissorRegion;
    recording = commandRecording;
    batching = recording && isBatching();
    primitivesFrame = isSdfPrimitives();
    // drawing at once: what the atlas pages hold now (pages are uploaded again when drawn)
    if (!recording)
        updateAtlasTextures();
    activeClipScope = -1;
    if (recording){
        recordLayers = 1;
        commands.clear();
        commandMatrices.clear();
        commandFilters.clear();
        commandRects.clear();
        commandClipShapes.clear();
        commandPrimitives.clear();
    }
    return true;
}

bool RmlRenderer::hasFrame(uint64_t frame) const{
    return framed && frameNumber == frame;
}

bool RmlRenderer::endFrame(){
    if (frameState == FrameState::NONE){
        Log::error("[HTML UI] endFrame without beginFrame");
        return false;
    }
    if (frameState == FrameState::DROPPED){
        frameState = FrameState::NONE;
        flushPendingReleases();
        return false;
    }

    // the recorded frame, drawn in one go
    lastFrameCommandCount = 0;
    if (recording){
        recording = false;
        lastFrameCommandCount = commands.size();
        replayCommands();
    }

    if (layersSize != 1)
        Log::error("[HTML UI] a frame ended with %d layers pushed", layersSize - 1);

    // layer 0 is cleared even when nothing was drawn, and resolved
    layersSize = std::max(layersSize, 1);
    ensureLayerCleared(0);
    if (Framebuffer* layer = target->getLayer(0))
        ensureResolved(*layer);
    endPass();

    stats.pipelineCacheMisses = (uint32_t)(SystemRender::getPipelineCacheMisses() - frameMissesStart);
    stats.pipelineApplies = (uint32_t)(SystemRender::getPipelineApplies() - frameAppliesStart);
    lastFrameDrawCount = stats.drawCalls;
    // the glyph atlas is shared by every context: its size and the distance glyphs it still makes
    const RmlFontEngine& fonts = HtmlUI::getFontEngine();
    stats.atlasBytes = (uint32_t)fonts.getAtlasBytes();
    stats.msdfGlyphsPending = (uint32_t)fonts.getPendingDistanceGlyphCount();

    frameState = FrameState::NONE;
    target = nullptr;
    // what the recording released goes now, before the frame's geometry is persisted (released
    // geometry is not, as when drawing at once)
    finishRecording();
    persistGeometry();
    flushPendingReleases();
    return true;
}

bool RmlRenderer::drawComposite(TextureRender& source, PipelineType compositePipeline, bool flipY){
    const Rml::Matrix4f clipFromQuad = flipY ? Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) : Rml::Matrix4f::Identity();
    return drawComposite(source, compositePipeline, clipFromQuad);
}

bool RmlRenderer::drawComposite(TextureRender& source, PipelineType compositePipeline, const Rml::Matrix4f& clipFromQuad,
                                int msaaSamples){
    if (!isReady() || !source.isCreated())
        return false;

    ObjectRender* object = &compositeObject;
    if (compositePipeline == PIP_MSAA_NODEPTH || compositePipeline == PIP_MSAA){
        // the fixed composite's state (premultiplied, no depth, no culling) at the target's
        // sample count
        PipelineState state;
        state.target = targetFormatFor(PIP_MSAA_NODEPTH, msaaSamples);
        state.blend = BlendMode::PREMULTIPLIED;
        state.depthCompare = CompareFunc::ALWAYS;
        state.depthWrite = false;
        object = &compositeMsaaObject;
        if (!object->beginDraw(state))
            return false;
    }else if (!compositeObject.beginDraw(compositePipeline)){
        return false;
    }

    object->addTexture(geometryTextureSlot, ShaderStageType::FRAGMENT, &source);
    HtmlUIParams params = makeParams(clipFromQuad, Rml::Vector2f(0.0f, 0.0f));
    object->applyUniformBlock(geometryParamsSlot, sizeof(HtmlUIParams), &params);
    object->applyUniformBlock(geometryClipSlot, sizeof(ClipParams), &noClip);
    object->draw(0, 6, 1);

    return true;
}

bool RmlRenderer::drawUnpremultiplied(RmlRenderTarget& source, FramebufferRender& dst, int width, int height){
    if (frameState != FrameState::NONE || !isReady() || !source.isCreated() || !dst.isCreated() || width <= 0 || height <= 0)
        return false;

    CameraRender copyPass;
    copyPass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    copyPass.setLabel("html ui texture");
    copyPass.startRenderPass(&dst);
    copyPass.applyViewportTopLeft(0, 0, width, height);
    copyPass.applyScissorTopLeft(0, 0, width, height);

    PostProgram& program = postPrograms[(int)HtmlUIPostVariant::UNPREMULTIPLY];
    program.object.addTexture(program.textureSlot, ShaderStageType::FRAGMENT, &source.getComposited()->getColorTexture());
    PipelineState state = postTargetState(BlendMode::NONE);
    bool drawn = false;
    if (program.object.beginDraw(state)){
        PostVsParams vs = {{1.0f, 1.0f, 0.0f, 0.0f}};
        program.object.applyUniformBlock(program.vsSlot, sizeof(PostVsParams), &vs);
        PostParams fs;
        program.object.applyUniformBlock(program.fsSlot, sizeof(PostParams), &fs);
        program.object.draw(0, 3, 1);
        drawn = true;
    }
    copyPass.endRenderPass();
    return drawn;
}

bool RmlRenderer::takeTexturesWithoutView(){
    const bool taken = texturesWithoutView;
    texturesWithoutView = false;
    return taken;
}

void RmlRenderer::releaseGpuResources(){
    // a frame in progress is dropped with it
    if (frameState != FrameState::NONE)
        abortFrame();
    flushPendingReleases();
    framed = false;

    destroyPipelineObjects();
    destroyGeometryBuffers();
    destroyGatherPages();

    // the handles stay valid for RmlUi, which releases them (Rml::ReleaseTextures)
    for (HtmlUITexture* texture : liveTextures){
        if (texture->dead)
            continue;
        if (texture->ownedRender)
            texture->ownedRender->destroyTexture();
        if (texture->ownedTarget)
            texture->ownedTarget->destroy();
        for (std::unique_ptr<TextureRender>& plane : texture->planes){
            if (plane)
                plane->destroyTexture();
        }
        texture->dead = true;
    }
    videoConversions.clear();
}

void RmlRenderer::destroyPipelineObjects(){
    // the effects' programs use the composite buffers as their layout template
    if (effects)
        effects->releaseGpuResources();
    geometryObject.destroy();
    gradientObject.destroy();
    compositeObject.destroy();
    compositeMsaaObject.destroy();
    primObject.destroy();
    for (PostProgram& program : postPrograms){
        program.object.destroy();
    }
    compositeVertexBuffer.destroyBuffer();
    compositeIndexBuffer.destroyBuffer();
    primQuadBuffer.destroyBuffer();
    primQuadIndexBuffer.destroyBuffer();
    primInstanceTemplate.destroyBuffer();
    placeholderWhite.destroyTexture();

    // the pool destroys a shader (and its cached pipelines) once no other renderer holds it
    if (geometryShader){
        geometryShader.reset();
        ShaderPool::remove(ShaderType::HTMLUI, ShaderPool::getHtmlUIProperties(true));
    }
    if (gradientShader){
        gradientShader.reset();
        ShaderPool::remove(ShaderType::HTMLUI_GRADIENT, 0);
    }
    if (primShader){
        primShader.reset();
        ShaderPool::remove(ShaderType::HTMLUI_PRIM, 0);
    }
    primParamsSlot = -1;
    primClipSlot = -1;
    primTextureSlot = {-1, -1};
    for (int i = 0; i < HTMLUI_POST_VARIANT_COUNT; i++){
        PostProgram& program = postPrograms[i];
        if (program.shader){
            program.shader.reset();
            ShaderPool::remove(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties((HtmlUIPostVariant)i));
        }
        program.vsSlot = -1;
        program.fsSlot = -1;
        program.textureSlot = {-1, -1};
        program.maskSlot = {-1, -1};
        program.chromaUSlot = {-1, -1};
        program.chromaVSlot = {-1, -1};
    }
    geometryParamsSlot = -1;
    geometryClipSlot = -1;
    geometryTextureSlot = {-1, -1};
    gradientParamsSlot = -1;
    gradientFsSlot = -1;
    pipelinesLoaded = false;
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: pure functions and statistics
//-------------------------------------------------------------------------------------------

Rect RmlRenderer::toTargetScissor(Rml::Rectanglei region, Rml::Vector2i contextSize, Rml::Vector2i targetSize, bool openGLFlipped){
    const float sx = (contextSize.x > 0) ? (float)targetSize.x / (float)contextSize.x : 1.0f;
    const float sy = (contextSize.y > 0) ? (float)targetSize.y / (float)contextSize.y : 1.0f;

    auto clampTo = [](float value, int limit){
        return std::min(std::max(value, 0.0f), (float)std::max(limit, 0));
    };

    const float x0 = clampTo(std::floor(region.Left() * sx), targetSize.x);
    const float x1 = std::max(x0, clampTo(std::ceil(region.Right() * sx), targetSize.x));
    const float y0 = clampTo(std::floor(region.Top() * sy), targetSize.y);
    const float y1 = std::max(y0, clampTo(std::ceil(region.Bottom() * sy), targetSize.y));

    // CameraRender::applyScissor counts y from the bottom; GL's flipped offscreen rendering
    // puts the document top at y = 0 there
    const float y = openGLFlipped ? y0 : (float)targetSize.y - y1;
    return Rect(x0, y, x1 - x0, y1 - y0);
}

Rml::Rectanglei RmlRenderer::toTargetRect(Rml::Rectanglei region, Rml::Vector2i contextSize, Rml::Vector2i targetSize){
    if (!region.Valid())
        return Rml::Rectanglei::FromSize(Rml::Vector2i(0, 0));
    const Rect rect = toTargetScissor(region, contextSize, targetSize, true);
    const Rml::Vector2i p0((int)rect.getX(), (int)rect.getY());
    return Rml::Rectanglei::FromPositionSize(p0, Rml::Vector2i((int)rect.getWidth(), (int)rect.getHeight()));
}

void RmlRenderer::sigmaToParameters(float desiredSigma, int& passLevel, float& sigma){
    constexpr int maxNumPasses = 10;
    constexpr float maxSinglePassSigma = 3.0f;
    passLevel = Rml::Math::Clamp(Rml::Math::Log2(int(desiredSigma * (2.0f / maxSinglePassSigma))), 0, maxNumPasses);
    sigma = Rml::Math::Clamp(desiredSigma / float(1 << passLevel), 0.0f, maxSinglePassSigma);
}

void RmlRenderer::blurWeights(float sigma, float weights[4]){
    float normalization = 0.0f;
    for (int i = 0; i < 4; i++){
        if (std::fabs(sigma) < 0.1f)
            weights[i] = float(i == 0);
        else
            weights[i] = std::exp(-float(i * i) / (2.0f * sigma * sigma)) / (std::sqrt(2.0f * Rml::Math::RMLUI_PI) * sigma);
        normalization += (i == 0 ? 1.0f : 2.0f) * weights[i];
    }
    for (int i = 0; i < 4; i++){
        weights[i] /= normalization;
    }
}

const HtmlUIRenderStats& RmlRenderer::getStats() const{
    return stats;
}

size_t RmlRenderer::getLiveGeometryCount() const{
    return liveGeometry.size();
}

size_t RmlRenderer::getLiveTextureCount() const{
    return liveTextures.size();
}

size_t RmlRenderer::getLastFrameDrawCount() const{
    return lastFrameDrawCount;
}

uint64_t RmlRenderer::getGpuCreateCount() const{
    return gpuCreateCount;
}

//-------------------------------------------------------------------------------------------
// RmlRenderer: contexts and <video> frames (phase 6)
//-------------------------------------------------------------------------------------------

namespace {

    std::unordered_map<const Rml::Context*, RmlRenderer*>& contextRenderers(){
        static std::unordered_map<const Rml::Context*, RmlRenderer*> value;
        return value;
    }

    std::unordered_map<const Rml::RenderManager*, RmlRenderer*>& managerRenderers(){
        static std::unordered_map<const Rml::RenderManager*, RmlRenderer*> value;
        return value;
    }

}

RmlRenderer* RmlRenderer::forRenderManager(const Rml::RenderManager* renderManager){
    auto it = managerRenderers().find(renderManager);
    return it == managerRenderers().end() ? nullptr : it->second;
}

RmlRenderer* RmlRenderer::forContext(const Rml::Context* context){
    auto it = contextRenderers().find(context);
    return it == contextRenderers().end() ? nullptr : it->second;
}

void RmlRenderer::bindContext(const Rml::Context* context, RmlRenderer* renderer){
    if (context && renderer){
        contextRenderers()[context] = renderer;
        managerRenderers()[&const_cast<Rml::Context*>(context)->GetRenderManager()] = renderer;
    }
}

void RmlRenderer::unbindContext(const Rml::Context* context){
    auto it = contextRenderers().find(context);
    if (it == contextRenderers().end())
        return;
    for (auto manager = managerRenderers().begin(); manager != managerRenderers().end();){
        if (manager->second == it->second)
            manager = managerRenderers().erase(manager);
        else
            ++manager;
    }
    contextRenderers().erase(it);
}

void RmlRenderer::queueVideoConversion(const VideoConversion& conversion){
    if (!conversion.target)
        return;
    for (VideoConversion& queued : videoConversions){
        if (queued.target == conversion.target){
            queued = conversion;
            return;
        }
    }
    videoConversions.push_back(conversion);
}

void RmlRenderer::cancelVideoConversion(const HtmlUITexture* target){
    videoConversions.erase(std::remove_if(videoConversions.begin(), videoConversions.end(),
                                          [target](const VideoConversion& queued){ return queued.target == target; }),
                           videoConversions.end());
}

bool RmlRenderer::hasVideoConversion(const HtmlUITexture* target) const{
    for (const VideoConversion& queued : videoConversions){
        if (queued.target == target)
            return true;
    }
    return false;
}

int RmlRenderer::runVideoConversions(){
    // outside any pass: before beginFrame, or in a frame before its first pass opens
    if (videoConversions.empty() || activeKind != TargetKind::NONE || frameState == FrameState::DROPPED || !isReady())
        return 0;
    PostProgram& program = postPrograms[(int)HtmlUIPostVariant::YUV];
    int drawn = 0;
    std::vector<VideoConversion> jobs;
    jobs.swap(videoConversions);
    for (const VideoConversion& job : jobs){
        HtmlUITexture* target = job.target;
        if (!target || target->dead || !target->ownedTarget || !target->ownedTarget->isCreated() ||
            !target->planes[0] || !target->planes[1] || !target->planes[2])
            continue;

        CameraRender pass;
        pass.setColorActions(LoadAction::DONTCARE, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        pass.setLabel("html ui video");
        pass.startRenderPass(&target->ownedTarget->getRender());
        pass.applyViewportTopLeft(0, 0, target->width, target->height);
        pass.applyScissorTopLeft(0, 0, target->width, target->height);
        program.object.addTexture(program.textureSlot, ShaderStageType::FRAGMENT, target->planes[0].get());
        program.object.addTexture(program.chromaUSlot, ShaderStageType::FRAGMENT, target->planes[1].get());
        program.object.addTexture(program.chromaVSlot, ShaderStageType::FRAGMENT, target->planes[2].get());
        if (program.object.beginDraw(postTargetState(BlendMode::NONE))){
            PostVsParams vs = {{1.0f, 1.0f, 0.0f, 0.0f}};
            program.object.applyUniformBlock(program.vsSlot, sizeof(PostVsParams), &vs);
            PostParams fs;
            fs.params[1] = job.chromaScale[0];
            fs.params[2] = job.chromaScale[1];
            std::memcpy(fs.colorMatrix, job.colorMatrix, sizeof(fs.colorMatrix));
            program.object.applyUniformBlock(program.fsSlot, sizeof(PostParams), &fs);
            program.object.draw(0, 3, 1);
            drawn++;
        }
        pass.endRenderPass();
    }
    // back to the placeholders: nothing keeps a plane bound
    program.object.addTexture(program.textureSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
    program.object.addTexture(program.chromaUSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
    program.object.addTexture(program.chromaVSlot, ShaderStageType::FRAGMENT, &placeholderWhite);
    videoConversionCount += (uint64_t)drawn;
    return drawn;
}

uint64_t RmlRenderer::getVideoConversionCount() const{
    return videoConversionCount;
}

size_t RmlRenderer::getGeometryBufferCount() const{
    return pages.size();
}

TextureRender* RmlRenderer::getTextureRender(Rml::TextureHandle handle) const{
    HtmlUITexture* texture = reinterpret_cast<HtmlUITexture*>(handle);
    if (!texture || liveTextures.count(texture) == 0 || texture->dead)
        return nullptr;
    return texture->render;
}

void RmlRenderer::getGpuHandles(std::vector<uint32_t>& buffers, std::vector<uint32_t>& images) const{
    buffers.clear();
    images.clear();

    auto addBuffer = [&buffers](const BufferRender& buffer){
        uint32_t id = const_cast<BufferRender&>(buffer).backend.get().id;
        if (id != 0) buffers.push_back(id);
    };
    addBuffer(compositeVertexBuffer);
    addBuffer(compositeIndexBuffer);
    for (const std::unique_ptr<RmlGeometryPage>& page : pages){
        addBuffer(page->buffer);
    }
    for (const std::unique_ptr<RmlTransientBuffer>& buffer : transientBuffers){
        addBuffer(buffer->buffer);
    }

    uint32_t placeholder = const_cast<TextureRender&>(placeholderWhite).backend.get().id;
    if (placeholder != 0) images.push_back(placeholder);
    for (HtmlUITexture* texture : liveTextures){
        if (texture->render){
            uint32_t id = texture->render->backend.get().id;
            if (id != 0) images.push_back(id);
        }
    }
}
