// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S92 frame reuse (docs/specs/S92-htmlui-render-cost.md R1-R5): the signature of a recorded frame,
// the sources it cannot version (R2), the tiles of layer 0 a frame wrote (R4), the depth-stencil
// plan of a replay (R5) and the verification of kept frames (R3). Whether a frame changed is
// decided from what RmlUi recorded, never from a dirty flag (D1): a frame whose signature equals
// the last drawn one's is not replayed, its target keeps the image.

#include "RmlRenderer.h"
#include "RmlRendererInternal.h"

#include "HtmlUI.h"
#include "HtmlUITexture.h"
#include "RmlFontEngine.h"

#include "Engine.h"
#include "Log.h"
#include "effects/EffectRenderer.h"
#include "effects/EffectRuntime.h"
#include "effects/EffectTypes.h"
#include "render/FrameReadback.h"
#include "render/ObjectRender.h"
#include "render/SystemRender.h"
#include "texture/Framebuffer.h"
#include "util/GlyphAtlas.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;

namespace {

    uint64_t frameMix(uint64_t h, uint64_t v){
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h * 0xff51afd7ed558ccdull;
    }

    uint64_t frameFloat(float f){
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }

    uint64_t frameFloats(uint64_t h, const float* values, size_t count){
        for (size_t i = 0; i + 1 < count; i += 2)
            h = frameMix(h, frameFloat(values[i]) | (frameFloat(values[i + 1]) << 32));
        if (count & 1)
            h = frameMix(h, frameFloat(values[count - 1]));
        return frameMix(h, count);
    }

    uint64_t frameBytes(uint64_t h, const void* data, size_t size){
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        size_t i = 0;
        for (; i + 8 <= size; i += 8){
            uint64_t word;
            std::memcpy(&word, bytes + i, 8);
            h = frameMix(h, word);
        }
        uint64_t tail = 0;
        if (size > i)
            std::memcpy(&tail, bytes + i, size - i);
        return frameMix(h, tail ^ (uint64_t)size);
    }

    // a translation relative to the origin, exactly: the difference of two floats is exact in a double
    uint64_t frameOffset(float x, float y, double dx, double dy){
        return frameFloat((float)((double)x - dx)) | (frameFloat((float)((double)y - dy)) << 32);
    }

    uint64_t geometrySerial(uintptr_t handle){
        const RmlGeometry* geometry = reinterpret_cast<const RmlGeometry*>(handle);
        return geometry ? geometry->generation : 0;
    }

    std::atomic<bool> frameReuseEnabled{true};
    std::atomic<uint8_t> depthStencilDiscard{(uint8_t)RmlRenderer::DepthStencilDiscard::PLANNED};
    // -1: JASPER_HTMLUI_VERIFY_KEPT decides (read once)
    std::atomic<int> keptVerification{-1};

}

void RmlRenderer::setFrameReuseForTesting(bool enabled){
    frameReuseEnabled = enabled;
}

bool RmlRenderer::isFrameReuse(){
    return frameReuseEnabled;
}

void RmlRenderer::setDepthStencilDiscardForTesting(DepthStencilDiscard mode){
    depthStencilDiscard = (uint8_t)mode;
}

void RmlRenderer::setKeptFrameVerificationForTesting(bool enabled){
    keptVerification = enabled ? 1 : 0;
}

bool RmlRenderer::isKeptFrameVerification(){
    int value = keptVerification;
    if (value < 0){
        const char* env = std::getenv("JASPER_HTMLUI_VERIFY_KEPT");
        value = (env && std::strcmp(env, "1") == 0) ? 1 : 0;
        keptVerification = value;
    }
    return value == 1;
}

void RmlRenderer::addLiveSource(uint32_t reasons){
    if (recording)
        frameLiveSources |= reasons;
    else
        pendingLiveSources |= reasons;
}

uint64_t RmlRenderer::textureSignature(const HtmlUITexture* texture, uint32_t& live) const{
    if (!texture)
        return 0;
    if (texture->liveContent)
        live |= LIVE_VIDEO;
    // an image the UI neither uploads nor draws into, not a glyph page's (whose generation it reads)
    if (texture->kind == HtmlUITexture::Kind::EXTERNAL && !texture->atlasGeneration)
        live |= LIVE_RENDER_TARGET;
    uint64_t h = frameMix(texture->serial, texture->contentVersion);
    // a glyph page's texels (the font engine's generation, uploaded before the replay draws)
    if (texture->atlasGeneration)
        h = frameMix(h, *texture->atlasGeneration);
    return frameMix(h, texture->dead ? 1u : 0u);
}

uint64_t RmlRenderer::filterSignature(Rml::CompiledFilterHandle handle, uint32_t& live) const{
    if (!handle)
        return 0;
    if (htmlui::EffectRenderer::isEffectHandle(handle))
        return shaderSignature(handle, live);
    const RmlCompiledFilter& filter = *reinterpret_cast<const RmlCompiledFilter*>(handle);
    uint64_t h = frameMix(0xf11e7ull, (uint64_t)filter.type);
    const float values[6] = {filter.blendFactor, filter.sigma, filter.offset.x, filter.offset.y, 0.0f, 0.0f};
    h = frameFloats(h, values, 4);
    h = frameMix(h, (uint64_t)filter.color.red | ((uint64_t)filter.color.green << 8) | ((uint64_t)filter.color.blue << 16) |
                    ((uint64_t)filter.color.alpha << 24));
    return frameFloats(h, filter.colorMatrix.data(), 16);
}

uint64_t RmlRenderer::shaderSignature(Rml::CompiledShaderHandle handle, uint32_t& live) const{
    if (!handle)
        return 0;
    if (htmlui::EffectRenderer::isEffectHandle(handle)){
        // what the effect's draw reads, as prepare left it at record time: its program, packed
        // parameters, frame block (area, pointer, clocks) and pointer history. A running clock or
        // a simulation changes it between frames without a change of these: live
        const htmlui::EffectInstance* instance = htmlui::EffectRenderer::instanceOf(handle);
        if (!htmlui::EffectRuntime::get().isLive(instance))
            return 1;
        if (!htmlui::EffectClock::isFixed() || (instance->manifest && instance->manifest->stateful()))
            live |= LIVE_EFFECT;
        uint64_t h = frameMix(0xeffec7ull, (uint64_t)(uintptr_t)instance->manifest);
        h = frameMix(h, (uint64_t)(uintptr_t)instance->shader.get());
        h = frameMix(h, ((uint64_t)instance->drawReady << 1) | (uint64_t)instance->filterForm);
        h = frameMix(h, (uint64_t)instance->blend);
        const float values[4] = {instance->opacity, instance->timeScale, instance->timeOffset, instance->seed};
        h = frameFloats(h, values, 4);
        h = frameBytes(h, instance->params.data(), instance->params.size());
        h = frameBytes(h, &instance->frame, sizeof(instance->frame));
        h = frameFloats(h, &instance->trail[0][0], (size_t)instance->trailUsed * 4);
        h = frameFloats(h, &instance->ripples[0][0], (size_t)instance->ripplesUsed * 4);
        return h;
    }
    const RmlCompiledShader& shader = *reinterpret_cast<const RmlCompiledShader*>(handle);
    uint64_t h = frameMix(0x5adeull, (uint64_t)shader.function);
    const float values[4] = {shader.p.x, shader.p.y, shader.v.x, shader.v.y};
    h = frameFloats(h, values, 4);
    h = frameFloats(h, shader.stopPositions.data(), shader.stopPositions.size());
    for (const Rml::Colourf& colour : shader.stopColors){
        const float rgba[4] = {colour.red, colour.green, colour.blue, colour.alpha};
        h = frameFloats(h, rgba, 4);
    }
    return h;
}

uint64_t RmlRenderer::signatureOf(size_t first, size_t end, Rml::Vector2i origin, uint32_t& live){
    end = std::min(end, commands.size());
    first = std::min(first, end);

    // the state in effect at first: the last transform, scissor and clip commands before it
    int transformAt = -1, scissorEnableAt = -1, scissorRegionAt = -1, clipEnableAt = -1;
    for (size_t i = first; i-- > 0 && (transformAt < 0 || scissorEnableAt < 0 || scissorRegionAt < 0 || clipEnableAt < 0);){
        switch (commands[i].type){
        case CommandType::TRANSFORM:
            if (transformAt < 0)
                transformAt = (int)i;
            break;
        case CommandType::SCISSOR_ENABLE:
            if (scissorEnableAt < 0)
                scissorEnableAt = (int)i;
            break;
        case CommandType::SCISSOR_REGION:
            if (scissorRegionAt < 0)
                scissorRegionAt = (int)i;
            break;
        case CommandType::CLIP_ENABLE:
            if (clipEnableAt < 0)
                clipEnableAt = (int)i;
            break;
        default:
            break;
        }
    }
    const bool transformedAtStart = transformAt >= 0 && commands[(size_t)transformAt].flag != 0;
    const bool clipAtStart = clipEnableAt >= 0 && commands[(size_t)clipEnableAt].flag != 0;

    // relative positions only where nothing but the projection maps them
    bool relative = origin.x != 0 || origin.y != 0;
    if (relative && transformedAtStart)
        relative = false;
    for (size_t i = first; relative && i < end; i++){
        if (commands[i].type == CommandType::TRANSFORM && commands[i].flag)
            relative = false;
    }
    const Rml::Vector2f pixel = getPixelScale();
    const double dx = relative ? (double)origin.x / (double)pixel.x : 0.0;
    const double dy = relative ? (double)origin.y / (double)pixel.y : 0.0;
    const int px = relative ? origin.x : 0;
    const int py = relative ? origin.y : 0;

    auto matrixHash = [this](uint64_t h, const Command& c){
        h = frameMix(h, c.flag);
        if (c.flag)
            h = frameFloats(h, commandMatrices[(size_t)c.i[0]].data(), 16);
        return h;
    };
    auto scissorHash = [&](uint64_t h, int x0, int y0, int x1, int y1){
        h = frameMix(h, (uint64_t)(uint32_t)(x0 - px) | ((uint64_t)(uint32_t)(y0 - py) << 32));
        return frameMix(h, (uint64_t)(uint32_t)(x1 - px) | ((uint64_t)(uint32_t)(y1 - py) << 32));
    };
    auto maskHash = [&](uint64_t h, const Command& c, double ox, double oy){
        h = frameMix(h, ((uint64_t)c.flag << 8) | 0x3u);
        h = frameMix(h, geometrySerial(c.a));
        h = frameMix(h, frameOffset(c.x, c.y, ox, oy));
        if (c.i[0] > 0 && (size_t)c.i[0] <= commandClipShapes.size()){
            const ClipShapeRecord& shape = commandClipShapes[(size_t)c.i[0] - 1];
            const float values[12] = {shape.rect.Left(), shape.rect.Top(), shape.rect.Right(), shape.rect.Bottom(),
                                      shape.radii[0].x, shape.radii[0].y, shape.radii[1].x, shape.radii[1].y,
                                      shape.radii[2].x, shape.radii[2].y, shape.radii[3].x, shape.radii[3].y};
            h = frameFloats(h, values, 12);
            // S92: a convex polygon's points
            if (shape.polygonPoints > 0)
                h = frameFloats(frameMix(h, (uint64_t)shape.polygonPoints), &shape.polygon[0].x, (size_t)shape.polygonPoints * 2);
        }
        return h;
    };

    uint64_t h = frameMix(0x592f4a3eull, end - first);
    h = transformAt >= 0 ? matrixHash(h, commands[(size_t)transformAt]) : frameMix(h, 0);
    const bool scissorAtStart = scissorEnableAt >= 0 && commands[(size_t)scissorEnableAt].flag != 0;
    h = frameMix(h, scissorAtStart ? 1u : 0u);
    // the region in effect at first matters when the scissor is on there, or turned on in the
    // range before a region of its own is set (a layer scope's key: where it lies otherwise not)
    bool startRegionUsed = scissorAtStart;
    for (size_t i = first; !startRegionUsed && i < end; i++){
        if (commands[i].type == CommandType::SCISSOR_REGION)
            break;
        if (commands[i].type == CommandType::SCISSOR_ENABLE && commands[i].flag)
            startRegionUsed = true;
    }
    if (!startRegionUsed){
    }else if (scissorRegionAt >= 0){
        const Command& c = commands[(size_t)scissorRegionAt];
        h = scissorHash(h, c.i[0], c.i[1], c.i[2], c.i[3]);
    }else{
        const Rml::Rectanglei start = toTargetPixels(frameStartScissor, contextSize, targetSize);
        h = start.Valid() ? scissorHash(h, start.Left(), start.Top(), start.Right(), start.Bottom()) : frameMix(h, 0);
    }
    h = frameMix(h, clipAtStart ? 1u : 0u);
    if (clipAtStart){
        // the mask the scope is tested against: its operations since the last Set, with the
        // transforms they were drawn with, where they are (a clip outside the scope)
        size_t set = (size_t)clipEnableAt;
        while (set > 0){
            const Command& c = commands[set - 1];
            set--;
            if (c.type == CommandType::CLIP_MASK && (Rml::ClipMaskOperation)c.flag != Rml::ClipMaskOperation::Intersect)
                break;
        }
        for (size_t i = set; i < first; i++){
            const Command& c = commands[i];
            if (c.type == CommandType::CLIP_MASK)
                h = maskHash(h, c, 0.0, 0.0);
            else if (c.type == CommandType::TRANSFORM)
                h = matrixHash(h, c);
        }
    }

    for (size_t i = first; i < end; i++){
        const Command& c = commands[i];
        h = frameMix(h, (uint64_t)c.type);
        switch (c.type){
        case CommandType::GEOMETRY:
            h = frameMix(h, geometrySerial(c.a));
            h = frameMix(h, frameOffset(c.x, c.y, dx, dy));
            h = frameMix(h, textureSignature(reinterpret_cast<const HtmlUITexture*>(c.b), live));
            break;
        case CommandType::CLIP_MASK:
            h = maskHash(h, c, dx, dy);
            break;
        case CommandType::SCISSOR_ENABLE:
        case CommandType::CLIP_ENABLE:
            h = frameMix(h, c.flag);
            break;
        case CommandType::SCISSOR_REGION:
            h = scissorHash(h, c.i[0], c.i[1], c.i[2], c.i[3]);
            break;
        case CommandType::TRANSFORM:
            h = matrixHash(h, c);
            break;
        case CommandType::PUSH_LAYER:
            h = frameMix(h, ((uint64_t)(uint32_t)c.i[0] << 8) | c.flag);
            if (c.flag){
                const Rml::Rectanglei& frame = commandRects[(size_t)c.a];
                h = scissorHash(h, frame.Left(), frame.Top(), frame.Right(), frame.Bottom());
            }
            break;
        case CommandType::POP_LAYER:
            break;
        case CommandType::COMPOSITE:
            h = frameMix(h, (uint64_t)(uint32_t)c.i[0] | ((uint64_t)(uint32_t)c.i[1] << 32));
            h = frameMix(h, ((uint64_t)(uint32_t)c.i[3] << 8) | c.flag);
            for (int f = 0; f < c.i[3]; f++)
                h = frameMix(h, filterSignature(commandFilters[(size_t)(c.i[2] + f)], live));
            break;
        case CommandType::SAVE_TEXTURE:
            h = frameMix(h, textureSignature(reinterpret_cast<const HtmlUITexture*>(c.a), live));
            h = frameMix(h, ((uint64_t)(uint32_t)c.i[0] << 32 | (uint32_t)c.i[1]) ^ c.flag);
            break;
        case CommandType::SAVE_MASK:
            break;
        case CommandType::SHADER:
            h = frameMix(h, shaderSignature(c.a, live));
            h = frameMix(h, geometrySerial(c.b));
            h = frameMix(h, frameOffset(c.x, c.y, dx, dy));
            break;
        case CommandType::PRIMITIVES:
            h = frameMix(h, (uint64_t)(uint32_t)c.i[1]);
            h = frameMix(h, (uint64_t)(uint32_t)c.i[2] | ((uint64_t)(uint32_t)c.i[3] << 32));
            h = frameMix(h, frameOffset(c.x, c.y, dx, dy));
            break;
        }
    }
    return h;
}

//-------------------------------------------------------------------------------------------
// R1: kept frames
//-------------------------------------------------------------------------------------------

uint64_t RmlRenderer::skippedWork() const{
    return (uint64_t)ObjectRender::getWaitingDrawCount() + SystemRender::getUniformSkippedDraws() + SystemRender::getDescriptorSkippedDraws() +
           SystemRender::getStreamSkippedUploads();
}

bool RmlRenderer::keepRecordedFrame(){
    lastFrameKept = false;
    const auto start = std::chrono::steady_clock::now();
    uint32_t live = frameLiveSources;
    // distance glyphs still to come: the text drawn now is not the final one
    if (HtmlUI::getFontEngine().getPendingDistanceGlyphCount() > 0)
        live |= LIVE_MSDF_PENDING;
    uint64_t signature = signatureOf(0, commands.size(), Rml::Vector2i(0, 0), live);
    // what the replay draws them into, and how (the replay modes give the same image; a switch
    // draws again, so a test that switches sees the mode it asked for)
    signature = frameMix(signature, (uint64_t)(uint32_t)contextSize.x | ((uint64_t)(uint32_t)contextSize.y << 32));
    signature = frameMix(signature, (uint64_t)(uint32_t)targetSize.x | ((uint64_t)(uint32_t)targetSize.y << 32));
    signature = frameMix(signature, ((uint64_t)target->getSampleCount() << 3) | ((uint64_t)isLayerCache() << 2) | ((uint64_t)batching << 1) |
                                        (uint64_t)primitivesFrame);
    frameLiveSources = live;
    frameSignature = signature;
    lastSignatureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    if (!isFrameReuse() || !keptValid || live != 0 || signature != keptSignature || keptTarget != target ||
        keptGeneration != target->getGeneration())
        return false;
    lastFrameKept = true;
    return true;
}

void RmlRenderer::finishKeptFrame(){
    stats.framesKept = 1;
    // the glyph pages the kept image shows are in use (the font engine's LRU eviction)
    for (const Command& c : commands){
        if (c.type != CommandType::GEOMETRY)
            continue;
        const HtmlUITexture* texture = reinterpret_cast<const HtmlUITexture*>(c.b);
        if (texture && texture->drawFrame)
            *texture->drawFrame = frameNumber;
    }
    if (isKeptFrameVerification())
        verifyKeptFrame();

    frames.assign(1, LayerFrame());
    frames[0].rect = standardFrame();
    maskOps.clear();
    // region layers an idle document does not use go as after a drawn frame
    target->trimRegions();
    closeFrameStats();
    frameState = FrameState::NONE;
    target = nullptr;
    finishRecording();
    persistGeometry();
    flushPendingReleases();
}

void RmlRenderer::rememberDrawnFrame(bool recorded){
    if (!recorded){
        // drawn at once: nothing to compare the next frame with, and no plan of its writes
        keptValid = false;
        tilesFull = true;
        tileSource = nullptr;
        return;
    }
    // a draw that waited for its pipeline, or an upload refused, left the image incomplete: the
    // next frame draws again whatever it records
    bool complete = skippedWork() == skippedBefore;
    for (const HtmlUITexture* texture : atlasTextures){
        if (!complete)
            break;
        if (texture->dead || !texture->atlas || !texture->atlasGeneration || !texture->drawFrame || *texture->drawFrame != frameNumber)
            continue;
        const size_t bytes = (size_t)texture->width * (size_t)texture->height * (texture->sample == HtmlUITexture::Sample::COVERAGE ? 1u : 4u);
        if (texture->atlas->getPixels().size() == bytes && *texture->atlasGeneration != texture->uploadedGeneration)
            complete = false;
    }
    keptValid = complete;
    keptSignature = frameSignature;
    keptTarget = target;
    keptGeneration = target->getGeneration();
    computeCompositeTiles();
}

uint32_t RmlRenderer::targetBytesOf(const RmlRenderTarget& renderTarget) const{
    // colour per layer, the one shared D24S8, the resolve images of a multisampled target, the
    // post-process targets and the region layers
    const uint64_t pixels = (uint64_t)renderTarget.getWidth() * (uint64_t)renderTarget.getHeight();
    const uint64_t samples = (uint64_t)std::max(renderTarget.getSampleCount(), 1);
    const uint64_t layers = (uint64_t)renderTarget.getLayerCount();
    uint64_t bytes = 0;
    if (layers > 0){
        bytes += pixels * 4u * samples * layers + pixels * 4u * samples;
        if (samples > 1)
            bytes += pixels * 4u * layers;
    }
    bytes += pixels * 4u * (uint64_t)renderTarget.getPostprocessCount();
    bytes += renderTarget.getRegionBytes();
    return (uint32_t)std::min<uint64_t>(bytes, UINT32_MAX);
}

void RmlRenderer::closeFrameStats(){
    stats.pipelineCacheMisses = (uint32_t)(SystemRender::getPipelineCacheMisses() - frameMissesStart);
    stats.pipelineApplies = (uint32_t)(SystemRender::getPipelineApplies() - frameAppliesStart);
    lastFrameDrawCount = stats.drawCalls;
    // the glyph atlas is shared by every context: its size and the distance glyphs it still makes
    const RmlFontEngine& fonts = HtmlUI::getFontEngine();
    stats.atlasBytes = (uint32_t)fonts.getAtlasBytes();
    stats.msdfGlyphsPending = (uint32_t)fonts.getPendingDistanceGlyphCount();
    stats.liveSources = frameLiveSources;
    stats.keptMismatches = keptMismatchCount;
    if (tilesFull){
        const int columns = (targetSize.x + TILE_SIZE - 1) / TILE_SIZE;
        const int rows = (targetSize.y + TILE_SIZE - 1) / TILE_SIZE;
        stats.compositeTiles = (uint32_t)(columns * rows);
        stats.compositePixels = (uint32_t)(targetSize.x * targetSize.y);
    }else{
        stats.compositeTiles = tileCount;
        stats.compositePixels = tilePixels;
    }
    if (target){
        stats.samples = (uint32_t)target->getSampleCount();
        stats.targetBytes = targetBytesOf(*target);
    }
}

//-------------------------------------------------------------------------------------------
// R4: composite tiles
//-------------------------------------------------------------------------------------------

void RmlRenderer::markTiles(float x0, float y0, float x1, float y1, bool scissorOn, const Rml::Rectanglei& scissor){
    // two pixels of margin: antialiased edges, a rasterizer's samples beyond a scissor's edge, a
    // composite scaled down reading a texel past them
    x0 -= 2.0f;
    y0 -= 2.0f;
    x1 += 2.0f;
    y1 += 2.0f;
    if (scissorOn){
        x0 = std::max(x0, (float)scissor.Left());
        y0 = std::max(y0, (float)scissor.Top());
        x1 = std::min(x1, (float)scissor.Right());
        y1 = std::min(y1, (float)scissor.Bottom());
    }
    x0 = std::max(x0, 0.0f);
    y0 = std::max(y0, 0.0f);
    x1 = std::min(x1, (float)targetSize.x);
    y1 = std::min(y1, (float)targetSize.y);
    if (!(x1 > x0) || !(y1 > y0))
        return;
    const int c0 = std::max(0, (int)std::floor(x0 / (float)TILE_SIZE));
    const int r0 = std::max(0, (int)std::floor(y0 / (float)TILE_SIZE));
    const int c1 = std::min(tileColumns, (int)std::ceil(x1 / (float)TILE_SIZE));
    const int r1 = std::min(tileRows, (int)std::ceil(y1 / (float)TILE_SIZE));
    for (int r = r0; r < r1; r++){
        uint8_t* row = &tileMask[(size_t)r * (size_t)tileColumns];
        for (int c = c0; c < c1; c++)
            row[c] = 1;
    }
}

void RmlRenderer::computeCompositeTiles(){
    tileColumns = (targetSize.x + TILE_SIZE - 1) / TILE_SIZE;
    tileRows = (targetSize.y + TILE_SIZE - 1) / TILE_SIZE;
    tileMask.assign((size_t)tileColumns * (size_t)tileRows, 0);
    tileSource = (target && target->getComposited()) ? &target->getComposited()->getColorTexture() : nullptr;
    const Rml::Vector2f pixel = getPixelScale();
    const float huge = 1e9f;

    // what each pushed layer holds (target px, its draws within their scissors): a composite of it
    // writes that, spread by its filters, within the composite's scissor
    struct Ink {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        bool empty = true;
    };
    std::vector<Ink> layers(1);

    // the recorded state as the replay sets it, and the layer stack's depth (1: layer 0 on top)
    bool scissorOn = false;
    Rml::Rectanglei scissor = toTargetPixels(frameStartScissor, contextSize, targetSize);
    const Rml::Matrix4f* matrix = nullptr;
    auto write = [&](size_t layer, float x0, float y0, float x1, float y1){
        if (scissorOn){
            x0 = std::max(x0, (float)scissor.Left());
            y0 = std::max(y0, (float)scissor.Top());
            x1 = std::min(x1, (float)scissor.Right());
            y1 = std::min(y1, (float)scissor.Bottom());
        }
        if (!(x1 > x0) || !(y1 > y0))
            return;
        if (layer == 0){
            markTiles(x0, y0, x1, y1, false, scissor);
            return;
        }
        if (layer >= layers.size())
            return;
        Ink& ink = layers[layer];
        if (ink.empty){
            ink = Ink{x0, y0, x1, y1, false};
            return;
        }
        ink.x0 = std::min(ink.x0, x0);
        ink.y0 = std::min(ink.y0, y0);
        ink.x1 = std::max(ink.x1, x1);
        ink.y1 = std::max(ink.y1, y1);
    };
    auto writeDocument = [&](const Rml::Rectanglef& local, float tx, float ty){
        const size_t layer = layers.size() - 1;
        const float x0 = local.Left() + tx, y0 = local.Top() + ty, x1 = local.Right() + tx, y1 = local.Bottom() + ty;
        if (!matrix){
            write(layer, x0 * pixel.x, y0 * pixel.y, x1 * pixel.x, y1 * pixel.y);
            return;
        }
        const Rml::Vector2f corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        float a0 = huge, b0 = huge, a1 = -huge, b1 = -huge;
        for (const Rml::Vector2f& corner : corners){
            const Rml::Vector4f p = (*matrix) * Rml::Vector4f(corner.x, corner.y, 0.0f, 1.0f);
            // behind the eye: anywhere within the scissor
            if (!(p.w > 1e-6f)){
                write(layer, -huge, -huge, huge, huge);
                return;
            }
            const float x = p.x / p.w * pixel.x, y = p.y / p.w * pixel.y;
            a0 = std::min(a0, x);
            b0 = std::min(b0, y);
            a1 = std::max(a1, x);
            b1 = std::max(b1, y);
        }
        write(layer, a0, b0, a1, b1);
    };
    // how far a composite's filters move content (target px; an effect may write anywhere)
    auto reachOf = [&](const Command& c){
        float reach = 0.0f;
        for (int f = 0; f < c.i[3]; f++){
            const Rml::CompiledFilterHandle handle = commandFilters[(size_t)(c.i[2] + f)];
            if (!handle)
                continue;
            if (htmlui::EffectRenderer::isEffectHandle(handle))
                return huge;
            const RmlCompiledFilter& filter = *reinterpret_cast<const RmlCompiledFilter*>(handle);
            const float scale = std::max(pixel.x, pixel.y);
            // six sigma: nothing of a Gaussian reaches an 8-bit value beyond it
            if (filter.type == RmlCompiledFilter::Type::BLUR)
                reach += 6.0f * filter.sigma * scale;
            else if (filter.type == RmlCompiledFilter::Type::DROP_SHADOW)
                reach += (std::fabs(filter.offset.x) + std::fabs(filter.offset.y) + 6.0f * filter.sigma) * scale;
        }
        return reach;
    };

    for (const Command& c : commands){
        switch (c.type){
        case CommandType::SCISSOR_ENABLE:
            scissorOn = c.flag != 0;
            break;
        case CommandType::SCISSOR_REGION:
            scissor = Rml::Rectanglei::FromCorners(Rml::Vector2i(c.i[0], c.i[1]), Rml::Vector2i(c.i[2], c.i[3]));
            break;
        case CommandType::TRANSFORM:
            matrix = c.flag ? &commandMatrices[(size_t)c.i[0]] : nullptr;
            break;
        case CommandType::PUSH_LAYER:
            layers.emplace_back();
            break;
        case CommandType::POP_LAYER:
            if (layers.size() > 1)
                layers.pop_back();
            break;
        case CommandType::GEOMETRY: {
            RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(c.a);
            if (geometry && !geometry->vertices.empty() && !geometry->indices.empty())
                writeDocument(geometryBounds(*geometry), c.x, c.y);
            break;
        }
        case CommandType::SHADER: {
            // an effect may paint beyond its geometry (scatter forms): its scissor
            if (htmlui::EffectRenderer::isEffectHandle(c.a)){
                write(layers.size() - 1, -huge, -huge, huge, huge);
                break;
            }
            RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(c.b);
            if (geometry && !geometry->vertices.empty() && !geometry->indices.empty())
                writeDocument(geometryBounds(*geometry), c.x, c.y);
            break;
        }
        case CommandType::PRIMITIVES:
            if (c.i[1] > 0)
                writeDocument(primitiveBounds((uint32_t)c.i[0], (uint32_t)c.i[1]), c.x, c.y);
            break;
        case CommandType::COMPOSITE: {
            const size_t source = (size_t)std::max(c.i[0], 0), destination = (size_t)std::max(c.i[1], 0);
            // a backdrop reads a layer below it: what that holds is not tracked
            if (source == 0 || source >= layers.size()){
                write(destination, -huge, -huge, huge, huge);
                break;
            }
            const Ink& ink = layers[source];
            if (ink.empty)
                break;
            const float reach = reachOf(c);
            write(destination, ink.x0 - reach, ink.y0 - reach, ink.x1 + reach, ink.y1 + reach);
            break;
        }
        default:
            break;
        }
    }
    buildTileQuads();
}

void RmlRenderer::buildTileQuads(){
    tileCount = 0;
    for (uint8_t tile : tileMask)
        tileCount += tile;
    // the quads: each row's runs of tiles, one quad for a run that continues the same run of the
    // rows above (D3: a dense document is a quad per row at most, a box many rows high one quad)
    tileVertices.clear();
    tilePixels = 0;
    struct Open { int c0, c1, r0; };
    std::vector<Open> open, next;
    const float w = (float)std::max(targetSize.x, 1), h = (float)std::max(targetSize.y, 1);
    const Rml::ColourbPremultiplied white(255, 255, 255, 255);
    auto emit = [&](const Open& run, int r1){
        const int x0 = run.c0 * TILE_SIZE, y0 = run.r0 * TILE_SIZE;
        const int x1 = std::min(run.c1 * TILE_SIZE, targetSize.x), y1 = std::min(r1 * TILE_SIZE, targetSize.y);
        tilePixels += (uint32_t)((x1 - x0) * (y1 - y0));
        // the composite quad's space: (-1, 1) top-left .. (1, -1) bottom-right, uv 0..1 from the top
        const float u0 = (float)x0 / w, u1 = (float)x1 / w, v0 = (float)y0 / h, v1 = (float)y1 / h;
        const Rml::Vertex corners[4] = {
            {Rml::Vector2f(u0 * 2.0f - 1.0f, 1.0f - v0 * 2.0f), white, Rml::Vector2f(u0, v0)},
            {Rml::Vector2f(u1 * 2.0f - 1.0f, 1.0f - v0 * 2.0f), white, Rml::Vector2f(u1, v0)},
            {Rml::Vector2f(u1 * 2.0f - 1.0f, 1.0f - v1 * 2.0f), white, Rml::Vector2f(u1, v1)},
            {Rml::Vector2f(u0 * 2.0f - 1.0f, 1.0f - v1 * 2.0f), white, Rml::Vector2f(u0, v1)},
        };
        tileVertices.insert(tileVertices.end(), corners, corners + 4);
    };
    for (int r = 0; r <= tileRows; r++){
        next.clear();
        if (r < tileRows){
            const uint8_t* row = &tileMask[(size_t)r * (size_t)tileColumns];
            for (int c = 0; c < tileColumns;){
                if (!row[c]){
                    c++;
                    continue;
                }
                int e = c;
                while (e < tileColumns && row[e])
                    e++;
                next.push_back(Open{c, e, r});
                c = e;
            }
        }
        // a run of the rows above that this row continues keeps going; the others end here
        for (const Open& run : open){
            auto same = std::find_if(next.begin(), next.end(), [&](const Open& o){ return o.c0 == run.c0 && o.c1 == run.c1; });
            if (same != next.end())
                same->r0 = run.r0;
            else
                emit(run, r);
        }
        open.swap(next);
    }
    tileQuads = (uint32_t)(tileVertices.size() / 4);

    // the GPU copy: a stream buffer written when the set changes, at most once a sokol frame
    if (tileQuads == 0 || (tileMask == uploadedTileMask && tileVertexBuffer.isCreated() && tileCapacity >= tileQuads)){
        tilesFull = false;
        return;
    }
    const uint64_t now = SystemRender::getFrameCount();
    if (tileUploadFrame == now){
        tilesFull = true;
        uploadedTileMask.clear();
        return;
    }
    if (tileCapacity < tileQuads){
        destroyTileBuffers();
        uint32_t capacity = 64;
        while (capacity < tileQuads)
            capacity *= 2;
        std::vector<uint32_t> indices((size_t)capacity * 6);
        for (uint32_t q = 0; q < capacity; q++){
            const uint32_t pattern[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++)
                indices[(size_t)q * 6 + (size_t)k] = q * 4 + pattern[k];
        }
        gpuCreateCount += 2;
        if (!tileVertexBuffer.createBuffer((unsigned int)(capacity * 4 * sizeof(Rml::Vertex)), nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM) ||
            !tileIndexBuffer.createBuffer((unsigned int)(indices.size() * sizeof(uint32_t)), indices.data(), BufferType::INDEX_BUFFER,
                                          BufferUsage::IMMUTABLE)){
            destroyTileBuffers();
            tilesFull = true;
            return;
        }
        tileCapacity = capacity;
    }
    if (!tileVertexBuffer.updateBuffer((unsigned int)(tileVertices.size() * sizeof(Rml::Vertex)), tileVertices.data())){
        // past the frame's stream staging buffer: the whole quad, the tiles again next frame
        tilesFull = true;
        uploadedTileMask.clear();
        return;
    }
    tileUploadFrame = now;
    uploadedTileMask = tileMask;
    tilesFull = false;
}

void RmlRenderer::destroyTileBuffers(){
    tileVertexBuffer.destroyBuffer();
    tileIndexBuffer.destroyBuffer();
    tileCapacity = 0;
    uploadedTileMask.clear();
}

//-------------------------------------------------------------------------------------------
// R5: the depth-stencil plan
//-------------------------------------------------------------------------------------------

void RmlRenderer::planDepthStencil(){
    // positions: the batched plan's ops, else the commands. A position breaks the open pass when
    // it draws into another layer than the drawing before it, or goes through other targets
    // (filters, saved layers, effects); pushing or popping a layer alone binds nothing
    const size_t count = batching ? replayOps.size() : commands.size();
    dsFlags.assign(count, 0);
    bool stencilOn = false;
    int32_t scope = -1;
    int depth = 1;             // the layer stack as recorded
    int drawnInto = -1;        // the layer the last drawing went to (-1: none, or another target since)
    auto stencilScope = [this](int32_t s){
        return s < 0 || clipScopes[(size_t)s].mode == ScopeMode::STENCIL;
    };
    auto drawInto = [&](int layer, uint8_t& flags){
        if (layer != drawnInto)
            flags |= DS_BREAK;
        drawnInto = layer;
    };
    for (size_t i = 0; i < count; i++){
        uint8_t flags = 0;
        if (batching && replayOps[i].kind == ReplayOp::Kind::RUN){
            const BatchRun& run = batchRuns[replayOps[i].index];
            if (run.stencil)
                flags |= DS_STENCIL;
            drawInto(run.layer, flags);
            dsFlags[i] = flags;
            continue;
        }
        const size_t ci = batching ? replayOps[i].index : i;
        const Command& c = commands[ci];
        switch (c.type){
        case CommandType::CLIP_ENABLE:
            if (batching){
                scope = replayOps[i].scope;
                stencilOn = c.flag != 0 && stencilScope(scope);
            }else if (primitivesFrame){
                scope = c.flag ? scopeOfCommand[ci] : -1;
                stencilOn = c.flag != 0 && stencilScope(scope);
            }else{
                stencilOn = c.flag != 0;
            }
            break;
        case CommandType::CLIP_MASK:
            // the replays skip the masks of a scope clipped without the stencil
            if (batching || !primitivesFrame || scope < 0 || clipScopes[(size_t)scope].mode == ScopeMode::STENCIL){
                flags |= DS_STENCIL;
                drawInto(depth - 1, flags);
            }
            break;
        case CommandType::GEOMETRY:
        case CommandType::PRIMITIVES:
            if (stencilOn)
                flags |= DS_STENCIL;
            drawInto(depth - 1, flags);
            break;
        case CommandType::SHADER:
            if (stencilOn)
                flags |= DS_STENCIL;
            if (htmlui::EffectRenderer::isEffectHandle(c.a)){
                // an effect's passes may use other targets
                flags |= DS_BREAK;
                drawnInto = -1;
            }else{
                drawInto(depth - 1, flags);
            }
            break;
        case CommandType::COMPOSITE:
            if (stencilOn)
                flags |= DS_STENCIL;
            // the source is read (its pass ends), filters go through the post-process targets
            flags |= DS_BREAK;
            drawnInto = c.i[1];
            break;
        case CommandType::PUSH_LAYER:
            depth++;
            break;
        case CommandType::POP_LAYER:
            depth = std::max(1, depth - 1);
            break;
        case CommandType::SAVE_TEXTURE:
        case CommandType::SAVE_MASK:
            flags |= DS_BREAK;
            drawnInto = -1;
            break;
        default:
            break;
        }
        dsFlags[i] = flags;
    }
    dsLastStencil = -1;
    dsNextBreak.assign(count, (uint32_t)count);
    uint32_t next = (uint32_t)count;
    for (size_t i = count; i-- > 0;){
        dsNextBreak[i] = next;
        if (dsFlags[i] & DS_BREAK)
            next = (uint32_t)i;
        if (dsLastStencil < 0 && (dsFlags[i] & DS_STENCIL))
            dsLastStencil = (int64_t)i;
    }
    replayPosition = 0;
    const DepthStencilDiscard mode = (DepthStencilDiscard)depthStencilDiscard.load();
    dsPlanned = mode != DepthStencilDiscard::OFF;
    dsEveryPass = mode == DepthStencilDiscard::EVERY_PASS;
}

//-------------------------------------------------------------------------------------------
// R3: verification of kept frames
//-------------------------------------------------------------------------------------------

namespace {

    const char* frameCommandName(int type){
        static const char* names[] = {"GEOMETRY", "CLIP_MASK", "SCISSOR_ENABLE", "SCISSOR_REGION", "CLIP_ENABLE", "TRANSFORM",
                                      "PUSH_LAYER", "POP_LAYER", "COMPOSITE", "SAVE_TEXTURE", "SAVE_MASK", "SHADER", "PRIMITIVES"};
        return (type >= 0 && type < (int)(sizeof(names) / sizeof(names[0]))) ? names[type] : "?";
    }

}

void RmlRenderer::verifyKeptFrame(){
    // what the message names: the first command that reads a source the signature cannot version
    verifySource = "no command reads an unversioned source";
    for (size_t i = 0; i < commands.size(); i++){
        const Command& c = commands[i];
        uint32_t live = 0;
        if (c.type == CommandType::GEOMETRY)
            textureSignature(reinterpret_cast<const HtmlUITexture*>(c.b), live);
        else if (c.type == CommandType::SAVE_TEXTURE)
            textureSignature(reinterpret_cast<const HtmlUITexture*>(c.a), live);
        else if (c.type == CommandType::SHADER)
            shaderSignature(c.a, live);
        else if (c.type == CommandType::COMPOSITE){
            for (int f = 0; f < c.i[3]; f++)
                filterSignature(commandFilters[(size_t)(c.i[2] + f)], live);
        }
        if (live != 0){
            verifySource = "command " + std::to_string(i) + " (" + frameCommandName((int)c.type) + ") reads live source bits " +
                           std::to_string(live);
            break;
        }
    }

    RmlRenderTarget* kept = target;
    if (!verifyTarget)
        verifyTarget = std::make_unique<RmlRenderTarget>();
    verifyTarget->ensure(kept->getWidth(), kept->getHeight(), kept->getSampleCount());
    if (!verifyTarget->isCreated())
        return;

    // the frame replayed into the scratch target as into a fresh one; its statistics are not the
    // kept frame's
    const HtmlUIRenderStats keptStats = stats;
    target = verifyTarget.get();
    target->newFrame();
    activeRegions = nullptr;
    deferred = DeferredComposite();
    regions.clear();
    regionsCache[0] = regionsCache[1] = RegionsCacheEntry();
    regionsGeneration = target->getGeneration();
    layerClearPending.assign(std::max<size_t>(1, target->layers.size()), false);
    layerClearPending[0] = true;
    depthStencilClearPending = true;
    activeKind = TargetKind::NONE;
    activeIndex = -1;
    activeFramebuffer = nullptr;
    sharedDsDiscarded = false;
    discardedRegionDs.clear();

    replayCommands();
    layersSize = std::max(layersSize, 1);
    ensureLayerCleared(0);
    if (Framebuffer* layer = target->getLayer(0))
        ensureResolved(*layer);
    endPass();
    for (size_t i = 1; i < frames.size(); i++){
        if (frames[i].pool >= 0)
            target->releaseRegionLayer(frames[i].pool);
    }
    target->trimRegions();
    dsPlanned = false;

    // what is known of the kept target's resolves holds no more: unknown is a full resolve
    target = kept;
    regions.clear();
    regionsCache[0] = regionsCache[1] = RegionsCacheEntry();
    regionsGeneration = 0;
    stats = keptStats;

    verifyPending = true;
    verifyFrame = SystemRender::getFrameCount();
    verifyKeptTarget = kept;
    verifyKeptGeneration = kept->getGeneration();
    // GL reads back at once; the other backends once the frame's commands are submitted
    if (Engine::isOpenGL())
        compareKeptFrame();
}

bool RmlRenderer::checkKeptFrame(){
    if (!verifyPending)
        return false;
    if (!Engine::isOpenGL() && SystemRender::getFrameCount() <= verifyFrame)
        return true;
    compareKeptFrame();
    return true;
}

void RmlRenderer::compareKeptFrame(){
    verifyPending = false;
    RmlRenderTarget* kept = verifyKeptTarget;
    verifyKeptTarget = nullptr;
    if (!kept || !verifyTarget || !kept->isCreated() || !verifyTarget->isCreated() || kept->getGeneration() != verifyKeptGeneration ||
        kept->getWidth() != verifyTarget->getWidth() || kept->getHeight() != verifyTarget->getHeight())
        return;
    const int width = kept->getWidth(), height = kept->getHeight();
    std::vector<uint8_t> keptPixels, freshPixels;
    std::string error;
    if (!FrameReadback::readTexture(kept->getComposited()->getColorTexture(), width, height, 4, keptPixels, &error) ||
        !FrameReadback::readTexture(verifyTarget->getComposited()->getColorTexture(), width, height, 4, freshPixels, &error)){
        Log::warn("[HTML UI] kept frame not verified: %s", error.c_str());
        return;
    }
    if (keptPixels == freshPixels)
        return;
    size_t first = 0;
    while (first < keptPixels.size() && keptPixels[first] == freshPixels[first])
        first++;
    const size_t pixel = first / 4;
    keptMismatchCount++;
    Log::error("[HTML UI] kept frame differs at (%d, %d): %s", (int)(pixel % (size_t)width), (int)(pixel / (size_t)width), verifySource.c_str());
}
