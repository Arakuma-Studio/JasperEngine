// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtRenderer.h"

#include "TitlePlanner.h"

#include "render/SystemRender.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtIO.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;
using namespace jasperengine::titleart;

// ---- the plan of a whole request ----

namespace {

    bool isFontPath(const std::string& path) {
        auto ends = [&path](const char* ext) {
            const size_t n = std::char_traits<char>::length(ext);
            return path.size() >= n && std::equal(path.end() - (long)n, path.end(), ext, [](char a, char b) { return std::tolower((unsigned char)a) == b; });
        };
        return ends(".ttf") || ends(".otf") || ends(".ttc");
    }

    void setMap(TitleGpuPass& pass, int index, const std::array<float, 4>& map) {
        pass.params.set(index, map[0], map[1], map[2], map[3]);
    }

}

TitleStepList TitlePlanner::plan() {
    const AssetResolver* assets = req.assets;
    AssetRevisionFn revision;
    if (assets) {
        revision = [assets](const TitleAssetRef& ref) {
            const ResolvedTitleAsset r = assets->resolve(ref, isFontPath(ref.path) ? TitleAssetKind::Font : TitleAssetKind::Image);
            return r.found ? r.revision : 0;
        };
    }
    docHashes = hashTitleArt(*document, revision);
    for (const TitleProblem& p : titleArtDocumentProblems(*document)) problem(p);
    if (outRegion.empty()) return std::move(steps);

    if (req.glyphCell) {
        TitlePx cell = planGlyphCell();
        if (cell.valid()) finishOutput(cell);
        return std::move(steps);
    }

    DrawTreeOptions options;
    options.includeGuides = req.includeGuides;
    options.isolateLayer = req.isolateLayer;
    options.content = req.content;
    options.contentLayers = req.contentLayers;
    inkOnly = req.content == OutputContent::Ink;
    const std::vector<DrawNode>& nodes = keepTree(buildDrawTree(*document, options));

    // the canvas background (transparent = real alpha); a layer alone, a mask and the light
    // are drawn over nothing
    const Json& background = document->root.value("canvas", Json::object()).value("background", Json::object());
    const bool transparent = background.value("transparent", true) || !req.isolateLayer.empty() || req.content == OutputContent::Mask ||
                             req.content == OutputContent::Glow;
    TitlePx acc{newSlot(TitleBuffer::Accumulator, outRegion.width, outRegion.height), outRegion};
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!transparent) {
        TitleColor c;
        parseTitleColor(background.value("color", Json("#000000")), c);
        clearColor[0] = srgbToLinear(c.r);
        clearColor[1] = srgbToLinear(c.g);
        clearColor[2] = srgbToLinear(c.b);
        clearColor[3] = 1.0f;
    }
    clearSlot("background", "", acc.slot, clearColor);
    acc = compose(nodes, acc, nullptr);

    if (req.content == OutputContent::Glow) {
        // only the light: everything minus its ink (§6.1 Outputs), colour only
        inkOnly = true;
        rects.clear();
        const std::vector<DrawNode>& inkNodes = keepTree(buildDrawTree(*document, options));
        TitlePx ink = clearedLike(outRegion, "");
        ink = compose(inkNodes, ink, nullptr);
        inkOnly = false;
        const bool floatTarget = svc.caps.format(TitleBuffer::Accumulator).format == ColorFormat::RGBA16F;
        const int inkSlot = ink.slot;
        const std::array<float, 4> same = uvMap(outRegion, outRegion);
        if (!floatTarget || svc.caps.floatBlend) {
            pass("glow split", "", acc.slot, {0, 0, outRegion.width, outRegion.height}, TitleRenderProgram::Copy,
                 [inkSlot, same](TitleJobState& s, TitleGpuPass& p, std::string&) {
                     p.tex[0] = s.texture(inkSlot);
                     setMap(p, 2, same);
                     p.params.set(3, 1.0f);
                     return true;
                 },
                 BlendMode::SUBTRACT_PREMULTIPLIED);
        } else {
            TitlePx backdrop = copyOf(acc, "");
            const int back = backdrop.slot;
            pass("glow split", "", acc.slot, {0, 0, outRegion.width, outRegion.height}, TitleRenderProgram::Blend,
                 [back, inkSlot, same](TitleJobState& s, TitleGpuPass& p, std::string&) {
                     p.tex[0] = s.texture(back);
                     p.tex[1] = s.texture(inkSlot);
                     setMap(p, 2, same);
                     setMap(p, 3, same);
                     p.params.set(5, 20.0f, 1.0f, 0.0f);
                     return true;
                 });
            release(backdrop.slot);
        }
        release(ink.slot);
    }
    finishOutput(acc);
    return std::move(steps);
}

TitlePx TitlePlanner::planGlyphCell() {
    const GlyphCellRequest& cell = *req.glyphCell;
    filteredLayer = cell.layerId;
    elementFilter.cluster = (int)cell.cluster;
    DrawTreeOptions options;
    options.includeGuides = true;
    options.isolateLayer = cell.layerId;
    const std::vector<DrawNode>& nodes = keepTree(buildDrawTree(*document, options));
    // the layer's node, under the ancestors that place it
    const DrawNode* node = nullptr;
    std::function<void(const std::vector<DrawNode>&)> find = [&](const std::vector<DrawNode>& list) {
        for (const DrawNode& n : list) {
            if (n.layerId == cell.layerId) node = &n;
            find(n.children);
        }
    };
    find(nodes);
    if (!node || !layerTypeHasStyle(node->type)) {
        TitleProblem p;
        p.severity = TitleSeverity::Error;
        p.code = "glyph_cell_invalid";
        p.layer = cell.layerId;
        p.message = "a glyph cell needs a text layer";
        problem(p);
        return {};
    }
    const TitleRectI rect = nodeRect(*node);
    if (rect.empty()) return {};
    // back: the rims with the layer's effects; front: the face alone, drawn over the back
    std::vector<std::string> rims = titleParts(*node->layer);
    rims.pop_back();
    TitlePx back;
    if (!rims.empty()) {
        TitlePx content = title2D(*node, rims, &rect);
        back = applyMasks(*node, applyEffects(node->layerId, node->layer->value("effects", Json::array()), content, false, {}, false, node->layer).px);
    } else {
        back = clearedLike(rect, node->layerId);
    }
    TitlePx front = applyMasks(*node, title2D(*node, {"face"}, &rect));
    sources["glyph:back"] = copyOf(back, node->layerId);
    sources["glyph:front"] = copyOf(front, node->layerId);
    TitlePx merged = copyOf(back, node->layerId);
    blendOnto(merged, front, 1.0f, (int)TitleBlend::Normal, nullptr, node->layerId);
    release(back.slot);
    release(front.slot);
    return merged;
}

void TitlePlanner::finishOutput(TitlePx acc) {
    const TitleRectI rect = acc.rect;
    int outSlot = acc.slot;
    ColorFormat format = svc.caps.format(TitleBuffer::Accumulator).format;
    bool premultiplied = true;
    if (req.output != TitleOutputFormat::Linear) {
        const RenderFeatures& features = SystemRender::getFeatures();
        const bool wants16 = req.output == TitleOutputFormat::Srgb16 || (req.output == TitleOutputFormat::Srgb8 && req.outputBitDepth == 16);
        const bool sixteen = wants16 && features.renderRGBA16F && !(req.testFlags & TITLE_ART_TEST_NO_FLOAT_TARGETS);
        if (wants16 && !sixteen) {
            TitleProblem p;
            p.code = "bit_depth_unsupported";
            p.message = "this device renders no half floats: the 16-bit output is 8-bit";
            problem(p);
        }
        TargetKey key;
        key.width = rect.width;
        key.height = rect.height;
        key.format = sixteen ? ColorFormat::RGBA16F : ColorFormat::RGBA;
        key.filter = TextureFilter::LINEAR;
        format = key.format;
        outSlot = newSlot(key);
        int mode = req.output == TitleOutputFormat::PremultipliedSrgb8 ? 2 : (sixteen ? 1 : 0);
        if (req.content == OutputContent::Mask) mode = 3;
        else if (req.content == OutputContent::Glow) mode = 4;
        premultiplied = mode == 2;
        const int from = acc.slot;
        const std::array<float, 4> map = uvMap(rect, rect);
        const float ox = (float)rect.x, oy = (float)rect.y;
        pass("output", "", outSlot, {0, 0, rect.width, rect.height}, TitleRenderProgram::Output, [from, map, mode, ox, oy](TitleJobState& s, TitleGpuPass& p, std::string&) {
            p.tex[0] = s.texture(from);
            p.params.set(2, (float)mode, ox, oy);
            setMap(p, 3, map);
            return true;
        });
        release(acc.slot);
    }
    // the image, the kept splits and the problems go to the job; the pool counts a render
    std::map<std::string, TitlePx> kept = sources;
    std::vector<TitleProblem> problems = found;
    const double s = scale;
    auto canvasRect = [s](const TitleRectI& r) {
        const int x0 = (int)std::floor(r.x / s), y0 = (int)std::floor(r.y / s);
        const int x1 = (int)std::ceil((r.x + r.width) / s), y1 = (int)std::ceil((r.y + r.height) / s);
        return TitleRectI{x0, y0, x1 - x0, y1 - y0};
    };
    cpu("finish", "", 5, [outSlot, rect, format, premultiplied, kept, problems, canvasRect](TitleJobState& state, std::string& error) {
        if (!state.alive(error)) return false;
        TitleJobState::Slot& slot = state.slots[(size_t)outSlot];
        TitleImage& image = state.job->resultImage();
        image.target = std::move(slot.ref);
        image.width = rect.width;
        image.height = rect.height;
        image.format = format;
        image.frame = canvasRect(rect);
        image.alphaBounds = {0, 0, rect.width, rect.height};
        image.premultiplied = premultiplied;
        for (const auto& [name, px] : kept) {
            if (name.rfind("split:", 0) != 0 && name.rfind("glyph:", 0) != 0) continue;
            TitleJobState::Slot& from = state.slots[(size_t)px.slot];
            TitleImage split;
            split.target = from.entry ? TargetRef() : std::move(from.ref);
            split.width = px.rect.width;
            split.height = px.rect.height;
            split.format = from.key.format;
            split.frame = canvasRect(px.rect);
            split.alphaBounds = {0, 0, px.rect.width, px.rect.height};
            split.premultiplied = true;
            if (name == "glyph:back") state.job->resultSplits().glyphBack = std::move(split);
            else if (name == "glyph:front") state.job->resultSplits().glyphFront = std::move(split);
            else state.job->resultSplits().layers[name.substr(6)] = std::move(split);
        }
        for (const TitleProblem& p : problems) state.job->addProblem(p);
        // every other target of the job goes back to the pool now
        for (size_t i = 0; i < state.slots.size(); i++) {
            if ((int)i != outSlot) state.release((int)i);
        }
        state.held.clear();
        state.services.pool.endRender();
        return true;
    });
}

// ---- generators ----

TitlePx TitlePlanner::generator(const DrawNode& node) {
    const Json& layer = *node.layer;
    const Json& gen = layer.value("generator", Json::object());
    const std::string type = gen.value("type", std::string());
    const GeneratorDef* def = findGenerator(type);
    const TitleRectI rect = nodeRect(node);
    if (!def || !def->render) {
        TitleProblem p;
        p.code = def ? "generator_unavailable" : "unknown_type";
        p.layer = node.layerId;
        p.message = def ? "the generator " + type + " has no pass code in this build: it draws nothing"
                        : "the generator type " + type + " is unknown to this build: it draws nothing";
        problem(p);
        return clearedLike(rect, node.layerId);
    }
    TitlePx out = clearedLike(rect, node.layerId);
    uint32_t passes = 1;
    static const Json empty = Json::object();
    auto paramsIt = gen.find("params");
    const Json params = paramsIt != gen.end() && paramsIt->is_object() ? *paramsIt : empty;
    if (def->estimate) passes = (uint32_t)std::max(1, def->estimate(ParamValues(params), FxSize{rect.width, rect.height, (float)scale}, FxTier::Desktop).passes);
    const std::shared_ptr<const TitleArtDoc> doc = document;
    const std::string id = node.layerId;
    const int outSlot = out.slot;
    const double s = scale;
    const PassPlace place = passPlace(rect, node.layer);
    kernel(type, id, rect, passes, 1.0f, [=](TitleJobState& state, TitleJobRunner& runner, std::string& error) {
        std::string why;
        if (!state.target(outSlot, why)) {
            error = why;
            return false;
        }
        const Json& light = doc->root.value("global_light", Json::object());
        GeneratorContext ctx(runner);
        ctx.width = rect.width;
        ctx.height = rect.height;
        ctx.pxScale = (float)s;
        ctx.frame = place.frame;
        ctx.canvasWidth = doc->canvasWidth();
        ctx.canvasHeight = doc->canvasHeight();
        ctx.documentToTarget = place.documentToTarget;
        ctx.layerToTarget = place.layerToTarget;
        ctx.seed = (uint32_t)doc->root.value("seed", (int64_t)1);
        ctx.lightAngle = light.value("angle", 120.0);
        ctx.lightAltitude = light.value("altitude", 30.0);
        ctx.layerId = id;
        def->render(ctx, state.slots[(size_t)outSlot].ref,
                    ParamValues(params, def->globalLight.angleKey ? &def->globalLight : nullptr, ctx.lightAngle, ctx.lightAltitude));
        if (!runner.error().empty()) {
            error = std::string(def->type) + ": " + runner.error();
            return false;
        }
        return true;
    });
    return out;
}

// ---- the renderer ----

struct TitleArtRenderer::Impl {
    explicit Impl(const RendererLimits& limits) : services(limits) {}
    TitleRenderServices services;
    mutable RendererStats counters;
    std::string path;
};

TitleArtRenderer::TitleArtRenderer(const RendererLimits& limits) : impl(std::make_unique<Impl>(limits)) {
}

TitleArtRenderer::~TitleArtRenderer() = default;

std::unique_ptr<TitleRenderJob> TitleArtRenderer::begin(const RenderRequest& request) {
    if (!request.doc) return nullptr;
    TitleRenderServices& svc = impl->services;
    // a new device (R19): every target, cache entry and program of the old one is gone
    svc.checkDevice();
    svc.caps = TitleArtCaps::select(SystemRender::getFeatures(), request.testFlags);
    svc.testFlags = request.testFlags;
    impl->path = svc.caps.pathName();
    // overrides apply to a copy of the snapshot, never to the document (R18a)
    std::shared_ptr<const TitleArtDoc> doc = request.doc;
    std::vector<TitleProblem> skipped;
    if (!request.overrides.empty()) {
        auto copy = std::make_shared<TitleArtDoc>(*request.doc);
        applyFieldOverrides(*copy, request.overrides, &skipped);
        doc = copy;
    }
    TitlePlanner planner(svc, request, doc);
    for (TitleProblem& p : skipped) planner.problem(std::move(p));
    TitleStepList steps = planner.plan();
    impl->counters.floodPasses += planner.floodPasses;
    impl->counters.floodPassesUnbounded += planner.unboundedFloodPasses;
    auto job = std::make_unique<TitleRenderJob>(std::move(steps));
    planner.state()->job = job.get();
    job->keepAlive(planner.state());
    // a request that draws nothing (an empty region) still reports what planning found
    if (job->passesTotal() == 0 && job->done()) {
        for (const TitleProblem& p : planner.problems()) job->addProblem(p);
    }
    impl->counters.renders++;
    return job;
}

const RendererStats& TitleArtRenderer::stats() const {
    const TitleRenderServices& svc = impl->services;
    RendererStats& s = impl->counters;
    s.passes = svc.gpu.stats().passes;
    s.bytesLive = svc.pool.stats().bytesLive;
    s.peakBytes = std::max(s.peakBytes, svc.pool.stats().peakBytes);
    s.cacheHits = svc.cache.hits;
    s.cacheMisses = svc.cache.misses;
    s.path = impl->path.c_str();
    return s;
}

void TitleArtRenderer::trim() {
    impl->services.cache.clear();
    impl->services.pool.trim();
}

void TitleArtRenderer::setContentProvider(std::shared_ptr<TitleContentProvider> provider) {
    impl->services.content = std::move(provider);
    impl->services.cache.clear();
}

void TitleArtRenderer::simulateDeviceLossForTesting() {
    impl->services.simulatedLoss = true;
    impl->services.checkDevice();
}

size_t TitleArtRenderer::cacheEntries() const {
    return impl->services.cache.size();
}

const TitleTargetPoolStats& TitleArtRenderer::poolStats() const {
    return impl->services.pool.stats();
}
