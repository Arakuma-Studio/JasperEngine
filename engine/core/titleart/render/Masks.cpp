// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/masks.ts (Masks.combined, draw, source; maskToComp, placeAt)
// by SafEight, used with the author's permission.

#include "TitlePlanner.h"

#include "titleart/TitleArtIO.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;

    // the channel a mask source holds its value in (MASK_VALUE's p[2].x)
    enum Channel : int { CHANNEL_VALUE = 0, CHANNEL_ALPHA = 1, CHANNEL_LUMINANCE = 2 };

    // move to (x, y) after turning by `deg` clockwise on screen (y-up)
    geometry::Affine placeAt(double x, double y, double deg) {
        return geometry::Affine::translate(x, y).after(geometry::Affine::rotateDegrees(-deg));
    }

    geometry::Affine inverse(const geometry::Affine& m) {
        const double det = m.a * m.d - m.b * m.c;
        if (!(std::abs(det) > 1e-12)) return {0, 0, 0, 0, 0, 0};
        geometry::Affine r;
        r.a = m.d / det;
        r.b = -m.b / det;
        r.c = -m.c / det;
        r.d = m.a / det;
        r.e = -(r.a * m.e + r.c * m.f);
        r.f = -(r.b * m.e + r.d * m.f);
        return r;
    }

    uint64_t hashOf(const void* data, size_t size) {
        return fnv1a64(std::string_view(static_cast<const char*>(data), size));
    }

    int channelOf(const Json& mask) {
        return mask.value("channel", std::string("alpha")) == "luminance" ? CHANNEL_LUMINANCE : CHANNEL_ALPHA;
    }

}

TitlePx TitlePlanner::applyMasks(const DrawNode& node, TitlePx px) {
    TitlePx mask = combinedMask(*node.layer, px.rect);
    if (!mask.valid()) return px;
    TitlePx out{newSlot(TitleBuffer::Accumulator, px.rect.width, px.rect.height), px.rect};
    const int from = px.slot, maskSlot = mask.slot;
    const std::array<float, 4> map = uvMap(px.rect, px.rect), maskMap = uvMap(px.rect, mask.rect);
    pass("masks", node.layerId, out.slot, {0, 0, px.rect.width, px.rect.height}, TitleRenderProgram::Copy,
         [from, maskSlot, map, maskMap](TitleJobState& s, TitleGpuPass& p, std::string&) {
             p.tex[0] = s.texture(from);
             p.tex[1] = s.texture(maskSlot);
             p.params.set(2, map[0], map[1], map[2], map[3]);
             p.params.set(3, 1.0f, 0.0f, 0.0f, 2.0f);
             p.params.set(4, maskMap[0], maskMap[1], maskMap[2], maskMap[3]);
             return true;
         });
    release(px.slot);
    release(mask.slot);
    return out;
}

TitlePx TitlePlanner::combinedMask(const Json& layer, const TitleRectI& rect) {
    const Json& masks = layer.value("masks", Json::array());
    const std::string layerId = layer.value("id", std::string());
    std::vector<const Json*> list;
    // the masks key (R14): each mask's content, the layer's placement for linked masks, the
    // sources' node hashes, the rect, px_scale and the formats
    uint64_t key = hashCombine(fnv1a64("masks"), layerHash(layerId, &LayerHashes::masks));
    const TitlePlacement place = placementOf(layer);
    const double placeKey[6] = {place.toDocument.a, place.toDocument.b, place.toDocument.c, place.toDocument.d, place.toDocument.e,
                                place.toDocument.f};
    std::map<const Json*, TitlePx> layerSources;
    for (const Json& m : masks) {
        if (!m.value("enabled", true)) continue;
        const std::string type = m.value("type", std::string());
        if (type == "layer") {
            // a source that gives no alpha (missing, a cycle) counts as fully shown
            const std::string source = m.value("layer", std::string());
            TitlePx px = layerSource(source, "mask_cycle", layerId);
            if (!px.valid()) continue;
            layerSources[&m] = px;
            key = hashCombine(key, layerHash(source, &LayerHashes::node));
        } else if (type == "shape" || type == "gradient" || type == "image") {
            if (m.value("linked", true)) key = hashCombine(key, hashOf(placeKey, sizeof(placeKey)));
        } else {
            continue;
        }
        list.push_back(&m);
    }
    if (list.empty()) return {};
    const int r[4] = {rect.x, rect.y, rect.width, rect.height};
    key = hashCombine(key, hashOf(r, sizeof(r)));
    key = hashCombine(key, hashOf(&scale, sizeof(scale)));
    key = hashCombine(key, fnv1a64(svc.caps.pathName()));
    if (auto hit = svc.cache.find(key)) return {entrySlot(hit), rect};

    const int cw = document->canvasWidth(), ch = document->canvasHeight();
    const double s = scale;
    // mask space -> document (linked masks ride the layer; unlinked and layer masks are canvas space)
    auto maskToDoc = [&](const Json& m) {
        if (!m.value("linked", true) || m.value("type", std::string()) == "layer") return geometry::Affine();
        return place.toDocument;
    };
    // the target pixel (fragment centre, rows down) -> document point
    geometry::Affine fragToDoc;
    fragToDoc.a = 1.0 / s;
    fragToDoc.d = -1.0 / s;
    fragToDoc.e = rect.x / s - cw * 0.5;
    fragToDoc.f = ch * 0.5 - rect.y / s;

    TitlePx acc;
    for (size_t i = 0; i < list.size(); i++) {
        const Json& m = *list[i];
        const std::string type = m.value("type", std::string());
        TitlePx src;
        int channel = CHANNEL_VALUE;
        if (type == "layer") {
            src = layerSources[&m];
            channel = channelOf(m);
        } else if (type == "gradient") {
            src = {newSlot(TitleBuffer::Satin, rect.width, rect.height), rect};
            const geometry::Affine local = inverse(maskToDoc(m)).after(fragToDoc);
            const TitleVec2 start = decodeVec2(m.value("position", Json::array({0, 0})));
            const double angle = m.value("angle", 0.0) * DEG;
            const float length = (float)std::max(0.0, m.value("length", 200.0));
            const float inner = (float)std::clamp(m.value("inner", 0.0), 0.0, 1.0);
            const float gamma = (float)(std::log(0.5) / std::log(std::clamp(m.value("mid", 0.5), 0.01, 0.99)));
            const bool radial = m.value("shape", std::string("linear")) == "radial";
            pass("mask gradient", layerId, src.slot, {0, 0, rect.width, rect.height}, TitleRenderProgram::MaskGradient,
                 [=](TitleJobState&, TitleGpuPass& p, std::string&) {
                     p.params.set(2, (float)local.a, (float)local.c, (float)local.e);
                     p.params.set(3, (float)local.b, (float)local.d, (float)local.f);
                     // the angle runs clockwise on screen (y-up: a negative angle)
                     p.params.set(4, (float)start.x, (float)start.y, (float)std::cos(angle), (float)-std::sin(angle));
                     p.params.set(5, length, inner, gamma, radial ? 1.0f : 0.0f);
                     return true;
                 });
        } else if (type == "shape" || type == "image") {
            // rasterized on the CPU at the target's pixels (rows down), uploaded once
            auto entry = std::make_shared<TitleCacheEntry>();
            entry->key = hashCombine(key, i + 1);
            const geometry::Affine toDoc = maskToDoc(m).after(placeAt(decodeVec2(m.value("position", Json::array({0, 0}))).x,
                                                                      decodeVec2(m.value("position", Json::array({0, 0}))).y,
                                                                      m.value("rotation", 0.0)));
            // document -> y-up target pixels with the target's top-left at the origin
            geometry::Affine docToPx;
            docToPx.a = s;
            docToPx.d = s;
            docToPx.e = cw * 0.5 * s - rect.x;
            docToPx.f = -ch * 0.5 * s + rect.y;
            const geometry::Affine shapeToPx = docToPx.after(toDoc);
            std::shared_ptr<TitleContentProvider> content = svc.content;
            const Json mask = m;
            const AssetResolver* assets = req.assets;
            const TitleRectI area = rect;
            const bool isShape = type == "shape";
            channel = isShape ? CHANNEL_VALUE : channelOf(m);
            if (isShape && !content) {
                TitleProblem p;
                p.code = "geometry_unavailable";
                p.layer = layerId;
                p.message = "this build has no shape geometry: the shape mask shows everything";
                problem(p);
                continue;
            }
            cpu(isShape ? "mask shape" : "mask image", layerId, (uint32_t)(area.width * area.height / 200 + 50),
                [entry, content, mask, assets, area, shapeToPx, isShape](TitleJobState&, std::string& error) {
                    if (isShape) {
                        geometry::TitleElement element;
                        geometry::Path path = content->shapeOutline(mask.value("shape", Json::object()));
                        path.transform(shapeToPx);
                        element.outlines.push_back(std::move(path));
                        geometry::MaskFrame frame;
                        frame.width = area.width;
                        frame.height = area.height;
                        frame.scale = 1.0;
                        frame.origin = {0.0, 0.0};
                        geometry::CoverageMask coverage = content->coverage(element, frame);
                        if (coverage.width != area.width || coverage.height != area.height) {
                            error = "the shape mask could not be rasterized";
                            return false;
                        }
                        entry->upload = std::make_unique<TitleUpload>();
                        return entry->upload->create("title art mask", area.width, area.height, ColorFormat::RED, coverage.pixels.data(),
                                                     coverage.pixels.size(), TextureFilter::LINEAR);
                    }
                    // an image mask: the texture centred at its position, `size` its larger side
                    TitleImageData image;
                    std::vector<uint8_t> pixels((size_t)area.width * area.height * 4, 0);
                    if (assets) {
                        const TitleAssetRef ref = decodeAssetRef(mask.value("image", Json()));
                        const ResolvedTitleAsset resolved = assets->resolve(ref, TitleAssetKind::Image);
                        std::string why;
                        if (resolved.found && assets->loadImage(resolved, image, &why) && image.width > 0 && image.height > 0) {
                            const double size = std::max(1.0, mask.value("size", 200.0));
                            const double k = size / std::max(image.width, image.height);
                            const double hw = image.width * k * 0.5, hh = image.height * k * 0.5;
                            TitleRect bounds{-hw, -hh, 2.0 * hw, 2.0 * hh};
                            const geometry::Affine pxToShape = [&shapeToPx]() {
                                const double det = shapeToPx.a * shapeToPx.d - shapeToPx.b * shapeToPx.c;
                                geometry::Affine r;
                                if (!(std::abs(det) > 1e-12)) return geometry::Affine{0, 0, 0, 0, 0, 0};
                                r.a = shapeToPx.d / det;
                                r.b = -shapeToPx.b / det;
                                r.c = -shapeToPx.c / det;
                                r.d = shapeToPx.a / det;
                                r.e = -(r.a * shapeToPx.e + r.c * shapeToPx.f);
                                r.f = -(r.b * shapeToPx.e + r.d * shapeToPx.f);
                                return r;
                            }();
                            for (int y = 0; y < area.height; y++) {
                                for (int x = 0; x < area.width; x++) {
                                    const TitleVec2 q = pxToShape.apply({x + 0.5, -(y + 0.5)});
                                    const double u = (q.x - bounds.x) / bounds.width * image.width - 0.5;
                                    const double v = (bounds.y + bounds.height - q.y) / bounds.height * image.height - 0.5;
                                    if (u < -0.5 || v < -0.5 || u > image.width - 0.5 || v > image.height - 0.5) continue;
                                    const int x0 = std::clamp((int)std::floor(u), 0, image.width - 1), y0 = std::clamp((int)std::floor(v), 0, image.height - 1);
                                    const int x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
                                    const double fx = std::clamp(u - x0, 0.0, 1.0), fy = std::clamp(v - y0, 0.0, 1.0);
                                    for (int c = 0; c < 4; c++) {
                                        auto at = [&](int ix, int iy) { return (double)image.rgba[((size_t)iy * image.width + ix) * 4 + c]; };
                                        const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
                                        const double bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
                                        pixels[((size_t)y * area.width + x) * 4 + c] = (uint8_t)std::lround(top + (bottom - top) * fy);
                                    }
                                }
                            }
                        }
                    }
                    // straight alpha: premultiply, as every mask source the value pass reads
                    for (size_t p = 0; p < pixels.size(); p += 4) {
                        const unsigned a = pixels[p + 3];
                        for (int c = 0; c < 3; c++) pixels[p + c] = (uint8_t)((pixels[p + c] * a + 127) / 255);
                    }
                    entry->upload = std::make_unique<TitleUpload>();
                    return entry->upload->create("title art mask image", area.width, area.height, ColorFormat::RGBA, pixels.data(),
                                                 pixels.size(), TextureFilter::LINEAR);
                });
            src = {entrySlot(entry), rect};
        }
        if (!src.valid()) continue;
        const double feather = std::max(0.0, m.value("feather", 0.0)) * s;
        if (feather > 0.0) {
            // the value first (luminance does not commute with the blur), then the Gaussian
            TitlePx value{newSlot(TitleBuffer::Satin, rect.width, rect.height), rect};
            const int from = src.slot;
            const std::array<float, 4> map = uvMap(rect, src.rect);
            pass("mask value", layerId, value.slot, {0, 0, rect.width, rect.height}, TitleRenderProgram::MaskValue,
                 [from, map, channel](TitleJobState& st2, TitleGpuPass& p, std::string&) {
                     p.tex[0] = st2.texture(from);
                     p.params.set(2, (float)channel, 0.0f, 1.0f, 1.0f);
                     p.params.set(3, map[0], map[1], map[2], map[3]);
                     return true;
                 });
            if (type != "layer") release(src.slot);
            src = blur(value, (float)feather, layerId);
            release(value.slot);
            channel = CHANNEL_VALUE;
        }
        TitlePx out{newSlot(TitleBuffer::Satin, rect.width, rect.height), rect};
        const int from = src.slot, accSlot = acc.slot;
        const std::array<float, 4> map = uvMap(rect, src.rect), accMap = uvMap(rect, rect);
        const bool invert = m.value("invert", false);
        const float density = (float)std::clamp(m.value("density", 1.0), 0.0, 1.0);
        pass("mask combine", layerId, out.slot, {0, 0, rect.width, rect.height}, TitleRenderProgram::MaskValue,
             [from, accSlot, map, accMap, channel, invert, density](TitleJobState& st2, TitleGpuPass& p, std::string&) {
                 p.tex[0] = st2.texture(from);
                 p.params.set(2, (float)channel, invert ? 1.0f : 0.0f, density, accSlot < 0 ? 1.0f : 0.0f);
                 p.params.set(3, map[0], map[1], map[2], map[3]);
                 if (accSlot >= 0) {
                     p.tex[1] = st2.texture(accSlot);
                     p.params.set(4, accMap[0], accMap[1], accMap[2], accMap[3]);
                 }
                 return true;
             });
        if (feather > 0.0 || type != "layer") release(src.slot);
        if (acc.valid()) release(acc.slot);
        acc = out;
    }
    if (!acc.valid()) return {};
    auto entry = std::make_shared<TitleCacheEntry>();
    entry->key = key;
    entry->rect = rect;
    store(entry, acc.slot);
    return acc;
}
