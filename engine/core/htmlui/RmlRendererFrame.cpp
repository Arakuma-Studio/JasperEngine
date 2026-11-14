// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S92 frame reuse (docs/specs/S92-htmlui-render-cost.md R1-R5): the signature of a recorded frame,
// the sources it cannot version (R2), the tiles of layer 0 a frame wrote (R4), the depth-stencil
// plan of a replay (R5) and the verification of kept frames (R3). Whether a frame changed is
// decided from what RmlUi recorded, never from a dirty flag (D1): a frame whose signature equals
// the last drawn one's is not replayed, its target keeps the image.

#include "RmlRenderer.h"
#include "RmlRendererInternal.h"

#include "HtmlUITexture.h"

#include "effects/EffectRenderer.h"
#include "effects/EffectRuntime.h"
#include "effects/EffectTypes.h"

#include <algorithm>
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
