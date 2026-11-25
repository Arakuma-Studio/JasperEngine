// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S92 layer cache (docs/specs/S92-htmlui-render-cost.md R9, R30). A layer scope is what RmlUi
// records between a PUSH_LAYER and the COMPOSITE of that layer into the one under it: an
// element's filters, its mask-image or clip-path mask, a rounded background's mask, a text
// shadow's blur, a group opacity. Its result (the filtered layer within the composite's scissor)
// is kept as an image, keyed by the scope's signature taken relative to that window (D10: no
// element identity; an element that moves, or two identical ones, share it), its filters but a
// trailing opacity, the blend mode, the window's size, the sample count and the pixel ratio. A
// later replay recording the same scope draws nothing of it but the image (its clip masks only
// set the mask the composite is tested against). The trailing opacity is drawn at the composite,
// in a miss as in a hit, so a fade hits and both give the same pixels.

#include "RmlRenderer.h"
#include "RmlRendererInternal.h"

#include "RmlFilterContext.h"

#include "Log.h"
#include "effects/EffectRenderer.h"
#include "pool/ShaderPool.h"
#include "render/FramebufferRender.h"
#include "texture/Framebuffer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

using namespace jasperengine;

namespace {

    std::atomic<bool> layerCacheEnabled{true};
    std::atomic<size_t> layerCacheBudgetOverride{0};

    // free images kept for the next stores (an element that changes every frame stores every frame)
    constexpr size_t kLayerCacheKeepFree = 4;
    // the target plane's granules (P7-F2's region frames)
    constexpr int kLayerGranule = 64;

    uint64_t layerCacheMix(uint64_t h, uint64_t v){
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h * 0xff51afd7ed558ccdull;
    }

    uint64_t layerCacheFloat(float f){
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }

    uint64_t layerCachePair(int a, int b){
        return (uint64_t)(uint32_t)a | ((uint64_t)(uint32_t)b << 32);
    }

    bool layerCacheEmpty(Rml::Rectanglei rect){
        return !rect.Valid() || rect.Width() <= 0 || rect.Height() <= 0;
    }

    // the rect out to whole granules of the target plane
    uint64_t layerCacheGranulePixels(Rml::Rectanglei rect){
        if (layerCacheEmpty(rect))
            return 0;
        auto floorTo = [](int v){ return (v >= 0) ? (v / kLayerGranule) * kLayerGranule : -((-v + kLayerGranule - 1) / kLayerGranule) * kLayerGranule; };
        const int x0 = floorTo(rect.Left()), y0 = floorTo(rect.Top());
        const int x1 = -floorTo(-rect.Right()), y1 = -floorTo(-rect.Bottom());
        return (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
    }

    size_t layerCacheImageBytes(const Framebuffer& image){
        return (size_t)image.getWidth() * (size_t)image.getHeight() * 4u;
    }

    uint64_t layerCacheBytesHash(uint64_t h, const void* data, size_t size){
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        size_t i = 0;
        for (; i + 8 <= size; i += 8){
            uint64_t word;
            std::memcpy(&word, bytes + i, 8);
            h = layerCacheMix(h, word);
        }
        uint64_t tail = 0;
        if (size > i)
            std::memcpy(&tail, bytes + i, size - i);
        return layerCacheMix(h, tail ^ (uint64_t)size);
    }

    // a geometry by its vertices and indices (hashed once per compiled geometry): equal elements
    // have separate geometries
    uint64_t layerCacheGeometry(uintptr_t handle){
        RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
        if (!geometry)
            return 0;
        if (geometry->contentHashOf != geometry->generation){
            const uint64_t h = layerCacheBytesHash(0xc0a7e47ull, geometry->vertices.data(), geometry->vertices.size() * sizeof(Rml::Vertex));
            geometry->contentHash = layerCacheBytesHash(h, geometry->indices.data(), geometry->indices.size() * sizeof(int));
            geometry->contentHashOf = geometry->generation;
        }
        return geometry->contentHash;
    }

    uint64_t layerCacheMatrix(const Rml::Matrix4f& matrix){
        return layerCacheBytesHash(0x3a7215ull, matrix.data(), 16 * sizeof(float));
    }

}

void RmlRenderer::setLayerCacheForTesting(bool enabled){
    layerCacheEnabled = enabled;
}

bool RmlRenderer::isLayerCache(){
    return layerCacheEnabled;
}

void RmlRenderer::setLayerCacheBudgetForTesting(size_t bytes){
    layerCacheBudgetOverride = bytes;
}

size_t RmlRenderer::getLayerCacheBudget(){
    const size_t forced = layerCacheBudgetOverride;
    if (forced > 0)
        return forced;
    // mobile and the web first: a quarter of the desktop's
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    return (size_t)8 * 1024 * 1024;
#else
    return (size_t)32 * 1024 * 1024;
#endif
}

size_t RmlRenderer::getLayerCacheEntryCount() const{
    return layerCache.size();
}

size_t RmlRenderer::getLayerCacheBytes() const{
    return layerCacheBytes;
}

void RmlRenderer::noteLayerPass(Rml::Rectanglei full, TargetKind kind, int index){
    // layer 0 is the document target, not a layer of an element
    Rml::Rectanglei window;
    if (kind == TargetKind::LAYER){
        if (index <= 0 || index >= (int)frames.size())
            return;
        window = frames[(size_t)index].window;
        layerPassStats.layerPasses++;
    }else if (kind == TargetKind::POSTPROCESS){
        layerPassStats.postPasses++;
        const int layer = (filterSourceLayer >= 0) ? filterSourceLayer : layersSize - 1;
        window = (layer >= 0 && layer < (int)frames.size()) ? frames[(size_t)layer].window : standardFrame();
    }else{
        return;
    }
    const uint32_t area = (uint32_t)std::max(full.Width(), 0) * (uint32_t)std::max(full.Height(), 0);
    stats.layerPixels += area;
    layerPassStats.windowPixels += layerCacheGranulePixels(layerCacheEmpty(window) ? standardFrame() : window);
    layerPassStats.largestPass = std::max(layerPassStats.largestPass, area);
}

uint64_t RmlRenderer::layerCacheTextureHash(const uint8_t* data, size_t bytes, int width, int height, int filter, int wrapU, int wrapV,
                                           int format){
    uint64_t h = layerCacheBytesHash(0x7e8e1ull, data, bytes);
    h = layerCacheMix(h, layerCachePair(width, height));
    h = layerCacheMix(h, layerCachePair(filter, format));
    return layerCacheMix(h | 1u, layerCachePair(wrapU, wrapV));
}

uint64_t RmlRenderer::scopeTextureSignature(const HtmlUITexture* texture, uint32_t& live) const{
    if (texture && texture->pixelHash && texture->pixelHashVersion == texture->contentVersion && !texture->liveContent && !texture->dead)
        return texture->pixelHash;
    return textureSignature(texture, live);
}

uint64_t RmlRenderer::scopeSignature(size_t first, size_t end, Rml::Vector2i origin, const ScopeState& start, int layer, uint32_t& live){
    // positions relative to the origin, unless a transform maps them (it holds where they are)
    bool relative = start.transform < 0;
    for (size_t i = first; relative && i < end; i++){
        if (commands[i].type == CommandType::TRANSFORM && commands[i].flag)
            relative = false;
    }
    const Rml::Vector2f pixel = getPixelScale();
    const double dx = relative ? (double)origin.x / (double)pixel.x : 0.0;
    const double dy = relative ? (double)origin.y / (double)pixel.y : 0.0;
    const Rml::Vector2i shift = relative ? origin : Rml::Vector2i(0, 0);
    auto offset = [&](float x, float y){
        return layerCacheFloat((float)((double)x - dx)) | (layerCacheFloat((float)((double)y - dy)) << 32);
    };
    auto rect = [&](uint64_t h, Rml::Rectanglei r){
        h = layerCacheMix(h, layerCachePair(r.Left() - shift.x, r.Top() - shift.y));
        return layerCacheMix(h, layerCachePair(r.Right() - shift.x, r.Bottom() - shift.y));
    };

    // the state every draw is made in, hashed when it changes
    ScopeState state = start;
    uint64_t stateHash = 0;
    auto hashState = [&](){
        uint64_t h = layerCacheMix(0x57a7eull, state.transform >= 0 ? layerCacheMatrix(commandMatrices[(size_t)state.transform]) : 0);
        h = layerCacheMix(h, state.scissorOn ? 1u : 0u);
        if (state.scissorOn)
            h = rect(h, state.scissor);
        h = layerCacheMix(h, state.clipOn ? 1u : 0u);
        stateHash = h;
    };
    hashState();
    // the mask the scope starts with (an outer clip's), where a draw is tested against it or a
    // mask intersects it: a scope that sets its own first keys the same after any other
    uint64_t h = 0x5c09eull;
    bool maskIsStart = true, startMixed = false;
    auto useStartMask = [&](){
        if (maskIsStart && !startMixed){
            h = layerCacheMix(h, start.mask);
            startMixed = true;
        }
    };

    for (size_t i = first; i < end; i++){
        const Command& c = commands[i];
        switch (c.type){
        case CommandType::TRANSFORM:
            state.transform = c.flag ? c.i[0] : -1;
            hashState();
            continue;
        case CommandType::SCISSOR_ENABLE:
            state.scissorOn = c.flag != 0;
            hashState();
            continue;
        case CommandType::SCISSOR_REGION:
            state.scissor = Rml::Rectanglei::FromCorners(Rml::Vector2i(c.i[0], c.i[1]), Rml::Vector2i(c.i[2], c.i[3]));
            hashState();
            continue;
        case CommandType::CLIP_ENABLE:
            state.clipOn = c.flag != 0;
            hashState();
            continue;
        default:
            break;
        }
        if (c.type == CommandType::CLIP_MASK){
            if ((Rml::ClipMaskOperation)c.flag == Rml::ClipMaskOperation::Intersect)
                useStartMask();
            maskIsStart = false;
        }else if (state.clipOn){
            useStartMask();
        }
        h = layerCacheMix(h, (uint64_t)c.type);
        h = layerCacheMix(h, stateHash);
        switch (c.type){
        case CommandType::GEOMETRY:
            h = layerCacheMix(h, layerCacheGeometry(c.a));
            h = layerCacheMix(h, offset(c.x, c.y));
            h = layerCacheMix(h, scopeTextureSignature(reinterpret_cast<const HtmlUITexture*>(c.b), live));
            break;
        case CommandType::CLIP_MASK:
            h = layerCacheMix(h, c.flag);
            h = layerCacheMix(h, layerCacheGeometry(c.a));
            h = layerCacheMix(h, offset(c.x, c.y));
            if (c.i[0] > 0 && (size_t)c.i[0] <= commandClipShapes.size()){
                const ClipShapeRecord& shape = commandClipShapes[(size_t)c.i[0] - 1];
                h = layerCacheBytesHash(h, &shape, sizeof(ClipShapeRecord));
            }
            break;
        case CommandType::PUSH_LAYER:
            // a layer of the scope: what it was asked to cover (its frame holds it on whole granules)
            h = layerCacheMix(h, (uint64_t)(uint32_t)(c.i[0] - layer));
            h = rect(h, c.b ? commandRects[c.b - 1] : (c.flag ? commandRects[c.a] : standardFrame()));
            break;
        case CommandType::COMPOSITE:
            h = layerCacheMix(h, layerCachePair(c.i[0] - layer, c.i[1] - layer));
            h = layerCacheMix(h, ((uint64_t)(uint32_t)c.i[3] << 8) | c.flag);
            for (int f = 0; f < c.i[3]; f++)
                h = layerCacheMix(h, filterSignature(commandFilters[(size_t)(c.i[2] + f)], live));
            break;
        case CommandType::SHADER:
            h = layerCacheMix(h, shaderSignature(c.a, live));
            h = layerCacheMix(h, layerCacheGeometry(c.b));
            h = layerCacheMix(h, offset(c.x, c.y));
            break;
        case CommandType::PRIMITIVES:
            // the instances by content: a gradient of more than four stops by its row's stops, not
            // the row it got (equal elements' gradients have rows of their own)
            h = layerCacheMix(h, (uint64_t)(uint32_t)c.i[1]);
            for (int k = 0; k < c.i[1]; k++){
                HtmlUIPrimitive instance = commandPrimitives[(size_t)(c.i[0] + k)];
                if (instance.params[0] == (float)HtmlUIPrimitive::GRADIENT && instance.f[1] > (float)HtmlUIPrimitive::GRADIENT_STOPS){
                    const size_t row = (size_t)std::max(instance.e[0], 0.0f);
                    h = layerCacheMix(h, row < stopRowHashes.size() ? stopRowHashes[row] : 0);
                    instance.e[0] = 0.0f;
                }
                h = layerCacheBytesHash(h, &instance, sizeof(HtmlUIPrimitive));
            }
            h = layerCacheMix(h, offset(c.x, c.y));
            break;
        case CommandType::SAVE_TEXTURE:
            h = layerCacheMix(h, textureSignature(reinterpret_cast<const HtmlUITexture*>(c.a), live));
            break;
        default:
            break;
        }
    }
    return h;
}

void RmlRenderer::planLayerCache(){
    cacheOps.clear();
    cacheScopeOf.clear();
    cacheScopes.clear();
    if (!layerCacheEnabled || !target || (recordedPushes == 0 && layerCache.empty() && layerCacheFree.empty()))
        return;
    // the cache's clock: replays (a kept frame draws nothing, S92 R1)
    layerCacheReplay++;
    if (recordedPushes == 0)
        return;

    // the scopes: a PUSH_LAYER, the composite of its layer into the one under it, its POP_LAYER;
    // kept only when nothing in it reads or writes a layer outside it (a backdrop reads the layer
    // under it) or fills a saved-layer texture (drawn into at replay only)
    struct Open {
        uint32_t push = 0;
        int layer = 0;
        ScopeState start;
        Rml::Rectanglei frame;
        Rml::Rectanglei request;      // the window RmlUi asked the layer to cover, within its frame
        int composite = -1;
        Rml::Rectanglei window;
        bool keep = true;
    };
    struct Candidate {
        uint32_t push = 0, composite = 0, pop = 0;
        int layer = 0;
        ScopeState start;
        Rml::Rectanglei frame, window;
    };
    std::vector<Open> open;
    std::vector<Candidate> candidates;
    struct FrameStore {
        uint64_t key;
        uint32_t pop;
        int scope;
    };
    std::vector<FrameStore> frameStores;
    // the replay's state as it goes (beginFrame's at the start)
    ScopeState state;
    state.scissor = toTargetPixels(frameStartScissor, contextSize, targetSize);
    bool& scissorOn = state.scissorOn;
    Rml::Rectanglei& scissor = state.scissor;
    for (size_t ci = 0; ci < commands.size(); ci++){
        const Command& c = commands[ci];
        switch (c.type){
        case CommandType::SCISSOR_ENABLE:
            scissorOn = c.flag != 0;
            break;
        case CommandType::SCISSOR_REGION:
            scissor = Rml::Rectanglei::FromCorners(Rml::Vector2i(c.i[0], c.i[1]), Rml::Vector2i(c.i[2], c.i[3]));
            break;
        case CommandType::TRANSFORM:
            state.transform = c.flag ? c.i[0] : -1;
            break;
        case CommandType::CLIP_ENABLE:
            state.clipOn = c.flag != 0;
            break;
        case CommandType::CLIP_MASK: {
            // the mask a scope starting later is tested against, where it is (no origin)
            uint64_t h = (Rml::ClipMaskOperation)c.flag == Rml::ClipMaskOperation::Intersect ? state.mask : 0x3a5cull;
            h = layerCacheMix(h, c.flag);
            h = layerCacheMix(h, layerCacheGeometry(c.a));
            h = layerCacheMix(h, layerCacheFloat(c.x) | (layerCacheFloat(c.y) << 32));
            h = layerCacheMix(h, state.transform >= 0 ? layerCacheMatrix(commandMatrices[(size_t)state.transform]) : 0);
            state.mask = h;
            break;
        }
        case CommandType::PUSH_LAYER: {
            Open scope;
            scope.push = (uint32_t)ci;
            scope.layer = c.i[0];
            scope.start = state;
            scope.frame = c.flag ? commandRects[c.a] : standardFrame();
            scope.request = c.b ? commandRects[c.b - 1] : scope.frame;
            open.push_back(scope);
            break;
        }
        case CommandType::COMPOSITE: {
            const int source = c.i[0], destination = c.i[1];
            for (size_t o = 0; o < open.size(); o++){
                Open& scope = open[o];
                const bool own = (o + 1 == open.size()) && source == scope.layer && destination == scope.layer - 1;
                if (own){
                    // a second composite of the layer: what is kept is not one result
                    if (scope.composite >= 0){
                        scope.keep = false;
                    }else{
                        // what can show: the composite's scissor within the layer, and within what
                        // the layer was asked to cover (the frame's granules around it are empty),
                        // so the window moves with its element
                        scope.composite = (int)ci;
                        if (scissorOn){
                            const Rml::Vector2i p0 = Rml::Math::Max(scissor.p0, scope.request.p0);
                            scope.window = Rml::Rectanglei::FromCorners(p0, Rml::Math::Max(p0, Rml::Math::Min(scissor.p1, scope.request.p1)));
                        }else{
                            scope.window = scope.request;
                        }
                    }
                }else if (source < scope.layer || destination < scope.layer){
                    scope.keep = false;
                }
            }
            break;
        }
        case CommandType::SAVE_TEXTURE:
            for (Open& scope : open)
                scope.keep = false;
            break;
        case CommandType::POP_LAYER:
            if (!open.empty()){
                const Open& scope = open.back();
                if (scope.keep && scope.composite >= 0 && !layerCacheEmpty(scope.window)){
                    Candidate candidate;
                    candidate.push = scope.push;
                    candidate.composite = (uint32_t)scope.composite;
                    candidate.pop = (uint32_t)ci;
                    candidate.layer = scope.layer;
                    candidate.start = scope.start;
                    candidate.frame = scope.frame;
                    candidate.window = scope.window;
                    candidates.push_back(candidate);
                }
                open.pop_back();
            }
            break;
        default:
            break;
        }
    }
    if (candidates.empty())
        return;

    // outer scopes first: a hit draws none of the scopes inside it
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b){ return a.push < b.push; });
    const Rml::Vector2f pixel = getPixelScale();
    const int samples = target->getSampleCount();
    uint32_t hitEnd = 0;
    for (const Candidate& candidate : candidates){
        if (candidate.push < hitEnd)
            continue;
        const Command& composite = commands[candidate.composite];

        LayerCacheScope scope;
        scope.window = candidate.window;
        scope.filters = std::max(composite.i[3], 0);
        // a trailing opacity (a group's, or the filter list's last) is drawn at the composite
        if (scope.filters > 0){
            const Rml::CompiledFilterHandle last = commandFilters[(size_t)(composite.i[2] + scope.filters - 1)];
            if (last && !htmlui::EffectRenderer::isEffectHandle(last)){
                const RmlCompiledFilter& filter = *reinterpret_cast<const RmlCompiledFilter*>(last);
                if (filter.type == RmlCompiledFilter::Type::PASSTHROUGH){
                    scope.opacity = filter.blendFactor;
                    scope.filters--;
                }
            }
        }

        // the scope relative to its window, then what the result depends on beyond it
        uint32_t live = 0;
        uint64_t key = scopeSignature(candidate.push + 1, candidate.composite, candidate.window.p0, candidate.start, candidate.layer, live);
        key = layerCacheMix(key, composite.flag);   // the blend mode: another one (mix-blend-mode, S93) is another entry
        key = layerCacheMix(key, (uint64_t)scope.filters);
        for (int f = 0; f < scope.filters; f++)
            key = layerCacheMix(key, filterSignature(commandFilters[(size_t)(composite.i[2] + f)], live));
        if (live)
            continue;
        key = layerCacheMix(key, layerCachePair(candidate.window.Width(), candidate.window.Height()));
        key = layerCacheMix(key, (uint64_t)samples);
        key = layerCacheMix(key, layerCacheFloat(pixel.x) | (layerCacheFloat(pixel.y) << 32));
        // A blur's halved levels start at its window (blurInto): its result moves with the window.
        // An effect filter reads its own coordinates: where it is is part of its result
        bool absolute = false;
        for (uint32_t k = candidate.push; k <= candidate.composite && !absolute; k++){
            const Command& c = commands[k];
            if (c.type != CommandType::COMPOSITE)
                continue;
            for (int f = 0; f < c.i[3]; f++){
                const Rml::CompiledFilterHandle handle = commandFilters[(size_t)(c.i[2] + f)];
                if (handle && htmlui::EffectRenderer::isEffectHandle(handle))
                    absolute = true;
            }
        }
        if (absolute){
            key = layerCacheMix(key, layerCachePair(candidate.window.Left(), candidate.window.Top()));
            key = layerCacheMix(key, layerCachePair(candidate.frame.Left(), candidate.frame.Top()));
        }
        scope.key = key;

        if (cacheOps.empty()){
            cacheOps.assign(commands.size(), CacheOp::NONE);
            cacheScopeOf.assign(commands.size(), -1);
        }
        auto found = layerCache.find(key);
        if (found != layerCache.end() && !(found->second.image && found->second.image->isCreated())){
            layerCacheBytes -= found->second.bytes;
            layerCache.erase(found);
            found = layerCache.end();
        }
        // a scope equal to one this replay stores earlier (a row of equal cards) draws that one's
        // result: the store keeps it whatever the budget, until the end of the replay
        int storedEarlier = -1;
        if (found == layerCache.end()){
            for (const FrameStore& store : frameStores){
                if (store.key == key && store.pop < candidate.push){
                    storedEarlier = store.scope;
                    break;
                }
            }
        }
        if (storedEarlier >= 0){
            cacheScopes[(size_t)storedEarlier].keep = true;
            scope.afterStore = true;
        }
        cacheScopeOf[candidate.composite] = (int32_t)cacheScopes.size();
        cacheScopes.push_back(scope);
        if (found == layerCache.end() && storedEarlier < 0){
            cacheOps[candidate.composite] = CacheOp::STORE;
            frameStores.push_back(FrameStore{key, candidate.pop, (int)cacheScopes.size() - 1});
            stats.layerCacheMisses++;
            continue;
        }
        // a hit: its commands draw nothing (the state ones stay, the clip masks only set the mask)
        if (found != layerCache.end())
            found->second.lastUsed = layerCacheReplay;
        stats.layerCacheHits++;
        for (uint32_t k = candidate.push; k <= candidate.pop; k++){
            switch (commands[k].type){
            case CommandType::TRANSFORM:
            case CommandType::SCISSOR_ENABLE:
            case CommandType::SCISSOR_REGION:
            case CommandType::CLIP_ENABLE:
                break;
            case CommandType::CLIP_MASK:
                cacheOps[k] = CacheOp::MASK;
                break;
            default:
                cacheOps[k] = CacheOp::SKIP;
                break;
            }
        }
        cacheOps[candidate.composite] = CacheOp::HIT;
        hitEnd = candidate.pop + 1;
    }
}

bool RmlRenderer::replayCached(size_t index, const Command& c){
    switch (cacheOps[index]){
    case CacheOp::NONE:
        return false;
    case CacheOp::SKIP:
        return true;
    case CacheOp::MASK:
        setClipMaskOnly(c);
        return true;
    case CacheOp::HIT: {
        const LayerCacheScope& scope = cacheScopes[(size_t)cacheScopeOf[index]];
        auto found = layerCache.find(scope.key);
        // the destination is the top layer: the scope's own layer was never pushed. An equal scope
        // stored earlier in the replay keeps none when its layer was empty: so is this one
        if (found != layerCache.end() && c.i[1] == layersSize - 1){
            found->second.hit = true;
            drawCachedLayer(found->second, scope.window, c.i[1], (Rml::BlendMode)c.flag, scope.opacity);
        }else if (!scope.afterStore || found != layerCache.end()){
            Log::error("[HTML UI] a cached layer scope could not be drawn (layer %d of %d)", c.i[1], layersSize);
        }
        return true;
    }
    case CacheOp::STORE: {
        const LayerCacheScope& scope = cacheScopes[(size_t)cacheScopeOf[index]];
        compositeLayersNow((Rml::LayerHandle)c.i[0], (Rml::LayerHandle)c.i[1], (Rml::BlendMode)c.flag,
                           Rml::Span<const Rml::CompiledFilterHandle>(commandFilters.data() + c.i[2], (size_t)c.i[3]), &scope);
        return true;
    }
    }
    return false;
}

void RmlRenderer::setClipMaskOnly(const Command& c){
    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(c.a);
    if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
        return;
    // renderToClipMaskNow's bookkeeping without its draw: every depth-stencil is now behind the
    // mask, so the next draw tested against it (the cached composite's) draws it in (ensureMask)
    const Rml::ClipMaskOperation operation = (Rml::ClipMaskOperation)c.flag;
    if (operation != Rml::ClipMaskOperation::Intersect)
        maskOps.clear();
    maskOps.push_back(MaskOp{operation, c.a, Rml::Vector2f(c.x, c.y), transform});
    maskVersion++;
    stencilRef = (operation == Rml::ClipMaskOperation::Intersect) ? (uint8_t)std::min(255, stencilRef + 1) : (uint8_t)1;
}

std::unique_ptr<Framebuffer> RmlRenderer::takeLayerCacheImage(int width, int height){
    for (size_t i = layerCacheFree.size(); i-- > 0;){
        Framebuffer& image = *layerCacheFree[i].image;
        if ((int)image.getWidth() != width || (int)image.getHeight() != height || !image.isCreated())
            continue;
        std::unique_ptr<Framebuffer> taken = std::move(layerCacheFree[i].image);
        layerCacheFree.erase(layerCacheFree.begin() + (std::ptrdiff_t)i);
        layerCacheBytes -= layerCacheImageBytes(*taken);
        return taken;
    }
    return nullptr;
}

bool RmlRenderer::makeLayerCacheRoom(size_t bytes){
    const size_t budget = getLayerCacheBudget();
    if (bytes > budget)
        return false;
    // free images first, then the least recently used entries this replay has not drawn
    while (layerCacheBytes + bytes > budget){
        if (!layerCacheFree.empty()){
            layerCacheBytes -= layerCacheImageBytes(*layerCacheFree.front().image);
            layerCacheFree.erase(layerCacheFree.begin());
            continue;
        }
        auto oldest = layerCache.end();
        for (auto it = layerCache.begin(); it != layerCache.end(); ++it){
            if (it->second.lastUsed < layerCacheReplay && (oldest == layerCache.end() || it->second.lastUsed < oldest->second.lastUsed))
                oldest = it;
        }
        if (oldest == layerCache.end())
            return false;
        layerCacheBytes -= oldest->second.bytes;
        layerCache.erase(oldest);
    }
    return true;
}

RmlRenderer::LayerCacheEntry* RmlRenderer::storeLayerResult(const LayerCacheScope& scope, RmlFilterContext& context){
    const uint64_t key = scope.key;
    const Rml::Rectanglei window = scope.window;
    // the same scope stored earlier in this replay (two identical elements): its image
    auto existing = layerCache.find(key);
    if (existing != layerCache.end()){
        existing->second.lastUsed = layerCacheReplay;
        return &existing->second;
    }
    const Rml::Rectanglei frame = context.getLayerFrame();
    // a region layer's filters leave the result in a post-process target of its frame: the
    // entry takes that image (no copy), the set gets a free one back or makes one when next used
    const bool take = !context.isPrimaryLayer() && context.postSet != nullptr && context.primaryIndex >= 0 &&
                      context.primaryIndex < RmlRenderTarget::POSTPROCESS_COUNT;
    // A copy keeps a pixel of the result around the window (within the layer): Mesa's MSAA
    // rasterizer lets half the samples of the row above and the column left of a scissor through
    // (MsaaScissor.HtmlUIClipEdge), and those read what the result has there, not the window's
    // edge clamped
    Rml::Rectanglei copied = window.Extend(1);
    {
        const Rml::Vector2i p0 = Rml::Math::Max(copied.p0, frame.p0);
        copied = Rml::Rectanglei::FromCorners(p0, Rml::Math::Max(p0, Rml::Math::Min(copied.p1, frame.p1)));
    }
    const Rml::Vector2i size = take ? frame.Size() : copied.Size();
    const size_t bytes = (size_t)size.x * (size_t)size.y * 4u;
    // no room: an equal scope later in the replay still draws it, the entry goes at its end
    const bool room = makeLayerCacheRoom(bytes);
    if (!room && !scope.keep)
        return nullptr;

    LayerCacheEntry entry;
    if (take){
        std::unique_ptr<Framebuffer>& slot = context.postSet->targets[context.primaryIndex];
        if (!slot)
            return nullptr;
        entry.image = std::move(slot);
        slot = takeLayerCacheImage(size.x, size.y);
        entry.rect = Rml::Rectanglei::FromPositionSize(window.p0 - frame.p0, window.Size());
    }else{
        entry.image = takeLayerCacheImage(size.x, size.y);
        if (!entry.image){
            FramebufferDesc desc;
            desc.width = size.x;
            desc.height = size.y;
            desc.color = ColorFormat::RGBA;
            desc.depth = DepthStencilFormat::NONE;
            desc.sampleCount = 1;
            desc.label = "htmlui-layer-cache";
            entry.image = std::make_unique<Framebuffer>();
            gpuCreateCount++;
            if (!entry.image->create(desc)){
                Log::error("[HTML UI] could not create a %d x %d layer cache image", size.x, size.y);
                return nullptr;
            }
        }
        // the window of the result (the layer itself, or a viewport-sized post-process target)
        PostDraw copy;
        copy.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
        copy.source = &context.primary().getRender().getColorTexture();
        const Rml::Vector2f frameSize(frame.Size());
        copy.uvTransform[0] = (float)copied.Width() / frameSize.x;
        copy.uvTransform[1] = (float)copied.Height() / frameSize.y;
        copy.uvTransform[2] = (float)(copied.Left() - frame.Left()) / frameSize.x;
        copy.uvTransform[3] = (float)(copied.Top() - frame.Top()) / frameSize.y;
        copy.viewport = Rml::Rectanglei::FromSize(copied.Size());
        copy.scissor = copy.viewport;
        if (!drawPost(*entry.image, LoadAction::CLEAR, copy, postTargetState(BlendMode::NONE)))
            return nullptr;
        stats.layerPixels += (uint32_t)size.x * (uint32_t)size.y;
        layerPassStats.windowPixels += layerCacheGranulePixels(window);
        layerPassStats.largestPass = std::max(layerPassStats.largestPass, (uint32_t)size.x * (uint32_t)size.y);
        entry.rect = Rml::Rectanglei::FromPositionSize(window.p0 - copied.p0, window.Size());
    }
    entry.bytes = bytes;
    entry.lastUsed = layerCacheReplay;
    entry.transient = !room;
    layerCacheBytes += bytes;
    LayerCacheEntry& stored = layerCache[key] = std::move(entry);
    return &stored;
}

void RmlRenderer::drawCachedLayer(const LayerCacheEntry& entry, Rml::Rectanglei window, int destination, Rml::BlendMode blendMode, float opacity){
    Framebuffer* destinationLayer = layerAt(destination);
    if (!destinationLayer || !entry.image)
        return;
    const Rml::Rectanglei destinationFrame = frameRectOf(destination);
    // within the composite's scissor, where the image holds the result
    const Rml::Rectanglei scissor = scissorIn(destinationFrame);
    const Rml::Rectanglei inDestination = Rml::Rectanglei::FromPositionSize(window.p0 - destinationFrame.p0, window.Size());
    const Rml::Vector2i p0 = Rml::Math::Max(scissor.p0, inDestination.p0);
    const Rml::Rectanglei drawn = Rml::Rectanglei::FromCorners(p0, Rml::Math::Max(p0, Rml::Math::Min(scissor.p1, inDestination.p1)));
    if (layerCacheEmpty(drawn))
        return;

    PostDraw draw;
    draw.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
    draw.source = &entry.image->getRender().getColorTexture();
    draw.scissor = drawn;
    draw.params.params[0] = opacity;
    // a destination pixel p shows the image's pixel p + destination.p0 - window.p0 + rect.p0
    const Rml::Vector2f imageSize((float)entry.image->getWidth(), (float)entry.image->getHeight());
    const Rml::Vector2f destinationSize(destinationFrame.Size());
    draw.uvTransform[0] = destinationSize.x / imageSize.x;
    draw.uvTransform[1] = destinationSize.y / imageSize.y;
    draw.uvTransform[2] = (float)(destinationFrame.Left() - window.Left() + entry.rect.Left()) / imageSize.x;
    draw.uvTransform[3] = (float)(destinationFrame.Top() - window.Top() + entry.rect.Top()) / imageSize.y;
    const BlendMode blend = (blendMode == Rml::BlendMode::Replace) ? BlendMode::NONE
                            : (blendMode == Rml::BlendMode::Plus) ? BlendMode::ADDITIVE_PREMULTIPLIED
                                                                    : BlendMode::PREMULTIPLIED;
    if (!applyCompositeClip(draw, destinationFrame))
        return;
    if (clipMaskEnabled)
        ensureMask(destination);
    drawPost(*destinationLayer, LoadAction::LOAD, draw, getGeometryState(blend));
}

bool RmlRenderer::compositeClipActive() const{
    return activeClipScope >= 0 && activeClipScope < (int)clipScopes.size() && clipScopes[(size_t)activeClipScope].mode == ScopeMode::ANALYTIC &&
           clipScopes[(size_t)activeClipScope].compositeClip;
}

bool RmlRenderer::applyCompositeClip(PostDraw& draw, Rml::Rectanglei destinationFrame) const{
    if (!compositeClipActive())
        return true;
    // A scissor at the clip's edge alone lets half the samples of the row above it and the column
    // left of it through on Mesa's MSAA rasterizer (MsaaScissor.HtmlUIClipEdge); the shader's
    // coverage at the pixel's centre leaves them out, and antialiases an edge within a pixel
    const Rml::Rectanglef& clip = clipScopes[(size_t)activeClipScope].compositeRect;
    const Rml::Vector2f origin(destinationFrame.p0);
    const Rml::Vector2f p0 = clip.p0 - origin, p1 = clip.p1 - origin;
    draw.params.params[1] = (float)destinationFrame.Width();
    draw.params.params[2] = (float)destinationFrame.Height();
    draw.params.params[3] = 1.0f;
    draw.params.texCoordMinMax[0] = p0.x;
    draw.params.texCoordMinMax[1] = p0.y;
    draw.params.texCoordMinMax[2] = p1.x;
    draw.params.texCoordMinMax[3] = p1.y;
    const Rml::Rectanglei full = Rml::Rectanglei::FromSize(destinationFrame.Size());
    const Rml::Rectanglei scissor = draw.scissor.Valid() ? draw.scissor : full;
    const Rml::Vector2i c0 = Rml::Math::Max(scissor.p0, Rml::Vector2i((int)std::floor(p0.x), (int)std::floor(p0.y)));
    const Rml::Vector2i c1 = Rml::Math::Min(scissor.p1, Rml::Vector2i((int)std::ceil(p1.x), (int)std::ceil(p1.y)));
    draw.scissor = Rml::Rectanglei::FromCorners(c0, Rml::Math::Max(c0, c1));
    return !layerCacheEmpty(draw.scissor);
}

void RmlRenderer::trimLayerCache(){
    for (size_t i = layerCacheFree.size(); i-- > 0;){
        if (layerCacheFree[i].freed < layerCacheReplay){
            layerCacheBytes -= layerCacheImageBytes(*layerCacheFree[i].image);
            layerCacheFree.erase(layerCacheFree.begin() + (std::ptrdiff_t)i);
        }
    }
    for (auto it = layerCache.begin(); it != layerCache.end();){
        LayerCacheEntry& entry = it->second;
        // unused for a while, or stored and not drawn again in the replay after (its element
        // changes every frame: the next store takes its image)
        const bool stale = entry.lastUsed + kLayerCacheKeepFrames < layerCacheReplay || entry.transient;
        const bool unread = !entry.hit && entry.lastUsed < layerCacheReplay;
        if (!stale && !unread){
            ++it;
            continue;
        }
        if (entry.image && entry.image->isCreated())
            layerCacheFree.push_back(LayerCacheFreeImage{std::move(entry.image), layerCacheReplay});
        else
            layerCacheBytes -= entry.bytes;
        it = layerCache.erase(it);
    }
    while (layerCacheFree.size() > kLayerCacheKeepFree || (!layerCacheFree.empty() && layerCacheBytes > getLayerCacheBudget())){
        layerCacheBytes -= layerCacheImageBytes(*layerCacheFree.front().image);
        layerCacheFree.erase(layerCacheFree.begin());
    }
}

void RmlRenderer::releaseLayerCache(){
    layerCache.clear();
    layerCacheFree.clear();
    layerCacheBytes = 0;
    cacheOps.clear();
    cacheScopeOf.clear();
    cacheScopes.clear();
}
