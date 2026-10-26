// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The batched replay of a recorded frame (S16 R1-R7). A frame is planned before anything is drawn:
//
// 1. Clip scopes: the commands under each clip mask list. A list whose every draw lies inside the
//    inner region of its shapes (the rectangle without its corner boxes, straight edges that fall
//    on pixel boundaries may be crossed) draws no mask: the scissor clips to the shapes' bounds.
//    Else up to two rounded rectangles (Set, then Intersect) under the content's own transform,
//    with nothing but geometry under them, are clipped in the fragment shader (u_fs_htmluiClip).
//    Everything else draws its masks into the stencil as before.
// 2. Runs: consecutive geometry drawn with the same layer, pipeline state, transform, analytic
//    clip and texture (untextured geometry joins a texture with a white texel block) and a
//    scissor that clips none of it differently becomes one draw. The translation is baked into
//    gathered vertices (32-bit indices). A run is cut into chunks; a chunk whose members (geometry,
//    translation, texture) are those of a chunk of the last frame reuses its gathered range.
//    Geometry above 4096 vertices is drawn from its own buffer and ends the run.
// 3. The new chunks are written into gather pages not used by this frame's cached ones, each page
//    uploaded once; then the plan is drawn in the recorded order. Runs never cross a layer, a
//    composite, a saved layer, a filter, a shader, a stencil write or a blending change.

#include "RmlRenderer.h"
#include "RmlRendererInternal.h"

#include "HtmlUITexture.h"

#include "Engine.h"
#include "Log.h"
#include "effects/EffectRenderer.h"
#include "render/SystemRender.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    std::atomic<bool> batchingEnabled{true};

    // a geometry above it is drawn from its own buffer (R1)
    constexpr size_t maxGatheredVertices = 4096;
    // run cache granularity: a chunk ends after this many members or vertices
    constexpr uint32_t chunkMembers = 64;
    constexpr uint32_t chunkVertices = 4096;
    constexpr size_t gatherPageBytes = 128 * 1024;   // at least: a page made in a frame holds what the frame gathers
    // a page whose live chunks fill less than this share of it is written again (compaction)
    constexpr float minimumLiveShare = 0.5f;
    // a coordinate within it of a whole pixel is on the pixel boundary
    constexpr float pixelEpsilon = 1e-3f;

    // An element transform that only translates and scales, as doc -> doc: x' = x * sx + ox
    struct AxisMap {
        bool valid = false;
        float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
    };

    AxisMap axisMapOf(const Rml::Matrix4f* transform){
        AxisMap map;
        if (!transform){
            map.valid = true;
            return map;
        }
        const Rml::Vector4f p0 = (*transform) * Rml::Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
        const Rml::Vector4f px = (*transform) * Rml::Vector4f(1.0f, 0.0f, 0.0f, 1.0f);
        const Rml::Vector4f py = (*transform) * Rml::Vector4f(0.0f, 1.0f, 0.0f, 1.0f);
        // no perspective, no rotation or skew
        if (std::fabs(p0.w) < 1e-6f || std::fabs(px.w - p0.w) > 1e-6f || std::fabs(py.w - p0.w) > 1e-6f)
            return map;
        const float w = 1.0f / p0.w;
        if (std::fabs(px.y - p0.y) * std::fabs(w) > 1e-6f || std::fabs(py.x - p0.x) * std::fabs(w) > 1e-6f)
            return map;
        map.valid = true;
        map.sx = (px.x - p0.x) * w;
        map.sy = (py.y - p0.y) * w;
        map.ox = p0.x * w;
        map.oy = p0.y * w;
        return map;
    }

    struct RectF {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        bool empty() const{ return !(x1 > x0) || !(y1 > y0); }
    };

    // doc px through the axis map into target px (top-left origin)
    RectF mapRect(const AxisMap& map, Rml::Vector2f pixel, float x0, float y0, float x1, float y1){
        float ax = (x0 * map.sx + map.ox) * pixel.x, bx = (x1 * map.sx + map.ox) * pixel.x;
        float ay = (y0 * map.sy + map.oy) * pixel.y, by = (y1 * map.sy + map.oy) * pixel.y;
        RectF r;
        r.x0 = std::min(ax, bx);
        r.x1 = std::max(ax, bx);
        r.y0 = std::min(ay, by);
        r.y1 = std::max(ay, by);
        return r;
    }

    // inside the integer rectangle (the draw cannot touch a pixel outside of it)
    bool insideRect(const RectF& r, Rml::Rectanglei s){
        return r.empty() || (r.x0 >= (float)s.Left() - pixelEpsilon && r.y0 >= (float)s.Top() - pixelEpsilon &&
                             r.x1 <= (float)s.Right() + pixelEpsilon && r.y1 <= (float)s.Bottom() + pixelEpsilon);
    }

    bool onPixel(float v){
        return std::fabs(v - std::round(v)) < pixelEpsilon;
    }

    // a clip shape's inner region in target px: its rectangle, which straight edges a draw may
    // cross (on a pixel boundary the scissor clips there exactly as the mask), and the corner
    // boxes of its curves, which a draw may not touch
    struct ShapeRegion {
        RectF rect;
        bool aligned[4] = {};   // left, top, right, bottom
        RectF corners[4];
        bool hasCorner[4] = {};

        bool admits(const RectF& d) const{
            if (d.empty())
                return true;
            if ((d.x0 < rect.x0 - pixelEpsilon && !aligned[0]) || (d.y0 < rect.y0 - pixelEpsilon && !aligned[1]) ||
                (d.x1 > rect.x1 + pixelEpsilon && !aligned[2]) || (d.y1 > rect.y1 + pixelEpsilon && !aligned[3]))
                return false;
            for (int i = 0; i < 4; i++){
                const RectF& c = corners[i];
                if (hasCorner[i] && d.x0 < c.x1 - pixelEpsilon && d.x1 > c.x0 + pixelEpsilon && d.y0 < c.y1 - pixelEpsilon &&
                    d.y1 > c.y0 + pixelEpsilon)
                    return false;
            }
            return true;
        }
    };

    bool sameMatrix(const Rml::Matrix4f* a, const Rml::Matrix4f* b){
        if (a == b)
            return true;
        const Rml::Matrix4f identity = Rml::Matrix4f::Identity();
        const Rml::Matrix4f& ma = a ? *a : identity;
        const Rml::Matrix4f& mb = b ? *b : identity;
        return std::memcmp(ma.data(), mb.data(), sizeof(float) * 16) == 0;
    }

    // a matrix that only moves x and y (z is not drawn): its offset
    bool translationOf(const Rml::Matrix4f& matrix, Rml::Vector2f& offset){
        const float* m = matrix.data();
        // column-major: the 3x3 part and the last row are the identity's
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (int i = 0; i < 16; i++){
            if (i == 12 || i == 13 || i == 14)
                continue;
            if (m[i] != identity[i])
                return false;
        }
        offset = Rml::Vector2f(m[12], m[13]);
        return true;
    }

    uint64_t mix(uint64_t h, uint64_t v){
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h * 0xff51afd7ed558ccdull;
    }

    uint64_t floatBits(float f){
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }

    bool filterSpreads(Rml::CompiledFilterHandle handle){
        if (!handle)
            return false;
        // an effect's filter may read and write anywhere in its region
        if (htmlui::EffectRenderer::isEffectHandle(handle))
            return true;
        const RmlCompiledFilter::Type type = reinterpret_cast<const RmlCompiledFilter*>(handle)->type;
        return type == RmlCompiledFilter::Type::BLUR || type == RmlCompiledFilter::Type::DROP_SHADOW;
    }

}

void RmlRenderer::setBatchingForTesting(bool enabled){
    batchingEnabled = enabled;
}

bool RmlRenderer::isBatching(){
    return batchingEnabled;
}

Rml::Rectanglef RmlRenderer::geometryBounds(RmlGeometry& geometry){
    if (!geometry.boundsValid){
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        bool first = true;
        for (const Rml::Vertex& vertex : geometry.vertices){
            if (first){
                x0 = x1 = vertex.position.x;
                y0 = y1 = vertex.position.y;
                first = false;
                continue;
            }
            x0 = std::min(x0, vertex.position.x);
            y0 = std::min(y0, vertex.position.y);
            x1 = std::max(x1, vertex.position.x);
            y1 = std::max(y1, vertex.position.y);
        }
        geometry.bounds[0] = x0;
        geometry.bounds[1] = y0;
        geometry.bounds[2] = x1;
        geometry.bounds[3] = y1;
        geometry.boundsValid = true;
    }
    return Rml::Rectanglef::FromCorners(Rml::Vector2f(geometry.bounds[0], geometry.bounds[1]), Rml::Vector2f(geometry.bounds[2], geometry.bounds[3]));
}

Rml::Rectanglei RmlRenderer::effectiveScissorTarget() const{
    const Rml::Rectanglei full = Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(targetSize.x, 1), std::max(targetSize.y, 1)));
    Rml::Rectanglei scissor = scissorEnabled ? scissorTarget : full;
    if (activeClipScope >= 0 && activeClipScope < (int)clipScopes.size() && clipScopes[(size_t)activeClipScope].mode == ScopeMode::ELIDED){
        const Rml::Rectanglei clip = clipScopes[(size_t)activeClipScope].scissor;
        const Rml::Vector2i p0 = Rml::Math::Max(scissor.p0, clip.p0);
        const Rml::Vector2i p1 = Rml::Math::Min(scissor.p1, clip.p1);
        scissor = Rml::Rectanglei::FromCorners(p0, Rml::Math::Max(p0, p1));
    }
    return scissor;
}

const RmlRenderer::ClipParams& RmlRenderer::currentClipParams(const HtmlUITexture* texture){
    const bool analytic = activeClipScope >= 0 && activeClipScope < (int)clipScopes.size() &&
                          clipScopes[(size_t)activeClipScope].mode == ScopeMode::ANALYTIC;
    const HtmlUITexture::Sample sample = texture ? texture->sample : HtmlUITexture::Sample::RGBA;
    if (!analytic && sample == HtmlUITexture::Sample::RGBA)
        return noClip;
    clipScratch = analytic ? clipScopes[(size_t)activeClipScope].clip : noClip;
    switch (sample){
    case HtmlUITexture::Sample::RGBA:
        clipScratch.params[1] = 0.0f;
        break;
    case HtmlUITexture::Sample::COVERAGE:
        clipScratch.params[1] = 1.0f;
        break;
    case HtmlUITexture::Sample::MSDF:
        clipScratch.params[1] = 2.0f;
        clipScratch.text[0] = texture->textEffect[0];
        clipScratch.text[1] = texture->textEffect[1];
        clipScratch.text[2] = texture->textEffect[2];
        clipScratch.text[3] = texture->distanceRange;
        break;
    }
    return clipScratch;
}

//-------------------------------------------------------------------------------------------
// primitives (R8-R10)
//-------------------------------------------------------------------------------------------

namespace {

    std::atomic<bool> sdfPrimitivesEnabled{true};

    void setColor(uint8_t out[4], Rml::ColourbPremultiplied colour){
        out[0] = colour.red;
        out[1] = colour.green;
        out[2] = colour.blue;
        out[3] = colour.alpha;
    }

    void setRect(float out[4], const Rml::Rectanglef& rect, Rml::Vector2f translation){
        out[0] = rect.Left() + translation.x;
        out[1] = rect.Top() + translation.y;
        out[2] = rect.Width();
        out[3] = rect.Height();
    }

    void setRadii(float x[4], float y[4], const Rml::Array<Rml::Vector2f, 4>& radii){
        for (int i = 0; i < 4; i++){
            const bool curved = radii[i].x > 0.0f && radii[i].y > 0.0f;
            x[i] = curved ? radii[i].x : 0.0f;
            y[i] = curved ? radii[i].y : 0.0f;
        }
    }

    // the instance moved by the translation (positions and rectangles in document px)
    void translateInstance(HtmlUIPrimitive& instance, Rml::Vector2f t){
        switch ((HtmlUIPrimitive::Mode)(int)instance.params[0]){
        case HtmlUIPrimitive::QUAD:
            instance.a[0] += t.x;
            instance.a[1] += t.y;
            instance.a[2] += t.x;
            instance.a[3] += t.y;
            instance.b[0] += t.x;
            instance.b[1] += t.y;
            instance.b[2] += t.x;
            instance.b[3] += t.y;
            break;
        case HtmlUIPrimitive::BOX:
            instance.a[0] += t.x;
            instance.a[1] += t.y;
            break;
        case HtmlUIPrimitive::SHADOW_OUTER:
        case HtmlUIPrimitive::SHADOW_INSET:
            instance.a[0] += t.x;
            instance.a[1] += t.y;
            instance.d[0] += t.x;
            instance.d[1] += t.y;
            break;
        }
    }

    uint64_t hashBytes(uint64_t h, const void* data, size_t size){
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        size_t i = 0;
        for (; i + 8 <= size; i += 8){
            uint64_t word;
            std::memcpy(&word, bytes + i, 8);
            h = mix(h, word);
        }
        uint64_t tail = 0;
        std::memcpy(&tail, bytes + i, size - i);
        return mix(h, tail ^ (uint64_t)size);
    }

}

void RmlRenderer::setSdfPrimitivesForTesting(bool enabled){
    sdfPrimitivesEnabled = enabled;
}

bool RmlRenderer::isSdfPrimitives(){
    return sdfPrimitivesEnabled;
}

bool RmlRenderer::SupportsPrimitives(){
    return sdfPrimitivesEnabled;
}

HtmlUIPrimitive RmlRenderer::toInstance(const Rml::BoxPrimitive& primitive, Rml::Vector2f translation){
    HtmlUIPrimitive instance;
    std::memset(&instance, 0, sizeof(instance));
    setRect(instance.a, primitive.border_box, translation);
    setRadii(instance.b, instance.c, primitive.radii);
    setColor(instance.color, primitive.fill);
    switch (primitive.kind){
    case Rml::BoxPrimitive::Kind::Box:
        instance.params[0] = (float)HtmlUIPrimitive::BOX;
        for (int side = 0; side < 4; side++){
            instance.d[side] = std::max(primitive.border_widths[side], 0.0f);
            instance.e[side] = std::max(primitive.fill_insets[side], 0.0f);
            setColor(instance.border[side], primitive.border_colors[(size_t)side]);
        }
        break;
    case Rml::BoxPrimitive::Kind::ShadowOuter:
    case Rml::BoxPrimitive::Kind::ShadowInset:
        instance.params[0] = (float)(primitive.kind == Rml::BoxPrimitive::Kind::ShadowOuter ? HtmlUIPrimitive::SHADOW_OUTER : HtmlUIPrimitive::SHADOW_INSET);
        instance.params[1] = std::max(primitive.sigma, 0.0f);
        setRect(instance.d, primitive.clip_box, translation);
        setRadii(instance.e, instance.f, primitive.clip_radii);
        break;
    }
    return instance;
}

bool RmlRenderer::classifyQuadList(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices, std::vector<HtmlUIPrimitive>* instances){
    const size_t quads = vertices.size() / 4;
    if (quads == 0 || vertices.size() != quads * 4 || indices.size() != quads * 6)
        return false;
    constexpr float epsilon = 1e-4f;
    if (instances){
        instances->clear();
        instances->reserve(quads);
    }
    for (size_t q = 0; q < quads; q++){
        // GenerateQuad's order: (0, 3, 1), (1, 3, 2)
        const int v0 = (int)(q * 4);
        const int* idx = indices.data() + q * 6;
        if (idx[0] != v0 || idx[1] != v0 + 3 || idx[2] != v0 + 1 || idx[3] != v0 + 1 || idx[4] != v0 + 3 || idx[5] != v0 + 2)
            return false;
        const Rml::Vertex* v = vertices.data() + q * 4;
        // a parallelogram: the diagonals share their midpoint
        const Rml::Vector2f d = (v[0].position + v[2].position) - (v[1].position + v[3].position);
        if (std::fabs(d.x) > epsilon || std::fabs(d.y) > epsilon)
            return false;
        if (v[1].colour != v[0].colour || v[2].colour != v[0].colour || v[3].colour != v[0].colour)
            return false;
        // uvs affine: corner 1 shares corner 0's v and corner 2's u, corner 3 the other two
        if (v[1].tex_coord.x != v[2].tex_coord.x || v[1].tex_coord.y != v[0].tex_coord.y ||
            v[3].tex_coord.x != v[0].tex_coord.x || v[3].tex_coord.y != v[2].tex_coord.y)
            return false;
        if (instances){
            HtmlUIPrimitive instance;
            std::memset(&instance, 0, sizeof(instance));
            instance.a[0] = v[0].position.x;
            instance.a[1] = v[0].position.y;
            instance.a[2] = v[1].position.x;
            instance.a[3] = v[1].position.y;
            instance.b[0] = v[2].position.x;
            instance.b[1] = v[2].position.y;
            instance.b[2] = v[3].position.x;
            instance.b[3] = v[3].position.y;
            instance.d[0] = v[0].tex_coord.x;
            instance.d[1] = v[0].tex_coord.y;
            instance.d[2] = v[2].tex_coord.x;
            instance.d[3] = v[2].tex_coord.y;
            instance.params[0] = (float)HtmlUIPrimitive::QUAD;
            setColor(instance.color, v[0].colour);
            instances->push_back(instance);
        }
    }
    return true;
}

bool RmlRenderer::isQuadList(RmlGeometry& geometry){
    if (geometry.quadList == RmlGeometry::QuadList::UNKNOWN)
        geometry.quadList = classifyQuadList(geometry.vertices, geometry.indices, &geometry.instances) ? RmlGeometry::QuadList::YES
                                                                                                       : RmlGeometry::QuadList::NO;
    return geometry.quadList == RmlGeometry::QuadList::YES;
}

void RmlRenderer::RenderPrimitives(Rml::Span<const Rml::BoxPrimitive> primitives, Rml::Vector2f translation){
    if (primitives.empty())
        return;
    if (frameState == FrameState::ACTIVE)
        stats.geometrySubmitted++;
    if (recording){
        Command& command = recordCommand(CommandType::PRIMITIVES);
        command.i[0] = (int32_t)commandPrimitives.size();
        command.i[1] = (int32_t)primitives.size();
        command.x = translation.x;
        command.y = translation.y;
        const size_t first = commandPrimitives.size();
        for (const Rml::BoxPrimitive& primitive : primitives)
            commandPrimitives.push_back(toInstance(primitive, Rml::Vector2f(0.0f, 0.0f)));
        const uint64_t hash = hashBytes(0x2545f4914f6cdd1dull, commandPrimitives.data() + first, primitives.size() * sizeof(HtmlUIPrimitive));
        command.i[2] = (int32_t)(uint32_t)(hash & 0xffffffffull);
        command.i[3] = (int32_t)(uint32_t)(hash >> 32);
        return;
    }
    if (frameState != FrameState::ACTIVE)
        return;
    // immediate drawing: the command pool holds this call's instances only
    commandPrimitives.clear();
    for (const Rml::BoxPrimitive& primitive : primitives)
        commandPrimitives.push_back(toInstance(primitive, Rml::Vector2f(0.0f, 0.0f)));
    drawPrimitivesNow(commandPrimitives.data(), commandPrimitives.size(), translation);
    commandPrimitives.clear();
}

void RmlRenderer::drawPrimitivesNow(const HtmlUIPrimitive* primitives, size_t count, Rml::Vector2f translation){
    if (frameState != FrameState::ACTIVE || count == 0)
        return;
    if (!bindDrawLayer(false))
        return;
    // clipped away: sokol widens an empty scissor to 1x1 on Metal/Vulkan/WebGPU
    const Rml::Rectanglei scissor = currentScissorTarget();
    if (scissor.Width() <= 0 || scissor.Height() <= 0)
        return;
    primitiveScratch.assign(primitives, primitives + count);
    for (HtmlUIPrimitive& instance : primitiveScratch)
        translateInstance(instance, translation);
    BufferRender* buffer = nullptr;
    size_t offset = 0;
    if (!appendTransient(primitiveScratch.data(), count * sizeof(HtmlUIPrimitive), false, buffer, offset))
        return;
    applyPassViewport(Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(targetSize.x, 1), std::max(targetSize.y, 1))));
    applyPassScissor(scissor);
    static const std::array<HtmlUITexture*, 4> none{};
    drawInstances(*buffer, offset, (uint32_t)count, none, transform, currentClipParams(nullptr), getGeometryState(BlendMode::PREMULTIPLIED), scissor);
}

bool RmlRenderer::drawInstances(BufferRender& buffer, size_t byteOffset, uint32_t count, const std::array<HtmlUITexture*, 4>& slots,
                                const Rml::Matrix4f& matrix, const ClipParams& clipParams, const PipelineState& state, Rml::Rectanglei scissor){
    primObject.replaceVertexBuffer(&primInstanceTemplate, &buffer, byteOffset);
    // u_fs_htmluiSlots: each slot's kind and MTSDF parameters
    struct SlotParams {
        float kind[4];
        float text[4][4];
    } slotParams;
    std::memset(&slotParams, 0, sizeof(slotParams));
    for (size_t i = 0; i < slots.size(); i++){
        HtmlUITexture* texture = slots[i];
        if (primTextureSlots[i].first >= 0)
            primObject.addTexture(primTextureSlots[i], ShaderStageType::FRAGMENT, texture ? texture->render : &placeholderWhite);
        if (!texture)
            continue;
        stats.textureBinds++;
        noteDrawn(texture);
        switch (texture->sample){
        case HtmlUITexture::Sample::RGBA:
            break;
        case HtmlUITexture::Sample::COVERAGE:
            slotParams.kind[i] = 1.0f;
            break;
        case HtmlUITexture::Sample::MSDF:
            slotParams.kind[i] = 2.0f;
            for (int k = 0; k < 3; k++)
                slotParams.text[i][k] = texture->textEffect[k];
            slotParams.text[i][3] = texture->distanceRange;
            break;
        }
    }
    if (!primObject.beginDraw(state))
        return false;
    HtmlUIPrimParams params;
    std::memcpy(params.transform, matrix.data(), sizeof(params.transform));
    params.viewport[0] = (float)std::max(targetSize.x, 1);
    params.viewport[1] = (float)std::max(targetSize.y, 1);
    params.viewport[2] = params.viewport[3] = 0.0f;
    // shapes drawn in document px (no element transform): their quads, widened for the edge's
    // antialiasing, cut to the scissor, so no fragment outside it is ever made (a rasterizer may
    // leak samples of a primitive across the scissor's edge: llvmpipe at 4 samples)
    params.scissor[0] = params.scissor[1] = params.scissor[2] = params.scissor[3] = 0.0f;
    if (std::memcmp(matrix.data(), projection.data(), sizeof(float) * 16) == 0){
        const Rml::Vector2f pixel = getPixelScale();
        params.viewport[2] = 1.0f;
        params.scissor[0] = (float)scissor.Left() / pixel.x;
        params.scissor[1] = (float)scissor.Top() / pixel.y;
        params.scissor[2] = (float)scissor.Right() / pixel.x;
        params.scissor[3] = (float)scissor.Bottom() / pixel.y;
    }
    primObject.applyUniformBlock(primParamsSlot, sizeof(HtmlUIPrimParams), &params);
    primObject.applyUniformBlock(primClipSlot, sizeof(ClipParams), const_cast<ClipParams*>(&clipParams));
    if (primSlotsSlot >= 0)
        primObject.applyUniformBlock(primSlotsSlot, sizeof(SlotParams), &slotParams);
    primObject.draw(0, 6, count);
    stats.drawCalls++;
    stats.instancesDrawn += count;
    noteColorWrite();
    return true;
}

//-------------------------------------------------------------------------------------------
// planning: clip scopes
//-------------------------------------------------------------------------------------------

void RmlRenderer::planClipScopes(){
    clipScopes.clear();
    scopeOfCommand.assign(commands.size(), -1);
    const Rml::Vector2f pixel = getPixelScale();
    const Rml::Rectanglei full = Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(targetSize.x, 1), std::max(targetSize.y, 1)));

    auto matrixOf = [this](int index) -> const Rml::Matrix4f* {
        return index < 0 ? nullptr : &commandMatrices[(size_t)index];
    };

    struct Mask {
        Rml::ClipMaskOperation operation;
        int shape;              // in commandClipShapes, -1: none
        Rml::Vector2f translation;
        int transform;          // in commandMatrices, -1: none
    };
    std::vector<Mask> masks;
    std::vector<ShapeRegion> regions;

    int transformIndex = -1;
    bool scissorOn = false;
    Rml::Rectanglei scissor = toTargetRect(frameStartScissor, contextSize, targetSize);
    const size_t count = commands.size();
    for (size_t ci = 0; ci < count; ci++){
        const Command& start = commands[ci];
        if (start.type == CommandType::TRANSFORM){
            transformIndex = start.flag ? start.i[0] : -1;
            continue;
        }
        if (start.type == CommandType::SCISSOR_ENABLE){
            scissorOn = start.flag != 0;
            continue;
        }
        if (start.type == CommandType::SCISSOR_REGION){
            scissor = Rml::Rectanglei::FromCorners(Rml::Vector2i(start.i[0], start.i[1]), Rml::Vector2i(start.i[2], start.i[3]));
            continue;
        }
        if (start.type != CommandType::CLIP_ENABLE || !start.flag)
            continue;

        // the mask list: the clip masks right after it, each under its own transform
        masks.clear();
        size_t k = ci + 1;
        int t = transformIndex;
        for (; k < count; k++){
            const Command& c = commands[k];
            if (c.type == CommandType::TRANSFORM){
                t = c.flag ? c.i[0] : -1;
                continue;
            }
            if (c.type != CommandType::CLIP_MASK)
                break;
            masks.push_back(Mask{(Rml::ClipMaskOperation)c.flag, c.i[0] - 1, Rml::Vector2f(c.x, c.y), t});
        }

        ClipScope scope;
        scope.masks = (uint32_t)masks.size();
        bool elidable = batching && !masks.empty();
        bool analytic = primitivesFrame && !masks.empty() && masks.size() <= 2 && masks[0].operation == Rml::ClipMaskOperation::Set &&
                        (masks.size() == 1 || masks[1].operation == Rml::ClipMaskOperation::Intersect);
        regions.clear();
        for (const Mask& mask : masks){
            if (mask.shape < 0 || mask.operation == Rml::ClipMaskOperation::SetInverse){
                elidable = analytic = false;
                break;
            }
            const AxisMap map = axisMapOf(matrixOf(mask.transform));
            if (!map.valid){
                elidable = false;
                continue;
            }
            const ClipShapeRecord& shape = commandClipShapes[(size_t)mask.shape];
            const float x0 = shape.rect.Left() + mask.translation.x, y0 = shape.rect.Top() + mask.translation.y;
            const float x1 = shape.rect.Right() + mask.translation.x, y1 = shape.rect.Bottom() + mask.translation.y;
            ShapeRegion region;
            region.rect = mapRect(map, pixel, x0, y0, x1, y1);
            region.aligned[0] = onPixel(region.rect.x0);
            region.aligned[1] = onPixel(region.rect.y0);
            region.aligned[2] = onPixel(region.rect.x1);
            region.aligned[3] = onPixel(region.rect.y1);
            // corner boxes in document px: top-left, top-right, bottom-right, bottom-left
            const Rml::Vector2f* r = shape.radii;
            const float boxes[4][4] = {
                {x0, y0, x0 + r[0].x, y0 + r[0].y},
                {x1 - r[1].x, y0, x1, y0 + r[1].y},
                {x1 - r[2].x, y1 - r[2].y, x1, y1},
                {x0, y1 - r[3].y, x0 + r[3].x, y1},
            };
            for (int i = 0; i < 4; i++){
                region.hasCorner[i] = r[i].x > 0.0f && r[i].y > 0.0f;
                if (region.hasCorner[i])
                    region.corners[i] = mapRect(map, pixel, boxes[i][0], boxes[i][1], boxes[i][2], boxes[i][3]);
            }
            regions.push_back(region);
        }

        // the content: up to the next clip change
        int contentTransform = t;
        bool contentScissorOn = scissorOn;
        Rml::Rectanglei contentScissor = scissor;
        auto admitted = [&](const RectF& draw){
            for (const ShapeRegion& region : regions){
                if (!region.admits(draw))
                    return false;
            }
            return true;
        };
        auto checkBounds = [&](const Rml::Rectanglef& b, Rml::Vector2f translation){
            if (analytic){
                for (const Mask& mask : masks){
                    if (!sameMatrix(matrixOf(mask.transform), matrixOf(contentTransform)))
                        analytic = false;
                }
            }
            if (!elidable)
                return;
            const AxisMap map = axisMapOf(matrixOf(contentTransform));
            if (!map.valid){
                elidable = false;
                return;
            }
            if (!admitted(mapRect(map, pixel, b.Left() + translation.x, b.Top() + translation.y, b.Right() + translation.x,
                                  b.Bottom() + translation.y)))
                elidable = false;
        };
        auto checkGeometry = [&](RmlGeometry* geometry, Rml::Vector2f translation){
            if (geometry && !geometry->vertices.empty() && !geometry->indices.empty())
                checkBounds(geometryBounds(*geometry), translation);
        };
        for (; k < count && (elidable || analytic); k++){
            const Command& c = commands[k];
            if (c.type == CommandType::CLIP_ENABLE)
                break;
            switch (c.type){
            case CommandType::TRANSFORM:
                contentTransform = c.flag ? c.i[0] : -1;
                break;
            case CommandType::SCISSOR_ENABLE:
                contentScissorOn = c.flag != 0;
                break;
            case CommandType::SCISSOR_REGION:
                contentScissor = Rml::Rectanglei::FromCorners(Rml::Vector2i(c.i[0], c.i[1]), Rml::Vector2i(c.i[2], c.i[3]));
                break;
            case CommandType::GEOMETRY:
                checkGeometry(reinterpret_cast<RmlGeometry*>(c.a), Rml::Vector2f(c.x, c.y));
                break;
            case CommandType::PRIMITIVES:
                if (c.i[1] > 0)
                    checkBounds(primitiveBounds((uint32_t)c.i[0], (uint32_t)c.i[1]), Rml::Vector2f(c.x, c.y));
                break;
            case CommandType::SHADER:
                // gradients and effects draw their geometry with their own programs: no analytic clip
                analytic = false;
                if (!htmlui::EffectRenderer::isEffectHandle(c.a))
                    checkGeometry(reinterpret_cast<RmlGeometry*>(c.b), Rml::Vector2f(c.x, c.y));
                else
                    elidable = false;
                break;
            case CommandType::COMPOSITE: {
                analytic = false;
                // a composite draws within the scissor; a filter that spreads may reach the corners
                for (int f = 0; f < c.i[3]; f++){
                    if (filterSpreads(commandFilters[(size_t)(c.i[2] + f)]))
                        elidable = false;
                }
                const Rml::Rectanglei s = contentScissorOn ? contentScissor : full;
                RectF r;
                r.x0 = (float)s.Left();
                r.y0 = (float)s.Top();
                r.x1 = (float)s.Right();
                r.y1 = (float)s.Bottom();
                if (!admitted(r))
                    elidable = false;
                break;
            }
            case CommandType::PUSH_LAYER:
            case CommandType::POP_LAYER:
            case CommandType::SAVE_TEXTURE:
            case CommandType::SAVE_MASK:
                // no stencil test of their own; what they hold was drawn by the scope's checked draws
                analytic = false;
                break;
            case CommandType::CLIP_MASK:
                elidable = analytic = false;
                break;
            case CommandType::CLIP_ENABLE:
                break;
            }
        }

        if (elidable && batching){
            scope.mode = ScopeMode::ELIDED;
            // the shapes' bounds, out to whole pixels (inside them every draw lies, or the edge is on a pixel)
            float x0 = (float)full.Left(), y0 = (float)full.Top(), x1 = (float)full.Right(), y1 = (float)full.Bottom();
            for (const ShapeRegion& region : regions){
                x0 = std::max(x0, std::floor(region.rect.x0 + pixelEpsilon));
                y0 = std::max(y0, std::floor(region.rect.y0 + pixelEpsilon));
                x1 = std::min(x1, std::ceil(region.rect.x1 - pixelEpsilon));
                y1 = std::min(y1, std::ceil(region.rect.y1 - pixelEpsilon));
            }
            const Rml::Vector2i p0((int)x0, (int)y0);
            scope.scissor = Rml::Rectanglei::FromCorners(p0, Rml::Math::Max(p0, Rml::Vector2i((int)x1, (int)y1)));
        }else if (analytic && primitivesFrame){
            scope.mode = ScopeMode::ANALYTIC;
            scope.clip.params[0] = (float)masks.size();
            for (size_t m = 0; m < masks.size(); m++){
                const ClipShapeRecord& shape = commandClipShapes[(size_t)masks[m].shape];
                scope.clip.rect[m][0] = shape.rect.Left() + masks[m].translation.x;
                scope.clip.rect[m][1] = shape.rect.Top() + masks[m].translation.y;
                scope.clip.rect[m][2] = shape.rect.Width();
                scope.clip.rect[m][3] = shape.rect.Height();
                for (int corner = 0; corner < 4; corner++){
                    scope.clip.radiiX[m][corner] = shape.radii[corner].x;
                    scope.clip.radiiY[m][corner] = shape.radii[corner].y;
                }
            }
        }
        scopeOfCommand[ci] = (int32_t)clipScopes.size();
        clipScopes.push_back(scope);
    }
}

//-------------------------------------------------------------------------------------------
// planning: runs
//-------------------------------------------------------------------------------------------

void RmlRenderer::planRuns(){
    runBreaks.fill(0);
    effectCounts = EffectCounts();
    for (const Command& c : commands){
        if (c.type == CommandType::SHADER)
            effectCounts.shaders++;
        else if (c.type == CommandType::COMPOSITE){
            effectCounts.composites++;
            effectCounts.filters += (uint32_t)std::max(c.i[3], 0);
        }else if (c.type == CommandType::CLIP_MASK)
            effectCounts.clipMasks++;
    }
    batchRuns.clear();
    batchChunks.clear();
    batchMembers.clear();
    replayOps.clear();
    pendingChunks.clear();
    nextRunCache.clear();
    for (std::unique_ptr<GatherPage>& page : gatherPages){
        page->liveBytes = 0;
        page->written = false;
    }

    const Rml::Vector2f pixel = getPixelScale();
    const Rml::Rectanglei full = Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(targetSize.x, 1), std::max(targetSize.y, 1)));
    const bool instancing = primitivesFrame;

    // the replay's state as the plan goes
    int layer = 0;
    int transformIndex = -1;
    // 0: no transform (or one that only translates, folded into the members' translation)
    uint32_t transformId = 0;
    const AxisMap identityMap = axisMapOf(nullptr);
    AxisMap transformMap = identityMap;
    struct Shift {
        bool valid = false;
        Rml::Vector2f offset;
    } transformShift;
    bool scissorOn = false;
    Rml::Rectanglei scissor = toTargetRect(frameStartScissor, contextSize, targetSize);
    bool stencil = false;
    uint8_t ref = 0;
    int scope = -1;          // the current scope, any mode
    BatchRun* run = nullptr;
    uint32_t chunkItemsUsed = 0;
    uint32_t transformIds = 0;

    // the run's chunks: found in the cache or left to gather, with its final texture
    auto flush = [&](){
        if (!run)
            return;
        for (uint32_t c = run->firstChunk; c < run->firstChunk + run->chunkCount; c++){
            BatchChunk& chunk = batchChunks[c];
            uint64_t hash = mix(mix(0x51ed270b27ull, (uint64_t)run->kind), (uint64_t)(uintptr_t)run->texture);
            for (HtmlUITexture* slot : run->slots)
                hash = mix(hash, (uint64_t)(uintptr_t)slot);
            for (uint32_t m = chunk.first; m < chunk.first + chunk.members; m++){
                const BatchMember& member = batchMembers[m];
                hash = mix(hash, (uint64_t)(uintptr_t)member.geometry);
                hash = mix(hash, member.generation);
                hash = mix(hash, member.count);
                hash = mix(hash, floatBits(member.translation.x) | (floatBits(member.translation.y) << 32));
                hash = mix(hash, (uint64_t)(uintptr_t)member.texture);
            }
            chunk.hash = hash;
            auto matches = [&](const CachedChunk& cached){
                if (cached.kind != run->kind || cached.runTexture != run->texture || cached.runSlots != run->slots || cached.members.size() != chunk.members)
                    return false;
                return std::equal(cached.members.begin(), cached.members.end(), batchMembers.begin() + chunk.first);
            };
            const CachedChunk* found = nullptr;
            auto again = nextRunCache.find(hash);
            if (again != nextRunCache.end() && matches(again->second)){
                found = &again->second;
            }else{
                auto it = runCache.find(hash);
                if (it != runCache.end() && matches(it->second) && it->second.page < gatherPages.size()){
                    found = &(nextRunCache[hash] = std::move(it->second));
                    runCache.erase(it);
                }
            }
            if (found){
                chunk.hit = true;
                chunk.page = found->page;
                chunk.dataFirst = found->vertexFirst;
                chunk.dataCount = found->vertexCount;
                chunk.firstIndex = found->first;
                chunk.count = found->count;
                gatherPages[found->page]->liveBytes += chunk.bytes;
            }else{
                pendingChunks.push_back(c);
            }
        }
        run = nullptr;
        chunkItemsUsed = 0;
    };
    auto emit = [&](uint32_t command){
        replayOps.push_back(ReplayOp{ReplayOp::Kind::COMMAND, command, -1});
    };

    // a geometry's vertices or a quad list's or primitives' instances into the current run (a new
    // one when it does not join it); localBounds: document px before the translation
    auto add = [&](RunKind kind, const BatchMember& source, const Rml::Rectanglef& localBounds, uint32_t vertices, uint32_t indices){
        HtmlUITexture* texture = source.texture;

        // the scissor it is drawn with: clipped away, nothing to draw
        const int analyticScope = (scope >= 0 && clipScopes[(size_t)scope].mode == ScopeMode::ANALYTIC) ? scope : -1;
        // a transform that only translates is folded into the translation, so the geometry joins
        // the runs around it (translateX centring, slides); not under an analytic clip, whose shapes
        // are in the space before the transform
        BatchMember member = source;
        const bool folded = transformShift.valid && analyticScope < 0;
        if (folded)
            member.translation += transformShift.offset;
        const Rml::Vector2f translation = member.translation;
        const uint32_t runTransformId = folded ? 0u : transformId;
        const AxisMap& map = folded ? identityMap : transformMap;

        Rml::Rectanglei own = scissorOn ? scissor : full;
        if (scope >= 0 && clipScopes[(size_t)scope].mode == ScopeMode::ELIDED){
            const Rml::Rectanglei clip = clipScopes[(size_t)scope].scissor;
            const Rml::Vector2i p0 = Rml::Math::Max(own.p0, clip.p0);
            own = Rml::Rectanglei::FromCorners(p0, Rml::Math::Max(p0, Rml::Math::Min(own.p1, clip.p1)));
        }
        if (own.Width() <= 0 || own.Height() <= 0)
            return;
        // R4: inside its scissor (a translate/scale transform), it may share a run with another scissor
        bool free = false;
        RectF bounds;
        if (map.valid){
            bounds = mapRect(map, pixel, localBounds.Left() + translation.x, localBounds.Top() + translation.y,
                             localBounds.Right() + translation.x, localBounds.Bottom() + translation.y);
            free = insideRect(bounds, own);
        }

        // why it does not join the current run (RunBreak), NONE when it does
        auto joins = [&]() -> RunBreak {
            if (!run)
                return RunBreak::COMMAND;
            if (run->kind != kind)
                return RunBreak::KIND;
            if (run->layer != layer || run->stencil != stencil || (stencil && run->stencilRef != ref) || run->scope != analyticScope)
                return RunBreak::CLIP;
            if (run->transformId != runTransformId)
                return RunBreak::TRANSFORM;
            if (kind == RunKind::INSTANCES){
                // an untextured instance draws with any textures bound; a textured one takes a slot
                // of the run's four (its own texture's, or a free one)
                if (texture && run->slotCount == run->slots.size() &&
                    std::find(run->slots.begin(), run->slots.end(), texture) == run->slots.end())
                    return RunBreak::TEXTURE;
            }else if (texture != run->texture){
                if (!texture){
                    // untextured vertices read the run texture's white texels
                    if (!run->texture->hasWhiteTexel)
                        return RunBreak::TEXTURE;
                }else if (run->texture || !run->allUntextured || !texture->hasWhiteTexel){
                    return RunBreak::TEXTURE;
                }
            }
            bool inside = true;
            if (run->scissorSet){
                inside = free ? insideRect(bounds, run->scissor) : own == run->scissor;
            }else if (!free && run->freeBoundsValid){
                RectF u;
                u.x0 = run->freeBounds.Left();
                u.y0 = run->freeBounds.Top();
                u.x1 = run->freeBounds.Right();
                u.y1 = run->freeBounds.Bottom();
                inside = insideRect(u, own);
            }
            return inside ? RunBreak::NONE : RunBreak::SCISSOR;
        };
        const RunBreak reason = joins();
        if (reason != RunBreak::NONE){
            runBreaks[(size_t)reason]++;
            flush();
            batchRuns.emplace_back();
            run = &batchRuns.back();
            run->kind = kind;
            run->layer = layer;
            run->stencil = stencil;
            run->stencilRef = ref;
            run->scope = analyticScope;
            run->transformId = runTransformId;
            run->transform = (folded || transformIndex < 0) ? projection : projection * commandMatrices[(size_t)transformIndex];
            run->firstChunk = (uint32_t)batchChunks.size();
            run->firstMember = (uint32_t)batchMembers.size();
            replayOps.push_back(ReplayOp{ReplayOp::Kind::RUN, (uint32_t)(batchRuns.size() - 1), -1});
        }
        if (texture){
            run->allUntextured = false;
            if (kind == RunKind::INSTANCES){
                if (std::find(run->slots.begin(), run->slots.begin() + run->slotCount, texture) == run->slots.begin() + run->slotCount)
                    run->slots[run->slotCount++] = texture;
            }else{
                run->texture = texture;
            }
        }
        if (free){
            const Rml::Rectanglef b = Rml::Rectanglef::FromCorners(Rml::Vector2f(bounds.x0, bounds.y0), Rml::Vector2f(bounds.x1, bounds.y1));
            run->freeBounds = run->freeBoundsValid ? run->freeBounds.Join(b) : b;
            run->freeBoundsValid = true;
        }else if (!run->scissorSet){
            run->scissorSet = true;
            run->scissor = own;
        }

        // the chunk it goes in (instances: vertices counts them)
        if (run->chunkCount == 0 || batchChunks.back().members >= chunkMembers || chunkItemsUsed + vertices > chunkVertices){
            batchChunks.emplace_back();
            BatchChunk& chunk = batchChunks.back();
            chunk.run = (uint32_t)(batchRuns.size() - 1);
            chunk.first = (uint32_t)batchMembers.size();
            run->chunkCount++;
            chunkItemsUsed = 0;
        }
        BatchChunk& chunk = batchChunks.back();
        chunk.members++;
        chunk.vertices += vertices;
        chunk.indices += indices;
        chunk.bytes += (kind == RunKind::INSTANCES) ? vertices * (uint32_t)sizeof(HtmlUIPrimitive)
                                                    : vertices * (uint32_t)sizeof(Rml::Vertex) + indices * 4u;
        chunkItemsUsed += vertices;
        batchMembers.push_back(member);
        run->members++;
    };

    const size_t count = commands.size();
    for (size_t ci = 0; ci < count; ci++){
        const Command& c = commands[ci];
        switch (c.type){
        case CommandType::TRANSFORM: {
            const int next = c.flag ? c.i[0] : -1;
            if (!sameMatrix(next < 0 ? nullptr : &commandMatrices[(size_t)next], transformIndex < 0 ? nullptr : &commandMatrices[(size_t)transformIndex])){
                transformId = ++transformIds;
                transformMap = axisMapOf(next < 0 ? nullptr : &commandMatrices[(size_t)next]);
                transformShift = Shift();
                Rml::Vector2f offset;
                if (next >= 0 && translationOf(commandMatrices[(size_t)next], offset)){
                    transformShift.valid = true;
                    transformShift.offset = offset;
                }else if (next < 0){
                    transformId = 0;
                }
            }
            transformIndex = next;
            emit((uint32_t)ci);
            break;
        }
        case CommandType::SCISSOR_ENABLE:
            scissorOn = c.flag != 0;
            emit((uint32_t)ci);
            break;
        case CommandType::SCISSOR_REGION:
            scissor = Rml::Rectanglei::FromCorners(Rml::Vector2i(c.i[0], c.i[1]), Rml::Vector2i(c.i[2], c.i[3]));
            emit((uint32_t)ci);
            break;
        case CommandType::CLIP_ENABLE: {
            scope = c.flag ? scopeOfCommand[ci] : -1;
            const bool stencilScope = scope >= 0 && clipScopes[(size_t)scope].mode == ScopeMode::STENCIL;
            stencil = stencilScope;
            replayOps.push_back(ReplayOp{ReplayOp::Kind::COMMAND, (uint32_t)ci, scope});
            break;
        }
        case CommandType::CLIP_MASK: {
            // drawn only for a stencil scope (its masks write the stencil: no run crosses them)
            if (scope >= 0 && clipScopes[(size_t)scope].mode != ScopeMode::STENCIL)
                break;
            flush();
            switch ((Rml::ClipMaskOperation)c.flag){
            case Rml::ClipMaskOperation::Set:
            case Rml::ClipMaskOperation::SetInverse:
                ref = 1;
                break;
            case Rml::ClipMaskOperation::Intersect:
                ref = (uint8_t)std::min(255, ref + 1);
                break;
            }
            emit((uint32_t)ci);
            break;
        }
        case CommandType::GEOMETRY: {
            RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(c.a);
            if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
                break;
            if (geometry->vertices.size() > maxGatheredVertices){
                flush();
                emit((uint32_t)ci);
                break;
            }
            BatchMember member;
            member.geometry = geometry;
            member.generation = geometry->generation;
            member.translation = Rml::Vector2f(c.x, c.y);
            member.texture = reinterpret_cast<HtmlUITexture*>(c.b);
            const Rml::Rectanglef bounds = geometryBounds(*geometry);
            // R10: a quad list draws as instances, joining the boxes and glyphs around it
            if (instancing && isQuadList(*geometry)){
                member.count = (uint32_t)geometry->instances.size();
                add(RunKind::INSTANCES, member, bounds, member.count, 0);
            }else{
                add(RunKind::VERTICES, member, bounds, (uint32_t)geometry->vertices.size(), (uint32_t)geometry->indices.size());
            }
            break;
        }
        case CommandType::PRIMITIVES: {
            if (c.i[1] <= 0)
                break;
            BatchMember member;
            member.generation = (uint64_t)(uint32_t)c.i[2] | ((uint64_t)(uint32_t)c.i[3] << 32);
            member.first = (uint32_t)c.i[0];
            member.count = (uint32_t)c.i[1];
            member.translation = Rml::Vector2f(c.x, c.y);
            add(RunKind::INSTANCES, member, primitiveBounds(member.first, member.count), member.count, 0);
            break;
        }
        case CommandType::PUSH_LAYER:
            flush();
            layer++;
            emit((uint32_t)ci);
            break;
        case CommandType::POP_LAYER:
            flush();
            layer = std::max(0, layer - 1);
            emit((uint32_t)ci);
            break;
        case CommandType::COMPOSITE:
        case CommandType::SAVE_TEXTURE:
        case CommandType::SAVE_MASK:
        case CommandType::SHADER:
            flush();
            emit((uint32_t)ci);
            break;
        }
    }
    flush();

    // compaction: a page whose live chunks fill less than half of it gives them up (they are
    // gathered again, into pages without live chunks), so that dead space stays bounded
    for (size_t p = 0; p < gatherPages.size(); p++){
        GatherPage& page = *gatherPages[p];
        const size_t used = page.dataTop + page.indexTop;
        if (page.liveBytes == 0 || used < gatherPageBytes / 8 || (float)page.liveBytes >= minimumLiveShare * (float)used)
            continue;
        for (uint32_t c = 0; c < (uint32_t)batchChunks.size(); c++){
            BatchChunk& chunk = batchChunks[c];
            if (chunk.hit && chunk.page == (uint32_t)p){
                chunk.hit = false;
                nextRunCache.erase(chunk.hash);
                pendingChunks.push_back(c);
            }
        }
        page.liveBytes = 0;
    }
    std::sort(pendingChunks.begin(), pendingChunks.end());
}

Rml::Rectanglef RmlRenderer::primitiveBounds(uint32_t first, uint32_t count) const{
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    for (uint32_t i = 0; i < count; i++){
        const HtmlUIPrimitive& p = commandPrimitives[first + i];
        // what the instance can touch: a box its border box, an outer shadow its shape and 3 sigma
        // of blur (the shader widens sigma by a pixel's box filter), an inset one its clip box; and
        // a pixel of antialiased edge around each
        const float* r = (p.params[0] == (float)HtmlUIPrimitive::SHADOW_INSET) ? p.d : p.a;
        const float reach = 1.0f + ((p.params[0] == (float)HtmlUIPrimitive::SHADOW_OUTER) ? 3.0f * (p.params[1] + 0.5f) : 0.0f);
        const float a0 = r[0] - reach, b0 = r[1] - reach, a1 = r[0] + r[2] + reach, b1 = r[1] + r[3] + reach;
        if (i == 0){
            x0 = a0;
            y0 = b0;
            x1 = a1;
            y1 = b1;
        }else{
            x0 = std::min(x0, a0);
            y0 = std::min(y0, b0);
            x1 = std::max(x1, a1);
            y1 = std::max(y1, b1);
        }
    }
    return Rml::Rectanglef::FromCorners(Rml::Vector2f(x0, y0), Rml::Vector2f(x1, y1));
}

//-------------------------------------------------------------------------------------------
// gathering
//-------------------------------------------------------------------------------------------

void RmlRenderer::uploadGathered(){
    const uint64_t frame = SystemRender::getFrameCount();
    // pages without chunks drawn this frame start over (one not updated this frame yet: sokol
    // takes one update per buffer and frame)
    for (std::unique_ptr<GatherPage>& page : gatherPages){
        page->writable = page->liveBytes == 0 && page->updateFrame != frame;
        if (page->writable){
            page->dataTop = 0;
            page->indexTop = 0;
        }
    }

    size_t current[2] = {SIZE_MAX, SIZE_MAX};   // by run kind
    // what is left to gather, by run kind: a new page holds all of it (one draw per run), at least
    // a page's size
    size_t remainingData[2] = {0, 0}, remainingIndex[2] = {0, 0};
    for (uint32_t c : pendingChunks){
        const BatchChunk& chunk = batchChunks[c];
        const bool instances = batchRuns[chunk.run].kind == RunKind::INSTANCES;
        remainingData[instances ? 1 : 0] += (size_t)chunk.vertices * (instances ? sizeof(HtmlUIPrimitive) : sizeof(Rml::Vertex));
        remainingIndex[instances ? 1 : 0] += instances ? 0 : (size_t)chunk.indices * 4u;
    }
    auto pageFor = [&](RunKind kind, size_t dataBytes, size_t indexBytes) -> size_t {
        auto fits = [&](const GatherPage& page){
            return page.kind == kind && page.writable && page.dataTop + dataBytes <= page.dataCapacity &&
                   page.indexTop + indexBytes <= page.indexCapacity;
        };
        size_t& last = current[kind == RunKind::INSTANCES ? 1 : 0];
        if (last != SIZE_MAX && fits(*gatherPages[last]))
            return last;
        for (size_t p = 0; p < gatherPages.size(); p++){
            if (fits(*gatherPages[p]))
                return last = p;
        }
        std::unique_ptr<GatherPage> page = std::make_unique<GatherPage>();
        page->kind = kind;
        const int k = (kind == RunKind::INSTANCES) ? 1 : 0;
        page->dataCapacity = std::max(gatherPageBytes, std::max(dataBytes, remainingData[k]));
        // (UI meshes have about 1.5 indices a vertex: 6 index bytes to 20 vertex bytes)
        page->indexCapacity = (kind == RunKind::VERTICES) ? std::max(gatherPageBytes / 4, std::max(indexBytes, remainingIndex[k])) : 0;
        page->dataShadow.resize(page->dataCapacity);
        page->indexShadow.resize(page->indexCapacity);
        page->writable = true;
        gpuCreateCount++;
        if (!page->data.createBuffer((unsigned int)page->dataCapacity, nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM)){
            Log::error("[HTML UI] could not create a %u-byte gather buffer", (unsigned int)page->dataCapacity);
            return SIZE_MAX;
        }
        if (page->indexCapacity > 0){
            gpuCreateCount++;
            if (!page->indices.createBuffer((unsigned int)page->indexCapacity, nullptr, BufferType::INDEX_BUFFER, BufferUsage::STREAM)){
                Log::error("[HTML UI] could not create a %u-byte gather buffer", (unsigned int)page->indexCapacity);
                page->data.destroyBuffer();
                return SIZE_MAX;
            }
        }
        gatherPages.push_back(std::move(page));
        return last = gatherPages.size() - 1;
    };

    for (uint32_t c : pendingChunks){
        BatchChunk& chunk = batchChunks[c];
        const BatchRun* run = &batchRuns[chunk.run];
        const bool instances = run->kind == RunKind::INSTANCES;
        const size_t dataBytes = (size_t)chunk.vertices * (instances ? sizeof(HtmlUIPrimitive) : sizeof(Rml::Vertex));
        const size_t indexBytes = instances ? 0 : (size_t)chunk.indices * 4u;
        const size_t p = pageFor(run->kind, dataBytes, indexBytes);
        remainingData[instances ? 1 : 0] -= dataBytes;
        remainingIndex[instances ? 1 : 0] -= indexBytes;
        if (p == SIZE_MAX){
            chunk.count = 0;
            chunk.dataCount = 0;
            continue;
        }
        GatherPage& page = *gatherPages[p];
        page.written = true;
        chunk.page = (uint32_t)p;

        if (instances){
            HtmlUIPrimitive* out = reinterpret_cast<HtmlUIPrimitive*>(page.dataShadow.data() + page.dataTop);
            chunk.dataFirst = (uint32_t)(page.dataTop / sizeof(HtmlUIPrimitive));
            chunk.firstIndex = 0;
            for (uint32_t m = chunk.first; m < chunk.first + chunk.members; m++){
                const BatchMember& member = batchMembers[m];
                const HtmlUIPrimitive* in = member.geometry ? member.geometry->instances.data() : commandPrimitives.data() + member.first;
                float textured = 0.0f;
                for (size_t k = 0; k < run->slots.size(); k++){
                    if (member.texture && run->slots[k] == member.texture)
                        textured = (float)(k + 1);
                }
                for (uint32_t i = 0; i < member.count; i++){
                    *out = in[i];
                    translateInstance(*out, member.translation);
                    out->params[2] = textured;
                    out++;
                }
            }
            chunk.dataCount = chunk.vertices;
            chunk.count = chunk.vertices;
        }else{
            // untextured members of a textured run read its white texels
            const HtmlUITexture* white = (run->texture && run->texture->hasWhiteTexel) ? run->texture : nullptr;
            Rml::Vertex* out = reinterpret_cast<Rml::Vertex*>(page.dataShadow.data() + page.dataTop);
            uint32_t* indices = reinterpret_cast<uint32_t*>(page.indexShadow.data() + page.indexTop);
            uint32_t base = (uint32_t)(page.dataTop / sizeof(Rml::Vertex));
            chunk.dataFirst = base;
            chunk.firstIndex = (uint32_t)(page.indexTop / 4u);
            for (uint32_t m = chunk.first; m < chunk.first + chunk.members; m++){
                const BatchMember& member = batchMembers[m];
                const Rml::Span<const Rml::Vertex> vertices = member.geometry->vertices;
                const bool whiteUV = !member.texture && white;
                for (size_t v = 0; v < vertices.size(); v++){
                    Rml::Vertex vertex = vertices[v];
                    vertex.position += member.translation;
                    if (whiteUV)
                        vertex.tex_coord = Rml::Vector2f(white->whiteU, white->whiteV);
                    *out++ = vertex;
                }
                for (int index : member.geometry->indices)
                    *indices++ = base + (uint32_t)index;
                base += (uint32_t)vertices.size();
            }
            chunk.dataCount = chunk.vertices;
            chunk.count = chunk.indices;
        }
        page.dataTop += dataBytes;
        page.indexTop += indexBytes;
        page.liveBytes += chunk.bytes;
        stats.verticesGathered += chunk.vertices;

        CachedChunk& cached = nextRunCache[chunk.hash];
        cached.kind = run->kind;
        cached.runTexture = run->texture;
        cached.runSlots = run->slots;
        cached.page = chunk.page;
        cached.first = chunk.firstIndex;
        cached.count = chunk.count;
        cached.vertexFirst = chunk.dataFirst;
        cached.vertexCount = chunk.dataCount;
        cached.members.assign(batchMembers.begin() + chunk.first, batchMembers.begin() + chunk.first + chunk.members);
    }

    for (std::unique_ptr<GatherPage>& page : gatherPages){
        if (!page->written)
            continue;
        page->data.updateBuffer((unsigned int)page->dataTop, page->dataShadow.data());
        if (page->indexCapacity > 0)
            page->indices.updateBuffer((unsigned int)page->indexTop, page->indexShadow.data());
        page->updateFrame = frame;
    }

    // the last frame's chunks not drawn again are gone; free capacity beyond what this frame
    // uses is given back (the stream and the cache stay within twice the frame's gathered bytes)
    runCache.clear();
    runCache.swap(nextRunCache);
    size_t live = 0, capacity = 0;
    for (const std::unique_ptr<GatherPage>& page : gatherPages){
        live += page->liveBytes;
        capacity += page->dataCapacity + page->indexCapacity;
    }
    for (size_t p = gatherPages.size(); p-- > 0 && capacity > std::max(2 * live, gatherPageBytes * 2);){
        GatherPage& page = *gatherPages[p];
        if (page.liveBytes > 0 || page.updateFrame == frame)
            continue;
        capacity -= page.dataCapacity + page.indexCapacity;
        page.data.destroyBuffer();
        page.indices.destroyBuffer();
        gatherPages.erase(gatherPages.begin() + (long)p);
        // cached chunks keep their page index: the ones after it move down
        for (auto& entry : runCache){
            if (entry.second.page > p)
                entry.second.page--;
        }
        for (BatchChunk& chunk : batchChunks){
            if (chunk.page > p)
                chunk.page--;
        }
    }
}

void RmlRenderer::destroyGatherPages(){
    for (std::unique_ptr<GatherPage>& page : gatherPages){
        page->data.destroyBuffer();
        page->indices.destroyBuffer();
    }
    gatherPages.clear();
    runCache.clear();
    nextRunCache.clear();
}

//-------------------------------------------------------------------------------------------
// drawing the plan
//-------------------------------------------------------------------------------------------

void RmlRenderer::drawRun(const BatchRun& run){
    if (!bindTopLayer())
        return;
    const Rml::Rectanglei full = Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(targetSize.x, 1), std::max(targetSize.y, 1)));
    const Rml::Rectanglei scissor = run.scissorSet ? run.scissor : full;
    if (scissor.Width() <= 0 || scissor.Height() <= 0)
        return;

    StencilState stencil;
    if (run.stencil){
        stencil.enabled = true;
        stencil.compare = CompareFunc::EQUAL;
        stencil.failOp = StencilOp::KEEP;
        stencil.depthFailOp = StencilOp::KEEP;
        stencil.passOp = StencilOp::KEEP;
        stencil.readMask = 0xFF;
        stencil.writeMask = 0x00;
        stencil.ref = run.stencilRef;
    }
    const PipelineState state = layerState(BlendMode::PREMULTIPLIED, ColorWriteMask::RGBA, stencil);
    const int previousScope = activeClipScope;
    activeClipScope = run.scope;
    const ClipParams clipParams = currentClipParams(run.texture);
    activeClipScope = previousScope;

    // the run's chunks, contiguous ones of a page in one draw
    uint32_t c = run.firstChunk;
    const uint32_t end = run.firstChunk + run.chunkCount;
    const bool instances = run.kind == RunKind::INSTANCES;
    while (c < end){
        const BatchChunk& first = batchChunks[c];
        uint32_t members = first.members;
        uint32_t count = first.count;
        uint32_t next = c + 1;
        while (next < end && batchChunks[next].page == first.page && batchChunks[next].count > 0 &&
               (instances ? batchChunks[next].dataFirst == first.dataFirst + count : batchChunks[next].firstIndex == first.firstIndex + count)){
            count += batchChunks[next].count;
            members += batchChunks[next].members;
            next++;
        }
        c = next;
        if (count == 0 || first.page >= gatherPages.size())
            continue;
        GatherPage& page = *gatherPages[first.page];

        applyPassViewport(full);
        applyPassScissor(scissor);
        if (instances){
            if (!drawInstances(page.data, (size_t)first.dataFirst * sizeof(HtmlUIPrimitive), count, run.slots, run.transform, clipParams, state, scissor))
                return;
        }else{
            geometryObject.replaceVertexBuffer(&compositeVertexBuffer, &page.data, 0);
            geometryObject.setIndexBuffer(&page.indices, 0);
            geometryObject.addTexture(geometryTextureSlot, ShaderStageType::FRAGMENT, run.texture ? run.texture->render : &placeholderWhite);
            stats.textureBinds++;
            noteDrawn(run.texture);
            if (!geometryObject.beginDraw(state))
                return;
            HtmlUIParams params;
            std::memcpy(params.transform, run.transform.data(), sizeof(params.transform));
            params.translation[0] = params.translation[1] = params.translation[2] = params.translation[3] = 0.0f;
            geometryObject.applyUniformBlock(geometryParamsSlot, sizeof(HtmlUIParams), &params);
            geometryObject.applyUniformBlock(geometryClipSlot, sizeof(ClipParams), const_cast<ClipParams*>(&clipParams));
            geometryObject.draw(first.firstIndex, count, 1);
            stats.drawCalls++;
            noteColorWrite();
        }
        if (members > 1)
            stats.runsMerged++;
    }
}

void RmlRenderer::applyClipEnable(const Command& command, int32_t scope){
    // a scope without stencil: no stencil test, its scissor or analytic clip instead
    const bool stencilScope = scope < 0 || clipScopes[(size_t)scope].mode == ScopeMode::STENCIL;
    activeClipScope = stencilScope ? -1 : scope;
    clipMaskEnabled = command.flag != 0 && stencilScope;
    if (!stencilScope){
        stats.stencilClipsElided += clipScopes[(size_t)scope].masks;
        if (clipScopes[(size_t)scope].mode == ScopeMode::ANALYTIC)
            stats.analyticClips++;
    }
}

void RmlRenderer::executePlan(){
    for (const ReplayOp& op : replayOps){
        if (op.kind == ReplayOp::Kind::RUN){
            drawRun(batchRuns[op.index]);
            continue;
        }
        const Command& c = commands[op.index];
        if (c.type == CommandType::CLIP_ENABLE){
            applyClipEnable(c, op.scope);
            continue;
        }
        replayCommand(c);
    }
    activeClipScope = -1;
}

void RmlRenderer::replayScoped(){
    int32_t scope = -1;
    for (size_t ci = 0; ci < commands.size(); ci++){
        const Command& c = commands[ci];
        if (c.type == CommandType::CLIP_ENABLE){
            scope = c.flag ? scopeOfCommand[ci] : -1;
            applyClipEnable(c, scope);
            continue;
        }
        // the masks of a scope clipped without the stencil are not drawn
        if (c.type == CommandType::CLIP_MASK && scope >= 0 && clipScopes[(size_t)scope].mode != ScopeMode::STENCIL)
            continue;
        replayCommand(c);
    }
    activeClipScope = -1;
}

double RmlRenderer::getLastGatherMs() const{
    return lastGatherMs;
}

void RmlRenderer::clearRunCacheForTesting(){
    runCache.clear();
}

size_t RmlRenderer::getGatherLiveBytes() const{
    size_t live = 0;
    for (const std::unique_ptr<GatherPage>& page : gatherPages)
        live += page->liveBytes;
    return live;
}

size_t RmlRenderer::getGatherCapacityBytes() const{
    size_t capacity = 0;
    for (const std::unique_ptr<GatherPage>& page : gatherPages)
        capacity += page->dataCapacity + page->indexCapacity;
    return capacity;
}

void RmlRenderer::replayBatched(){
    const auto start = std::chrono::steady_clock::now();
    planClipScopes();
    planRuns();
    uploadGathered();
    lastGatherMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    for (const BatchChunk& chunk : batchChunks){
        if (chunk.hit)
            stats.runCacheHits++;
    }
    executePlan();
}
