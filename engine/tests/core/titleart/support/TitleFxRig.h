// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GPU rig of S83's effect tests (docs/specs/S83-title-effects-generators.md §7): linear and
// 8-bit images, the comparisons of S80 §6.12 (premultiplied pixels, AC12's blocks and
// histograms), the standard title rendered by the renderer, the analytic fixture, and EffectRig,
// which runs one definition's `apply` on an uploaded input through a TitlePassKit. The same rig
// as S80's TitleArtEffectsGpuTest.cpp (kept there for S80's ten effects), with the warm-up of S83's
// programs (titleart_fx.frag) added.

#pragma once

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "texture/TexturePrepare.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectCatalog.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TfxPrograms.h"
#include "titleart/effects/TitleFx.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePassKit.h"
#include "shader/RuntimeShaderCompiler.h"

#include "TitleStandardTitle.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "Readback.h"
#include "TestPaths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>

namespace jasperengine::titleart::fxtest {

    using namespace jasperengine::test;
    namespace standard = jasperengine::titleart::test;

    // one test unit includes this rig: its helpers stay internal to it
    namespace {

    // ---- images ----

    // linear premultiplied RGBA floats, rows top first
    struct LinearImage {
        int width = 0, height = 0;
        std::vector<float> rgba;
        float* at(int x, int y) { return &rgba[((size_t)y * width + x) * 4]; }
        const float* at(int x, int y) const { return &rgba[((size_t)y * width + x) * 4]; }
    };

    // straight sRGB 8-bit RGBA, rows top first (a PNG's pixels)
    struct Rgba8Image {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    uint8_t toByte(double v) {
        return (uint8_t)std::lround(std::min(1.0, std::max(0.0, v)) * 255.0);
    }

    Rgba8Image toRgba8(const LinearImage& image) {
        Rgba8Image out;
        out.width = image.width;
        out.height = image.height;
        out.rgba.resize(image.rgba.size());
        for (size_t i = 0; i < image.rgba.size(); i += 4) {
            const float a = std::min(1.0f, std::max(0.0f, image.rgba[i + 3]));
            for (int c = 0; c < 3; c++) {
                const float straight = a > 0.0f ? image.rgba[i + c] / a : 0.0f;
                out.rgba[i + c] = toByte(linearToSrgb(std::min(1.0f, std::max(0.0f, straight))));
            }
            out.rgba[i + 3] = toByte(a);
        }
        return out;
    }

    LinearImage downsample(const LinearImage& image, int f) {
        LinearImage out;
        out.width = image.width / f;
        out.height = image.height / f;
        out.rgba.assign((size_t)out.width * out.height * 4, 0.0f);
        for (int y = 0; y < out.height; y++) {
            for (int x = 0; x < out.width; x++) {
                float* o = out.at(x, y);
                for (int dy = 0; dy < f; dy++) {
                    for (int dx = 0; dx < f; dx++) {
                        const float* s = image.at(x * f + dx, y * f + dy);
                        for (int c = 0; c < 4; c++) o[c] += s[c] / (float)(f * f);
                    }
                }
            }
        }
        return out;
    }

    Rgba8Image trim(const Rgba8Image& image, int threshold) {
        int x0 = image.width, y0 = image.height, x1 = -1, y1 = -1;
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                if (image.rgba[((size_t)y * image.width + x) * 4 + 3] >= threshold) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
        }
        Rgba8Image out;
        if (x1 < 0) return out;
        out.width = x1 - x0 + 1;
        out.height = y1 - y0 + 1;
        out.rgba.resize((size_t)out.width * out.height * 4);
        for (int y = 0; y < out.height; y++) {
            std::copy_n(&image.rgba[((size_t)(y + y0) * image.width + x0) * 4], (size_t)out.width * 4, &out.rgba[(size_t)y * out.width * 4]);
        }
        return out;
    }

    // `image` centred on a transparent canvas of that size (Pachislop compare.ts `centred`)
    Rgba8Image centred(const Rgba8Image& image, int width, int height) {
        Rgba8Image out;
        out.width = width;
        out.height = height;
        out.rgba.assign((size_t)width * height * 4, 0);
        const int ox = (int)std::floor((width - image.width) / 2.0), oy = (int)std::floor((height - image.height) / 2.0);
        for (int y = 0; y < image.height; y++) {
            const int ty = y + oy;
            if (ty < 0 || ty >= height) continue;
            for (int x = 0; x < image.width; x++) {
                const int tx = x + ox;
                if (tx < 0 || tx >= width) continue;
                std::copy_n(&image.rgba[((size_t)y * image.width + x) * 4], 4, &out.rgba[((size_t)ty * width + tx) * 4]);
            }
        }
        return out;
    }

    double premul(const Rgba8Image& image, size_t i, int c) {
        return c == 3 ? image.rgba[i + 3] : image.rgba[i + c] * image.rgba[i + 3] / 255.0;
    }

    struct PixelStats {
        int max = 0;
        double mean = 0.0;
        double fractionOver8 = 0.0;
        std::string text() const {
            char t[160];
            std::snprintf(t, sizeof(t), "max %d/255, mean %.3f/255, %.3f %% of px > 8/255", max, mean, fractionOver8 * 100.0);
            return t;
        }
    };

    // premultiplied, as Pachislop's compare.ts and S80's ImageDiff
    PixelStats compareImages(const Rgba8Image& a, const Rgba8Image& b) {
        PixelStats s;
        if (a.width != b.width || a.height != b.height) {
            s.max = 255;
            s.mean = 255;
            s.fractionOver8 = 1;
            return s;
        }
        double sum = 0;
        size_t over = 0;
        for (size_t i = 0; i < a.rgba.size(); i += 4) {
            double worst = 0;
            for (int c = 0; c < 4; c++) {
                const double d = std::fabs(premul(a, i, c) - premul(b, i, c));
                worst = std::max(worst, d);
                sum += d;
            }
            s.max = std::max(s.max, (int)std::lround(worst));
            if (worst > 8.0) over++;
        }
        const size_t pixels = a.rgba.size() / 4;
        s.mean = pixels ? sum / (double)(pixels * 4) : 0.0;
        s.fractionOver8 = pixels ? (double)over / (double)pixels : 0.0;
        return s;
    }

    struct BlockStats {
        double fractionOver = 0.0;
        double histogramDistance = 0.0;
        std::string text() const {
            char t[160];
            std::snprintf(t, sizeof(t), "%.2f %% of 4 x 4 blocks over the threshold, histogram distance %.2f %%", fractionOver * 100.0,
                          histogramDistance);
            return t;
        }
    };

    // S80 AC12's metric on premultiplied 8-bit values: the mean of each 4 x 4 block (a partial one
    // at the edges averages what it has) differing by more than `threshold` in any channel, and
    // the largest total-variation distance of the 16-bin channel histograms
    BlockStats compareBlocks(const Rgba8Image& a, const Rgba8Image& b, int threshold = 24) {
        BlockStats s;
        if (a.width != b.width || a.height != b.height) {
            s.fractionOver = 1;
            s.histogramDistance = 100;
            return s;
        }
        size_t blocks = 0, over = 0;
        for (int by = 0; by < a.height; by += 4) {
            for (int bx = 0; bx < a.width; bx += 4) {
                double ma[4] = {}, mb[4] = {};
                int n = 0;
                for (int y = by; y < std::min(by + 4, a.height); y++) {
                    for (int x = bx; x < std::min(bx + 4, a.width); x++) {
                        const size_t i = ((size_t)y * a.width + x) * 4;
                        for (int c = 0; c < 4; c++) {
                            ma[c] += premul(a, i, c);
                            mb[c] += premul(b, i, c);
                        }
                        n++;
                    }
                }
                double worst = 0;
                for (int c = 0; c < 4; c++) worst = std::max(worst, std::fabs(ma[c] - mb[c]) / n);
                blocks++;
                if (worst > threshold) over++;
            }
        }
        s.fractionOver = blocks ? (double)over / (double)blocks : 0.0;
        const size_t pixels = a.rgba.size() / 4;
        for (int c = 0; c < 4; c++) {
            std::vector<double> ha(16, 0.0), hb(16, 0.0);
            for (size_t i = 0; i < a.rgba.size(); i += 4) {
                ha[std::min(15, (int)(premul(a, i, c) / 16.0))] += 1.0;
                hb[std::min(15, (int)(premul(b, i, c) / 16.0))] += 1.0;
            }
            double tv = 0;
            for (int k = 0; k < 16; k++) tv += std::fabs(ha[k] - hb[k]);
            s.histogramDistance = std::max(s.histogramDistance, 50.0 * tv / (double)std::max<size_t>(1, pixels));
        }
        return s;
    }

    // ---- the synthetic fixture ----
    // (ScaleInvariance: analytic, so the half-scale input is drawn exactly, not filtered down)

    constexpr int FIXTURE_WIDTH = 400, FIXTURE_HEIGHT = 220;

    // the colour of the fixture at canvas point (x, y) (image space) and its signed distance to the
    // shape, canvas px, negative inside
    void fixtureAt(double x, double y, double& d, float rgb[3]) {
        auto box = [](double px, double py, double x0, double y0, double x1, double y1) {
            const double qx = std::max(x0 - px, px - x1), qy = std::max(y0 - py, py - y1);
            return qx > 0 && qy > 0 ? std::hypot(qx, qy) : std::max(qx, qy);
        };
        // the "F" (asymmetric: a flip shows), a gold vertical gradient
        const double f = std::min({box(x, y, 80, 50, 104, 150), box(x, y, 80, 50, 164, 72), box(x, y, 80, 92, 144, 112)});
        const double ring = std::fabs(std::hypot(x - 260, y - 100) - 34) - 10;
        const double disc = std::hypot(x - 350, y - 140) - 14;
        d = std::min({f, ring, disc});
        auto hex = [](unsigned v, float out[3]) {
            out[0] = ((v >> 16) & 255) / 255.0f;
            out[1] = ((v >> 8) & 255) / 255.0f;
            out[2] = (v & 255) / 255.0f;
        };
        if (d == f) {
            float top[3], bottom[3];
            hex(0xffe070, top);
            hex(0xc05a08, bottom);
            const float t = (float)std::min(1.0, std::max(0.0, (y - 50) / 100.0));
            for (int c = 0; c < 3; c++) rgb[c] = top[c] + (bottom[c] - top[c]) * t;
        } else if (d == ring) {
            hex(y < 100 ? 0x3060e0 : 0xe03040, rgb);
        } else {
            hex(0x30c070, rgb);
        }
    }

    LinearImage fixture(float s) {
        LinearImage image;
        image.width = (int)std::lround(FIXTURE_WIDTH * s);
        image.height = (int)std::lround(FIXTURE_HEIGHT * s);
        image.rgba.assign((size_t)image.width * image.height * 4, 0.0f);
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                double cover = 0;
                float acc[3] = {0, 0, 0};
                for (int sy = 0; sy < 4; sy++) {
                    for (int sx = 0; sx < 4; sx++) {
                        double d;
                        float rgb[3];
                        fixtureAt((x + (sx + 0.5) / 4.0) / s, (y + (sy + 0.5) / 4.0) / s, d, rgb);
                        if (d <= 0) {
                            cover += 1.0 / 16.0;
                            for (int c = 0; c < 3; c++) acc[c] += srgbToLinear(rgb[c]) / 16.0f;
                        }
                    }
                }
                float* o = image.at(x, y);
                for (int c = 0; c < 3; c++) o[c] = acc[c];
                o[3] = (float)cover;
            }
        }
        return image;
    }

    // ---- the standard title ----

    // a job run to the end, frame by frame (its programs compile on the way)
    bool runJob(TitleRenderJob& job, std::string& error) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::minutes(3)) {
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            const bool done = job.step(StepBudget{});
            SystemRender::commit();
            if (done) return true;
            if (job.failed()) {
                error = "the render failed: " + job.error();
                return false;
            }
        }
        error = "the render did not finish";
        return false;
    }

    // a document rendered whole at `scale` with the module's own text and geometry and the test
    // fonts: `format` Linear gives linear premultiplied floats, Srgb8 straight sRGB bytes
    bool renderDocument(const TitleArtDoc& doc, float scale, TitleOutputFormat format, LinearImage* linear, Rgba8Image* srgb,
                        std::string& error) {
        static standard::TestAssetResolver assets;
        TitleArtRenderer renderer;
        RenderRequest request;
        request.doc = std::make_shared<const TitleArtDoc>(doc);
        request.pxScale = scale;
        request.output = format;
        request.assets = &assets;
        std::unique_ptr<TitleRenderJob> job = renderer.begin(request);
        if (!job || !runJob(*job, error)) return false;
        for (const TitleProblem& p : job->problems()) {
            if (p.code == "glyph_missing" || p.code == "asset_missing" || p.code == "geometry_unavailable") {
                error = p.code + ": " + p.message;
                return false;
            }
        }
        const TitleImage& image = job->image();
        TextureRender* texture = image.target.get() ? image.target.get()->colorTexture() : nullptr;
        if (!texture) {
            error = "the render has no image";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (linear) {
            if (!readTextureRaw(*texture, image.width, image.height, image.format, bytes)) {
                error = "readback failed";
                return false;
            }
            linear->width = image.width;
            linear->height = image.height;
            linear->rgba.resize((size_t)image.width * image.height * 4);
            if (image.format == ColorFormat::RGBA16F) {
                const uint16_t* halves = reinterpret_cast<const uint16_t*>(bytes.data());
                for (size_t i = 0; i < linear->rgba.size(); i++) linear->rgba[i] = halfToFloat(halves[i]);
            } else {
                // the RGBA8 path's SRGB8A8 accumulator: sRGB-encoded premultiplied colour
                for (size_t i = 0; i < linear->rgba.size(); i++) linear->rgba[i] = (i % 4 == 3) ? bytes[i] / 255.0f : srgbToLinear(bytes[i] / 255.0f);
            }
        }
        if (srgb) {
            if (!readTexture(*texture, image.width, image.height, bytes)) {
                error = "readback failed";
                return false;
            }
            srgb->width = image.width;
            srgb->height = image.height;
            srgb->rgba = std::move(bytes);
        }
        return true;
    }

    // the standard title at `scale`: the canvas cut to the title's pixels grown by `margin`, its
    // left and top edges on even pixels and its size even (a 2 x box filter halves it exactly)
    bool standardTitle(float scale, int margin, LinearImage& out, std::string& error) {
        LinearImage canvas;
        if (!renderDocument(standard::standardTitleDocument(), scale, TitleOutputFormat::Linear, &canvas, nullptr, error)) return false;
        int x0 = canvas.width, y0 = canvas.height, x1 = -1, y1 = -1;
        for (int y = 0; y < canvas.height; y++) {
            for (int x = 0; x < canvas.width; x++) {
                if (canvas.at(x, y)[3] <= 0.0f) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
        if (x1 < 0) {
            error = "the standard title drew nothing";
            return false;
        }
        x0 = std::max(0, (x0 - margin) & ~1);
        y0 = std::max(0, (y0 - margin) & ~1);
        x1 = std::min(canvas.width, x1 + 1 + margin);
        y1 = std::min(canvas.height, y1 + 1 + margin);
        out.width = (x1 - x0) & ~1;
        out.height = (y1 - y0) & ~1;
        out.rgba.resize((size_t)out.width * out.height * 4);
        for (int y = 0; y < out.height; y++) std::copy_n(canvas.at(x0, y0 + y), (size_t)out.width * 4, out.at(0, y));
        return true;
    }

    // the effects' input: the standard title at scale 1, rendered once per run
    bool titleInput(LinearImage& out, std::string& error) {
        static LinearImage title;
        static std::string failure;
        static const bool made = standardTitle(1.0f, 0, title, failure);
        out = title;
        error = failure;
        return made;
    }

    // the generators' canvas (the golden size they were drawn at)
    constexpr int GENERATOR_WIDTH = FIXTURE_WIDTH, GENERATOR_HEIGHT = FIXTURE_HEIGHT;

    // ---- running definitions ----

    TitleAffine documentToTarget(int canvasWidth, int canvasHeight, float s, const TitleRectI& canvas) {
        // canvas px (centre origin, +y up) -> target px: the canvas's top-left at (canvas.x, canvas.y)
        TitleAffine m;
        m.a = s;
        m.d = -s;
        m.tx = canvas.x + canvasWidth * 0.5 * s;
        m.ty = canvas.y + canvasHeight * 0.5 * s;
        return m;
    }

    class EffectRig {
    public:
        explicit EffectRig(uint32_t testFlags = 0)
            : passes(std::make_unique<TitlePassKit>(TitleArtCaps::select(SystemRender::getFeatures(), testFlags))) {}

        TitlePassKit& kit() { return *passes; }
        // the quality tier the next runs render for (S83 D11, D12)
        FxTier tier = FxTier::Desktop;

        // false at once when a program fails to build (the kit's error says which)
        bool warmUp() {
            const bool ran = GpuEnvironment::runFrames([this]() {
                if (!passes->error().empty()) return true;
                bool ready = true;
                for (int p = 1; p <= TITLE_PROGRAM_LAST; p++) {
                    const TitleProgram program = (TitleProgram)p;
                    ready = passes->prepare(program, titleProgramVariant(program, passes->caps())) && ready;
                }
                for (int p = 1; p <= tfx::PROGRAM_LAST; p++) {
                    const tfx::Program program = (tfx::Program)p;
                    ready = passes->prepare((TitleProgram)p, tfx::programVariant(program, passes->caps())) && ready;
                }
                return ready;
            });
            return ran && passes->error().empty();
        }

        TargetRef* upload(const LinearImage& image) {
            std::vector<uint16_t> halves(image.rgba.size());
            for (size_t i = 0; i < halves.size(); i++) halves[i] = floatToHalf(image.rgba[i]);
            auto texture = std::make_unique<TextureRender>();
            void* data[1] = {halves.data()};
            size_t size[1] = {halves.size() * sizeof(uint16_t)};
            if (!texture->createTexture("TitleFxGpu input", image.width, image.height, ColorFormat::RGBA16F, TextureType::TEXTURE_2D, 1, data, size,
                                        TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)) {
                return nullptr;
            }
            TargetRef* target = passes->acquire(passes->accumulatorKey(image.width, image.height));
            if (!target) return nullptr;
            TitlePass pass;
            pass.program = TitleProgram::Copy;
            pass.label = "upload";
            pass.textures[0].texture = texture.get();
            pass.output = target;
            pass.set(2, 1.0f);
            if (!passes->run(pass)) return nullptr;
            uploads.push_back(std::move(texture));
            return target;
        }

        bool read(TargetRef& target, LinearImage& out) {
            const TargetKey& key = target.key();
            TargetRef* source = &target;
            TargetRef* copy = nullptr;
            if (key.format != ColorFormat::RGBA16F) {
                // the RGBA8 path's SRGB8A8 decoded through a copy: what the next pass would sample
                TargetKey floatKey = key;
                floatKey.format = ColorFormat::RGBA16F;
                floatKey.filter = TextureFilter::NEAREST;
                copy = passes->acquire(floatKey);
                if (!copy) return false;
                TitlePass pass;
                pass.program = TitleProgram::Copy;
                pass.label = "readback copy";
                pass.textures[0].target = &target;
                pass.output = copy;
                pass.set(2, 1.0f);
                if (!passes->run(pass)) return false;
                source = copy;
            }
            SystemRender::commit();
            std::vector<uint8_t> bytes;
            const bool ok = readTextureRaw(*titleTargetTexture(*source), key.width, key.height, ColorFormat::RGBA16F, bytes);
            if (copy) passes->release(copy);
            if (!ok) return false;
            out.width = key.width;
            out.height = key.height;
            out.rgba.resize((size_t)key.width * key.height * 4);
            const uint16_t* halves = reinterpret_cast<const uint16_t*>(bytes.data());
            for (size_t i = 0; i < out.rgba.size(); i++) out.rgba[i] = halfToFloat(halves[i]);
            return true;
        }

        bool runEffect(const std::string& type, const Json& overrides, const LinearImage& input, float pxScale, LinearImage& out,
                       std::string& error, TitleRectI canvas = {}, float contextPxScale = -1.0f) {
            const EffectDef* def = findEffect(type);
            if (!def) {
                error = type + " is not registered";
                return false;
            }
            Json params = overrides.is_object() ? overrides : Json::object();
            NormalizeContext normalize;
            normalizeParams(def->params, params, "params", normalize);
            passes->clearError();
            TargetRef* in = upload(input);
            TargetRef* result = passes->acquire(passes->accumulatorKey(input.width, input.height));
            if (!in || !result) {
                error = passes->error();
                return false;
            }
            if (canvas.empty()) canvas = {0, 0, input.width, input.height};
            EffectContext ctx(*passes);
            ctx.width = input.width;
            ctx.height = input.height;
            ctx.pxScale = contextPxScale > 0.0f ? contextPxScale : pxScale;
            ctx.frame = canvas;
            const int canvasWidth = (int)std::lround(canvas.width / pxScale), canvasHeight = (int)std::lround(canvas.height / pxScale);
            ctx.documentToTarget = documentToTarget(canvasWidth, canvasHeight, pxScale, canvas);
            ctx.layerOrigin = ctx.documentToTarget.apply(TitleVec2{0.0, 0.0});
            ctx.layerId = "00000001";
            ctx.tier = tier;
            const ParamValues values(params, &def->globalLight, 120.0, 30.0);
            def->apply(ctx, *in, *result, values);
            const bool ok = passes->error().empty() && read(*result, out);
            if (!ok) error = passes->error().empty() ? "readback failed" : passes->error();
            passes->release(in);
            passes->release(result);
            return ok;
        }

        bool runGenerator(const std::string& type, const Json& overrides, int canvasWidth, int canvasHeight, float pxScale, LinearImage& out,
                          std::string& error) {
            const GeneratorDef* def = findGenerator(type);
            if (!def) {
                error = type + " is not registered";
                return false;
            }
            Json params = overrides.is_object() ? overrides : Json::object();
            NormalizeContext normalize;
            normalizeParams(def->params, params, "params", normalize);
            passes->clearError();
            const int w = (int)std::lround(canvasWidth * pxScale), h = (int)std::lround(canvasHeight * pxScale);
            TargetRef* target = passes->acquire(passes->accumulatorKey(w, h));
            if (!target) {
                error = passes->error();
                return false;
            }
            GeneratorContext ctx(*passes);
            ctx.width = w;
            ctx.height = h;
            ctx.pxScale = pxScale;
            ctx.frame = {0, 0, w, h};
            ctx.canvasWidth = canvasWidth;
            ctx.canvasHeight = canvasHeight;
            ctx.documentToTarget = documentToTarget(canvasWidth, canvasHeight, pxScale, ctx.frame);
            // the generator layer at the canvas centre, unturned: its space is document space
            ctx.layerToTarget = ctx.documentToTarget;
            ctx.layerId = "00000002";
            const ParamValues values(params, &def->globalLight, 120.0, 30.0);
            def->render(ctx, *target, values);
            const bool ok = passes->error().empty() && read(*target, out);
            if (!ok) error = passes->error().empty() ? "readback failed" : passes->error();
            passes->release(target);
            return ok;
        }

    private:
        std::unique_ptr<TitlePassKit> passes;
        std::vector<std::unique_ptr<TextureRender>> uploads;
    };

    std::filesystem::path goldenPath(const std::string& relative) {
        return std::filesystem::path(getGoldenDirectory()) / relative;
    }

    bool readImage(const std::filesystem::path& path, Rgba8Image& out) {
        return readPng(path.string(), out.rgba, out.width, out.height);
    }

    // the golden of `name` (golden/titleart/effects/<name>.png), premultiplied with S80 §6.12's
    // golden limits; JASPER_TEST_UPDATE_GOLDEN=1 writes it (glcore)
    struct Padded {
        LinearImage image;
        TitleRectI canvas;
    };

    Padded padded(const LinearImage& image, int border) {
        Padded p;
        p.image.width = image.width + 2 * border;
        p.image.height = image.height + 2 * border;
        p.image.rgba.assign((size_t)p.image.width * p.image.height * 4, 0.0f);
        for (int y = 0; y < image.height; y++) {
            std::copy_n(image.at(0, y), (size_t)image.width * 4, p.image.at(border, y + border));
        }
        p.canvas = {border, border, image.width, image.height};
        return p;
    }


    }

}
