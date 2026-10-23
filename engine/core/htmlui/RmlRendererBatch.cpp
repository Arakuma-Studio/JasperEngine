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
    constexpr size_t gatherPageBytes = 256 * 1024;
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

const RmlRenderer::ClipParams& RmlRenderer::currentClipParams(bool coverageTexture){
    const bool analytic = activeClipScope >= 0 && activeClipScope < (int)clipScopes.size() &&
                          clipScopes[(size_t)activeClipScope].mode == ScopeMode::ANALYTIC;
    if (!analytic && !coverageTexture)
        return noClip;
    clipScratch = analytic ? clipScopes[(size_t)activeClipScope].clip : noClip;
    clipScratch.params[1] = coverageTexture ? 1.0f : 0.0f;
    return clipScratch;
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
        bool elidable = !masks.empty();
        bool analytic = !masks.empty() && masks.size() <= 2 && masks[0].operation == Rml::ClipMaskOperation::Set &&
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
        auto checkGeometry = [&](RmlGeometry* geometry, Rml::Vector2f translation){
            if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
                return;
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
            const Rml::Rectanglef b = geometryBounds(*geometry);
            if (!admitted(mapRect(map, pixel, b.Left() + translation.x, b.Top() + translation.y, b.Right() + translation.x,
                                  b.Bottom() + translation.y)))
                elidable = false;
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

        if (elidable){
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
        }else if (analytic){
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

    // the replay's state as the plan goes
    int layer = 0;
    int transformIndex = -1;
    uint32_t transformId = 0;
    AxisMap transformMap = axisMapOf(nullptr);
    bool scissorOn = false;
    Rml::Rectanglei scissor = toTargetRect(frameStartScissor, contextSize, targetSize);
    bool stencil = false;
    uint8_t ref = 0;
    int scope = -1;          // the current scope, any mode
    BatchRun* run = nullptr;
    uint32_t chunkVerticesUsed = 0;

    // the run's chunks: found in the cache or left to gather, with its final texture
    auto flush = [&](){
        if (!run)
            return;
        for (uint32_t c = run->firstChunk; c < run->firstChunk + run->chunkCount; c++){
            BatchChunk& chunk = batchChunks[c];
            uint64_t hash = mix(mix(0x51ed270b27ull, (uint64_t)run->kind), (uint64_t)(uintptr_t)run->texture);
            for (uint32_t m = chunk.first; m < chunk.first + chunk.members; m++){
                const BatchMember& member = batchMembers[m];
                hash = mix(hash, (uint64_t)(uintptr_t)member.geometry);
                hash = mix(hash, member.generation);
                hash = mix(hash, floatBits(member.translation.x) | (floatBits(member.translation.y) << 32));
                hash = mix(hash, (uint64_t)(uintptr_t)member.texture);
            }
            chunk.hash = hash;
            auto matches = [&](const CachedChunk& cached){
                if (cached.kind != run->kind || cached.runTexture != run->texture || cached.members.size() != chunk.members)
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
        chunkVerticesUsed = 0;
    };
    auto emit = [&](uint32_t command){
        replayOps.push_back(ReplayOp{ReplayOp::Kind::COMMAND, command, -1});
    };

    const size_t count = commands.size();
    for (size_t ci = 0; ci < count; ci++){
        const Command& c = commands[ci];
        switch (c.type){
        case CommandType::TRANSFORM: {
            const int next = c.flag ? c.i[0] : -1;
            if (!sameMatrix(next < 0 ? nullptr : &commandMatrices[(size_t)next], transformIndex < 0 ? nullptr : &commandMatrices[(size_t)transformIndex])){
                transformId++;
                transformMap = axisMapOf(next < 0 ? nullptr : &commandMatrices[(size_t)next]);
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
            if (scope >= 0 && !stencilScope){
                stats.stencilClipsElided += clipScopes[(size_t)scope].masks;
                if (clipScopes[(size_t)scope].mode == ScopeMode::ANALYTIC)
                    stats.analyticClips++;
            }
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
            HtmlUITexture* texture = reinterpret_cast<HtmlUITexture*>(c.b);
            const Rml::Vector2f translation(c.x, c.y);

            // the scissor it is drawn with: clipped away, nothing to draw
            Rml::Rectanglei own = scissorOn ? scissor : full;
            const int analyticScope = (scope >= 0 && clipScopes[(size_t)scope].mode == ScopeMode::ANALYTIC) ? scope : -1;
            if (scope >= 0 && clipScopes[(size_t)scope].mode == ScopeMode::ELIDED){
                const Rml::Rectanglei clip = clipScopes[(size_t)scope].scissor;
                const Rml::Vector2i p0 = Rml::Math::Max(own.p0, clip.p0);
                own = Rml::Rectanglei::FromCorners(p0, Rml::Math::Max(p0, Rml::Math::Min(own.p1, clip.p1)));
            }
            if (own.Width() <= 0 || own.Height() <= 0)
                break;
            // R4: inside its scissor (a translate/scale transform), it may share a run with another scissor
            bool free = false;
            RectF bounds;
            if (transformMap.valid){
                const Rml::Rectanglef b = geometryBounds(*geometry);
                bounds = mapRect(transformMap, pixel, b.Left() + translation.x, b.Top() + translation.y, b.Right() + translation.x,
                                 b.Bottom() + translation.y);
                free = insideRect(bounds, own);
            }

            auto joins = [&](){
                if (!run || run->kind != RunKind::VERTICES || run->layer != layer || run->stencil != stencil ||
                    (stencil && run->stencilRef != ref) || run->transformId != transformId || run->scope != analyticScope)
                    return false;
                if (texture != run->texture){
                    if (!texture){
                        if (!run->texture->hasWhiteTexel)
                            return false;
                    }else if (run->texture || !run->allUntextured || !texture->hasWhiteTexel){
                        return false;
                    }
                }
                if (run->scissorSet)
                    return free ? insideRect(bounds, run->scissor) : own == run->scissor;
                if (free)
                    return true;
                if (!run->freeBoundsValid)
                    return true;
                RectF u;
                u.x0 = run->freeBounds.Left();
                u.y0 = run->freeBounds.Top();
                u.x1 = run->freeBounds.Right();
                u.y1 = run->freeBounds.Bottom();
                return insideRect(u, own);
            };
            if (!joins()){
                flush();
                batchRuns.emplace_back();
                run = &batchRuns.back();
                run->kind = RunKind::VERTICES;
                run->layer = layer;
                run->stencil = stencil;
                run->stencilRef = ref;
                run->scope = analyticScope;
                run->transformId = transformId;
                run->transform = transformIndex < 0 ? projection : projection * commandMatrices[(size_t)transformIndex];
                run->firstChunk = (uint32_t)batchChunks.size();
                run->firstMember = (uint32_t)batchMembers.size();
                replayOps.push_back(ReplayOp{ReplayOp::Kind::RUN, (uint32_t)(batchRuns.size() - 1), -1});
            }
            if (texture){
                run->texture = texture;
                run->allUntextured = false;
            }
            if (free){
                const Rml::Rectanglef b = Rml::Rectanglef::FromCorners(Rml::Vector2f(bounds.x0, bounds.y0), Rml::Vector2f(bounds.x1, bounds.y1));
                run->freeBounds = run->freeBoundsValid ? run->freeBounds.Join(b) : b;
                run->freeBoundsValid = true;
            }else if (!run->scissorSet){
                run->scissorSet = true;
                run->scissor = own;
            }

            // the chunk it goes in
            const uint32_t vertices = (uint32_t)geometry->vertices.size();
            if (run->chunkCount == 0 || batchChunks.back().members >= chunkMembers || chunkVerticesUsed + vertices > chunkVertices){
                batchChunks.emplace_back();
                BatchChunk& chunk = batchChunks.back();
                chunk.run = (uint32_t)(batchRuns.size() - 1);
                chunk.first = (uint32_t)batchMembers.size();
                run->chunkCount++;
                chunkVerticesUsed = 0;
            }
            BatchChunk& chunk = batchChunks.back();
            chunk.members++;
            chunk.vertices += vertices;
            chunk.indices += (uint32_t)geometry->indices.size();
            chunk.bytes += vertices * (uint32_t)sizeof(Rml::Vertex) + (uint32_t)geometry->indices.size() * 4u;
            chunkVerticesUsed += vertices;
            batchMembers.push_back(BatchMember{geometry, geometry->generation, translation, texture});
            run->members++;
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

    size_t current = SIZE_MAX;
    auto pageFor = [&](RunKind kind, size_t dataBytes, size_t indexBytes) -> size_t {
        auto fits = [&](const GatherPage& page){
            return page.kind == kind && page.writable && page.dataTop + dataBytes <= page.dataCapacity &&
                   page.indexTop + indexBytes <= page.indexCapacity;
        };
        if (current != SIZE_MAX && fits(*gatherPages[current]))
            return current;
        for (size_t p = 0; p < gatherPages.size(); p++){
            if (fits(*gatherPages[p]))
                return current = p;
        }
        std::unique_ptr<GatherPage> page = std::make_unique<GatherPage>();
        page->kind = kind;
        page->dataCapacity = std::max(gatherPageBytes, dataBytes);
        page->indexCapacity = (kind == RunKind::VERTICES) ? std::max(gatherPageBytes, indexBytes) : 0;
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
        return current = gatherPages.size() - 1;
    };

    for (uint32_t c : pendingChunks){
        BatchChunk& chunk = batchChunks[c];
        const BatchRun* run = &batchRuns[chunk.run];
        const size_t dataBytes = (size_t)chunk.vertices * sizeof(Rml::Vertex);
        const size_t indexBytes = (size_t)chunk.indices * 4u;
        const size_t p = pageFor(run->kind, dataBytes, indexBytes);
        if (p == SIZE_MAX){
            chunk.count = 0;
            continue;
        }
        GatherPage& page = *gatherPages[p];
        page.written = true;

        // untextured members of a textured run read its white texels
        const HtmlUITexture* white = (run->texture && run->texture->hasWhiteTexel) ? run->texture : nullptr;
        Rml::Vertex* out = reinterpret_cast<Rml::Vertex*>(page.dataShadow.data() + page.dataTop);
        uint32_t* indices = reinterpret_cast<uint32_t*>(page.indexShadow.data() + page.indexTop);
        uint32_t base = (uint32_t)(page.dataTop / sizeof(Rml::Vertex));
        chunk.page = (uint32_t)p;
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
        page.dataTop += dataBytes;
        page.indexTop += indexBytes;
        page.liveBytes += chunk.bytes;
        stats.verticesGathered += chunk.vertices;

        CachedChunk& cached = nextRunCache[chunk.hash];
        cached.kind = run->kind;
        cached.runTexture = run->texture;
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
    const bool coverage = run.texture && run.texture->sample == HtmlUITexture::Sample::COVERAGE;
    const int previousScope = activeClipScope;
    activeClipScope = run.scope;
    const ClipParams clipParams = currentClipParams(coverage);
    activeClipScope = previousScope;

    // the run's chunks, contiguous ones of a page in one draw
    uint32_t c = run.firstChunk;
    const uint32_t end = run.firstChunk + run.chunkCount;
    while (c < end){
        const BatchChunk& first = batchChunks[c];
        uint32_t members = first.members;
        uint32_t indexCount = first.count;
        uint32_t next = c + 1;
        while (next < end && batchChunks[next].page == first.page && batchChunks[next].firstIndex == first.firstIndex + indexCount &&
               batchChunks[next].count > 0){
            indexCount += batchChunks[next].count;
            members += batchChunks[next].members;
            next++;
        }
        c = next;
        if (indexCount == 0 || first.page >= gatherPages.size())
            continue;
        GatherPage& page = *gatherPages[first.page];

        applyPassViewport(full);
        applyPassScissor(scissor);
        geometryObject.replaceVertexBuffer(&compositeVertexBuffer, &page.data, 0);
        geometryObject.setIndexBuffer(&page.indices, 0);
        geometryObject.addTexture(geometryTextureSlot, ShaderStageType::FRAGMENT, run.texture ? run.texture->render : &placeholderWhite);
        stats.textureBinds++;
        if (!geometryObject.beginDraw(state))
            return;
        HtmlUIParams params;
        std::memcpy(params.transform, run.transform.data(), sizeof(params.transform));
        params.translation[0] = params.translation[1] = params.translation[2] = params.translation[3] = 0.0f;
        geometryObject.applyUniformBlock(geometryParamsSlot, sizeof(HtmlUIParams), &params);
        geometryObject.applyUniformBlock(geometryClipSlot, sizeof(ClipParams), const_cast<ClipParams*>(&clipParams));
        geometryObject.draw(first.firstIndex, indexCount, 1);
        stats.drawCalls++;
        if (members > 1)
            stats.runsMerged++;
        noteColorWrite();
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
            // a scope without stencil: no stencil test, its scissor or analytic clip instead
            const bool stencilScope = op.scope < 0 || clipScopes[(size_t)op.scope].mode == ScopeMode::STENCIL;
            activeClipScope = stencilScope ? -1 : op.scope;
            clipMaskEnabled = c.flag != 0 && stencilScope;
            continue;
        }
        replayCommand(c);
    }
    activeClipScope = -1;
}

void RmlRenderer::replayBatched(){
    planClipScopes();
    planRuns();
    uploadGathered();
    for (const BatchChunk& chunk : batchChunks){
        if (chunk.hit)
            stats.runCacheHits++;
    }
    executePlan();
}
