// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/layer2d/Title2DRenderer.ts (frames, masks, かすれ, spans,
// knockout, shading order, placement), src/engine/layer2d/materialPass.ts (the fields each
// material needs and the element program's parameters), src/engine/patterns.ts
// (setPatternUniforms) and src/engine/layer2d/reflectionMaps.ts (the panorama blur) by SafEight,
// used with the author's permission.

#include "TitleFields.h"
#include "TitlePlanner.h"

#include "titleart/TitleArtIO.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;
    constexpr double MITER_LIMIT = 4.0;
    constexpr int MAX_FRAME_SIDE = 4096;
    constexpr double MAX_FRAME_PIXELS = 6000000.0;
    constexpr int FRAME_MARGIN = 3;         // px each side
    constexpr int FRAME_SNAP = 64;          // frame edges snap to this grid of mask px from the layer origin

    uint64_t hashBytes(const void* data, size_t size) {
        return fnv1a64(std::string_view(static_cast<const char*>(data), size));
    }
    uint64_t hashDouble(uint64_t h, double v) {
        return hashCombine(h, hashBytes(&v, sizeof(v)));
    }

    const Json& obj(const Json& o, const char* key) {
        static const Json empty = Json::object();
        if (!o.is_object()) return empty;
        auto it = o.find(key);
        return it != o.end() && it->is_object() ? *it : empty;
    }
    double num(const Json& o, const char* key, double fallback) {
        if (!o.is_object()) return fallback;
        auto it = o.find(key);
        return it != o.end() && it->is_number() ? it->get<double>() : fallback;
    }
    bool on(const Json& o, const char* key = "enabled", bool fallback = false) {
        if (!o.is_object()) return fallback;
        auto it = o.find(key);
        return it != o.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }
    std::string str(const Json& o, const char* key, const char* fallback) {
        if (!o.is_object()) return fallback;
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : fallback;
    }
    TitleColor color(const Json& o, const char* key, const char* fallback) {
        TitleColor c;
        auto it = o.is_object() ? o.find(key) : o.end();
        if (it == o.end() || !parseTitleColor(*it, c)) parseTitleColor(Json(fallback), c);
        return c;
    }

    int indexOf(const std::string& value, std::initializer_list<const char*> names, int fallback = 0) {
        int i = 0;
        for (const char* n : names) {
            if (value == n) return i;
            i++;
        }
        return fallback;
    }

    int fillKindId(const std::string& kind) {
        return indexOf(kind, {"solid", "linear", "radial", "angle", "reflected", "diamond", "shape_burst", "image"}, 0);
    }
    int profileId(const std::string& p) {
        return indexOf(p, {"smooth", "button", "mesa", "chamfer", "ridge", "cove", "stepped", "custom"}, 0);
    }
    int glossId(const std::string& g) {
        return indexOf(g, {"linear", "ring", "double_ring", "cove", "rounded_steps", "sawtooth", "custom"}, 0);
    }
    int textureId(const std::string& t) {
        return indexOf(t, {"none", "noise", "sparkle", "brushed", "ripple", "grunge", "clouds", "hammered", "crumple", "liquid", "hairline",
                           "spun", "flakes"},
                       0);
    }
    int textureBlendId(const std::string& b) { return indexOf(b, {"overlay", "soft_light", "screen", "multiply"}, 0); }
    int satinBlendId(const std::string& b) { return indexOf(b, {"normal", "multiply", "screen", "overlay"}, 1); }

    // the 15 procedural kinds and the custom tile (titleart/patterns.glsl)
    int patternKindId(const std::string& kind) {
        return indexOf(kind, {"none", "zebra", "tiger", "giraffe", "leopard", "checker", "hazard", "stripes", "dots", "sakura", "seigaiha",
                              "asanoha", "shippo", "kikko", "uroko", "custom"},
                       0);
    }

    // the layer-space angle (deg, counter-clockwise, y-up) of screen direction `deg` under the
    // placement: undoes the rotation, then the flip, so lights stay fixed on screen
    double layerAngle(double deg, const TitlePlacement& t) {
        const double a = deg + t.rotation;
        return t.flipX ? 180.0 - a : a;
    }
    // the inverse: the screen direction of layer-space angle `deg`
    double screenAngle(double deg, const TitlePlacement& t) {
        return t.flipX ? 180.0 - deg - t.rotation : deg - t.rotation;
    }

    // one box pass of radius r along rows (wrapping: the panorama is periodic) or columns (clamped)
    void boxPass(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int r, bool horizontal) {
        const int n = horizontal ? w : h;
        const int lines = horizontal ? h : w;
        const double k = 1.0 / (2 * r + 1);
        auto at = [&](int line, int i) {
            const int j = horizontal ? ((i % w) + w) % w : std::clamp(i, 0, h - 1);
            return (size_t)(horizontal ? line * w + j : j * w + line) * 3;
        };
        for (int line = 0; line < lines; line++) {
            for (int c = 0; c < 3; c++) {
                double sum = 0.0;
                for (int i = -r; i <= r; i++) sum += src[at(line, i) + c];
                for (int i = 0; i < n; i++) {
                    dst[at(line, i) + c] = (float)(sum * k);
                    sum += src[at(line, i + r + 1) + c] - src[at(line, i - r) + c];
                }
            }
        }
    }

    // the coverage of a frame, rows top first (the provider's), as the frame's y-up rows
    std::vector<uint8_t> flipRows(const std::vector<uint8_t>& pixels, int w, int h, int bytesPerPixel) {
        std::vector<uint8_t> out(pixels.size());
        const size_t row = (size_t)w * bytesPerPixel;
        for (int y = 0; y < h; y++) std::memcpy(out.data() + (size_t)(h - 1 - y) * row, pixels.data() + (size_t)y * row, row);
        return out;
    }

    bool partDrawn(const std::vector<std::string>& parts, const geometry::TitleElement& e) {
        if (parts.empty()) return true;
        const std::string key = e.kind == geometry::ElementKind::Face ? "face" : "rim:" + e.rimId;
        return std::find(parts.begin(), parts.end(), key) != parts.end();
    }

    void setP(TitleGpuPass& p, int i, const std::array<float, 4>& v) { p.params.set(i, v[0], v[1], v[2], v[3]); }

}

std::vector<std::string> TitlePlanner::titleParts(const Json& layer) {
    // back to front: the outermost rim first, the face last (Pachislop's layerParts)
    std::vector<std::string> parts;
    const Json& rims = obj(layer, "style").value("rims", Json::array());
    for (auto it = rims.rbegin(); it != rims.rend(); ++it) {
        if (!on(*it, "enabled", true) || !(num(*it, "width", 0) > 0)) continue;
        parts.push_back("rim:" + it->value("id", std::string()));
    }
    parts.push_back("face");
    return parts;
}

const TitlePlanner::TitleContentPlan& TitlePlanner::contentPlan(const DrawNode& node) {
    auto known = contentPlans.find(node.layerId);
    if (known != contentPlans.end()) return known->second;
    TitleContentPlan& plan = contentPlans[node.layerId];
    const Json& layer = *node.layer;
    if (!svc.content) {
        TitleProblem p;
        p.code = "geometry_unavailable";
        p.layer = node.layerId;
        p.message = "this build has no text and geometry provider: the layer draws nothing";
        problem(p);
        return plan;
    }
    const TitleElementFilter filter = node.layerId == filteredLayer ? elementFilter : TitleElementFilter{};
    plan.elements = svc.content->elements(layer, *document, req.assets, filter);
    for (const TitleProblem& p : plan.elements.problems) problem(p);
    plan.place = placementOf(layer);
    plan.size = layerSourceSize(layer);
    plan.geometry = hashCombine(layerHash(node.layerId, &LayerHashes::geometry), (uint64_t)(filter.cluster + 2));
    if (plan.elements.elements.empty() || !(plan.place.scale > 0.0)) return plan;

    // the frame (Pachislop's frameFor): every element's base bounds plus its pad (miter: grow x
    // 4, else grow), at m = layer scale x max(px_scale, 1) mask px per layer unit, so a 0.5x
    // preview shades exactly like a 1x bake; its edges snap to a 64 px grid from the layer
    // origin, so a small rim change keeps the frame and every other element's caches
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    const TitleRect b = plan.elements.baseBounds;
    for (const geometry::TitleElement& e : plan.elements.elements) {
        const bool miter = e.join == geometry::StrokeJoin::Miter && !plan.elements.fromAlpha;
        const double pad = miter ? e.grow * MITER_LIMIT : e.grow;
        x0 = std::min(x0, b.x - pad + e.offset.x);
        y0 = std::min(y0, b.y - pad + e.offset.y);
        x1 = std::max(x1, b.x + b.width + pad + e.offset.x);
        y1 = std::max(y1, b.y + b.height + pad + e.offset.y);
    }
    const double fw = std::max(x1 - x0, 1e-3), fh = std::max(y1 - y0, 1e-3);
    double m = plan.place.scale * std::max(scale, 1.0);
    m = std::min({m, (MAX_FRAME_SIDE - 2.0 * FRAME_MARGIN - 2.0 * FRAME_SNAP) / fw, (MAX_FRAME_SIDE - 2.0 * FRAME_MARGIN - 2.0 * FRAME_SNAP) / fh});
    if ((fw * m + 2 * FRAME_MARGIN) * (fh * m + 2 * FRAME_MARGIN) > MAX_FRAME_PIXELS) {
        m = std::sqrt(MAX_FRAME_PIXELS / ((fw + 1.0) * (fh + 1.0))) * 0.98;
    }
    if (m < plan.place.scale * std::max(scale, 1.0) * 0.999) {
        TitleProblem p;
        p.code = "title_frame_reduced";
        p.layer = node.layerId;
        p.message = "the title is too large to shade at full resolution: it is shaded coarser";
        problem(p);
    }
    auto snapDown = [m](double v) { return std::floor((v * m - FRAME_MARGIN) / FRAME_SNAP) * FRAME_SNAP / m; };
    auto snapUp = [m](double v) { return std::ceil((v * m + FRAME_MARGIN) / FRAME_SNAP) * FRAME_SNAP / m; };
    plan.x0 = snapDown(x0);
    plan.y0 = snapDown(y0);
    plan.x1 = snapUp(x1);
    plan.y1 = snapUp(y1);
    plan.m = m;
    plan.w = std::max(1, (int)std::lround((plan.x1 - plan.x0) * m));
    plan.h = std::max(1, (int)std::lround((plan.y1 - plan.y0) * m));
    uint64_t key = hashCombine(plan.geometry, fnv1a64("frame"));
    for (double v : {plan.x0, plan.y0, plan.x1, plan.y1, plan.m}) key = hashDouble(key, v);
    key = hashCombine(key, fnv1a64(svc.caps.pathName()));
    plan.key = key;
    plan.valid = true;
    return plan;
}

TitleRectI TitlePlanner::title2DRect(const DrawNode& node) {
    const TitleContentPlan& plan = contentPlan(node);
    if (!plan.valid) return {};
    const TitleFrame whole = TitleFrame::canvas(document->canvasWidth(), document->canvasHeight(), scale);
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const TitleVec2& corner : {TitleVec2{plan.x0, plan.y0}, TitleVec2{plan.x1, plan.y0}, TitleVec2{plan.x0, plan.y1}, TitleVec2{plan.x1, plan.y1}}) {
        const TitleVec2 px = whole.toFrame(plan.place.toDocument.apply(corner));
        minX = std::min(minX, px.x);
        minY = std::min(minY, px.y);
        maxX = std::max(maxX, px.x);
        maxY = std::max(maxY, px.y);
    }
    const int ix0 = (int)std::floor(minX), iy0 = (int)std::floor(minY);
    return intersectRect({ix0, iy0, (int)std::ceil(maxX) - ix0, (int)std::ceil(maxY) - iy0}, outRegion);
}

std::shared_ptr<TitleCacheEntry> TitlePlanner::assetTexture(const Json& refJson, TitleAssetKind kind, TextureWrap wrap, int blur,
                                                            const std::string& layerId, const std::string& path) {
    const TitleAssetRef ref = decodeAssetRef(refJson);
    if (ref.empty()) return nullptr;
    auto missing = [&](const std::string& why) {
        TitleProblem p;
        p.code = "asset_missing";
        p.layer = layerId;
        p.path = path;
        p.message = (ref.path.empty() ? ref.guid : ref.path) + ": " + why;
        problem(p);
        return nullptr;
    };
    if (!req.assets) return missing("no asset resolver for this render");
    const ResolvedTitleAsset resolved = req.assets->resolve(ref, kind);
    if (!resolved.found) return missing("not found");
    uint64_t key = hashCombine(fnv1a64("asset"), fnv1a64(resolved.guid + "|" + resolved.assetPath));
    key = hashCombine(key, resolved.revision);
    key = hashCombine(key, (uint64_t)blur + 1);
    key = hashCombine(key, (uint64_t)wrap);
    if (auto hit = svc.cache.find(key)) return hit;
    TitleImageData image;
    std::string why;
    if (!req.assets->loadImage(resolved, image, &why) || image.width <= 0 || image.height <= 0) return missing(why.empty() ? "unreadable" : why);
    std::vector<uint8_t> pixels = image.rgba;
    if (blur > 0) {
        // the reflection panorama's blur (Eye Candy's 100 = sigma 4 % of the width), three box
        // passes each way, wrapping horizontally
        const int w = image.width, h = image.height;
        const double sigma = blur / 100.0 * 0.04 * w;
        const int r = (int)std::lround((std::sqrt(4.0 * sigma * sigma + 1.0) - 1.0) / 2.0);
        if (r >= 1) {
            std::vector<float> a((size_t)w * h * 3), tmp(a.size());
            for (size_t i = 0; i < (size_t)w * h; i++) {
                for (int c = 0; c < 3; c++) a[i * 3 + c] = pixels[i * 4 + c] / 255.0f;
            }
            for (bool horizontal : {true, false}) {
                for (int i = 0; i < 3; i++) {
                    boxPass(a, tmp, w, h, r, horizontal);
                    std::swap(a, tmp);
                }
            }
            for (size_t i = 0; i < (size_t)w * h; i++) {
                for (int c = 0; c < 3; c++) pixels[i * 4 + c] = (uint8_t)std::lround(std::clamp(a[i * 3 + c], 0.0f, 1.0f) * 255.0f);
                pixels[i * 4 + 3] = 255;
            }
        }
    }
    auto entry = std::make_shared<TitleCacheEntry>();
    entry->key = key;
    entry->upload = std::make_unique<TitleUpload>();
    if (!entry->upload->create("title art image", image.width, image.height, ColorFormat::RGBA, pixels.data(), pixels.size(),
                               TextureFilter::LINEAR, wrap, TextureWrap::CLAMP_TO_EDGE)) {
        return missing("the texture could not be made");
    }
    entry->value = (float)image.width / (float)std::max(1, image.height);
    svc.cache.insert(entry);
    return entry;
}

TitlePx TitlePlanner::title2D(const DrawNode& node, const std::vector<std::string>& parts, const TitleRectI* fixedRect) {
    const TitleContentPlan& plan = contentPlan(node);
    const TitleRectI rect = fixedRect ? *fixedRect : nodeRect(node);
    if (!plan.valid || rect.empty()) return {};
    const Json& layer = *node.layer;
    const std::string& id = node.layerId;
    const std::vector<geometry::TitleElement>& elements = plan.elements.elements;
    const int W = plan.w, H = plan.h;
    const double m = plan.m;
    const double unit = m * plan.size / 100.0;          // mask px per 1 % of the size
    const Json& style = obj(layer, "style");
    const bool knockout = on(style, "knockout");
    const Json& erode = obj(style, "erode");
    const bool eroding = on(erode) && num(erode, "amount", 0.5) > 0 && num(erode, "edge", 6) > 0;
    const int64_t docSeed = document->root.value("seed", (int64_t)1);
    std::shared_ptr<TitleContentProvider> content = svc.content;
    const AssetResolver* assets = req.assets;
    std::shared_ptr<const TitleArtDoc> doc = document;

    // ---- 2. coverage masks (CPU raster, uploaded once per mask key) ----
    std::vector<uint64_t> maskKeys(elements.size());
    std::vector<int> maskSlots(elements.size(), -1);
    const std::array<float, 4> distEnc = titleEncoding(svc.caps.format(TitleBuffer::Distance), TitleValueKind::Distance);
    for (size_t i = 0; i < elements.size(); i++) {
        const geometry::TitleElement& e = elements[i];
        uint64_t key = hashCombine(plan.key, fnv1a64("mask"));
        key = hashCombine(key, (uint64_t)e.kind);
        key = hashCombine(key, (uint64_t)(e.faceGroup + 1));
        key = hashDouble(key, e.grow);
        key = hashCombine(key, (uint64_t)e.join);
        key = hashDouble(key, e.miterLimit);
        key = hashCombine(key, e.fillCounters ? 1 : 2);
        key = hashDouble(key, e.offset.x);
        key = hashDouble(key, e.offset.y);
        if (e.kind == geometry::ElementKind::Rim) {
            // the rim's plate and join decide its outline (its width is the cumulative grow)
            const Json& rims = style.value("rims", Json::array());
            if (e.rimIndex >= 0 && e.rimIndex < (int)rims.size()) key = hashCombine(key, fnv1a64(writeTitleArtJson(obj(rims[(size_t)e.rimIndex], "plate"))));
        }
        maskKeys[i] = key;
        if (auto hit = svc.cache.find(key)) {
            maskSlots[i] = entrySlot(hit);
            continue;
        }
        auto entry = std::make_shared<TitleCacheEntry>();
        entry->key = key;
        geometry::MaskFrame frame;
        frame.width = W;
        frame.height = H;
        frame.scale = m;
        // the frame shifted against the element's offset draws it offset
        frame.origin = {plan.x0 - e.offset.x, plan.y1 - e.offset.y};
        const bool grownFromAlpha = plan.elements.fromAlpha && e.kind == geometry::ElementKind::Rim && e.grow > 0.0;
        if (!grownFromAlpha) {
            const geometry::TitleElement element = e;
            cpu("coverage", id, (uint32_t)(W * H / 400 + 100), [entry, content, element, frame](TitleJobState&, std::string& error) {
                geometry::CoverageMask mask = content->coverage(element, frame);
                if (mask.width != frame.width || mask.height != frame.height || mask.pixels.size() != (size_t)frame.width * frame.height) {
                    error = "an element's coverage could not be rasterized";
                    return false;
                }
                const std::vector<uint8_t> rows = flipRows(mask.pixels, frame.width, frame.height, 1);
                entry->upload = std::make_unique<TitleUpload>();
                return entry->upload->create("title art coverage", frame.width, frame.height, ColorFormat::RED, rows.data(), rows.size(),
                                             TextureFilter::LINEAR);
            });
            maskSlots[i] = entrySlot(entry);
            cpu("cache", id, 1, [entry](TitleJobState& s, std::string&) {
                s.services.cache.insert(entry);
                return true;
            });
            continue;
        }
        // an image (or artwork) rim grows from the alpha's distance field (round joins)
        geometry::TitleElement face = e;
        face.kind = geometry::ElementKind::Face;
        face.grow = 0.0;
        face.offset = {};
        geometry::MaskFrame faceFrame = frame;
        faceFrame.origin = {plan.x0, plan.y1};
        auto faceEntry = std::make_shared<TitleCacheEntry>();
        cpu("coverage", id, (uint32_t)(W * H / 400 + 100), [faceEntry, content, face, faceFrame](TitleJobState&, std::string& error) {
            geometry::CoverageMask mask = content->coverage(face, faceFrame);
            if (mask.width != faceFrame.width || mask.height != faceFrame.height) {
                error = "an element's coverage could not be rasterized";
                return false;
            }
            const std::vector<uint8_t> rows = flipRows(mask.pixels, faceFrame.width, faceFrame.height, 1);
            faceEntry->upload = std::make_unique<TitleUpload>();
            return faceEntry->upload->create("title art coverage", faceFrame.width, faceFrame.height, ColorFormat::RED, rows.data(),
                                             rows.size(), TextureFilter::LINEAR);
        });
        TitlePx alpha{entrySlot(faceEntry), {0, 0, W, H}};
        const double growPx = e.grow * m;
        std::array<float, 4> enc;
        TitlePx sdf = distanceField(alpha, false, 0.5f, (float)growPx + 2.0f, id, &enc);
        TitlePx grown{newSlot(TitleBuffer::Coverage, W, H), {0, 0, W, H}};
        const float offX = (float)(e.offset.x * m), offY = (float)(e.offset.y * m);
        const int sdfSlot = sdf.slot;
        pass("rim grow", id, grown.slot, {0, 0, W, H}, TitleRenderProgram::Grow, [sdfSlot, enc, growPx, offX, offY](TitleJobState& s, TitleGpuPass& p, std::string&) {
            p.raw[0] = s.texture(sdfSlot);
            setP(p, TITLE_PARAM_RAW_ENCODING, enc);
            p.params.set(2, (float)growPx, offX, offY);
            return true;
        });
        release(sdf.slot);
        release(alpha.slot);
        store(entry, grown.slot);
        maskSlots[i] = grown.slot;
    }

    // ---- 3. かすれ: the union's distance -> the keep-mask -> every mask x keep ----
    std::vector<uint64_t> shapeKeys = maskKeys;
    std::vector<int> shapeSlots = maskSlots;
    if (eroding) {
        uint64_t unionKey = hashCombine(plan.key, fnv1a64("erode"));
        for (uint64_t k : maskKeys) unionKey = hashCombine(unionKey, k);
        uint64_t keepKey = hashCombine(unionKey, fnv1a64(writeTitleArtJson(erode)));
        keepKey = hashCombine(keepKey, (uint64_t)docSeed);
        keepKey = hashDouble(keepKey, unit);
        int keepSlot = -1;
        if (auto hit = svc.cache.find(keepKey)) {
            keepSlot = entrySlot(hit);
        } else {
            TitlePx unionPx{newSlot(TitleBuffer::Coverage, W, H), {0, 0, W, H}};
            const float zero[4] = {0, 0, 0, 0};
            clearSlot("erode union", id, unionPx.slot, zero);
            for (int slot : maskSlots) {
                pass("erode union", id, unionPx.slot, {0, 0, W, H}, TitleRenderProgram::Copy,
                     [slot, W, H](TitleJobState& s, TitleGpuPass& p, std::string&) {
                         setupResample(p, s.texture(slot), W, H);
                         p.params.set(3, 1.0f, 1.0f);
                         return true;
                     },
                     BlendMode::MAX_PREMULTIPLIED);
            }
            const double edgePx = std::max(0.5, num(erode, "edge", 6) * unit);
            std::array<float, 4> enc;
            TitlePx sdf = distanceField(unionPx, false, 0.5f, (float)(edgePx + 3.0), id, &enc);
            TitlePx keep{newSlot(TitleBuffer::Coverage, W, H), {0, 0, W, H}};
            const double angle = num(erode, "angle", 0) * DEG;
            const float amount = (float)std::clamp(num(erode, "amount", 0.5), 0.0, 1.0);
            const float lenPx = (float)std::max(1.0, num(erode, "scale", 12) * unit);
            const float lanePx = (float)std::max(0.5, unit * 0.6);
            // the element seed with the document's (decision 8): seed 1 on both is Pachislop's
            const double seed = num(erode, "seed", 1) + (double)(docSeed - 1);
            const float seedX = (float)std::fmod(seed * 37.31, 997.0), seedY = (float)std::fmod(seed * 17.73, 991.0);
            const float originX = (float)(plan.x0 * m), originY = (float)(plan.y0 * m);
            const int u = unionPx.slot, sd = sdf.slot;
            pass("erode", id, keep.slot, {0, 0, W, H}, TitleRenderProgram::Erode, [=](TitleJobState& s, TitleGpuPass& p, std::string&) {
                p.tex[0] = s.texture(u);
                p.raw[0] = s.texture(sd);
                setP(p, TITLE_PARAM_RAW_ENCODING, enc);
                // clockwise on screen, 0 = a horizontal drag, as a y-up vector
                p.params.set(2, (float)std::cos(angle), (float)-std::sin(angle), amount, (float)edgePx);
                p.params.set(3, lenPx, lanePx, seedX, seedY);
                p.params.set(4, originX, originY);
                return true;
            });
            release(sdf.slot);
            release(unionPx.slot);
            auto entry = std::make_shared<TitleCacheEntry>();
            entry->key = keepKey;
            store(entry, keep.slot);
            keepSlot = keep.slot;
        }
        for (size_t i = 0; i < elements.size(); i++) {
            const uint64_t key = hashCombine(maskKeys[i], keepKey);
            shapeKeys[i] = key;
            if (auto hit = svc.cache.find(key)) {
                shapeSlots[i] = entrySlot(hit);
                continue;
            }
            TitlePx eroded{newSlot(TitleBuffer::Coverage, W, H), {0, 0, W, H}};
            const int maskSlot = maskSlots[i];
            pass("erode mask", id, eroded.slot, {0, 0, W, H}, TitleRenderProgram::MaskKeep, [maskSlot, keepSlot](TitleJobState& s, TitleGpuPass& p, std::string&) {
                p.tex[0] = s.texture(maskSlot);
                p.tex[1] = s.texture(keepSlot);
                return true;
            });
            auto entry = std::make_shared<TitleCacheEntry>();
            entry->key = key;
            store(entry, eroded.slot);
            shapeSlots[i] = eroded.slot;
        }
        release(keepSlot);
    }

    // ---- 9. the span map (line / char gradients): nearest glyph per frame pixel ----
    int spanSlot = -1, glyphSlot = -1;
    TitleSpans spans;
    bool needSpan = false;
    for (const geometry::TitleElement& e : elements) {
        if (!e.material) continue;
        const Json& fill = obj(*e.material, "fill");
        const std::string kind = str(fill, "kind", "solid");
        if (kind != "solid" && kind != "shape_burst" && str(fill, "span", "layer") != "layer") needSpan = true;
    }
    const TitleElementFilter filter = node.layerId == filteredLayer ? elementFilter : TitleElementFilter{};
    if (needSpan && content->spans(layer, *document, assets, filter, spans) && !spans.chars.empty()) {
        const int n = (int)spans.chars.size();
        const uint64_t tableKey = hashCombine(plan.geometry, fnv1a64("glyph boxes"));
        std::shared_ptr<TitleCacheEntry> table = svc.cache.find(tableKey);
        if (!table) {
            table = std::make_shared<TitleCacheEntry>();
            table->key = tableKey;
            std::vector<float> data((size_t)n * 2 * 4);
            for (int g = 0; g < n; g++) {
                const TitleRect& c = spans.chars[(size_t)g];
                const TitleRect& l = g < (int)spans.lines.size() ? spans.lines[(size_t)g] : spans.chars[(size_t)g];
                const float cb[4] = {(float)c.x, (float)c.y, (float)(c.x + c.width), (float)(c.y + c.height)};
                const float lb[4] = {(float)l.x, (float)l.y, (float)(l.x + l.width), (float)(l.y + l.height)};
                std::memcpy(&data[(size_t)g * 4], cb, sizeof(cb));
                std::memcpy(&data[(size_t)(n + g) * 4], lb, sizeof(lb));
            }
            table->upload = std::make_unique<TitleUpload>();
            if (table->upload->create("title art glyph boxes", n, 2, ColorFormat::RGBA32F, data.data(), data.size() * sizeof(float),
                                      TextureFilter::NEAREST)) {
                svc.cache.insert(table);
            } else {
                table.reset();
            }
        }
        if (table) {
            glyphSlot = entrySlot(table);
            const uint64_t spanKey = hashCombine(plan.key, fnv1a64("span"));
            if (auto hit = svc.cache.find(spanKey)) {
                spanSlot = entrySlot(hit);
            } else {
                TitlePx span{newSlot(TitleBuffer::Height, W, H), {0, 0, W, H}};
                const std::array<float, 4> enc = titleEncoding(svc.caps.format(TitleBuffer::Height), TitleValueKind::Index);
                const float fm = (float)m, fx0 = (float)plan.x0, fy0 = (float)plan.y0;
                const int gs = glyphSlot;
                pass("span", id, span.slot, {0, 0, W, H}, TitleRenderProgram::Span, [gs, n, fm, fx0, fy0, enc](TitleJobState& s, TitleGpuPass& p, std::string&) {
                    p.raw[0] = s.texture(gs);
                    setP(p, TITLE_PARAM_OUT_ENCODING, enc);
                    p.params.set(2, (float)n, fm, fx0, fy0);
                    return true;
                }, BlendMode::NONE, false, 4.0f * (float)n / 16.0f);
                auto entry = std::make_shared<TitleCacheEntry>();
                entry->key = spanKey;
                store(entry, span.slot);
                spanSlot = span.slot;
            }
        }
    }

    // ---- the stack (cached by everything it shows) ----
    const Json& light = document->root.value("global_light", Json::object());
    uint64_t stackKey = hashCombine(plan.key, fnv1a64("stack"));
    stackKey = hashCombine(stackKey, layerHash(id, &LayerHashes::stack));
    for (uint64_t k : shapeKeys) stackKey = hashCombine(stackKey, k);
    for (const std::string& part : parts) stackKey = hashCombine(stackKey, fnv1a64(part));
    stackKey = hashDouble(stackKey, plan.place.rotation);
    stackKey = hashCombine(stackKey, plan.place.flipX ? 1 : 2);
    stackKey = hashCombine(stackKey, fnv1a64(writeTitleArtJson(light)));
    stackKey = hashCombine(stackKey, (uint64_t)docSeed);
    int stackSlot = -1;
    if (auto hit = svc.cache.find(stackKey)) {
        stackSlot = entrySlot(hit);
    } else {
        const bool floatStack = svc.caps.format(TitleBuffer::Accumulator).format == ColorFormat::RGBA16F;
        const bool blends = !floatStack || svc.caps.floatBlend;
        TitlePx stack{newSlot(TitleBuffer::Accumulator, W, H), {0, 0, W, H}};
        const float zero[4] = {0, 0, 0, 0};
        clearSlot("stack", id, stack.slot, zero);
        std::vector<int> fieldSlots;
        // draws one element's ELEMENT pass into the stack: blended in fixed function, or (RGBA16F
        // without float blending) into a scratch target composed by the BLEND program
        auto drawIntoStack = [&](const std::string& label, std::function<bool(TitleJobState&, TitleGpuPass&, std::string&)> fill, bool punch) {
            if (blends) {
                pass(label, id, stack.slot, {0, 0, W, H}, TitleRenderProgram::Element, fill,
                     punch ? BlendMode::ERASE_PREMULTIPLIED : BlendMode::PREMULTIPLIED, false, 3.0f);
                return;
            }
            const int scratch = newSlot(TitleBuffer::Accumulator, W, H);
            pass(label, id, scratch, {0, 0, W, H}, TitleRenderProgram::Element, fill, BlendMode::NONE, false, 3.0f);
            const int backdrop = newSlot(TitleBuffer::Accumulator, W, H);
            const int stackS = stack.slot;
            pass("stack backdrop", id, backdrop, {0, 0, W, H}, TitleRenderProgram::Copy, [stackS, W, H](TitleJobState& s, TitleGpuPass& p, std::string&) {
                setupResample(p, s.texture(stackS), W, H);
                return true;
            });
            const std::array<float, 4> same{1.0f / W, 1.0f / H, 0.0f, 0.0f};
            pass("stack compose", id, stack.slot, {0, 0, W, H}, TitleRenderProgram::Blend, [backdrop, scratch, same, punch](TitleJobState& s, TitleGpuPass& p, std::string&) {
                p.tex[0] = s.texture(backdrop);
                p.tex[1] = s.texture(scratch);
                setP(p, 2, same);
                setP(p, 3, same);
                p.params.set(5, punch ? 19.0f : 0.0f, 1.0f, 0.0f);
                return true;
            });
            release(backdrop);
            release(scratch);
        };
        for (size_t ri = elements.size(); ri-- > 0;) {
            const geometry::TitleElement& e = elements[ri];
            const bool face = e.kind == geometry::ElementKind::Face;
            const bool drawn = partDrawn(parts, e) && e.material;
            const int coverage = shapeSlots[ri];
            if (face && knockout) {
                // knockout erases what the rims drew under each face first
                drawIntoStack("knockout", [coverage](TitleJobState& s, TitleGpuPass& p, std::string&) {
                    p.tex[0] = s.texture(coverage);
                    p.params.set(29, 1.0f);
                    return true;
                }, true);
            }
            if (!drawn) continue;
            const Json& mat = *e.material;
            const Json& fill = obj(mat, "fill");
            const Json& bevel = obj(mat, "bevel");
            const Json& innerGlow = obj(mat, "inner_glow");
            const Json& satin = obj(mat, "satin");
            const std::string fillKind = str(fill, "kind", plan.elements.fromAlpha ? "source" : "solid");
            // a rim's band (its own width, frame px): shape bursts and Fit tubes run across it
            double bandPx = 0.0;
            if (!face && e.rimIndex >= 0) {
                auto rims = style.find("rims");
                if (rims != style.end() && rims->is_array() && e.rimIndex < (int)rims->size()) {
                    bandPx = num((*rims)[(size_t)e.rimIndex], "width", 0) / 100.0 * plan.size * m;
                }
            }
            const bool burst = fillKind == "shape_burst";
            const bool bevelOn = on(bevel);
            const bool fit = str(bevel, "size_mode", "fixed") == "fit";
            const double bevelPx = std::max(0.5, num(bevel, "size", 3) * unit);
            const double softPx = std::max(0.0, num(bevel, "soften", 0) * unit);
            const double glowPx = std::max(0.5, num(innerGlow, "size", 2) * unit);
            const bool needSdf = bevelOn || on(innerGlow) || burst;

            // ---- 4. the distance field, flooded only as far as its consumers read (R27) ----
            double reach = 0.0;
            bool unbounded = false;
            if (bevelOn) {
                if (fit && face) unbounded = true;
                else reach = std::max(reach, (fit ? bandPx : bevelPx) + 2.0);
            }
            if (on(innerGlow)) reach = std::max(reach, glowPx + 2.0);
            if (burst) {
                if (face) unbounded = true;
                else reach = std::max(reach, bandPx + 2.0);
            }
            const float R = unbounded || (req.testFlags & TITLE_ART_TEST_UNBOUNDED_FIELDS) ? 0.0f : (float)std::ceil(reach);
            int sdfSlot = -1;
            uint64_t sdfKey = 0;
            if (needSdf) {
                sdfKey = hashCombine(hashCombine(shapeKeys[ri], fnv1a64("sdf")), (uint64_t)R + 1);
                if (auto hit = svc.cache.find(sdfKey)) {
                    sdfSlot = entrySlot(hit);
                } else {
                    TitlePx sdf = distanceField({coverage, {0, 0, W, H}}, false, 0.5f, R, id, nullptr);
                    auto entry = std::make_shared<TitleCacheEntry>();
                    entry->key = sdfKey;
                    store(entry, sdf.slot);
                    sdfSlot = sdf.slot;
                }
                fieldSlots.push_back(sdfSlot);
            }
            const std::array<float, 4> heightEnc = titleEncoding(svc.caps.format(TitleBuffer::Height), TitleValueKind::Unit);
            const std::array<float, 4> pixelsEnc = titleEncoding(svc.caps.format(TitleBuffer::Height), TitleValueKind::Pixels);

            // ---- 5, 6. Fit cover and the bevel height ----
            int heightSlot = -1;
            if (bevelOn && sdfSlot >= 0) {
                uint64_t key = hashCombine(sdfKey, fnv1a64("height"));
                key = hashCombine(key, fnv1a64(writeTitleArtJson(bevel)));
                key = hashDouble(key, unit);
                key = hashDouble(key, bandPx);
                if (auto hit = svc.cache.find(key)) {
                    heightSlot = entrySlot(hit);
                } else {
                    int coverSlot = -1;
                    if (fit && bandPx <= 0.0) {
                        // the radius of the largest inscribed circle covering each pixel, smoothed
                        const int a0 = newSlot(TitleBuffer::Seeds, W, H), b0 = newSlot(TitleBuffer::Seeds, W, H);
                        const std::array<float, 4> seedsEnc = titleEncoding(svc.caps.format(TitleBuffer::Seeds), TitleValueKind::Seeds);
                        const int sd = sdfSlot;
                        pass("cover seeds", id, a0, {0, 0, W, H}, TitleRenderProgram::CoverInit, [sd, distEnc, seedsEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
                            p.raw[0] = s.texture(sd);
                            setP(p, TITLE_PARAM_RAW_ENCODING, distEnc);
                            setP(p, TITLE_PARAM_OUT_ENCODING, seedsEnc);
                            return true;
                        });
                        int a = a0, b = b0;
                        const std::vector<float> steps = floodSteps(0.0f, W, H);
                        // Pachislop's cover flood has no extra step-1 pass
                        for (size_t k = 0; k + 1 < steps.size(); k++) {
                            const float stepPx = steps[k];
                            const int in = a;
                            pass("cover flood", id, b, {0, 0, W, H}, TitleRenderProgram::CoverFlood, [in, sd, stepPx, seedsEnc, distEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
                                p.raw[0] = s.texture(in);
                                p.raw[1] = s.texture(sd);
                                setP(p, TITLE_PARAM_RAW_ENCODING, seedsEnc);
                                setP(p, TITLE_PARAM_RAW_ENCODING + 1, distEnc);
                                setP(p, TITLE_PARAM_OUT_ENCODING, seedsEnc);
                                p.params.set(2, stepPx);
                                return true;
                            });
                            std::swap(a, b);
                        }
                        release(b);
                        const int cover = newSlot(TitleBuffer::Height, W, H);
                        const int seeds = a;
                        pass("cover radius", id, cover, {0, 0, W, H}, TitleRenderProgram::CoverRadius, [seeds, sd, seedsEnc, distEnc, pixelsEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
                            p.raw[0] = s.texture(seeds);
                            p.raw[1] = s.texture(sd);
                            setP(p, TITLE_PARAM_RAW_ENCODING, seedsEnc);
                            setP(p, TITLE_PARAM_RAW_ENCODING + 1, distEnc);
                            setP(p, TITLE_PARAM_OUT_ENCODING, pixelsEnc);
                            return true;
                        });
                        release(seeds);
                        const float sigma = (float)std::clamp(unit * 0.8, 1.5, 32.0);
                        const int tmp = newSlot(TitleBuffer::Height, W, H);
                        for (int dir = 0; dir < 2; dir++) {
                            const int from = dir == 0 ? cover : tmp, to = dir == 0 ? tmp : cover;
                            pass("cover blur", id, to, {0, 0, W, H}, TitleRenderProgram::BlurRaw, [from, dir, sigma, pixelsEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
                                p.raw[0] = s.texture(from);
                                setP(p, TITLE_PARAM_RAW_ENCODING, pixelsEnc);
                                setP(p, TITLE_PARAM_OUT_ENCODING, pixelsEnc);
                                p.params.set(2, dir == 0 ? 1.0f : 0.0f, dir == 0 ? 0.0f : 1.0f, sigma);
                                return true;
                            });
                        }
                        release(tmp);
                        coverSlot = cover;
                    }
                    const int height = newSlot(TitleBuffer::Height, W, H);
                    const int profile = profileId(str(bevel, "profile", "smooth"));
                    std::shared_ptr<TitleCacheEntry> profileLut;
                    if (profile == 7) {
                        profileLut = svc.curveLut(decodeCurve(bevel.value("profile_curve", Json::object())));
                        hold(profileLut);
                    }
                    const float fitK = (float)std::max(0.01, num(bevel, "fit", 70) / 100.0);
                    const bool down = str(bevel, "direction", "up") == "down";
                    const int sd = sdfSlot, cv = coverSlot;
                    const float size = (float)bevelPx, band = (float)(fit ? bandPx : 0.0);
                    pass("height", id, height, {0, 0, W, H}, TitleRenderProgram::Height,
                         [sd, cv, profileLut, profile, size, fitK, band, fit, down, distEnc, pixelsEnc, heightEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
                             p.raw[0] = s.texture(sd);
                             if (cv >= 0) p.raw[1] = s.texture(cv);
                             if (profileLut) p.tex[0] = profileLut->texture();
                             setP(p, TITLE_PARAM_RAW_ENCODING, distEnc);
                             setP(p, TITLE_PARAM_RAW_ENCODING + 1, pixelsEnc);
                             setP(p, TITLE_PARAM_OUT_ENCODING, heightEnc);
                             p.params.set(2, size, (float)profile, fitK, band);
                             p.params.set(3, fit ? 1.0f : 0.0f, down ? 1.0f : 0.0f);
                             return true;
                         });
                    if (coverSlot >= 0) release(coverSlot);
                    // soften = blur radius; a minimum blur smooths the distance field's residual noise
                    const float sigma = (float)std::clamp(softPx / 2.0, 1.0, 32.0);
                    const int tmp = newSlot(TitleBuffer::Height, W, H);
                    for (int dir = 0; dir < 2; dir++) {
                        const int from = dir == 0 ? height : tmp, to = dir == 0 ? tmp : height;
                        pass("height blur", id, to, {0, 0, W, H}, TitleRenderProgram::BlurRaw, [from, dir, sigma, heightEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
                            p.raw[0] = s.texture(from);
                            setP(p, TITLE_PARAM_RAW_ENCODING, heightEnc);
                            setP(p, TITLE_PARAM_OUT_ENCODING, heightEnc);
                            p.params.set(2, dir == 0 ? 1.0f : 0.0f, dir == 0 ? 0.0f : 1.0f, sigma);
                            return true;
                        }, BlendMode::NONE, false, 2.0f * sigma);
                    }
                    release(tmp);
                    auto entry = std::make_shared<TitleCacheEntry>();
                    entry->key = key;
                    store(entry, height);
                    heightSlot = height;
                }
                fieldSlots.push_back(heightSlot);
            }

            // ---- 7. the shape burst's depth of a face: the max inside distance, reduced to 1 x 1 ----
            int burstSlot = -1;
            if (burst && face && sdfSlot >= 0) {
                const uint64_t key = hashCombine(sdfKey, fnv1a64("burst"));
                if (auto hit = svc.cache.find(key)) {
                    burstSlot = entrySlot(hit);
                } else {
                    int src = sdfSlot;
                    std::array<float, 4> srcEnc = distEnc;
                    int w = W, h = H;
                    bool first = true;
                    while (first || w > 1 || h > 1) {
                        w = (w + 7) / 8;
                        h = (h + 7) / 8;
                        const int dst = newSlot(TitleBuffer::Height, w, h);
                        const int from = src;
                        const std::array<float, 4> inEnc = srcEnc;
                        pass("burst max", id, dst, {0, 0, w, h}, TitleRenderProgram::ReduceMax, [from, inEnc, pixelsEnc, first](TitleJobState& s, TitleGpuPass& p, std::string&) {
                            p.raw[0] = s.texture(from);
                            setP(p, TITLE_PARAM_RAW_ENCODING, inEnc);
                            setP(p, TITLE_PARAM_OUT_ENCODING, pixelsEnc);
                            p.params.set(2, first ? 1.0f : 0.0f);
                            return true;
                        }, BlendMode::NONE, false, 64.0f);
                        if (!first) release(src);
                        src = dst;
                        srcEnc = pixelsEnc;
                        first = false;
                    }
                    auto entry = std::make_shared<TitleCacheEntry>();
                    entry->key = key;
                    store(entry, src);
                    burstSlot = src;
                }
                fieldSlots.push_back(burstSlot);
            }

            // ---- 8. satin: the mask blurred by its size (Photoshop's size = blur radius ~ 2 sigma) ----
            int satinSlot = -1;
            if (on(satin)) {
                const float sigma = (float)std::clamp(num(satin, "size", 7) * unit / 2.0, 0.5, 32.0);
                const uint64_t key = hashDouble(hashCombine(shapeKeys[ri], fnv1a64("satin")), sigma);
                if (auto hit = svc.cache.find(key)) {
                    satinSlot = entrySlot(hit);
                } else {
                    // the coverage as the Satin buffer, then the Gaussian (radius 3 sigma)
                    TitlePx value{newSlot(TitleBuffer::Satin, W, H), {0, 0, W, H}};
                    pass("satin source", id, value.slot, {0, 0, W, H}, TitleRenderProgram::Copy, [coverage, W, H](TitleJobState& s, TitleGpuPass& p, std::string&) {
                        setupResample(p, s.texture(coverage), W, H);
                        p.params.set(3, 1.0f, 1.0f);
                        return true;
                    });
                    TitlePx blurred = blur(value, sigma * 3.0f, id);
                    release(value.slot);
                    auto entry = std::make_shared<TitleCacheEntry>();
                    entry->key = key;
                    store(entry, blurred.slot);
                    satinSlot = blurred.slot;
                }
                fieldSlots.push_back(satinSlot);
            }

            // ---- assets: images, panorama, LUTs ----
            const Json& fillImage = obj(fill, "image");
            std::shared_ptr<TitleCacheEntry> fillTex;
            if (fillKind == "image") fillTex = assetTexture(fillImage.value("image", Json()), TitleAssetKind::Image, TextureWrap::REPEAT, 0, id, "style.face.fill.image.image");
            const Json& pattern = obj(mat, "pattern");
            const std::string patternKind = str(pattern, "kind", "none");
            std::shared_ptr<TitleCacheEntry> patternTex;
            if (patternKind == "custom") patternTex = assetTexture(pattern.value("image", Json()), TitleAssetKind::Image, TextureWrap::REPEAT, 0, id, "pattern.image");
            const Json& reflection = obj(mat, "reflection");
            std::shared_ptr<TitleCacheEntry> panorama;
            if (on(reflection)) {
                panorama = assetTexture(reflection.value("map", Json()), TitleAssetKind::Environment, TextureWrap::REPEAT,
                                        (int)std::lround(std::clamp(num(reflection, "blur", 75), 0.0, 100.0)), id, "reflection.map");
            }
            const TitleCurve glossCurve = decodeCurve(bevel.value("gloss_curve", Json::object()));
            const TitleCurve satinCurve = decodeCurve(satin.value("contour", Json::object()));
            std::shared_ptr<TitleCacheEntry> luts = svc.elementLuts(decodeGradient(fill.value("gradient", Json::object())),
                                                                    decodeGradient(obj(mat, "finish").value("gradient", Json::object())),
                                                                    glossCurve.points.empty() ? nullptr : &glossCurve,
                                                                    satinCurve.points.empty() ? nullptr : &satinCurve);
            hold(fillTex);
            hold(patternTex);
            hold(panorama);
            hold(luts);
            // the layer's own pixels (images, artwork vectors: fill `source`)
            std::shared_ptr<TitleCacheEntry> picture;
            if (fillKind == "source") {
                const uint64_t key = hashCombine(plan.key, fnv1a64("picture"));
                picture = svc.cache.find(key);
                if (!picture) {
                    picture = std::make_shared<TitleCacheEntry>();
                    picture->key = key;
                    geometry::MaskFrame frame;
                    frame.width = W;
                    frame.height = H;
                    frame.scale = m;
                    frame.origin = {plan.x0, plan.y1};
                    const Json layerCopy = layer;
                    std::shared_ptr<TitleCacheEntry> pic = picture;
                    cpu("picture", id, (uint32_t)(W * H / 200 + 100), [pic, content, layerCopy, doc, assets, frame](TitleJobState& s, std::string&) {
                        TitleImageData image;
                        if (!content->picture(layerCopy, *doc, assets, frame, image) || image.width != frame.width || image.height != frame.height) {
                            // a text or shape has no pixels of its own: solid white (fill_source_without_pixels)
                            image.width = frame.width;
                            image.height = frame.height;
                            image.rgba.assign((size_t)frame.width * frame.height * 4, 255);
                        }
                        const std::vector<uint8_t> rows = flipRows(image.rgba, image.width, image.height, 4);
                        pic->upload = std::make_unique<TitleUpload>();
                        if (!pic->upload->create("title art picture", image.width, image.height, ColorFormat::RGBA, rows.data(), rows.size(),
                                                 TextureFilter::LINEAR)) {
                            return false;
                        }
                        s.services.cache.insert(pic);
                        return true;
                    });
                }
                hold(picture);
            }

            // ---- 10. the element's shading parameters (materialPass.ts shade()) ----
            TitleGpuPass params;
            const int fk = fillKind == "source" ? 0 : fillKindId(fillKind);
            const double fillAngle = num(fill, "angle", 90) * DEG;
            params.params.set(2, (float)fk, (float)num(fill, "scale", 1), (float)num(fill, "offset", 0), 1.0f);
            const TitleColor fillColor = color(fill, "color", "#ffffff");
            params.params.set(3, fillColor.r, fillColor.g, fillColor.b, fillTex ? 1.0f : 0.0f);
            // 0 = left to right, 90 = top to bottom (clockwise on screen) as a y-up vector
            params.params.set(4, (float)std::cos(fillAngle), (float)-std::sin(fillAngle), fillTex ? fillTex->value : 1.0f,
                              (float)num(fillImage, "scale", 1));
            const TitleVec2 imgOff = decodeVec2(fillImage.value("offset", Json::array({0, 0})));
            const double imgRot = num(fillImage, "rotation", 0) * DEG;
            params.params.set(5, (float)imgOff.x, (float)imgOff.y, (float)std::cos(imgRot), (float)std::sin(imgRot));
            params.params.set(6, on(fillImage, "tile") ? 1.0f : 0.0f, picture ? 1.0f : 0.0f, 0.0f, sdfSlot >= 0 ? 1.0f : 0.0f);
            // the element box: its base outline grown by `grow`, shifted (frame px, y-up)
            const TitleRect bb = plan.elements.baseBounds;
            params.params.set(7, (float)((bb.x - e.grow + e.offset.x - plan.x0) * m), (float)((bb.y - e.grow + e.offset.y - plan.y0) * m),
                              (float)((bb.x + bb.width + e.grow + e.offset.x - plan.x0) * m),
                              (float)((bb.y + bb.height + e.grow + e.offset.y - plan.y0) * m));
            // Photoshop "Use Global Light": the document's angle and altitude replace the bevel's
            const bool global = on(bevel, "use_global_light", true);
            const double la = global ? num(light, "angle", 120) : num(bevel, "light_angle", 120);
            const double le = global ? num(light, "altitude", 30) : num(bevel, "light_altitude", 30);
            const double A = layerAngle(la, plan.place) * DEG, E = std::clamp(le, 0.0, 90.0) * DEG;
            const std::string span = str(fill, "span", "layer");
            const int spanMode = (fk == 0 || fk == 6 || span == "layer" || spanSlot < 0) ? 0 : (span == "char" ? 1 : 2);
            params.params.set(8, (float)(std::cos(E) * std::cos(A)), (float)(std::cos(E) * std::sin(A)), (float)std::sin(E), (float)spanMode);
            params.params.set(9, (float)plan.x0, (float)plan.y0, (float)m, (float)(e.grow * m));
            params.params.set(10, (float)(e.offset.x * m), (float)(e.offset.y * m), (float)bandPx, bevelOn && heightSlot >= 0 ? 1.0f : 0.0f);
            params.params.set(11, (float)(bevelPx * num(bevel, "depth", 3) * 0.35), (float)glossId(str(bevel, "gloss", "ring")),
                              (float)num(bevel, "highlight_opacity", 0.85), (float)num(bevel, "shadow_opacity", 0.6));
            const TitleColor hi = color(bevel, "highlight_color", "#ffffff"), sh = color(bevel, "shadow_color", "#000000");
            params.params.set(12, hi.r, hi.g, hi.b, (float)(num(mat, "refraction", 0) * unit));
            const Json& glass = obj(mat, "glass");
            params.params.set(13, sh.r, sh.g, sh.b, on(glass) ? 1.0f : 0.0f);
            const TitleColor gc = color(glass, "color", "#3cc8ff");
            params.params.set(14, gc.r, gc.g, gc.b, (float)num(glass, "tint", 1));
            // light focuses across 1.6 bevel widths
            params.params.set(15, (float)num(glass, "opacity", 0.7), (float)num(glass, "edge", 0.7), (float)num(glass, "caustic", 0.7),
                              (float)(bevelPx * 1.6));
            params.params.set(16, panorama ? 1.0f : 0.0f, (float)num(reflection, "amount", 1), (float)(num(reflection, "rotation", 0) / 360.0),
                              (float)num(reflection, "colorize", 0));
            // columns: the screen directions of the layer's x and y axes
            const double ax = screenAngle(0, plan.place) * DEG, ay = screenAngle(90, plan.place) * DEG;
            params.params.set(17, (float)std::cos(ax), (float)std::sin(ax), (float)std::cos(ay), (float)std::sin(ay));
            // the global light keeps the satin direction fixed on screen like the bevel light
            const double sa = (on(satin, "use_global_light") ? layerAngle(num(light, "angle", 120), plan.place) : num(satin, "angle", 19)) * DEG;
            const double dist = num(satin, "distance", 5) * unit;
            params.params.set(18, satinSlot >= 0 ? 1.0f : 0.0f, (float)(std::cos(sa) * dist), (float)(std::sin(sa) * dist), (float)num(satin, "opacity", 0.5));
            const TitleColor satinColor = color(satin, "color", "#000000");
            params.params.set(19, satinColor.r, satinColor.g, satinColor.b, (float)satinBlendId(str(satin, "blend", "multiply")));
            const Json& finish = obj(mat, "finish");
            const Json& texture = obj(mat, "texture");
            const int tex = textureId(str(texture, "kind", "none"));
            params.params.set(20, on(satin, "invert", true) ? 1.0f : 0.0f, on(finish) ? 1.0f : 0.0f, (float)num(finish, "amount", 1), (float)tex);
            const double texPx = std::max(0.5, num(texture, "scale", 4) * unit);
            params.params.set(21, (float)num(texture, "amount", 0.3), (float)texPx, (float)textureBlendId(str(texture, "blend", "overlay")),
                              str(texture, "target", "color") == "surface" ? 1.0f : 0.0f);
            // seed -> an offset in feature space; features are anchored to the layer, not the frame
            const double texSeed = num(texture, "seed", 1) + (double)(docSeed - 1);
            const double texAngle = num(texture, "angle", 0) * DEG;
            params.params.set(22, (float)(std::fmod(texSeed * 127.1, 1000.0) + plan.x0 * m / texPx),
                              (float)(std::fmod(texSeed * 311.7, 1000.0) + plan.y0 * m / texPx), (float)std::cos(texAngle), (float)std::sin(texAngle));
            const Json& sheen = obj(mat, "sheen");
            params.params.set(23, on(sheen) && num(sheen, "amount", 0.7) > 0 ? 1.0f : 0.0f, (float)num(sheen, "amount", 0.7),
                              (float)num(sheen, "position", -0.25), (float)std::max(0.05, num(sheen, "width", 0.35)));
            const TitleColor sheenColor = color(sheen, "color", "#ffffff");
            params.params.set(24, sheenColor.r, sheenColor.g, sheenColor.b, on(innerGlow) ? 1.0f : 0.0f);
            const TitleColor glowColor = color(innerGlow, "color", "#ffffff");
            params.params.set(25, glowColor.r, glowColor.g, glowColor.b, (float)glowPx);
            params.params.set(26, (float)num(innerGlow, "opacity", 0.6), (float)num(innerGlow, "noise", 0), 0.0f, 0.0f);
            const Json& gloss = obj(mat, "gloss");
            params.params.set(27, on(gloss) ? 1.0f : 0.0f, (float)num(gloss, "opacity", 0.55), (float)num(gloss, "position", 0.45),
                              (float)num(gloss, "softness", 0.15));
            // the band's tilt: "down" across the band, turned clockwise on screen by the angle (y-up)
            const double capAngle = num(gloss, "angle", 0) * DEG;
            params.params.set(28, (float)num(gloss, "curve", 0.25), (float)-std::sin(capAngle), (float)-std::cos(capAngle),
                              (float)std::clamp(num(mat, "opacity", 1), 0.0, 1.0));
            // the pattern in element space, anchored to the layer origin and moving with a rim's offset
            const bool patternActive = patternKind != "none" && (num(pattern, "opacity", 1) > 0 || num(pattern, "relief", 0) > 0) &&
                                       (patternKind != "custom" || patternTex);
            const double pk = 1.0 / std::max(1e-3, num(pattern, "scale", 12) / 100.0 * plan.size);
            const double pa = num(pattern, "angle", 0) * DEG;
            const double aspect = patternTex ? patternTex->value : 1.0;
            const double pc = std::cos(pa) * pk, ps = std::sin(pa) * pk;
            const double xa = pc, xb = -ps, xc = ps * aspect, xd = pc * aspect;
            const double ox = plan.x0 - e.offset.x, oy = plan.y0 - e.offset.y;
            const double patternSeed = num(pattern, "seed", 1) + (double)(docSeed - 1);
            params.params.set(30, patternActive ? (float)patternKindId(patternKind) : 0.0f, (float)patternSeed, (float)num(pattern, "opacity", 1),
                              (float)num(pattern, "shade", 0.6));
            params.params.set(31, (float)(xa / m), (float)(xb / m), (float)(xc / m), (float)(xd / m));
            params.params.set(32, (float)(xa * ox + xb * oy), (float)(xc * ox + xd * oy), (float)num(pattern, "relief", 0));
            const TitleColor pc1 = color(pattern, "color", "#000000"), pc2 = color(pattern, "color2", "#ffffff");
            params.params.set(33, pc1.r, pc1.g, pc1.b);
            params.params.set(34, pc2.r, pc2.g, pc2.b);
            setP(params, 35, pixelsEnc);
            setP(params, TITLE_PARAM_RAW_ENCODING, distEnc);
            setP(params, TITLE_PARAM_RAW_ENCODING + 1, heightEnc);
            setP(params, TITLE_PARAM_RAW_ENCODING + 2, titleEncoding(svc.caps.format(TitleBuffer::Height), TitleValueKind::Index));
            const TitleParams values = params.params;
            const int covSlot = coverage, sdfS = sdfSlot, heightS = heightSlot, satinS = satinSlot, spanS = spanMode ? spanSlot : -1;
            const int glyphS = spanMode ? glyphSlot : -1, burstS = burstSlot;
            drawIntoStack(face ? "shade face" : "shade rim", [=](TitleJobState& s, TitleGpuPass& p, std::string&) {
                p.params = values;
                p.tex[0] = s.texture(covSlot);
                if (satinS >= 0) p.tex[1] = s.texture(satinS);
                if (patternTex) p.tex[2] = patternTex->texture();
                if (fillTex) p.tex[3] = fillTex->texture();
                if (panorama) p.tex[4] = panorama->texture();
                if (luts) p.tex[5] = luts->texture();
                if (picture) p.tex[6] = picture->texture();
                if (sdfS >= 0) p.raw[0] = s.texture(sdfS);
                if (heightS >= 0) p.raw[1] = s.texture(heightS);
                if (spanS >= 0) p.raw[2] = s.texture(spanS);
                if (glyphS >= 0) p.raw[3] = s.texture(glyphS);
                if (burstS >= 0) p.raw[4] = s.texture(burstS);
                return true;
            }, false);
        }
        for (int slot : fieldSlots) release(slot);
        auto entry = std::make_shared<TitleCacheEntry>();
        entry->key = stackKey;
        store(entry, stack.slot);
        stackSlot = stack.slot;
    }
    for (int slot : shapeSlots) release(slot);
    if (eroding) {
        for (int slot : maskSlots) release(slot);
    }
    if (spanSlot >= 0) release(spanSlot);
    if (glyphSlot >= 0) release(glyphSlot);

    // ---- 11. placement into the node target ----
    TitlePx out{newSlot(TitleBuffer::Accumulator, rect.width, rect.height), rect};
    const TitleFrame whole = TitleFrame::canvas(document->canvasWidth(), document->canvasHeight(), scale);
    const TitleVec2 anchor = whole.toFrame({plan.place.toDocument.e, plan.place.toDocument.f});
    const geometry::Affine& t = plan.place.toDocument;
    const double det = t.a * t.d - t.b * t.c;
    const double ia = t.d / det, ic = -t.c / det, ib = -t.b / det, id2 = t.a / det;
    // mask px per target px: up to 4 x 4 taps when the frame is finer than the target
    const double ratio = m / (plan.place.scale * scale);
    const int taps = std::min(4, std::max(1, (int)std::ceil(ratio - 0.01)));
    const float relX = (float)(rect.x - anchor.x), relY = (float)(rect.y - anchor.y);
    const float fs = (float)scale;
    const float fx0 = (float)plan.x0, fy0 = (float)plan.y0;
    const float iw = (float)(1.0 / (plan.x1 - plan.x0)), ih = (float)(1.0 / (plan.y1 - plan.y0));
    const int stackS = stackSlot;
    pass("place", id, out.slot, {0, 0, rect.width, rect.height}, TitleRenderProgram::Place,
         [stackS, relX, relY, fs, taps, ia, ic, ib, id2, fx0, fy0, iw, ih](TitleJobState& s, TitleGpuPass& p, std::string&) {
             p.tex[0] = s.texture(stackS);
             p.params.set(2, relX, relY, fs, (float)taps);
             p.params.set(3, (float)ia, (float)ic, (float)ib, (float)id2);
             p.params.set(4, fx0, fy0, iw, ih);
             return true;
         },
         BlendMode::NONE, false, (float)(taps * taps));
    release(stackSlot);
    return out;
}
