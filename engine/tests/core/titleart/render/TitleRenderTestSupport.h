// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the renderer's GPU tests share: a content provider of exact analytic shapes (boxes,
// circles, an "F" per character) standing in for the text and geometry module, so the passes
// are tested on known coverage; an asset resolver of generated images; documents built from
// JSON; driving a job frame by frame on the headless device; reading its image back; and the
// comparisons of §6.3's tolerance (premultiplied).

#pragma once

#include "GpuEnvironment.h"
#include "Readback.h"

#include "render/SystemRender.h"
#include "shader/RuntimeShaderCompiler.h"
#include "texture/TexturePrepare.h"
#include "titleart/TitleArtIO.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleContent.h"
#include "titleart/render/TitleGpu.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart::test {

    // ---- analytic shapes as element outlines ----
    // A box outline is the rectangle path of its corners; a circle is a two-point path with
    // evenOdd set (its diameter). The provider's coverage reads them back exactly.

    inline geometry::Path boxPath(double x0, double y0, double x1, double y1) {
        geometry::Path p;
        p.moveTo(x0, y0);
        p.lineTo(x1, y0);
        p.lineTo(x1, y1);
        p.lineTo(x0, y1);
        p.close();
        return p;
    }

    inline geometry::Path circlePath(double cx, double cy, double r) {
        geometry::Path p;
        p.moveTo(cx - r, cy);
        p.lineTo(cx + r, cy);
        p.evenOdd = true;
        return p;
    }

    struct Shape {
        bool circle = false;
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;      // box, or the circle's diameter on y0
    };

    inline Shape shapeOf(const geometry::Path& p) {
        Shape s;
        const auto& pts = p.pointList();
        if (p.evenOdd && pts.size() == 2) {
            s.circle = true;
            s.x0 = pts[0].x;
            s.x1 = pts[1].x;
            s.y0 = s.y1 = pts[0].y;
            return s;
        }
        const TitleRect b = p.controlBounds();
        s.x0 = b.x;
        s.y0 = b.y;
        s.x1 = b.x + b.width;
        s.y1 = b.y + b.height;
        return s;
    }

    // true when p is within `grow` of the shape by the join's metric
    inline bool insideGrown(const Shape& s, double px, double py, double grow, geometry::StrokeJoin join) {
        if (s.circle) {
            const double cx = (s.x0 + s.x1) * 0.5, r = (s.x1 - s.x0) * 0.5;
            return std::hypot(px - cx, py - s.y0) <= r + grow;
        }
        const double dx = std::max({s.x0 - px, 0.0, px - s.x1});
        const double dy = std::max({s.y0 - py, 0.0, py - s.y1});
        if (dx == 0.0 && dy == 0.0) return true;
        switch (join) {
            case geometry::StrokeJoin::Miter: return dx <= grow && dy <= grow;
            case geometry::StrokeJoin::Round: return std::hypot(dx, dy) <= grow;
            case geometry::StrokeJoin::Bevel: return dx <= grow && dy <= grow && dx + dy <= grow;
        }
        return false;
    }

    // 4 x 4 samples per pixel
    inline geometry::CoverageMask rasterize(const std::vector<Shape>& shapes, double grow, geometry::StrokeJoin join, const geometry::MaskFrame& frame) {
        geometry::CoverageMask mask;
        mask.width = frame.width;
        mask.height = frame.height;
        mask.pixels.assign((size_t)frame.width * frame.height, 0);
        for (int y = 0; y < frame.height; y++) {
            for (int x = 0; x < frame.width; x++) {
                int hits = 0;
                for (int j = 0; j < 4; j++) {
                    for (int i = 0; i < 4; i++) {
                        const double px = frame.origin.x + (x + (i + 0.5) / 4.0) / frame.scale;
                        const double py = frame.origin.y - (y + (j + 0.5) / 4.0) / frame.scale;
                        for (const Shape& s : shapes) {
                            if (insideGrown(s, px, py, grow, join)) {
                                hits++;
                                break;
                            }
                        }
                    }
                }
                mask.pixels[(size_t)y * frame.width + x] = (uint8_t)std::lround(hits * 255.0 / 16.0);
            }
        }
        return mask;
    }

    // ---- the provider ----
    // shape layers: `rect` (size tall, size x aspect wide) or `ellipse` (a circle of the size);
    // text layers: an "F" of three boxes per character, 0.7 em apart, centred; image layers: a
    // box of the size (fromAlpha) whose picture is a horizontal red-to-blue ramp
    class TestContent : public TitleContentProvider {
    public:
        int elementCalls = 0;
        int coverageCalls = 0;

        static std::vector<Shape> letterF(double size, double dx) {
            const double s = size;
            return {shapeOf(boxPath(dx - 0.30 * s, -0.40 * s, dx - 0.12 * s, 0.40 * s)),
                    shapeOf(boxPath(dx - 0.30 * s, 0.25 * s, dx + 0.30 * s, 0.40 * s)),
                    shapeOf(boxPath(dx - 0.30 * s, -0.05 * s, dx + 0.15 * s, 0.10 * s))};
        }

        static std::vector<std::vector<Shape>> glyphs(const Json& layer) {
            const LayerType type = layerTypeOf(layer);
            const double size = layerSourceSize(layer);
            std::vector<std::vector<Shape>> out;
            if (type == LayerType::Text) {
                const std::string text = layer.value("text", std::string("F"));
                const int n = std::max(1, (int)text.size());
                for (int i = 0; i < n; i++) out.push_back(letterF(size, (i - (n - 1) * 0.5) * 0.7 * size));
            } else if (type == LayerType::Shape && layer.value("shape", std::string("rect")) == "ellipse") {
                out.push_back({shapeOf(circlePath(0, 0, size * 0.5))});
            } else {
                const double w = size * (type == LayerType::Shape ? layer.value("aspect", 1.0) : 1.0);
                out.push_back({shapeOf(boxPath(-w * 0.5, -size * 0.5, w * 0.5, size * 0.5))});
            }
            return out;
        }

        static geometry::Path pathOf(const Shape& s) {
            if (s.circle) return circlePath((s.x0 + s.x1) * 0.5, s.y0, (s.x1 - s.x0) * 0.5);
            return boxPath(s.x0, s.y0, s.x1, s.y1);
        }

        geometry::LayerElements elements(const Json& layer, const TitleArtDoc&, const AssetResolver*, const TitleElementFilter& filter) override {
            elementCalls++;
            geometry::LayerElements out;
            const double size = layerSourceSize(layer);
            out.unit = size / 100.0;
            out.fromAlpha = layerTypeOf(layer) == LayerType::Image;
            const auto all = glyphs(layer);
            std::vector<geometry::Path> outlines;
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (size_t g = 0; g < all.size(); g++) {
                if (filter.cluster >= 0 && (int)g != filter.cluster) continue;
                for (const Shape& s : all[g]) {
                    outlines.push_back(pathOf(s));
                    x0 = std::min(x0, s.x0);
                    x1 = std::max(x1, s.x1);
                    y0 = std::min(y0, s.circle ? s.y0 - (s.x1 - s.x0) * 0.5 : s.y0);
                    y1 = std::max(y1, s.circle ? s.y0 + (s.x1 - s.x0) * 0.5 : s.y1);
                }
            }
            if (outlines.empty()) return out;
            out.baseBounds = {x0, y0, x1 - x0, y1 - y0};
            const Json& style = layer.at("style");
            geometry::TitleElement face;
            face.kind = geometry::ElementKind::Face;
            face.outlines = outlines;
            face.material = &style.at("face");
            out.elements.push_back(face);
            double grow = 0.0;
            const Json& rims = style.at("rims");
            for (size_t i = 0; i < rims.size(); i++) {
                const Json& rim = rims[i];
                if (!rim.value("enabled", true) || !(rim.value("width", 0.0) > 0.0)) continue;
                grow += rim.value("width", 0.0) * size / 100.0;
                geometry::TitleElement e;
                e.kind = geometry::ElementKind::Rim;
                e.rimIndex = (int)i;
                e.rimId = rim.value("id", std::string());
                e.outlines = outlines;
                e.grow = grow;
                const std::string join = rim.value("join", std::string("miter"));
                e.join = join == "round" ? geometry::StrokeJoin::Round : join == "bevel" ? geometry::StrokeJoin::Bevel : geometry::StrokeJoin::Miter;
                e.fillCounters = rim.value("fill_counters", false);
                const TitleVec2 offset = decodeVec2(rim.value("offset", Json::array({0, 0})));
                e.offset = {offset.x * size / 100.0, offset.y * size / 100.0};
                e.material = &rim.at("material");
                out.elements.push_back(e);
            }
            return out;
        }

        geometry::CoverageMask coverage(const geometry::TitleElement& element, const geometry::MaskFrame& frame) override {
            coverageCalls++;
            std::vector<Shape> shapes;
            for (const geometry::Path& p : element.outlines) shapes.push_back(shapeOf(p));
            return rasterize(shapes, element.grow, element.join, frame);
        }

        bool picture(const Json& layer, const TitleArtDoc&, const AssetResolver*, const geometry::MaskFrame& frame, TitleImageData& out) override {
            if (layerTypeOf(layer) != LayerType::Image) return false;
            TitleImageData ramp;
            ramp.width = 16;
            ramp.height = 16;
            for (int y = 0; y < 16; y++) {
                for (int x = 0; x < 16; x++) {
                    ramp.rgba.push_back((uint8_t)(255 - x * 17));
                    ramp.rgba.push_back((uint8_t)(y * 17));
                    ramp.rgba.push_back((uint8_t)(x * 17));
                    ramp.rgba.push_back(255);
                }
            }
            const double size = layerSourceSize(layer);
            drawImageToFrame(ramp, {-size * 0.5, -size * 0.5, size, size}, frame, out);
            return true;
        }

        bool spans(const Json& layer, const TitleArtDoc&, const AssetResolver*, const TitleElementFilter& filter, TitleSpans& out) override {
            if (layerTypeOf(layer) != LayerType::Text) return false;
            const auto all = glyphs(layer);
            TitleRect line{1e300, 1e300, 0, 0};
            double lx1 = -1e300, ly1 = -1e300;
            std::vector<TitleRect> boxes;
            for (size_t g = 0; g < all.size(); g++) {
                double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
                for (const Shape& s : all[g]) {
                    x0 = std::min(x0, s.x0);
                    y0 = std::min(y0, s.y0);
                    x1 = std::max(x1, s.x1);
                    y1 = std::max(y1, s.y1);
                }
                if (filter.cluster >= 0 && (int)g != filter.cluster) continue;
                boxes.push_back({x0, y0, x1 - x0, y1 - y0});
                line.x = std::min(line.x, x0);
                line.y = std::min(line.y, y0);
                lx1 = std::max(lx1, x1);
                ly1 = std::max(ly1, y1);
            }
            line.width = lx1 - line.x;
            line.height = ly1 - line.y;
            out.chars = boxes;
            out.lines.assign(boxes.size(), line);
            return true;
        }

        geometry::Path shapeOutline(const Json& shape) override {
            const double size = shape.value("size", 200.0);
            if (shape.value("shape", std::string("rect")) == "ellipse") return circlePath(0, 0, size * 0.5);
            const double w = size * shape.value("aspect", 1.0);
            return boxPath(-w * 0.5, -size * 0.5, w * 0.5, size * 0.5);
        }
    };

    // generated images by asset path: "tile.png" a 8 x 8 black-and-white checker, "sky.png" a
    // 64 x 32 panorama (blue zenith, white horizon, brown ground)
    class TestAssets : public AssetResolver {
    public:
        ResolvedTitleAsset resolve(const TitleAssetRef& ref, TitleAssetKind) const override {
            ResolvedTitleAsset r;
            r.found = ref.path == "tile.png" || ref.path == "sky.png";
            r.path = r.assetPath = ref.path;
            r.guid = ref.guid;
            r.revision = r.found ? 7 : 0;
            return r;
        }
        std::shared_ptr<FontFace> loadFont(const ResolvedTitleAsset&, int, std::string* error) const override {
            if (error) *error = "no fonts in the renderer tests";
            return nullptr;
        }
        bool readBytes(const ResolvedTitleAsset&, std::vector<uint8_t>&, std::string* error) const override {
            if (error) *error = "no files in the renderer tests";
            return false;
        }
        bool loadImage(const ResolvedTitleAsset& asset, TitleImageData& out, std::string*) const override {
            out.rgba.clear();
            if (asset.path == "tile.png") {
                out.width = out.height = 8;
                for (int y = 0; y < 8; y++) {
                    for (int x = 0; x < 8; x++) {
                        const uint8_t v = ((x / 4 + y / 4) % 2) ? 255 : 0;
                        out.rgba.insert(out.rgba.end(), {v, v, v, 255});
                    }
                }
                return true;
            }
            out.width = 64;
            out.height = 32;
            for (int y = 0; y < 32; y++) {
                for (int x = 0; x < 64; x++) {
                    const double t = y / 31.0;
                    const uint8_t r = (uint8_t)(t < 0.5 ? 60 + 390 * t : 120 + 60 * (1.0 - t));
                    const uint8_t g = (uint8_t)(t < 0.5 ? 120 + 260 * t : 90 + 40 * (1.0 - t));
                    const uint8_t b = (uint8_t)(t < 0.5 ? 220 + 70 * t : 40);
                    out.rgba.insert(out.rgba.end(), {r, g, b, 255});
                }
            }
            return true;
        }
        std::shared_ptr<FontFace> builtinFont() const override { return nullptr; }
    };

    // ---- documents ----

    // a normalized document from a partial root (the defaults fill the rest)
    inline std::shared_ptr<const TitleArtDoc> makeDoc(Json root) {
        root["format"] = TITLE_ART_FORMAT;
        root["version"] = TITLE_ART_VERSION;
        if (!root.contains("guid")) root["guid"] = "0123456789abcdef0123456789abcdef";
        TitleArtParseResult parsed = parseTitleArtJson(std::move(root));
        EXPECT_TRUE(parsed.ok) << parsed.error.message;
        return std::make_shared<const TitleArtDoc>(parsed.doc);
    }

    inline Json canvas(int w, int h) {
        return Json{{"width", w}, {"height", h}, {"background", {{"transparent", true}, {"color", "#000000"}}}};
    }

    inline Json solidFace(const char* color) {
        return Json{{"fill", {{"kind", "solid"}, {"color", color}}}};
    }

    inline Json shapeLayer(const char* id, const char* shape, double size, const Json& face, Json extra = Json::object()) {
        Json layer{{"id", id}, {"type", "shape"}, {"name", id}, {"shape", shape}, {"size", size}, {"style", {{"face", face}, {"rims", Json::array()}}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) layer[it.key()] = it.value();
        return layer;
    }

    // ---- running a job ----

    // frames until the job is done (its programs compile on the way, a cold shader cache takes
    // a while on a software device); false on failure or after three minutes
    inline ::testing::AssertionResult runJob(TitleRenderJob& job, const StepBudget& budget = {}, int maxFrames = 1000000) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < maxFrames && std::chrono::steady_clock::now() - start < std::chrono::minutes(3); i++) {
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            const bool done = job.step(budget);
            SystemRender::commit();
            if (done) return ::testing::AssertionSuccess();
            if (job.failed()) return ::testing::AssertionFailure() << "the job failed: " << job.error();
        }
        return ::testing::AssertionFailure() << "the job did not finish (" << job.passesDone() << " of " << job.passesTotal() << " passes)";
    }

    struct Pixels {
        int width = 0, height = 0;
        std::vector<float> rgba;            // 0..1 (straight or premultiplied as the image is)
        std::vector<uint8_t> bytes;         // RGBA8 images: the bytes
        bool premultiplied = false;

        const float* at(int x, int y) const { return &rgba[((size_t)y * width + x) * 4]; }
    };

    inline Pixels readImage(const TitleImage& image) {
        Pixels out;
        out.width = image.width;
        out.height = image.height;
        out.premultiplied = image.premultiplied;
        auto* target = image.target.as<SokolTitleTarget>();
        if (!target) {
            ADD_FAILURE() << "the image has no target";
            return out;
        }
        if (image.format == ColorFormat::RGBA) {
            EXPECT_TRUE(jasperengine::test::readTexture(target->texture(), image.width, image.height, out.bytes));
            for (uint8_t b : out.bytes) out.rgba.push_back(b / 255.0f);
            return out;
        }
        std::vector<uint8_t> raw;
        EXPECT_TRUE(jasperengine::test::readTextureRaw(target->texture(), image.width, image.height, image.format, raw));
        if (image.format == ColorFormat::RGBA16F) {
            for (size_t i = 0; i + 1 < raw.size(); i += 2) {
                uint16_t h;
                std::memcpy(&h, &raw[i], 2);
                out.rgba.push_back(halfToFloat(h));
            }
        } else {
            for (uint8_t b : raw) out.rgba.push_back(b / 255.0f);
        }
        return out;
    }

    // renders a request to the end and reads the image back
    inline Pixels render(TitleArtRenderer& renderer, const RenderRequest& request, std::unique_ptr<TitleRenderJob>* keep = nullptr) {
        std::unique_ptr<TitleRenderJob> job = renderer.begin(request);
        if (!job) {
            ADD_FAILURE() << "no job";
            return {};
        }
        const ::testing::AssertionResult ran = runJob(*job);
        Pixels p;
        if (ran) p = readImage(job->image());
        else ADD_FAILURE() << ran.message();
        if (keep) *keep = std::move(job);
        return p;
    }

    // §6.3's comparison of two 8-bit images, premultiplied: mean and max in 1/255, and the share
    // of pixels with a channel over `over`/255
    struct Difference {
        double mean = 0.0;
        double max = 0.0;
        double overFraction = 0.0;
    };

    inline Difference compare(const Pixels& a, const Pixels& b, double over = 8.0) {
        Difference d;
        if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
            d.mean = d.max = d.overFraction = 1e9;
            return d;
        }
        const size_t n = (size_t)a.width * a.height;
        size_t overCount = 0;
        double sum = 0.0;
        for (size_t i = 0; i < n; i++) {
            const float* pa = &a.rgba[i * 4];
            const float* pb = &b.rgba[i * 4];
            const double aa = a.premultiplied ? 1.0 : pa[3], ab = b.premultiplied ? 1.0 : pb[3];
            double worst = 0.0;
            for (int c = 0; c < 4; c++) {
                const double va = c < 3 ? pa[c] * aa : pa[3], vb = c < 3 ? pb[c] * ab : pb[3];
                const double diff = std::abs(va - vb) * 255.0;
                worst = std::max(worst, diff);
                sum += diff;
            }
            d.max = std::max(d.max, worst);
            if (worst > over) overCount++;
        }
        d.mean = sum / (double)(n * 4);
        d.overFraction = (double)overCount / (double)n;
        return d;
    }

    inline bool identical(const Pixels& a, const Pixels& b) {
        return a.width == b.width && a.height == b.height && a.bytes == b.bytes && !a.bytes.empty();
    }

    // the renderer every GPU test starts from: the test content, a fresh cache
    inline std::unique_ptr<TitleArtRenderer> makeRenderer() {
        auto renderer = std::make_unique<TitleArtRenderer>();
        renderer->setContentProvider(std::make_shared<TestContent>());
        return renderer;
    }

}
