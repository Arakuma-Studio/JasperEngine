// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The element rules (cumulative rim growth, plates, per-character face splits, outline
// operations warp -> distort -> weight per glyph over the block's frame, SVG fitting, image
// hulls) are ported from Pachislop src/text/ink.ts, src/engine/layer2d/outline2d.ts and
// Title2DRenderer.ts (elementSpecs, faceSpecs) by SafEight, used with the author's permission.

#include "TitleGeometry.h"

#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/geometry/Distort.h"
#include "titleart/geometry/Offset.h"
#include "titleart/geometry/Plate.h"
#include "titleart/geometry/Shapes.h"
#include "titleart/geometry/SvgPaths.h"
#include "titleart/geometry/Trace.h"
#include "titleart/geometry/Warp.h"
#include "titleart/text/TitleTextLayout.h"

#include "lunasvg.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <list>
#include <map>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    constexpr size_t BUILT_CACHE_ENTRIES = 64;
    constexpr size_t PLATE_CACHE_ENTRIES = 64;
    // the proxy an image's or artwork's hull is taken from (Pachislop's)
    constexpr int HULL_PROXY_SIDE = 256;

    double num(const Json& o, const char* key, double fallback) {
        const auto it = o.find(key);
        return it != o.end() && it->is_number() ? it->get<double>() : fallback;
    }

    std::string str(const Json& o, const char* key, const std::string& fallback = {}) {
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    const Json& member(const Json& o, const char* key) {
        static const Json null;
        const auto it = o.find(key);
        return it != o.end() ? *it : null;
    }

    struct Extent {
        double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
        bool empty() const { return x1 < x0; }
        void add(const TitleRect& r) {
            if (r.width <= 0 && r.height <= 0 && r.x == 0 && r.y == 0) return;
            x0 = std::min(x0, r.x);
            y0 = std::min(y0, r.y);
            x1 = std::max(x1, r.x + r.width);
            y1 = std::max(y1, r.y + r.height);
        }
        void add(const TitleVec2& p) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        TitleRect rect() const { return empty() ? TitleRect{} : TitleRect{x0, y0, x1 - x0, y1 - y0}; }
    };

    TitleRect transformRect(const TitleRect& r, const Affine& m) {
        if (r.width <= 0 && r.height <= 0) return {};
        Extent e;
        for (const TitleVec2& p : {TitleVec2{r.x, r.y}, TitleVec2{r.x + r.width, r.y}, TitleVec2{r.x, r.y + r.height},
                                   TitleVec2{r.x + r.width, r.y + r.height}}) {
            e.add(m.apply(p));
        }
        return e.rect();
    }

    TitleRect growRect(const TitleRect& r, double d) {
        if (r.width <= 0 && r.height <= 0) return r;
        return {r.x - d, r.y - d, r.width + 2 * d, r.height + 2 * d};
    }

    // a layer's transform: position, then rotation (clockwise on screen: negative in y up), scale,
    // and the flips (a mirror about the position, before the rotation)
    Affine layerMatrix(const Json& layer) {
        const TitleTransform t = decodeTransform(member(layer, "transform"));
        return Affine::translate(t.position.x, t.position.y)
            .after(Affine::rotateDegrees(-t.rotation))
            .after(Affine::scale(t.scale * (t.flipX ? -1 : 1), t.scale * (t.flipY ? -1 : 1)));
    }

    // the ancestors of the layer with `id` (outermost first), false when it is not in `layers`
    bool ancestorsOf(const Json& layers, const std::string& id, std::vector<const Json*>& chain) {
        if (!layers.is_array()) return false;
        for (const Json& layer : layers) {
            if (str(layer, "id") == id) return true;
            if (layerTypeOf(layer) == LayerType::Group) {
                chain.push_back(&layer);
                if (ancestorsOf(member(layer, "children"), id, chain)) return true;
                chain.pop_back();
            }
        }
        return false;
    }

    // a layer's placement in document space: its groups' transforms, then its own
    Affine worldMatrix(const Json& layer, const TitleArtDoc& doc) {
        std::vector<const Json*> chain;
        Affine m;
        if (ancestorsOf(doc.root["layers"], str(layer, "id"), chain)) {
            for (const Json* group : chain) m = m.after(layerMatrix(*group));
        }
        return m.after(layerMatrix(layer));
    }

    bool draws(const Json& layer) {
        const auto visible = layer.find("visible");
        const auto guide = layer.find("guide");
        return (visible == layer.end() || !visible->is_boolean() || visible->get<bool>()) && num(layer, "opacity", 1) > 0 &&
               !(guide != layer.end() && guide->is_boolean() && guide->get<bool>());
    }

    StrokeJoin joinOf(const std::string& join) {
        return join == "round" ? StrokeJoin::Round : join == "bevel" ? StrokeJoin::Bevel : StrokeJoin::Miter;
    }

    StrokeJoin joinOf(OffsetJoin join) {
        return join == OffsetJoin::Round ? StrokeJoin::Round : join == OffsetJoin::Bevel ? StrokeJoin::Bevel : StrokeJoin::Miter;
    }

    StrokeCap capOf(OffsetCap cap) {
        return cap == OffsetCap::Round ? StrokeCap::Round : cap == OffsetCap::Square ? StrokeCap::Square : StrokeCap::Butt;
    }

    OffsetJoin offsetJoin(StrokeJoin join) {
        return join == StrokeJoin::Round ? OffsetJoin::Round : join == StrokeJoin::Bevel ? OffsetJoin::Bevel : OffsetJoin::Miter;
    }

    OffsetCap offsetCap(StrokeCap cap) {
        return cap == StrokeCap::Round ? OffsetCap::Round : cap == StrokeCap::Square ? OffsetCap::Square : OffsetCap::Butt;
    }

    bool hasInk(const Path& p) {
        return std::any_of(p.verbList().begin(), p.verbList().end(), [](PathVerb v) { return v != PathVerb::Move && v != PathVerb::Close; });
    }

    // the convex hull (layer space, y up) of an RGBA picture's opaque pixels drawn over `rect`
    Path pictureHull(const TitleImageData& img, const TitleRect& rect) {
        const int w = img.width, h = img.height;
        Polygon pts;
        // row extremes are enough: every hull vertex is a leftmost or rightmost opaque pixel of a row
        for (int y = 0; y < h; y++) {
            int x0 = -1, x1 = -1;
            for (int x = 0; x < w; x++) {
                if (img.rgba[((size_t)y * w + x) * 4 + 3] < 128) continue;
                if (x0 < 0) x0 = x;
                x1 = x;
            }
            if (x0 < 0) continue;
            for (const TitleVec2& p : {TitleVec2{(double)x0, (double)y}, TitleVec2{(double)x0, y + 1.0}, TitleVec2{x1 + 1.0, (double)y},
                                       TitleVec2{x1 + 1.0, y + 1.0}}) {
                pts.push_back(p);
            }
        }
        if (pts.empty()) pts = {{0, 0}, {(double)w, 0}, {(double)w, (double)h}, {0, (double)h}};
        Polygon hull = convexHull(pts);
        for (TitleVec2& p : hull) p = {rect.x + p.x / std::max(w, 1) * rect.width, rect.y + rect.height - p.y / std::max(h, 1) * rect.height};
        return polygonsToPath({hull});
    }

    // an image reduced (area average) so its larger side is at most `side`
    TitleImageData reducedImage(const TitleImageData& img, int side) {
        const int m = std::max(img.width, img.height);
        if (m <= side) return img;
        MaskFrame frame;
        const TitleRect rect{0, 0, (double)img.width, (double)img.height};
        frame.scale = (double)side / m;
        frame.width = std::max(1, (int)std::lround(img.width * frame.scale));
        frame.height = std::max(1, (int)std::lround(img.height * frame.scale));
        frame.origin = {0, (double)img.height};
        ElementSource source;
        source.image = std::make_shared<TitleImageData>(img);
        source.rect = rect;
        return rasterizeSource(source, frame);
    }

    // artwork drawn so its larger side is `side` pixels
    TitleImageData artworkRaster(const std::shared_ptr<const std::string>& svg, const TitleRect& rect, int side) {
        MaskFrame frame;
        const double m = std::max(rect.width, rect.height);
        frame.scale = m > 0 ? side / m : 1;
        frame.width = std::max(1, (int)std::lround(rect.width * frame.scale));
        frame.height = std::max(1, (int)std::lround(rect.height * frame.scale));
        frame.origin = {rect.x, rect.y + rect.height};
        ElementSource source;
        source.svg = svg;
        source.rect = rect;
        return rasterizeSource(source, frame);
    }

    // what a layer's outlines are, before its style
    struct Built {
        std::vector<GlyphOutline> glyphs;   // after warp, distort, weight
        std::vector<uint32_t> charIndex;    // text: per glyph, the code point index of its cluster
        std::vector<GlyphSpan> spans;
        std::vector<StrokedPath> strokes;   // Vector `shape` mode
        std::vector<TitleProblem> problems;
        double size = 0, shear = 0;
        bool fromAlpha = false;
        ElementSource source;
        Path hull;                          // pixel layers: hull / box plates grow from it
        std::shared_ptr<const TitleImageData> image;
        uint64_t key = 0;                   // the geometry key's hash (the plate cache names it)
    };

    // one cluster's part of a layer (a glyph cell): its glyphs and span boxes (lines keep their
    // whole box, so a `line` gradient shades the cell as it shades the whole title); a shape's,
    // an SVG's or a picture's only cluster is 0. The plate cache tells it apart by its key
    std::shared_ptr<const Built> onlyCluster(const std::shared_ptr<const Built>& whole, uint32_t cluster) {
        auto b = std::make_shared<Built>(*whole);
        b->glyphs.clear();
        b->charIndex.clear();
        b->spans.clear();
        for (size_t i = 0; i < whole->glyphs.size(); i++) {
            if (whole->glyphs[i].cluster != cluster) continue;
            b->glyphs.push_back(whole->glyphs[i]);
            if (i < whole->charIndex.size()) b->charIndex.push_back(whole->charIndex[i]);
        }
        for (const GlyphSpan& span : whole->spans) {
            if (span.cluster == cluster) b->spans.push_back(span);
        }
        if (cluster != 0) {
            b->strokes.clear();
            b->source = {};
            b->hull = {};
            b->image.reset();
            b->fromAlpha = false;
        }
        b->key = hashCombine(whole->key, (uint64_t)cluster + 1);
        return b;
    }

    Path allGlyphs(const Built& b) {
        Path all;
        for (const GlyphOutline& g : b.glyphs) all.append(g.path);
        return all;
    }

}

struct TitleGeometry::Impl {
    const AssetResolver* assets = nullptr;
    TitleTextLayout text;
    std::list<std::pair<uint64_t, std::shared_ptr<const Built>>> built;
    std::list<std::pair<uint64_t, Path>> plates;

    explicit Impl(const AssetResolver* a) : assets(a), text(a) {}

    void problem(Built& b, const char* code, const std::string& layer, const std::string& path, const std::string& message, Json detail = Json::object()) {
        TitleProblem p;
        p.severity = TitleSeverity::Warning;
        p.code = code;
        p.layer = layer;
        p.path = path;
        p.message = message;
        p.detail = std::move(detail);
        b.problems.push_back(std::move(p));
    }

    // an asset's resolution and the key part its content revision adds
    ResolvedTitleAsset resolve(const Json& ref, TitleAssetKind kind, std::string& key) {
        const TitleAssetRef r = decodeAssetRef(ref);
        ResolvedTitleAsset a;
        if (!r.empty() && assets) a = assets->resolve(r, kind);
        key += "|" + r.path + "|" + r.guid + "|" + a.path + "#" + std::to_string(a.revision);
        return a;
    }

    std::shared_ptr<const Built> build(const Json& layer, const TitleArtDoc& doc);
    void buildText(const Json& layer, Built& b);
    void buildShape(const Json& layer, const TitleArtDoc& doc, Built& b);
    void buildVector(const Json& layer, const ResolvedTitleAsset& asset, Built& b);
    void buildImage(const Json& layer, const ResolvedTitleAsset& asset, Built& b);
    void applyOps(const Json& layer, Built& b, TitleRect frame);
    const Path& plate(uint64_t geometry, const Built& b, const PlateParams& plate);
};

const Path& TitleGeometry::Impl::plate(uint64_t geometry, const Built& b, const PlateParams& p) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%d|%.9g|%.9g|%.9g|%.9g|%.9g", (int)p.kind, p.close, p.corner, p.slant, b.size, b.shear);
    const uint64_t key = hashCombine(geometry, fnv1a64(buffer));
    for (auto it = plates.begin(); it != plates.end(); ++it) {
        if (it->first == key) {
            plates.splice(plates.begin(), plates, it);
            return plates.front().second;
        }
    }
    Path input = b.fromAlpha ? b.hull : allGlyphs(b);
    // stroke centre lines join the plate input, so hull and box plates enclose them
    for (const StrokedPath& s : b.strokes) input.append(s.path);
    plates.emplace_front(key, plateBase(input, p, b.size, b.fromAlpha ? 0 : b.shear));
    while (plates.size() > PLATE_CACHE_ENTRIES) plates.pop_back();
    return plates.front().second;
}

void TitleGeometry::Impl::applyOps(const Json& layer, Built& b, TitleRect frame) {
    // warp over the frame (re-centred), distort over the warped frame, then weight
    const WarpParams warp = decodeWarp(member(layer, "warp"));
    if (const std::optional<Warper> w = makeWarper(frame, warp)) {
        for (GlyphOutline& g : b.glyphs) g.path = remapPath(g.path, w->map);
        for (StrokedPath& s : b.strokes) s.path = remapPath(s.path, w->map);
        frame = w->frame;
    }
    if (const std::optional<PointMap> d = makeDistorter(frame, decodeDistort(member(layer, "distort")))) {
        for (GlyphOutline& g : b.glyphs) g.path = remapPath(g.path, *d);
        for (StrokedPath& s : b.strokes) s.path = remapPath(s.path, *d);
    }
    const double delta = num(layer, "weight", 0) / 100 * b.size;
    if (delta != 0) {
        std::vector<GlyphOutline> kept;
        std::vector<uint32_t> keptChars;
        for (size_t i = 0; i < b.glyphs.size(); i++) {
            Path weighted = weightPath(b.glyphs[i].path, delta, b.size);
            // glyphs whose strokes are thinner than 2 |delta| vanish
            if (!hasInk(weighted)) continue;
            kept.push_back({b.glyphs[i].cluster, std::move(weighted)});
            if (i < b.charIndex.size()) keptChars.push_back(b.charIndex[i]);
        }
        b.glyphs = std::move(kept);
        if (!b.charIndex.empty()) b.charIndex = std::move(keptChars);
        // an SVG stroke offsets on both sides
        for (StrokedPath& s : b.strokes) s.width = std::max(0.0, s.width + 2 * delta);
        b.strokes.erase(std::remove_if(b.strokes.begin(), b.strokes.end(), [](const StrokedPath& s) { return !(s.width > 0); }),
                        b.strokes.end());
    }
}

void TitleGeometry::Impl::buildText(const Json& layer, Built& b) {
    const LaidOutText& laid = text.layout(layer);
    b.problems = laid.problems;
    b.size = num(layer, "size", 200);
    b.shear = num(layer, "shear", 0);
    std::vector<uint32_t> lines;
    for (size_t c = 0; c < laid.clusters.size(); c++) {
        for (const LaidOutGlyph& g : laid.clusters[c].glyphs) {
            if (!hasInk(g.outline)) continue;
            b.glyphs.push_back({(uint32_t)c, g.outline});
            b.charIndex.push_back(laid.clusters[c].index);
            lines.push_back(laid.clusters[c].line);
        }
    }
    applyOps(layer, b, laid.frame);
    // the span boxes follow the outline operations
    std::map<uint32_t, Extent> lineExtent;
    std::vector<TitleRect> charBoxes;
    for (const GlyphOutline& g : b.glyphs) {
        const TitleRect box = g.path.controlBounds();
        charBoxes.push_back(box);
        lineExtent[laid.clusters[g.cluster].line].add(box);
    }
    for (size_t i = 0; i < b.glyphs.size(); i++) {
        b.spans.push_back({b.glyphs[i].cluster, charBoxes[i], lineExtent[laid.clusters[b.glyphs[i].cluster].line].rect()});
    }
}

void TitleGeometry::Impl::buildShape(const Json& layer, const TitleArtDoc& doc, Built& b) {
    ShapeParams shape = decodeShape(layer);
    // the document's seed is added to every element seed (Reseed changes every random element)
    shape.seed += (int64_t)num(doc.root, "seed", 1);
    b.size = shape.size;
    const Path path = shapePath(shape);
    if (path.empty()) return;
    b.glyphs.push_back({0, path});
    applyOps(layer, b, path.controlBounds());
}

void TitleGeometry::Impl::buildVector(const Json& layer, const ResolvedTitleAsset& asset, Built& b) {
    const std::string id = str(layer, "id");
    b.size = num(layer, "size", 200);
    if (!asset.found) {
        if (!decodeAssetRef(member(layer, "svg")).empty()) {
            problem(b, "asset_missing", id, "svg", "SVG '" + decodeAssetRef(member(layer, "svg")).path + "' not found");
        }
        return;
    }
    std::vector<uint8_t> bytes;
    std::string error;
    if (!assets->readBytes(asset, bytes, &error)) {
        problem(b, "asset_missing", id, "svg", "SVG '" + asset.assetPath + "' could not be read: " + error);
        return;
    }
    auto svg = std::make_shared<const std::string>(bytes.begin(), bytes.end());
    if (str(layer, "mode", "shape") == "artwork") {
        // the SVG's own picture over its viewport, the larger side `size`
        std::unique_ptr<lunasvg::Document> d = lunasvg::Document::loadFromData(*svg);
        if (!d || d->width() <= 0 || d->height() <= 0) {
            problem(b, "asset_missing", id, "svg", "SVG '" + asset.assetPath + "' has no picture to draw");
            return;
        }
        const TitleVec2 half = imageHalfExtents((int)std::lround(d->width() * 1000), (int)std::lround(d->height() * 1000), b.size);
        b.fromAlpha = true;
        b.source.svg = svg;
        b.source.rect = {-half.x, -half.y, 2 * half.x, 2 * half.y};
        const TitleImageData proxy = artworkRaster(svg, b.source.rect, HULL_PROXY_SIDE);
        b.hull = pictureHull(proxy, b.source.rect);
        return;
    }
    const SvgGeometry g = parseSvgGeometry(*svg);
    if (!g.ok) {
        problem(b, "asset_missing", id, "svg", "SVG '" + asset.assetPath + "' could not be read: " + g.error);
        return;
    }
    if (!g.ignored.empty()) {
        Json features = Json::array();
        std::string list;
        for (const std::string& f : g.ignored) {
            features.push_back(f);
            list += (list.empty() ? "" : ", ") + f;
        }
        problem(b, "svg_feature_ignored", id, "svg", "SVG features drawn as nothing in shape mode: " + list, Json{{"features", features}});
    }
    // fit: the larger side of the ink bounds (stroke widths included) = size, centred, y flipped
    Extent e;
    for (const Path& f : g.fills) e.add(f.controlBounds());
    for (const SvgStroke& s : g.strokes) e.add(growRect(s.path.controlBounds(), s.width / 2));
    if (e.empty()) return;
    const double span = std::max({e.x1 - e.x0, e.y1 - e.y0, 1e-6});
    const double k = b.size / span;
    const double cx = (e.x0 + e.x1) / 2, cy = (e.y0 + e.y1) / 2;
    const Affine fit{k, 0, 0, -k, -cx * k, cy * k};
    for (const Path& f : g.fills) {
        Path p = f;
        p.transform(fit);
        b.glyphs.push_back({0, std::move(p)});
    }
    for (const SvgStroke& s : g.strokes) {
        StrokedPath sp;
        sp.path = s.path;
        sp.path.transform(fit);
        sp.width = s.width * k;
        sp.join = joinOf(s.join);
        sp.cap = capOf(s.cap);
        sp.miterLimit = s.miterLimit;
        b.strokes.push_back(std::move(sp));
    }
    const double hw = (e.x1 - e.x0) * k / 2, hh = (e.y1 - e.y0) * k / 2;
    applyOps(layer, b, {-hw, -hh, 2 * hw, 2 * hh});
}

void TitleGeometry::Impl::buildImage(const Json& layer, const ResolvedTitleAsset& asset, Built& b) {
    const std::string id = str(layer, "id");
    b.size = num(layer, "size", 200);
    if (!asset.found) {
        if (!decodeAssetRef(member(layer, "image")).empty()) {
            problem(b, "asset_missing", id, "image", "image '" + decodeAssetRef(member(layer, "image")).path + "' not found");
        }
        return;
    }
    auto image = std::make_shared<TitleImageData>();
    std::string error;
    if (!assets->loadImage(asset, *image, &error) || image->width <= 0 || image->height <= 0) {
        problem(b, "asset_missing", id, "image", "image '" + asset.assetPath + "' could not be loaded: " + error);
        return;
    }
    const TitleVec2 half = imageHalfExtents(image->width, image->height, b.size);
    b.fromAlpha = true;
    b.image = image;
    b.source.image = image;
    b.source.rect = {-half.x, -half.y, 2 * half.x, 2 * half.y};
    b.source.nearest = str(layer, "filter", "linear") == "nearest";
    b.hull = pictureHull(reducedImage(*image, HULL_PROXY_SIDE), b.source.rect);
}

std::shared_ptr<const Built> TitleGeometry::Impl::build(const Json& layer, const TitleArtDoc& doc) {
    const LayerType type = layerTypeOf(layer);
    // the geometry key: the members outlines depend on and the content revisions of their assets
    Json members = Json::object();
    static const char* const COMMON[] = {"type", "warp", "distort", "weight", "size"};
    for (const char* m : COMMON) {
        if (layer.contains(m)) members[m] = layer[m];
    }
    std::string key;
    ResolvedTitleAsset asset;
    switch (type) {
        case LayerType::Text:
            for (const char* m : {"text", "font", "face_index", "fallback_fonts", "variations", "features", "language", "tracking",
                                  "line_height", "kerning", "align", "vertical", "tate_chu_yoko", "tate_chu_yoko_max", "proportional",
                                  "h_scale", "v_scale", "shear", "lines"}) {
                if (layer.contains(m)) members[m] = layer[m];
            }
            if (layer.contains("chars") && layer["chars"].is_object()) {
                Json chars = layer["chars"];
                for (auto& c : chars) {
                    if (c.is_object()) c.erase("face");
                }
                members["chars"] = chars;
            }
            if (assets) {
                const auto font = [&](const Json& ref) { resolve(ref, TitleAssetKind::Font, key); };
                font(member(layer, "font"));
                if (layer.contains("fallback_fonts") && layer["fallback_fonts"].is_array()) {
                    for (const Json& ref : layer["fallback_fonts"]) font(ref);
                }
            }
            break;
        case LayerType::Shape:
            for (const char* m : {"shape", "aspect", "corner", "slant", "points", "inner", "jitter", "seed", "tail"}) {
                if (layer.contains(m)) members[m] = layer[m];
            }
            members["document_seed"] = num(doc.root, "seed", 1);
            break;
        case LayerType::Vector:
            members["mode"] = str(layer, "mode", "shape");
            asset = resolve(member(layer, "svg"), TitleAssetKind::Vector, key);
            break;
        case LayerType::Image:
            members["filter"] = str(layer, "filter", "linear");
            asset = resolve(member(layer, "image"), TitleAssetKind::Image, key);
            break;
        default:
            break;
    }
    key = writeTitleArtJson(members) + key;
    const uint64_t hash = fnv1a64(key);
    for (auto it = built.begin(); it != built.end(); ++it) {
        if (it->first == hash) {
            built.splice(built.begin(), built, it);
            return built.front().second;
        }
    }
    auto b = std::make_shared<Built>();
    b->key = hash;
    switch (type) {
        case LayerType::Text: buildText(layer, *b); break;
        case LayerType::Shape: buildShape(layer, doc, *b); break;
        case LayerType::Vector: buildVector(layer, asset, *b); break;
        case LayerType::Image: buildImage(layer, asset, *b); break;
        default: break;
    }
    built.emplace_front(hash, b);
    while (built.size() > BUILT_CACHE_ENTRIES) built.pop_back();
    return b;
}

TitleGeometry::TitleGeometry(const AssetResolver* assets) : impl(std::make_unique<Impl>(assets)) {}

TitleGeometry::~TitleGeometry() = default;

void TitleGeometry::clear() {
    impl->built.clear();
    impl->plates.clear();
    impl->text.clear();
}

LayerElements TitleGeometry::elements(const Json& layer, const TitleArtDoc& doc, int cluster) {
    LayerElements out;
    const LayerType type = layerTypeOf(layer);
    if (type != LayerType::Text && type != LayerType::Shape && type != LayerType::Vector && type != LayerType::Image) return out;
    std::shared_ptr<const Built> b = impl->build(layer, doc);
    if (cluster >= 0) b = onlyCluster(b, (uint32_t)cluster);
    out.problems = b->problems;
    out.unit = b->size / 100;
    out.fromAlpha = b->fromAlpha;
    out.spans = b->spans;
    const Json& style = member(layer, "style");
    const Json& face = member(style, "face");

    // the face, split into one element per distinct material when characters carry a face
    // override; glyphs without one use the layer's face
    TitleElement whole;
    whole.kind = ElementKind::Face;
    whole.material = face.is_object() ? &face : nullptr;
    whole.strokes = b->strokes;
    whole.source = b->source;
    std::vector<TitleElement> faces;
    if (type == LayerType::Text && layer.contains("chars") && layer["chars"].is_object()) {
        const Json& chars = layer["chars"];
        const std::string faceKey = face.dump();
        std::vector<std::string> keys;
        for (size_t g = 0; g < b->glyphs.size(); g++) {
            const auto adjust = chars.find(std::to_string(b->charIndex[g]));
            const Json* material = &face;
            if (adjust != chars.end() && adjust->is_object() && adjust->contains("face") && (*adjust)["face"].is_object()) {
                material = &(*adjust)["face"];
            }
            const std::string key = material == &face ? faceKey : material->dump();
            const auto found = std::find(keys.begin(), keys.end(), key);
            if (found == keys.end()) {
                keys.push_back(key);
                TitleElement e = whole;
                e.material = material;
                e.faceGroup = key == faceKey ? 0 : (int)std::count_if(keys.begin(), keys.end(), [&](const std::string& k) { return k != faceKey; });
                e.outlines.push_back(b->glyphs[g].path);
                faces.push_back(std::move(e));
            } else {
                faces[found - keys.begin()].outlines.push_back(b->glyphs[g].path);
            }
        }
    }
    if (faces.empty()) {
        for (const GlyphOutline& g : b->glyphs) whole.outlines.push_back(g.path);
        faces.push_back(std::move(whole));
    }
    for (TitleElement& f : faces) out.elements.push_back(std::move(f));

    // rims, innermost first, each grown from the face by the widths of the rims inside it
    double grow = 0;
    int rimIndex = 0;
    const Json& rims = member(style, "rims");
    if (rims.is_array()) {
        for (const Json& rim : rims) {
            const auto enabled = rim.find("enabled");
            if ((enabled != rim.end() && enabled->is_boolean() && !enabled->get<bool>()) || !(num(rim, "width", 0) > 0)) continue;
            grow += num(rim, "width", 0) * out.unit;
            TitleElement e;
            e.kind = ElementKind::Rim;
            e.rimIndex = rimIndex++;
            e.rimId = str(rim, "id");
            e.grow = grow;
            e.join = joinOf(str(rim, "join", "miter"));
            e.miterLimit = 4.0;
            const auto counters = rim.find("fill_counters");
            e.fillCounters = counters != rim.end() && counters->is_boolean() && counters->get<bool>();
            const TitleVec2 offset = decodeVec2(member(rim, "offset"));
            e.offset = {offset.x * out.unit, offset.y * out.unit};
            const Json& material = member(rim, "material");
            e.material = material.is_object() ? &material : nullptr;
            const PlateParams plate = decodePlate(member(rim, "plate"));
            if (b->fromAlpha && plate.kind == PlateKind::Contour) {
                // pixel layers grow contour rims through the alpha's distance field
                e.source = b->source;
                e.close = std::max(0.0, plate.close) / 100 * b->size;
            } else if (!b->fromAlpha && plateIsIdentity(plate)) {
                for (const GlyphOutline& g : b->glyphs) e.outlines.push_back(g.path);
                e.strokes = b->strokes;
            } else {
                const Path& base = impl->plate(b->key, *b, plate);
                if (!base.empty()) e.outlines.push_back(base);
            }
            out.elements.push_back(std::move(e));
        }
    }

    Extent bounds;
    for (const TitleElement& e : out.elements) {
        for (const Path& p : e.outlines) bounds.add(p.controlBounds());
        for (const StrokedPath& s : e.strokes) bounds.add(growRect(s.path.controlBounds(), s.width / 2));
        if (!e.source.empty()) bounds.add(e.source.rect);
    }
    out.baseBounds = bounds.rect();
    return out;
}

std::vector<GlyphOutline> TitleGeometry::outlines(const Json& layer, const TitleArtDoc& doc) {
    const LayerType type = layerTypeOf(layer);
    if (type != LayerType::Text && type != LayerType::Shape && type != LayerType::Vector && type != LayerType::Image) return {};
    const std::shared_ptr<const Built> b = impl->build(layer, doc);
    if (!b->fromAlpha) {
        std::vector<GlyphOutline> out = b->glyphs;
        // SVG strokes as outlines (S86's meshes, S87's exports)
        for (const StrokedPath& s : b->strokes) {
            Path outline = strokeOutline(s.path, s.width, offsetJoin(s.join), offsetCap(s.cap), s.miterLimit, b->size);
            if (hasInk(outline)) out.push_back({0, std::move(outline)});
        }
        return out;
    }
    // pixel layers: the traced alpha
    TraceParams trace;
    if (type == LayerType::Image) trace = decodeTrace(member(layer, "trace"));
    if (b->image) return {{0, traceImage(*b->image, b->size, trace)}};
    if (b->source.svg) {
        const int side = std::clamp((int)std::ceil(b->size * 2), 1, 1024);
        return {{0, traceImage(artworkRaster(b->source.svg, b->source.rect, side), b->size, trace)}};
    }
    return {};
}

std::vector<double> TitleGeometry::rimBands(const Json& layer) {
    std::vector<double> out;
    const double unit = layerSourceSize(layer) / 100;
    const Json& rims = member(member(layer, "style"), "rims");
    if (!rims.is_array()) return out;
    double grow = 0;
    for (const Json& rim : rims) {
        const auto enabled = rim.find("enabled");
        if ((enabled != rim.end() && enabled->is_boolean() && !enabled->get<bool>()) || !(num(rim, "width", 0) > 0)) continue;
        grow += num(rim, "width", 0) * unit;
        out.push_back(grow);
    }
    return out;
}

TitleRect TitleGeometry::contentBounds(const Json& layer, const TitleArtDoc& doc) {
    const LayerType type = layerTypeOf(layer);
    const TitleRect canvas{-doc.canvasWidth() / 2.0, -doc.canvasHeight() / 2.0, (double)doc.canvasWidth(), (double)doc.canvasHeight()};
    switch (type) {
        case LayerType::Text:
        case LayerType::Shape:
        case LayerType::Vector:
        case LayerType::Image: {
            const LayerElements els = elements(layer, doc);
            Extent local;
            for (const TitleElement& e : els.elements) local.add(elementBounds(e));
            return transformRect(local.rect(), worldMatrix(layer, doc));
        }
        case LayerType::Group: {
            Extent e;
            for (const Json& child : layerChildren(layer)) {
                if (draws(child)) e.add(reachBounds(child, doc, 1.0));
            }
            return e.rect();
        }
        case LayerType::Generator: {
            const Json& generator = member(layer, "generator");
            const GeneratorDef* def = findGenerator(str(generator, "type"));
            if (!def) return {};
            if (def->bounds) {
                const Json& params = member(generator, "params");
                const TitleRect r = def->bounds(ParamValues(params));
                if (!r.empty()) return transformRect(r, worldMatrix(layer, doc));
            }
            return canvas;
        }
        case LayerType::Adjustment:
            return canvas;
        default:
            return {};
    }
}

TitleRect TitleGeometry::reachBounds(const Json& layer, const TitleArtDoc& doc, double pxScale) {
    TitleRect r = contentBounds(layer, doc);
    if (r.empty()) return r;
    const TitleRect canvas{-doc.canvasWidth() / 2.0, -doc.canvasHeight() / 2.0, (double)doc.canvasWidth(), (double)doc.canvasHeight()};
    bool fillsCanvas = false;
    const auto reachOf = [&](const Json& effect) {
        const auto enabled = effect.find("enabled");
        if (enabled != effect.end() && enabled->is_boolean() && !enabled->get<bool>()) return 0.0;
        const EffectDef* def = findEffect(str(effect, "type"));
        if (!def) return 0.0;
        const Json& params = member(effect, "params");
        const ParamValues values(params);
        if (def->fillsCanvas && def->fillsCanvas(values)) fillsCanvas = true;
        return def->reach ? std::max(0.0, (double)def->reach(values)) : 0.0;
    };
    // part effects reach from their part's pixels (inside the content), then the layer's stack
    double partReach = 0;
    const Json& parts = member(layer, "part_effects");
    if (parts.is_object()) {
        for (const Json& stack : parts) {
            double sum = 0;
            if (stack.is_array()) {
                for (const Json& effect : stack) sum += reachOf(effect);
            }
            partReach = std::max(partReach, sum);
        }
    }
    double reach = partReach;
    const Json& effects = member(layer, "effects");
    if (effects.is_array()) {
        for (const Json& effect : effects) reach += reachOf(effect);
    }
    if (fillsCanvas) return canvas;
    double feather = 0;
    const Json& masks = member(layer, "masks");
    if (masks.is_array()) {
        for (const Json& mask : masks) {
            const auto enabled = mask.find("enabled");
            if (enabled != mask.end() && enabled->is_boolean() && !enabled->get<bool>()) continue;
            feather = std::max(feather, num(mask, "feather", 0));
        }
    }
    return growRect(r, reach + 1.5 * feather + 1.0 / std::max(pxScale, 1e-6));
}
