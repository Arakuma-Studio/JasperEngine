// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/compositor.ts (drawNodes / drawInk, unit, unitContent,
// renderLeafContent's part runs, adjustOnto, blendOnto, holdsLight, contentLight) and
// src/engine/effectsRun.ts (applyEffects and its ink) by SafEight, used with the author's
// permission.

#include "TitlePlanner.h"

#include "titleart/TitleArtIO.h"
#include "titleart/effects/TitleAdjust.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

// ---- the job's shared state ----

TitleJobState::TitleJobState(TitleRenderServices& renderServices, std::shared_ptr<const TitleArtDoc> snapshot)
    : services(renderServices), doc(std::move(snapshot)), generation(renderServices.generation) {
}

bool TitleJobState::alive(std::string& error) const {
    if (generation == services.generation) return true;
    error = "device_lost: the GPU device was lost during the render; render again";
    return false;
}

TitleTarget* TitleJobState::target(int slot, std::string& error) {
    if (slot < 0 || slot >= (int)slots.size()) {
        error = "title art render: no target " + std::to_string(slot);
        return nullptr;
    }
    Slot& s = slots[(size_t)slot];
    if (s.entry) {
        TitleTarget* t = s.entry->target.get();
        if (!t) error = "device_lost: a cached title art target is gone";
        return t;
    }
    if (!s.ref) {
        if (s.released) {
            error = "title art render: target " + std::to_string(slot) + " used after its release";
            return nullptr;
        }
        std::string why;
        s.ref = services.pool.acquire(s.key, &why);
        if (!s.ref) {
            error = why.rfind("out_of_target_memory", 0) == 0 ? why : "out_of_target_memory: " + why;
            return nullptr;
        }
    }
    return s.ref.get();
}

TextureRender* TitleJobState::texture(int slot) {
    if (slot < 0 || slot >= (int)slots.size()) return nullptr;
    Slot& s = slots[(size_t)slot];
    if (s.entry) return s.entry->texture();
    auto* t = s.ref.as<SokolTitleTarget>();
    return t ? &t->texture() : nullptr;
}

void TitleJobState::release(int slot) {
    if (slot < 0 || slot >= (int)slots.size()) return;
    Slot& s = slots[(size_t)slot];
    s.ref.reset();
    s.entry.reset();
    s.released = true;
}

std::array<float, 4> titleart::uvMap(const TitleRectI& into, const TitleRectI& from) {
    const float w = (float)std::max(1, from.width), h = (float)std::max(1, from.height);
    return {1.0f / w, 1.0f / h, (float)(into.x - from.x) / w, (float)(into.y - from.y) / h};
}

namespace {

    void setMap(TitleGpuPass& pass, int index, const std::array<float, 4>& map) {
        pass.params.set(index, map[0], map[1], map[2], map[3]);
    }

    TitleRectI relative(const TitleRectI& r, const TitleRectI& to) {
        return {r.x - to.x, r.y - to.y, r.width, r.height};
    }

    std::string blendNameOf(const Json& layer) {
        return layer.value("blend", std::string("normal"));
    }

    int blendOf(const Json& layer) {
        TitleBlend blend = TitleBlend::Normal;
        titleBlendFromName(blendNameOf(layer), blend);
        return (int)blend;
    }

    float opacityOf(const Json& layer) {
        return (float)std::clamp(layer.value("opacity", 1.0), 0.0, 1.0);
    }

    const Json& emptyArray() {
        static const Json a = Json::array();
        return a;
    }

    const Json& effectsOf(const Json& layer) {
        auto it = layer.find("effects");
        return it != layer.end() && it->is_array() ? *it : emptyArray();
    }

    const Json& partEffects(const Json& layer, const std::string& part) {
        auto pe = layer.find("part_effects");
        if (pe == layer.end() || !pe->is_object()) return emptyArray();
        auto it = pe->find(part);
        return it != pe->end() && it->is_array() ? *it : emptyArray();
    }

    bool anyEnabled(const Json& effects) {
        for (const Json& e : effects) {
            if (e.value("enabled", true)) return true;
        }
        return false;
    }

    ParamValues paramsOf(const EffectDef& def, const Json& effect, const TitleArtDoc& doc) {
        static const Json empty = Json::object();
        const Json& light = doc.root.value("global_light", Json::object());
        auto it = effect.find("params");
        return ParamValues(it != effect.end() && it->is_object() ? *it : empty, def.globalLight.angleKey ? &def.globalLight : nullptr,
                           light.value("angle", 120.0), light.value("altitude", 30.0));
    }

    bool effectAddsLight(const EffectDef& def, const ParamValues& params) {
        return def.addsLight && def.addsLight(params);
    }

    // a generator blended as light (Add / Screen: glints, flares) is light, not ink, where it lands
    bool isLightBlend(const DrawNode& node) {
        if (node.type != LayerType::Generator || !node.layer) return false;
        const std::string blend = blendNameOf(*node.layer);
        return blend == "add" || blend == "screen";
    }

}

// ---- recording ----

TitlePlanner::TitlePlanner(TitleRenderServices& services, const RenderRequest& request, std::shared_ptr<const TitleArtDoc> doc)
    : svc(services), req(request), document(std::move(doc)) {
    st = std::make_shared<TitleJobState>(svc, document);
    scale = std::max(1e-3, (double)req.pxScale);
    const int cw = document->canvasWidth(), ch = document->canvasHeight();
    TitleRectI canvasPx{0, 0, cw, ch};
    TitleRectI region = req.region.empty() ? canvasPx : intersectRect(req.region, canvasPx);
    const TitleFrame whole = TitleFrame::canvas(cw, ch, scale);
    outRegion = intersectRect(whole.canvasToFrame(region), whole.region);
    forEachLayer(document->root, [this](const Json& layer, const Json* parent, int) {
        parents[layer.value("id", std::string())] = parent;
        return true;
    });
}

int TitlePlanner::newSlot(const TargetKey& key) {
    TitleJobState::Slot slot;
    slot.key = key;
    st->slots.push_back(std::move(slot));
    return (int)st->slots.size() - 1;
}

int TitlePlanner::newSlot(TitleBuffer buffer, int width, int height) {
    return newSlot(svc.keyFor(buffer, std::max(1, width), std::max(1, height)));
}

int TitlePlanner::entrySlot(const std::shared_ptr<TitleCacheEntry>& entry) {
    TitleJobState::Slot slot;
    slot.entry = entry;
    if (entry && entry->target) slot.key = entry->target.key();
    st->slots.push_back(std::move(slot));
    hold(entry);
    return (int)st->slots.size() - 1;
}

void TitlePlanner::hold(const std::shared_ptr<TitleCacheEntry>& entry) {
    if (entry) st->held.push_back(entry);
}

void TitlePlanner::problem(TitleProblem p) {
    for (const TitleProblem& q : found) {
        if (q.code == p.code && q.layer == p.layer && q.path == p.path) return;
    }
    found.push_back(std::move(p));
}

void TitlePlanner::pass(const std::string& label, const std::string& layerId, int slot, const TitleRectI& scissor, TitleRenderProgram program,
                        PassFill fill, BlendMode blend, bool clear, float costWeight, bool splittable) {
    TitleRenderStep step;
    step.kind = StepKind::Gpu;
    step.label = label;
    step.layerId = layerId;
    step.scissor = scissor;
    step.costWeight = costWeight;
    step.splittable = splittable && !clear;
    step.shader = titleRenderProgramKey((uint32_t)program);
    TitleRenderServices* services = &svc;
    step.ready = [services, program]() {
        std::string error;
        // a program that failed to build is "ready": its run reports the error
        return services->gpu.ensureProgram(program, &error) || !error.empty();
    };
    std::shared_ptr<TitleJobState> state = st;
    step.run = [state, slot, program, fill, blend, clear](const TitleRectI& rows, std::string& error) {
        if (!state->alive(error)) return false;
        TitleGpuPass p;
        p.program = program;
        p.target = state->target(slot, error);
        if (!p.target) return false;
        p.scissor = rows;
        p.blend = blend;
        p.clear = clear;
        if (fill && !fill(*state, p, error)) return false;
        return state->services.gpu.run(p, error);
    };
    steps.add(std::move(step));
}

void TitlePlanner::clearSlot(const std::string& label, const std::string& layerId, int slot, const float color[4]) {
    TitleRenderStep step;
    step.kind = StepKind::Gpu;
    step.label = label;
    step.layerId = layerId;
    step.countsAsPass = false;
    step.costWeight = 0.05f;
    const TargetKey& key = st->slots[(size_t)slot].key;
    step.scissor = {0, 0, key.width, key.height};
    std::shared_ptr<TitleJobState> state = st;
    const std::array<float, 4> c{color[0], color[1], color[2], color[3]};
    step.run = [state, slot, c](const TitleRectI&, std::string& error) {
        if (!state->alive(error)) return false;
        TitleTarget* t = state->target(slot, error);
        if (!t) return false;
        return state->services.gpu.clear(t, c.data(), error);
    };
    steps.add(std::move(step));
}

void TitlePlanner::cpu(const std::string& label, const std::string& layerId, uint32_t us, std::function<bool(TitleJobState&, std::string&)> run) {
    TitleRenderStep step;
    step.kind = StepKind::Cpu;
    step.label = label;
    step.layerId = layerId;
    step.cpuUs = us;
    std::shared_ptr<TitleJobState> state = st;
    step.run = [state, run](const TitleRectI&, std::string& error) { return run(*state, error); };
    steps.add(std::move(step));
}

void TitlePlanner::kernel(const std::string& label, const std::string& layerId, const TitleRectI& rect, uint32_t passes, float costWeight,
                          std::function<bool(TitleJobState&, TitleJobRunner&, std::string&)> run) {
    TitleRenderStep step;
    step.kind = StepKind::Gpu;
    step.label = label;
    step.layerId = layerId;
    step.scissor = {0, 0, rect.width, rect.height};
    step.costWeight = costWeight * (float)std::max<uint32_t>(1, passes);
    step.passes = std::max<uint32_t>(1, passes);
    step.splittable = false;
    // the programs a run met while they were being compiled: the step waits for them and runs
    // again (TitleJobRunner.h)
    auto compiling = std::make_shared<std::vector<uint32_t>>();
    TitleRenderServices* services = &svc;
    step.ready = [services, compiling]() {
        bool ready = true;
        for (uint32_t program : *compiling) {
            std::string error;
            ready = (services->gpu.ensureProgram(program, &error) || !error.empty()) && ready;
        }
        if (ready) compiling->clear();
        return ready;
    };
    std::shared_ptr<TitleJobState> state = st;
    step.run = [state, run, compiling](const TitleRectI&, std::string& error) {
        if (!state->alive(error)) return false;
        TitleJobRunner runner(state->services);
        if (!run(*state, runner, error)) return false;
        *compiling = runner.compiling();
        return true;
    };
    steps.add(std::move(step));
}

void TitlePlanner::release(int slot) {
    if (slot < 0) return;
    TitleRenderStep step;
    step.kind = StepKind::Cpu;
    step.label = "release";
    std::shared_ptr<TitleJobState> state = st;
    step.run = [state, slot](const TitleRectI&, std::string&) {
        state->release(slot);
        return true;
    };
    steps.add(std::move(step));
}

void TitlePlanner::store(const std::shared_ptr<TitleCacheEntry>& entry, int slot) {
    std::shared_ptr<TitleJobState> state = st;
    TitleRenderStep step;
    step.kind = StepKind::Cpu;
    step.label = "cache";
    step.run = [state, entry, slot](const TitleRectI&, std::string& error) {
        if (!state->alive(error)) return false;
        TitleJobState::Slot& s = state->slots[(size_t)slot];
        if (s.entry) return true;
        entry->target = std::move(s.ref);
        s.entry = entry;
        state->services.cache.insert(entry);
        return true;
    };
    steps.add(std::move(step));
}

uint64_t TitlePlanner::layerHash(const std::string& id, uint64_t LayerHashes::*member) const {
    auto it = docHashes.layers.find(id);
    return it != docHashes.layers.end() ? it->second.*member : 0;
}

TitleFrame TitlePlanner::frameOf(const TitleRectI& rect) const {
    TitleFrame frame = TitleFrame::canvas(document->canvasWidth(), document->canvasHeight(), scale);
    frame.region = rect;
    return frame;
}

TitlePlanner::PassPlace TitlePlanner::passPlace(const TitleRectI& rect, const Json* layer) const {
    PassPlace out;
    const int canvasW = document->canvasWidth(), canvasH = document->canvasHeight();
    out.frame = {-rect.x, -rect.y, (int)std::ceil(canvasW * scale), (int)std::ceil(canvasH * scale)};
    // TitleFrame::toFrame as an affine map: document (+y up, origin at the centre) -> rows down
    out.documentToTarget.a = scale;
    out.documentToTarget.d = -scale;
    out.documentToTarget.tx = canvasW * 0.5 * scale - rect.x;
    out.documentToTarget.ty = canvasH * 0.5 * scale - rect.y;
    out.layerToTarget = out.documentToTarget;
    if (layer) {
        const geometry::Affine m = placementOf(*layer).toDocument;
        TitleAffine toDocument;
        toDocument.a = m.a;
        toDocument.b = m.c;
        toDocument.c = m.b;
        toDocument.d = m.d;
        toDocument.tx = m.e;
        toDocument.ty = m.f;
        out.layerToTarget = out.documentToTarget.after(toDocument);
    }
    out.layerOrigin = out.layerToTarget.apply(TitleVec2{0.0, 0.0});
    return out;
}

TitlePlanner::PassImages TitlePlanner::passImages(const std::vector<ParamDef>& params, const Json& values, const std::string& layerId) {
    PassImages out;
    for (const ParamDef& def : params) {
        if (def.kind != ParamKind::Asset) continue;
        auto it = values.find(def.key);
        if (it == values.end() || it->is_null()) continue;
        const TitleAssetRef ref = decodeAssetRef(*it);
        if (ref.empty()) continue;
        out.emplace_back(ref, assetTexture(*it, TitleAssetKind::Image, TextureWrap::REPEAT, 0, layerId, def.key));
    }
    return out;
}

std::function<const TextureRender*(const TitleAssetRef&, int&, int&)> TitlePlanner::imageLookup(PassImages images) {
    if (images.empty()) return {};
    return [images = std::move(images)](const TitleAssetRef& ref, int& width, int& height) -> const TextureRender* {
        for (const auto& image : images) {
            if (image.first.path != ref.path || image.first.guid != ref.guid || !image.second || !image.second->upload) continue;
            width = image.second->upload->width();
            height = image.second->upload->height();
            return image.second->texture();
        }
        width = height = 0;
        return nullptr;
    };
}

TitlePlacement TitlePlanner::placementOf(const Json& layer) const {
    // the chain from the top: each transform maps its layer's space into its parent's
    std::vector<const Json*> chain;
    const Json* at = &layer;
    while (at) {
        chain.push_back(at);
        auto it = parents.find(at->value("id", std::string()));
        at = it != parents.end() ? it->second : nullptr;
    }
    TitlePlacement out;
    geometry::Affine m;
    double rotation = 0.0;
    bool flip = false;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const TitleTransform t = decodeTransform((*it)->value("transform", Json::object()));
        // flips (before rotation, about the position), then clockwise rotation (negative in
        // y-up), uniform scale, then the position
        geometry::Affine local = geometry::Affine::scale((t.flipX ? -1.0 : 1.0) * t.scale, (t.flipY ? -1.0 : 1.0) * t.scale);
        local = geometry::Affine::rotateDegrees(-t.rotation).after(local);
        local = geometry::Affine::translate(t.position.x, t.position.y).after(local);
        m = m.after(local);
        out.scale *= std::max(1e-6, t.scale);
        // a flip on one axis turns the rotations that follow it the other way
        const bool flipped = t.flipX != t.flipY;
        rotation += flip ? -t.rotation : t.rotation;
        if (t.flipX && t.flipY) rotation += flip ? -180.0 : 180.0;
        if (flipped) flip = !flip;
    }
    out.toDocument = m;
    out.rotation = rotation;
    out.flipX = flip;
    return out;
}

// ---- regions of interest ----

TitleRectI TitlePlanner::alignToAnchor(const TitleRectI& bounds, int anchorX, int anchorY) const {
    if (bounds.empty()) return {};
    const int align = TITLE_ROI_ALIGN;
    auto floorTo = [align](int v) { return v >= 0 ? v / align * align : -((-v + align - 1) / align) * align; };
    auto ceilTo = [&](int v) { return -floorTo(-v); };
    const int x0 = anchorX + floorTo(bounds.x - anchorX), y0 = anchorY + floorTo(bounds.y - anchorY);
    const int x1 = anchorX + ceilTo(bounds.x + bounds.width - anchorX), y1 = anchorY + ceilTo(bounds.y + bounds.height - anchorY);
    return intersectRect({x0, y0, x1 - x0, y1 - y0}, outRegion);
}

float TitlePlanner::effectsReach(const Json& effects) const {
    float reach = 0.0f;
    for (const Json& e : effects) {
        if (!e.value("enabled", true)) continue;
        const EffectDef* def = findEffect(e.value("type", std::string()));
        if (!def || !def->reach) continue;
        reach += std::max(0.0f, def->reach(paramsOf(*def, e, *document)));
    }
    return reach;
}

bool TitlePlanner::effectsFillCanvas(const Json& effects) const {
    for (const Json& e : effects) {
        if (!e.value("enabled", true)) continue;
        const EffectDef* def = findEffect(e.value("type", std::string()));
        if (def && def->fillsCanvas && def->fillsCanvas(paramsOf(*def, e, *document))) return true;
    }
    return false;
}

TitleRectI TitlePlanner::contentRect(const DrawNode& node) {
    switch (node.kind) {
        case DrawNodeKind::Adjust: return outRegion;
        case DrawNodeKind::Group: {
            TitleRectI r;
            for (const DrawNode& child : node.children) r = unionRect(r, nodeRect(child));
            return r;
        }
        case DrawNodeKind::Leaf:
            break;
    }
    if (node.type == LayerType::Generator) {
        const Json& gen = node.layer->value("generator", Json::object());
        const GeneratorDef* def = findGenerator(gen.value("type", std::string()));
        if (!def || !def->bounds) return outRegion;
        static const Json empty = Json::object();
        auto params = gen.find("params");
        const TitleRect b = def->bounds(ParamValues(params != gen.end() && params->is_object() ? *params : empty));
        if (b.empty()) return outRegion;
        const TitleFrame whole = TitleFrame::canvas(document->canvasWidth(), document->canvasHeight(), scale);
        return intersectRect(whole.toFrameRect(b), outRegion);
    }
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    if (layerTypeHasStyle(node.type) && is3D(node)) return title3DRect(node);
#endif
    if (layerTypeHasStyle(node.type)) return title2DRect(node);
    return {};
}

TitleRectI TitlePlanner::nodeRect(const DrawNode& node) {
    auto cached = rects.find(&node);
    if (cached != rects.end()) return cached->second;
    TitleRectI out;
    if (node.kind == DrawNodeKind::Adjust) {
        out = outRegion;
    } else {
        const Json& layer = *node.layer;
        TitleRectI content = contentRect(node);
        float reach = effectsReach(effectsOf(layer)) * (float)scale;
        if (layerTypeHasStyle(node.type)) {
            // part effects reach from their part's pixels, which the content bounds hold
            float partReach = 0.0f;
            for (const std::string& part : titleParts(layer)) partReach = std::max(partReach, effectsReach(partEffects(layer, part)));
            reach += partReach * (float)scale;
        }
        float feather = 0.0f;
        for (const Json& m : layer.value("masks", Json::array())) {
            if (m.value("enabled", true)) feather = std::max(feather, (float)m.value("feather", 0.0));
        }
        reach += 1.5f * feather * (float)scale;
        const bool canvas = effectsFillCanvas(effectsOf(layer));
        TitleRectI grown = content;
        if (!grown.empty()) {
            const int r = (int)std::ceil(reach);
            grown = {grown.x - r, grown.y - r, grown.width + 2 * r, grown.height + 2 * r};
        }
        if (canvas) grown = outRegion;
        // a clip group is drawn inside its base's content: the base's rect holds it
        int ax = 0, ay = 0;
        if (node.kind == DrawNodeKind::Leaf && layerTypeHasStyle(node.type)) {
            const TitlePlacement place = placementOf(layer);
            const TitleFrame whole = TitleFrame::canvas(document->canvasWidth(), document->canvasHeight(), scale);
            const TitleVec2 anchor = whole.toFrame({place.toDocument.e, place.toDocument.f});
            ax = (int)std::floor(anchor.x);
            ay = (int)std::floor(anchor.y);
        }
        out = alignToAnchor(grown, ax, ay);
    }
    rects[&node] = out;
    return out;
}

// ---- compositing ----

const std::vector<DrawNode>& TitlePlanner::keepTree(std::vector<DrawNode> nodes) {
    trees.push_back(std::make_unique<std::vector<DrawNode>>(std::move(nodes)));
    return *trees.back();
}

TitlePx TitlePlanner::clearedLike(const TitleRectI& rect, const std::string& layerId) {
    TitlePx px{newSlot(TitleBuffer::Accumulator, rect.width, rect.height), rect};
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    clearSlot("clear", layerId, px.slot, zero);
    return px;
}

TitlePx TitlePlanner::copyOf(TitlePx src, const std::string& layerId) {
    TitlePx out{newSlot(TitleBuffer::Accumulator, src.rect.width, src.rect.height), src.rect};
    const int from = src.slot;
    const std::array<float, 4> map = uvMap(out.rect, src.rect);
    pass("copy", layerId, out.slot, {0, 0, out.rect.width, out.rect.height}, TitleRenderProgram::Copy,
         [from, map](TitleJobState& s, TitleGpuPass& p, std::string&) {
             p.tex[0] = s.texture(from);
             setMap(p, 2, map);
             p.params.set(3, 1.0f);
             return true;
         });
    return out;
}

void TitlePlanner::blendOnto(TitlePx dst, TitlePx src, float opacity, int mode, const TitlePx* clipMask, const std::string& layerId) {
    const TitleRectI r = intersectRect(src.rect, dst.rect);
    if (r.empty() || !(opacity > 0.0f)) return;
    const TitleRectI rows = relative(r, dst.rect);
    const int srcSlot = src.slot;
    const int clipSlot = clipMask ? clipMask->slot : -1;
    const std::array<float, 4> srcMap = uvMap(dst.rect, src.rect);
    const std::array<float, 4> clipMap = clipMask ? uvMap(dst.rect, clipMask->rect) : std::array<float, 4>{};
    const bool floatTarget = svc.caps.format(TitleBuffer::Accumulator).format == ColorFormat::RGBA16F;
    const bool fixed = (mode == (int)TitleBlend::Normal || mode == (int)TitleBlend::Add) && (!floatTarget || svc.caps.floatBlend);
    if (fixed) {
        // Normal and Add by fixed-function blending straight into the parent (§6.4): Add is the
        // light blend, colour added and coverage unioned, as blend.glsl's add
        const BlendMode blend = mode == (int)TitleBlend::Normal ? BlendMode::PREMULTIPLIED : BlendMode::LIGHT_PREMULTIPLIED;
        pass(mode == (int)TitleBlend::Normal ? "compose normal" : "compose add", layerId, dst.slot, rows, TitleRenderProgram::Copy,
             [srcSlot, clipSlot, srcMap, clipMap, opacity](TitleJobState& s, TitleGpuPass& p, std::string&) {
                 p.tex[0] = s.texture(srcSlot);
                 setMap(p, 2, srcMap);
                 p.params.set(3, opacity, 0.0f, 1.0f, clipSlot >= 0 ? 1.0f : 0.0f);
                 if (clipSlot >= 0) {
                     p.tex[1] = s.texture(clipSlot);
                     setMap(p, 4, clipMap);
                 }
                 return true;
             },
             blend);
        return;
    }
    // every other mode reads the backdrop: a copy of the parent's region only, then the mode
    // over it with blending off
    TitlePx backdrop{newSlot(TitleBuffer::Accumulator, r.width, r.height), r};
    const int dstSlot = dst.slot;
    const std::array<float, 4> intoBackdrop = uvMap(r, dst.rect);
    pass("backdrop copy", layerId, backdrop.slot, {0, 0, r.width, r.height}, TitleRenderProgram::Copy,
         [dstSlot, intoBackdrop](TitleJobState& s, TitleGpuPass& p, std::string&) {
             p.tex[0] = s.texture(dstSlot);
             setMap(p, 2, intoBackdrop);
             p.params.set(3, 1.0f);
             return true;
         });
    const int backSlot = backdrop.slot;
    const std::array<float, 4> backMap = uvMap(dst.rect, r);
    pass(std::string("blend ") + titleBlendName((TitleBlend)std::min(mode, TITLE_BLEND_COUNT - 1)), layerId, dst.slot, rows, TitleRenderProgram::Blend,
         [backSlot, srcSlot, clipSlot, srcMap, backMap, clipMap, opacity, mode](TitleJobState& s, TitleGpuPass& p, std::string&) {
             p.tex[0] = s.texture(backSlot);
             p.tex[1] = s.texture(srcSlot);
             setMap(p, 2, srcMap);
             setMap(p, 3, backMap);
             if (clipSlot >= 0) {
                 p.tex[2] = s.texture(clipSlot);
                 setMap(p, 4, clipMap);
             }
             p.params.set(5, (float)mode, opacity, clipSlot >= 0 ? 1.0f : 0.0f);
             return true;
         });
    release(backdrop.slot);
}

TitlePx TitlePlanner::compose(const std::vector<DrawNode>& nodes, TitlePx acc, const TitlePx* clipMask) {
    return composeInk(nodes, acc, {}, clipMask, false).first;
}

std::pair<TitlePx, TitlePx> TitlePlanner::composeInk(const std::vector<DrawNode>& nodes, TitlePx acc, TitlePx ink, const TitlePx* clipMask,
                                                     bool wantInk) {
    for (const DrawNode& n : nodes) {
        if (n.kind == DrawNodeKind::Adjust) {
            acc = adjustOnto(acc, n, clipMask);
            if (ink.valid()) ink = adjustOnto(ink, n, clipMask);
            continue;
        }
        const bool light = isLightBlend(n);
        // the ink output leaves light out: Add-blended generators draw nothing there
        if (inkOnly && light) continue;
        const bool lit = wantInk && !light && holdsLight(n);
        Unit u = unit(n, lit);
        if (!u.px.valid()) continue;
        if (wantInk && !ink.valid() && (light || u.ink.valid())) {
            // the first light: the ink so far is the composite so far
            ink = copyOf(acc, n.layerId);
        }
        const float opacity = opacityOf(*n.layer);
        const int mode = blendOf(*n.layer);
        blendOnto(acc, u.px, opacity, mode, clipMask, n.layerId);
        if (ink.valid() && !light) blendOnto(ink, u.ink.valid() ? u.ink : u.px, opacity, mode, clipMask, n.layerId);
        release(u.px.slot);
        if (u.ink.valid()) release(u.ink.slot);
    }
    return {acc, ink};
}

bool TitlePlanner::holdsLight(const DrawNode& node) {
    if (node.kind == DrawNodeKind::Adjust) return false;
    for (const Json& e : effectsOf(*node.layer)) {
        if (!e.value("enabled", true)) continue;
        const EffectDef* def = findEffect(e.value("type", std::string()));
        if (def && effectAddsLight(*def, paramsOf(*def, e, *document))) return true;
    }
    for (const DrawNode& c : node.clipped) {
        if (c.kind != DrawNodeKind::Adjust && (isLightBlend(c) || holdsLight(c))) return true;
    }
    return contentLight(node);
}

bool TitlePlanner::contentLight(const DrawNode& node) {
    if (node.kind == DrawNodeKind::Group) {
        for (const DrawNode& c : node.children) {
            if (c.kind != DrawNodeKind::Adjust && (isLightBlend(c) || holdsLight(c))) return true;
        }
        return false;
    }
    if (node.kind != DrawNodeKind::Leaf) return false;
    if (node.type == LayerType::Generator) {
        const Json& gen = node.layer->value("generator", Json::object());
        const GeneratorDef* def = findGenerator(gen.value("type", std::string()));
        if (!def || !def->addsLight) return false;
        static const Json empty = Json::object();
        auto params = gen.find("params");
        return def->addsLight(ParamValues(params != gen.end() && params->is_object() ? *params : empty));
    }
    if (!layerTypeHasStyle(node.type)) return false;
    for (const std::string& part : titleParts(*node.layer)) {
        for (const Json& e : partEffects(*node.layer, part)) {
            if (!e.value("enabled", true)) continue;
            const EffectDef* def = findEffect(e.value("type", std::string()));
            if (def && effectAddsLight(*def, paramsOf(*def, e, *document))) return true;
        }
    }
    return false;
}

TitlePlanner::Unit TitlePlanner::unit(const DrawNode& node, bool wantInk) {
    const Json& layer = *node.layer;
    const TitleRectI rect = nodeRect(node);
    if (rect.empty()) return {};
    const bool clip = !node.clipped.empty();
    bool tracesInk = false;
    for (const Json& e : effectsOf(layer)) {
        const EffectDef* def = e.value("enabled", true) ? findEffect(e.value("type", std::string())) : nullptr;
        if (def && def->outline) tracesInk = true;
    }
    const bool contentInk = (wantInk || tracesInk) && contentLight(node);
    const bool keepSplit = std::find(req.keepSplits.begin(), req.keepSplits.end(), node.layerId) != req.keepSplits.end();
    // what this node's definitions report about canvas positions (nested nodes keep their own)
    std::shared_ptr<bool> reads = std::make_shared<bool>(false);
    std::shared_ptr<bool> outerReads = canvasReads;
    canvasReads = reads;
    struct RestoreReads {
        std::shared_ptr<bool>& slot;
        std::shared_ptr<bool> outer;
        ~RestoreReads() { slot = outer; }
    } restoreReads{canvasReads, outerReads};

    // node pixels from the cache (R14): keyed by the layer's content (its own position left
    // out), px_scale, the formats, the output content and where it lands inside a whole pixel
    const TitlePlacement place = placementOf(layer);
    const TitleFrame whole = TitleFrame::canvas(document->canvasWidth(), document->canvasHeight(), scale);
    const TitleVec2 anchor = whole.toFrame({place.toDocument.e, place.toDocument.f});
    const int ax = (int)std::floor(anchor.x), ay = (int)std::floor(anchor.y);
    const bool leaf2D = node.kind == DrawNodeKind::Leaf && layerTypeHasStyle(node.type);
    uint64_t key = 0;
    const bool cacheable = !wantInk && !contentInk && !req.glyphCell;
    if (cacheable) {
        key = hashCombine(fnv1a64("node"), layerHash(node.layerId, &LayerHashes::content));
        const double s = scale;
        key = hashCombine(key, fnv1a64(std::string_view(reinterpret_cast<const char*>(&s), sizeof(s))));
        key = hashCombine(key, fnv1a64(svc.caps.pathName()));
        key = hashCombine(key, inkOnly ? 1 : 2);
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
        // a 3D layer's pixels depend on the tier, the 3D path's test flags and the debug view too (S86)
        if (is3D(node)) {
            key = hashCombine(hashCombine(key, (uint64_t)(req.tier + 1)), (uint64_t)req.testFlags + 1);
            key = hashCombine(key, (uint64_t)req.debugView3D + 1);
        }
#endif
        // the ancestors' rotation, scale and flips shape the pixels
        const geometry::Affine& m = place.toDocument;
        const double linear[4] = {m.a, m.b, m.c, m.d};
        key = hashCombine(key, fnv1a64(std::string_view(reinterpret_cast<const char*>(linear), sizeof(linear))));
        if (leaf2D && !clip) {
            // the part of the position inside a pixel shapes the pixels; whole pixels only move them
            const double phase[2] = {anchor.x - ax, anchor.y - ay};
            key = hashCombine(key, fnv1a64(std::string_view(reinterpret_cast<const char*>(phase), sizeof(phase))));
        } else {
            const double position[2] = {m.e, m.f};
            key = hashCombine(key, fnv1a64(std::string_view(reinterpret_cast<const char*>(position), sizeof(position))));
            const int r[4] = {rect.x, rect.y, rect.width, rect.height};
            key = hashCombine(key, fnv1a64(std::string_view(reinterpret_cast<const char*>(r), sizeof(r))));
            // a clip group shows its clipped layers as they are now
            for (const DrawNode& c : node.clipped) key = hashCombine(key, layerHash(c.layerId, &LayerHashes::node));
        }
        if (auto hit = svc.cache.find(key)) {
            const bool moves = leaf2D && !clip;
            const TitleRectI placed = moves ? TitleRectI{hit->rect.x + ax, hit->rect.y + ay, hit->rect.width, hit->rect.height} : hit->rect;
            // a moved node's pixels serve only where they are whole (position-free, unclipped)
            if ((!moves || hit->positionFree) && placed == rect) {
                Unit u;
                u.px = {entrySlot(hit), rect};
                if (keepSplit) sources["split:" + node.layerId] = copyOf(u.px, node.layerId);
                return u;
            }
        }
    }

    Content content = unitContent(node, clip, contentInk);
    if (!content.px.valid()) return {};
    const bool maskIsPx = content.mask.slot == content.px.slot;
    Effects post = applyEffects(node.layerId, effectsOf(layer), content.px, clip && maskIsPx, content.ink, wantInk, &layer);
    Unit out;
    bool positionFree = leaf2D && !clip;
    if (!clip) {
        out.px = applyMasks(node, post.px);
        if (post.ink.valid()) out.ink = applyMasks(node, post.ink);
    } else {
        TitlePx buf = post.px;
        if (buf.slot == content.mask.slot) {
            // no base effects ran: the clip group needs a buffer of its own, the mask stays intact
            buf = copyOf(content.mask, node.layerId);
        }
        auto drawn = composeInk(node.clipped, buf, post.ink, &content.mask, wantInk);
        release(content.mask.slot);
        out.px = applyMasks(node, drawn.first);
        if (drawn.second.valid()) out.ink = applyMasks(node, drawn.second);
    }
    // masks in canvas space do not move with the layer
    for (const Json& m : layer.value("masks", Json::array())) {
        if (!m.value("enabled", true)) continue;
        if (!m.value("linked", true) || m.value("type", std::string()) == "layer" || m.value("type", std::string()) == "image") positionFree = false;
    }
    if (cacheable && out.px.valid()) {
        auto entry = std::make_shared<TitleCacheEntry>();
        entry->key = key;
        entry->rect = leaf2D && !clip ? TitleRectI{rect.x - ax, rect.y - ay, rect.width, rect.height} : rect;
        // a node the region's edge cut is not whole at another offset
        const bool whole = positionFree && rect.x > outRegion.x && rect.y > outRegion.y &&
                           rect.x + rect.width < outRegion.x + outRegion.width && rect.y + rect.height < outRegion.y + outRegion.height;
        // and one whose definitions read canvas positions is known only once they ran
        cpu("position free", node.layerId, 1, [entry, whole, reads](TitleJobState&, std::string&) {
            entry->positionFree = whole && !*reads;
            return true;
        });
        store(entry, out.px.slot);
    }
    if (keepSplit && out.px.valid()) sources["split:" + node.layerId] = copyOf(out.px, node.layerId);
    return out;
}

TitlePlanner::Content TitlePlanner::unitContent(const DrawNode& node, bool wantMask, bool wantInk) {
    const TitleRectI rect = nodeRect(node);
    Content out;
    if (node.kind == DrawNodeKind::Group) {
        TitlePx buf = clearedLike(rect, node.layerId);
        if (wantInk) {
            auto drawn = composeInk(node.children, buf, {}, nullptr, true);
            out.px = drawn.first;
            out.mask = drawn.first;
            out.ink = drawn.second;
        } else {
            out.px = compose(node.children, buf, nullptr);
            out.mask = out.px;
        }
        return out;
    }
    if (node.type == LayerType::Generator) {
        out.px = generator(node);
        out.mask = out.px;
        return out;
    }
    if (!layerTypeHasStyle(node.type)) return out;
    const Json& layer = *node.layer;
    const std::vector<std::string> parts = titleParts(layer);
    std::vector<bool> withFx;
    bool anyFx = false;
    for (const std::string& part : parts) {
        withFx.push_back(anyEnabled(partEffects(layer, part)));
        anyFx = anyFx || withFx.back();
    }
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    const bool threeD = is3D(node);
    auto content = [&](const std::vector<std::string>& run, const TitleRectI* fixed) {
        return threeD ? title3D(node, run, fixed) : title2D(node, run, fixed);
    };
#else
    auto content = [&](const std::vector<std::string>& run, const TitleRectI* fixed) { return title2D(node, run, fixed); };
#endif
    if (!anyFx) {
        out.px = content({}, nullptr);
        out.mask = out.px;
        return out;
    }
    // part effects: runs back to front, consecutive parts without effects together, a part with
    // effects alone through its own stack; 2D runs stack Normal (§6.3 step 1)
    TitlePx acc, mask, ink;
    if (wantMask) mask = clearedLike(rect, node.layerId);
    for (size_t i = 0; i < parts.size();) {
        const bool alone = withFx[i];
        size_t j = i + 1;
        if (!alone) {
            while (j < parts.size() && !withFx[j]) j++;
        }
        const std::vector<std::string> run(parts.begin() + (long)i, parts.begin() + (long)j);
        TitlePx rt = content(run, &rect);
        if (mask.valid()) blendOnto(mask, rt, 1.0f, (int)TitleBlend::Normal, nullptr, node.layerId);
        Effects post{rt, {}};
        if (alone) post = applyEffects(node.layerId + "#" + parts[i], partEffects(layer, parts[i]), rt, false, {}, wantInk, &layer);
        if (wantInk) {
            if (ink.valid()) {
                blendOnto(ink, post.ink.valid() ? post.ink : post.px, 1.0f, (int)TitleBlend::Normal, nullptr, node.layerId);
                if (post.ink.valid()) release(post.ink.slot);
            } else if (post.ink.valid()) {
                ink = post.ink;
            } else {
                ink = copyOf(post.px, node.layerId);
            }
        }
        if (acc.valid()) {
            blendOnto(acc, post.px, 1.0f, (int)TitleBlend::Normal, nullptr, node.layerId);
            release(post.px.slot);
        } else {
            acc = post.px;
        }
        i = j;
    }
    if (!acc.valid()) acc = clearedLike(rect, node.layerId);
    out.px = acc;
    out.mask = mask.valid() ? mask : acc;
    out.ink = ink;
    return out;
}

TitlePx TitlePlanner::adjustOnto(TitlePx acc, const DrawNode& node, const TitlePx* clipMask) {
    const Json& layer = *node.layer;
    const Json& adj = layer.value("adjustment", Json::object());
    TitlePx adjusted;
    if (titleAdjustmentIsIdentity(adj)) {
        // an identity adjustment only runs its effects on what is below it (none: zero passes)
        Effects post = applyEffects(node.layerId, effectsOf(layer), acc, true, {}, false, &layer);
        if (post.px.slot == acc.slot) return acc;
        adjusted = post.px;
    } else {
        // the operation is the `adjust` effect's (TitleAdjust.h): one implementation for both
        adjusted = {newSlot(TitleBuffer::Accumulator, acc.rect.width, acc.rect.height), acc.rect};
        const int from = acc.slot, to = adjusted.slot;
        const TitleRectI rect = acc.rect;
        const Json adjustment = adj;
        const PassPlace place = passPlace(rect, &layer);
        const uint32_t seed = (uint32_t)document->root.value("seed", (int64_t)1);
        const float pxScale = (float)scale;
        const std::string layerId = node.layerId;
        kernel(std::string("adjust ") + adj.value("type", std::string()), layerId, rect, 1, 1.0f,
               [from, to, rect, adjustment, place, seed, pxScale, layerId](TitleJobState& state, TitleJobRunner& runner, std::string& error) {
                   std::string why;
                   if (!state.target(to, why) || !state.target(from, why)) {
                       error = why;
                       return false;
                   }
                   auto refOf = [&state](int slot) -> const TargetRef& {
                       TitleJobState::Slot& sl = state.slots[(size_t)slot];
                       return sl.entry ? sl.entry->target : sl.ref;
                   };
                   EffectContext ctx(runner);
                   ctx.width = rect.width;
                   ctx.height = rect.height;
                   ctx.pxScale = pxScale;
                   ctx.frame = place.frame;
                   ctx.documentToTarget = place.documentToTarget;
                   ctx.layerOrigin = place.layerOrigin;
                   ctx.seed = seed;
                   ctx.layerId = layerId;
                   if (!applyTitleAdjustment(ctx, refOf(from), adjustment, state.slots[(size_t)to].ref)) {
                       error = "adjust: " + (runner.error().empty() ? std::string("the operation could not run") : runner.error());
                       return false;
                   }
                   return true;
               });
        adjusted = applyEffects(node.layerId, effectsOf(layer), adjusted, false, {}, false, &layer).px;
    }
    const float opacity = opacityOf(layer);
    const int mode = blendOf(layer);
    TitlePx layerMask = combinedMask(layer, acc.rect);
    if (opacity >= 1.0f && mode == (int)TitleBlend::Normal && !clipMask && !layerMask.valid()) {
        release(acc.slot);
        return adjusted;
    }
    TitlePx next{newSlot(TitleBuffer::Accumulator, acc.rect.width, acc.rect.height), acc.rect};
    const int dstSlot = acc.slot, srcSlot = adjusted.slot;
    const int clipSlot = clipMask ? clipMask->slot : -1, maskSlot = layerMask.slot;
    const std::array<float, 4> same = uvMap(acc.rect, acc.rect);
    const std::array<float, 4> clipMap = clipMask ? uvMap(acc.rect, clipMask->rect) : std::array<float, 4>{};
    const std::array<float, 4> maskMap = layerMask.valid() ? uvMap(acc.rect, layerMask.rect) : std::array<float, 4>{};
    pass("adjust mix", node.layerId, next.slot, {0, 0, acc.rect.width, acc.rect.height}, TitleRenderProgram::AdjustMix,
         [=](TitleJobState& s, TitleGpuPass& p, std::string&) {
             p.tex[0] = s.texture(dstSlot);
             p.tex[1] = s.texture(srcSlot);
             p.params.set(2, opacity, (float)mode, clipSlot >= 0 ? 1.0f : 0.0f, maskSlot >= 0 ? 1.0f : 0.0f);
             setMap(p, 3, same);
             if (clipSlot >= 0) {
                 p.tex[2] = s.texture(clipSlot);
                 setMap(p, 4, clipMap);
             }
             if (maskSlot >= 0) {
                 p.tex[3] = s.texture(maskSlot);
                 setMap(p, 5, maskMap);
             }
             return true;
         });
    release(adjusted.slot);
    release(acc.slot);
    if (layerMask.valid()) release(layerMask.slot);
    return next;
}

TitlePlanner::Effects TitlePlanner::applyEffects(const std::string& stackId, const Json& effects, TitlePx input, bool keepInput,
                                                 TitlePx inputInk, bool wantInk, const Json* layer) {
    struct Run {
        const EffectDef* def;
        Json params;
        Json effect;
    };
    std::vector<Run> run;
    int lastOutline = -1;
    for (const Json& fx : effects) {
        if (!fx.value("enabled", true)) continue;
        const std::string type = fx.value("type", std::string());
        const EffectDef* def = findEffect(type);
        if (!def || !def->apply) {
            TitleProblem p;
            p.code = def ? "effect_unavailable" : "unknown_type";
            p.layer = layer ? layer->value("id", std::string()) : std::string();
            p.message = def ? "the effect " + type + " has no pass code in this build: it draws nothing"
                            : "the effect type " + type + " is unknown to this build: it draws nothing";
            problem(p);
            continue;
        }
        const ParamValues values = paramsOf(*def, fx, *document);
        // the ink output leaves light out
        if (inkOnly && effectAddsLight(*def, values)) continue;
        if (def->outline) lastOutline = (int)run.size();
        run.push_back({def, fx.value("params", Json::object()), fx});
    }
    const int inkUntil = wantInk ? (int)run.size() : lastOutline;
    TitlePx cur = input;
    TitlePx ink = inputInk;
    auto drop = [&](TitlePx rt) {
        if (rt.valid() && rt.slot != input.slot && rt.slot != cur.slot && rt.slot != ink.slot) release(rt.slot);
    };
    auto dropInk = [&]() {
        TitlePx prev = ink;
        ink = {};
        drop(prev);
    };
    const std::shared_ptr<const TitleArtDoc> doc = document;
    const double s = scale;
    const Json& light = document->root.value("global_light", Json::object());
    const double lightAngle = light.value("angle", 120.0), lightAltitude = light.value("altitude", 30.0);
    const uint32_t seed = (uint32_t)document->root.value("seed", (int64_t)1);
    const std::string layerId = layer ? layer->value("id", std::string()) : std::string();
    // run one definition on `src` into a new target, with the context's content and ink
    auto apply = [&](const Run& r, TitlePx src, TitlePx contentPx, TitlePx inkPx, TitlePx inputInkPx, const std::string& id) {
        TitlePx out{newSlot(TitleBuffer::Accumulator, src.rect.width, src.rect.height), src.rect};
        uint32_t passes = 1;
        if (r.def->estimate) {
            const FxCost cost = r.def->estimate(paramsOf(*r.def, r.effect, *doc), FxSize{src.rect.width, src.rect.height, (float)s}, FxTier::Desktop);
            passes = (uint32_t)std::max(1, cost.passes);
        }
        const EffectDef* def = r.def;
        const Json effect = r.effect;
        const int srcSlot = src.slot, outSlot = out.slot, contentSlot = contentPx.slot, inkSlot = inkPx.slot, inputInkSlot = inputInkPx.slot;
        const TitleRectI rect = src.rect;
        const PassPlace place = passPlace(rect, layer);
        auto image = imageLookup(passImages(def->params, r.params, layerId.empty() ? id : layerId));
        std::shared_ptr<bool> reads = canvasReads;
        kernel(std::string(def->type), stackId, rect, passes, 1.0f, [=](TitleJobState& state, TitleJobRunner& runner, std::string& error) {
            std::string why;
            if (!state.target(outSlot, why) || !state.target(srcSlot, why)) {
                error = why;
                return false;
            }
            auto refOf = [&state](int slot) -> const TargetRef* {
                if (slot < 0) return nullptr;
                TitleJobState::Slot& sl = state.slots[(size_t)slot];
                return sl.entry ? &sl.entry->target : &sl.ref;
            };
            EffectContext ctx(runner);
            ctx.width = rect.width;
            ctx.height = rect.height;
            ctx.pxScale = (float)s;
            ctx.frame = place.frame;
            ctx.documentToTarget = place.documentToTarget;
            ctx.layerOrigin = place.layerOrigin;
            ctx.seed = seed;
            ctx.lightAngle = lightAngle;
            ctx.lightAltitude = lightAltitude;
            ctx.content = refOf(contentSlot);
            ctx.ink = refOf(inkSlot >= 0 ? inkSlot : contentSlot);
            ctx.inputInk = refOf(inputInkSlot >= 0 ? inputInkSlot : srcSlot);
            ctx.layerId = id;
            ctx.image = image;
            TitleJobState::Slot& outRef = state.slots[(size_t)outSlot];
            def->apply(ctx, *refOf(srcSlot), outRef.ref, paramsOf(*def, effect, *doc));
            if (reads && ctx.usesCanvasPosition) *reads = true;
            if (!runner.error().empty()) {
                error = std::string(def->type) + ": " + runner.error();
                return false;
            }
            return true;
        });
        return out;
    };
    for (int i = 0; i < (int)run.size(); i++) {
        const Run& r = run[(size_t)i];
        const TitlePx inputInkPx = r.def->outline && ink.valid() ? ink : cur;
        TitlePx out = apply(r, cur, input, inputInk.valid() ? inputInk : input, inputInkPx, stackId);
        if (i < inkUntil) {
            if (effectAddsLight(*r.def, ParamValues(r.params))) {
                // the light stays out of the ink: the stack before it is the ink from here on
                if (!ink.valid()) ink = cur;
            } else if (ink.valid()) {
                // the same effect on the ink alone (its own id: what effects keep per layer stays apart)
                TitlePx contentInk = inputInk.valid() ? inputInk : input;
                TitlePx inkOut = apply(r, ink, contentInk, contentInk, ink, stackId + "~ink");
                TitlePx prev = ink;
                ink = inkOut;
                drop(prev);
            }
        } else {
            dropInk();
        }
        TitlePx prev = cur;
        cur = out;
        drop(prev);
    }
    if (!wantInk) {
        dropInk();
    } else if (ink.valid() && ink.slot == input.slot && keepInput) {
        // the caller keeps `input` itself: the ink is a target of its own
        ink = copyOf(input, stackId);
    }
    if (cur.slot != input.slot && !keepInput && ink.slot != input.slot) release(input.slot);
    return {cur, ink};
}
