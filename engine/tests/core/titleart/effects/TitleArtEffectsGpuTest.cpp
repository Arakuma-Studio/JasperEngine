// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The ten core effects and the two generators on the GPU (docs/specs/S80-title-art-core.md R30,
// R12, R17, R38, §6.5, AC12, AC17, AC18, AC24). Each definition runs as the renderer runs it, one
// stack entry at a time: its `apply` (or `render`) with a constructed EffectContext around a
// TitlePassKit, on an input uploaded into a pooled target.
// - TitleArtEffectsGpu.Goldens: every effect at defaults and with two settings on the standard
//   title (§7: 確定, Dela Gothic One, the classic style, rendered by the renderer with the module's
//   text and geometry), every generator alone, against golden/titleart/effects/ (written on
//   glcore, Vulkan matches), premultiplied with the golden limits of §6.12.
// - TitleArtParityGpu: the effects with a Pachislop reference (tests/golden/refs/effect, copied to
//   golden/titleart/pachislop/) at defaults on the standard title, each on the title layer
//   through the renderer at scale 0.25 and trimmed (Pachislop's GOLDEN_STILL), and the Pattern
//   Fill presets against theirs, with AC12's block and histogram metric.
// - TitleArtEffectsGpu.ScaleInvariance: px_scale 0.5 equals px_scale 1 box-filtered down on an
//   analytic fixture (an "F", a ring and a disc drawn exactly at both scales), and an effect that
//   ignores px_scale fails the same check (red control).
// - TitleArtEffectsGpu.Deterministic: two renders are byte-identical.
// - TitleArtFallbackGpu: the RGBA8 path (TITLE_ART_TEST_NO_FLOAT_TARGETS) agrees with the float
//   path within each definition's declared class (F0: §6.3's tolerance; F1: S83 §6.5's).

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "texture/TexturePrepare.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectCatalog.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TitleFx.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePassKit.h"
#include "shader/RuntimeShaderCompiler.h"

#include "../support/TitleStandardTitle.h"

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

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::test;
namespace standard = jasperengine::titleart::test;

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

        // false at once when a program fails to build (the kit's error says which)
        bool warmUp() {
            const bool ran = GpuEnvironment::runFrames([this]() {
                if (!passes->error().empty()) return true;
                bool ready = true;
                for (int p = 1; p <= TITLE_PROGRAM_LAST; p++) {
                    const TitleProgram program = (TitleProgram)p;
                    ready = passes->prepare(program, titleProgramVariant(program, passes->caps())) && ready;
                }
                // S83's generators run here too (every registered generator is a case)
                for (gen::GenProgram program : gen::genPrograms()) {
                    ready = passes->prepare((TitleProgram)(uint8_t)program, gen::genProgramVariant(program, passes->caps())) && ready;
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
    void expectGolden(const std::string& name, const Rgba8Image& image) {
        const std::filesystem::path path = goldenPath("titleart/effects/" + name + ".png");
        if (isGoldenUpdateMode()) {
            if (std::string(HeadlessContext::getBackendName()) == "glcore") {
                std::filesystem::create_directories(path.parent_path());
                ASSERT_TRUE(writePng(path.string(), image.rgba, image.width, image.height)) << path.string();
            }
            return;
        }
        Rgba8Image golden;
        ASSERT_TRUE(readImage(path, golden)) << "no golden " << path.string() << " (JASPER_TEST_UPDATE_GOLDEN=1 writes it)";
        const PixelStats stats = compareImages(golden, image);
        const bool ok = stats.max <= 48 && stats.mean <= 0.25 && stats.fractionOver8 <= 0.001;
        if (!ok) {
            const std::filesystem::path actual = std::filesystem::path(getOutputDirectory()) / ("titleart-" + name + ".actual.png");
            std::filesystem::create_directories(actual.parent_path());
            writePng(actual.string(), image.rgba, image.width, image.height);
        }
        EXPECT_TRUE(ok) << name << ": " << stats.text() << (golden.width != image.width || golden.height != image.height ? " (size differs)" : "");
    }

    struct Case {
        std::string name;       // the golden's name: "<type>" at defaults, "<type>-<variant>"
        std::string type;
        Json params;
    };

    // every effect at defaults and with two settings (the second exercising the other technique,
    // mode or fill path)
    std::vector<Case> effectCases() {
        std::vector<Case> cases;
        for (const EffectDef* d : TitleEffectRegistry::get().effects()) cases.push_back({d->type, d->type, Json::object()});
        auto add = [&](const char* name, const char* type, Json params) { cases.push_back({name, type, std::move(params)}); };
        add("outer_glow-precise", "outer_glow", Json{{"technique", "precise"}, {"spread", 0.4}, {"size", 24}});
        add("outer_glow-add-gradient", "outer_glow", Json{{"mode", "add"}, {"fill", "gradient"}, {"angle", 45}, {"intensity", 2.5}});
        add("inner_glow-precise-center", "inner_glow", Json{{"technique", "precise"}, {"source", "center"}, {"color", "#ff2040"}});
        add("inner_glow-distance", "inner_glow", Json{{"fill", "distance"}, {"mode", "add"}, {"size", 10}});
        add("drop_shadow-colored", "drop_shadow", Json{{"use_global_light", false}, {"angle", 135}, {"distance", 20}, {"color", "#301060"}});
        add("drop_shadow-hard", "drop_shadow", Json{{"use_global_light", false}, {"angle", -30}, {"size", 2}, {"spread", 0.8}, {"opacity", 1}});
        add("inner_shadow-choke", "inner_shadow", Json{{"use_global_light", false}, {"angle", -45}, {"distance", 12}, {"choke", 0.4}});
        add("inner_shadow-blue", "inner_shadow", Json{{"color", "#0030ff"}, {"size", 20}, {"opacity", 1}});
        add("outline-wide", "outline", Json{{"width", 6}, {"color", "#ffffff"}});
        add("outline-soft-stack", "outline", Json{{"width", 3}, {"softness", 0.8}, {"follow", "stack"}, {"color", "#ff0000"}});
        add("color_overlay-multiply", "color_overlay", Json{{"color", "#20a0ff"}, {"opacity", 0.7}, {"mode", "multiply"}});
        add("color_overlay-color", "color_overlay", Json{{"color", "#20ff60"}, {"mode", "color"}});
        add("gradient_overlay-radial-mirror", "gradient_overlay", Json{{"style", "radial"}, {"wrap", "mirror"}, {"scale", 0.5}});
        add("gradient_overlay-sweep-light", "gradient_overlay", Json{{"style", "sweep"}, {"position", 30}, {"length", 60}, {"mode", "light"}});
        add("pattern_overlay-zebra", "pattern_overlay", Json{{"pattern", "zebra"}, {"scale", 30}, {"angle", 30}});
        add("pattern_overlay-checker-canvas", "pattern_overlay",
            Json{{"pattern", "checker"}, {"color2", "#ff0000"}, {"align", "canvas"}, {"mode", "multiply"}, {"scale", 16}});
        add("gaussian_blur-large", "gaussian_blur", Json{{"radius", 30}});
        add("gaussian_blur-horizontal-edges", "gaussian_blur", Json{{"radius", 12}, {"scale_x", 200}, {"scale_y", 0}, {"repeat_edges", false}});
        add("adjust-levels", "adjust",
            Json{{"adjustment", Json{{"type", "levels"}, {"channels", Json{{"rgb", Json{{"in_white", 180}, {"gamma", 1.4}}}}}}}});
        add("adjust-gradient_map", "adjust", Json{{"adjustment", Json{{"type", "gradient_map"}}}});
        return cases;
    }

    std::vector<Case> generatorCases() {
        std::vector<Case> cases;
        for (const GeneratorDef* d : TitleEffectRegistry::get().generators()) cases.push_back({d->type, d->type, Json::object()});
        cases.push_back({"fill-linear", "fill", Json{{"kind", "linear"}, {"angle", 45}}});
        cases.push_back({"fill-radial", "fill", Json{{"kind", "radial"}, {"scale", 0.7}, {"offset", 0.1}}});
        cases.push_back({"pattern_fill-tilt", "pattern_fill", Json{{"kind", "seigaiha"}, {"tilt", 50}, {"scale", 60}}});
        cases.push_back({"pattern_fill-leopard", "pattern_fill", Json{{"kind", "leopard"}, {"rotation", 20}, {"bg_opacity", 40}}});
        return cases;
    }

    // the image inside a transparent border: what an effect draws past the ink (glows, shadows,
    // strokes, blurs) has room there, and an ink-bound effect must leave it empty
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

    class TitleArtEffectsGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            rig = std::make_unique<EffectRig>();
            ASSERT_TRUE(rig->warmUp()) << "the title art programs did not compile: " << rig->kit().error();
        }
        void TearDown() override { rig.reset(); }
        std::unique_ptr<EffectRig> rig;
    };

}

TEST_F(TitleArtEffectsGpu, Goldens) {
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, 32);
    for (const Case& c : effectCases()) {
        SCOPED_TRACE(c.name);
        LinearImage out;
        std::string error;
        ASSERT_TRUE(rig->runEffect(c.type, c.params, input.image, 1.0f, out, error, input.canvas)) << error;
        expectGolden(c.name, toRgba8(out));
        // an ink-bound effect draws nothing in the border
        float leak = 0.0f;
        for (int y = 0; y < out.height; y++) {
            for (int x = 0; x < out.width; x++) {
                const bool inside = x >= input.canvas.x && y >= input.canvas.y && x < input.canvas.x + input.canvas.width &&
                                    y < input.canvas.y + input.canvas.height;
                if (!inside) leak = std::max(leak, out.at(x, y)[3]);
            }
        }
        if (c.type != "gaussian_blur" && c.type != "outer_glow" && c.type != "drop_shadow" && c.type != "outline") {
            EXPECT_LT(leak, 1.0f / 255.0f) << "an ink-bound effect drew past the ink";
        }
    }
    for (const Case& c : generatorCases()) {
        SCOPED_TRACE(c.name);
        LinearImage out;
        std::string error;
        ASSERT_TRUE(rig->runGenerator(c.type, c.params, GENERATOR_WIDTH, GENERATOR_HEIGHT, 1.0f, out, error)) << error;
        expectGolden(c.name, toRgba8(out));
    }
}

// a changed setting moves the image well past the golden limits (the goldens' red control)
TEST_F(TitleArtEffectsGpu, GoldensSeeASettingChange) {
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, 32);
    LinearImage base, moved;
    std::string error;
    ASSERT_TRUE(rig->runEffect("drop_shadow", Json::object(), input.image, 1.0f, base, error, input.canvas)) << error;
    ASSERT_TRUE(rig->runEffect("drop_shadow", Json{{"distance", 13}}, input.image, 1.0f, moved, error, input.canvas)) << error;
    const PixelStats stats = compareImages(toRgba8(base), toRgba8(moved));
    EXPECT_TRUE(stats.max > 48 || stats.mean > 0.25 || stats.fractionOver8 > 0.001) << "3 px more distance passes the golden: " << stats.text();
}

// R17: two renders of one effect are byte-identical
TEST_F(TitleArtEffectsGpu, Deterministic) {
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const LinearImage input = downsample(title, 2);
    for (const Case& c : effectCases()) {
        SCOPED_TRACE(c.name);
        LinearImage a, b;
        std::string error;
        ASSERT_TRUE(rig->runEffect(c.type, c.params, input, 0.5f, a, error)) << error;
        ASSERT_TRUE(rig->runEffect(c.type, c.params, input, 0.5f, b, error)) << error;
        EXPECT_TRUE(a.rgba == b.rgba) << "two renders differ";
    }
}

// decision 5: effect pixel params scale with px_scale, so a render at 0.5 is the render at 1
// filtered down (on the analytic fixture, drawn exactly at both scales)
TEST_F(TitleArtEffectsGpu, ScaleInvariance) {
    const Padded full = padded(fixture(1.0f), 32);
    const Padded half = padded(fixture(0.5f), 16);
    auto check = [&](const Case& c, float halfContextScale, PixelStats& stats) {
        LinearImage a, b;
        std::string error;
        if (!rig->runEffect(c.type, c.params, full.image, 1.0f, a, error, full.canvas)) return error;
        if (!rig->runEffect(c.type, c.params, half.image, 0.5f, b, error, half.canvas, halfContextScale)) return error;
        stats = compareImages(toRgba8(downsample(a, 2)), toRgba8(b));
        return std::string();
    };
    for (const Case& c : effectCases()) {
        SCOPED_TRACE(c.name);
        PixelStats stats;
        const std::string error = check(c, -1.0f, stats);
        ASSERT_TRUE(error.empty()) << error;
        EXPECT_LE(stats.mean, 1.5) << stats.text();
        EXPECT_LE(stats.fractionOver8, 0.03) << stats.text();
    }
    // red control: a drop shadow that ignored px_scale (its distance and blur in output px) fails
    PixelStats broken;
    ASSERT_TRUE(check({"drop_shadow", "drop_shadow", Json::object()}, 1.0f, broken).empty());
    EXPECT_TRUE(broken.mean > 1.5 || broken.fractionOver8 > 0.03) << "ignoring px_scale passed: " << broken.text();
}

// AC17 with the classes the definitions declare: the RGBA8 path agrees with the float path
TEST_F(TitleArtEffectsGpu, FallbackAgreesWithinItsClass) {
    EffectRig fallback(TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F);
    ASSERT_FALSE(fallback.kit().caps().floatPath);
    ASSERT_EQ(fallback.kit().caps().format(TitleBuffer::Seeds).encoding, TitleEncoding::Fixed12_4);
    ASSERT_EQ(fallback.kit().caps().format(TitleBuffer::Distance).encoding, TitleEncoding::Fixed16);
    ASSERT_TRUE(fallback.warmUp()) << fallback.kit().error();
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, 32);
    for (const Case& c : effectCases()) {
        SCOPED_TRACE(c.name);
        LinearImage f, e;
        std::string error;
        ASSERT_TRUE(rig->runEffect(c.type, c.params, input.image, 1.0f, f, error, input.canvas)) << error;
        ASSERT_TRUE(fallback.runEffect(c.type, c.params, input.image, 1.0f, e, error, input.canvas)) << error;
        const PixelStats stats = compareImages(toRgba8(f), toRgba8(e));
        const FallbackClass cls = findEffect(c.type)->fallback;
        // F0: S80 §6.3; F1: S83 §6.5 (multi-pass blurs and accumulations)
        const double maxMean = cls == FallbackClass::F0 ? 1.0 : 1.5;
        const int maxDiff = cls == FallbackClass::F0 ? 48 : 64;
        const double maxOver = cls == FallbackClass::F0 ? 0.005 : 0.02;
        EXPECT_LE(stats.mean, maxMean) << fallbackClassName(cls) << ": " << stats.text();
        EXPECT_LE(stats.max, maxDiff) << fallbackClassName(cls) << ": " << stats.text();
        EXPECT_LE(stats.fractionOver8, maxOver) << fallbackClassName(cls) << ": " << stats.text();
    }
    for (const Case& c : generatorCases()) {
        SCOPED_TRACE(c.name);
        LinearImage f, e;
        std::string error;
        ASSERT_TRUE(rig->runGenerator(c.type, c.params, GENERATOR_WIDTH, GENERATOR_HEIGHT, 1.0f, f, error)) << error;
        ASSERT_TRUE(fallback.runGenerator(c.type, c.params, GENERATOR_WIDTH, GENERATOR_HEIGHT, 1.0f, e, error)) << error;
        const PixelStats stats = compareImages(toRgba8(f), toRgba8(e));
        // S83 §6.5's classes: F0 as the effects', F1 for accumulations, F2 where light above 1 shapes the look
        const FallbackClass cls = findGenerator(c.type)->fallback;
        const double maxMean = cls == FallbackClass::F0 ? 1.0 : cls == FallbackClass::F1 ? 1.5 : 3.0;
        const int maxDiff = cls == FallbackClass::F0 ? 48 : cls == FallbackClass::F1 ? 64 : 96;
        const double maxOver = cls == FallbackClass::F0 ? 0.005 : cls == FallbackClass::F1 ? 0.02 : 0.05;
        EXPECT_LE(stats.mean, maxMean) << fallbackClassName(cls) << ": " << stats.text();
        EXPECT_LE(stats.max, maxDiff) << fallbackClassName(cls) << ": " << stats.text();
        EXPECT_LE(stats.fractionOver8, maxOver) << fallbackClassName(cls) << ": " << stats.text();
    }
}

// R27: a bounded distance field shades exactly as the unbounded one within the reach
TEST_F(TitleArtEffectsGpu, BoundedDistanceFieldMatchesTheUnbounded) {
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, 32);
    LinearImage bounded, unbounded;
    std::string error;
    ASSERT_TRUE(rig->runEffect("outline", Json{{"width", 6}}, input.image, 1.0f, bounded, error, input.canvas)) << error;
    // the same outline through an unbounded field: a reach past the whole target
    const EffectDef* def = findEffect("outline");
    EffectDef unboundedOutline = *def;
    unboundedOutline.type = "test_outline_unbounded";
    unboundedOutline.label = "Test Outline Unbounded";
    unboundedOutline.apply = [](EffectContext& ctx, const TargetRef& in, TargetRef& out, const ParamValues& p) {
        fx::Scratch field = fx::distanceField(ctx, in, (float)p.number("threshold"), 0.0f);
        ASSERT_TRUE(field);
        TitlePass pass = fx::makePass(ctx, TitleProgram::Outline, "outline", out);
        pass.textures[0].target = &in;
        pass.textures[1].target = &in;
        pass.raw[0].target = field.get();
        const TitleColor c = fx::linearOf(p.color("color"));
        pass.set(2, c.r, c.g, c.b, (float)p.number("width") * ctx.pxScale);
        pass.set(3, (float)p.number("softness"), (float)p.number("opacity"), 1.0f, 0.0f);
        ctx.runner.run(pass);
    };
    ASSERT_TRUE(TitleEffectRegistry::get().add(unboundedOutline));
    const bool ran = rig->runEffect("test_outline_unbounded", Json{{"width", 6}}, input.image, 1.0f, unbounded, error, input.canvas);
    TitleEffectRegistry::get().removeEffect("test_outline_unbounded");
    ASSERT_TRUE(ran) << error;
    const PixelStats stats = compareImages(toRgba8(bounded), toRgba8(unbounded));
    EXPECT_LE(stats.max, 1) << stats.text();
}

// ---- Pachislop parity (AC12) ----

namespace {

    void expectParity(const char* reference, const Rgba8Image& rendered) {
        Rgba8Image ref;
        ASSERT_TRUE(readImage(goldenPath(std::string("titleart/pachislop/") + reference + ".png"), ref)) << reference;
        // Pachislop's goldens are trimmed (alpha >= 8, no padding): ours the same, centred on theirs
        const Rgba8Image ours = centred(trim(rendered, 8), ref.width, ref.height);
        const BlockStats stats = compareBlocks(ref, ours);
        const bool ok = stats.fractionOver <= 0.03 && stats.histogramDistance <= 6.0;
        if (!ok || isGoldenUpdateMode()) {
            const std::filesystem::path actual = std::filesystem::path(getOutputDirectory()) / ("titleart-parity-" + std::string(reference).substr(std::string(reference).find('/') + 1) + ".png");
            std::filesystem::create_directories(actual.parent_path());
            writePng(actual.string(), ours.rgba, ours.width, ours.height);
        }
        std::printf("[parity] %s: %s\n", reference, stats.text().c_str());
        EXPECT_TRUE(ok) << reference << ": " << stats.text();
    }

    class TitleArtParityGpu : public TitleArtEffectsGpu {};

}

TEST_F(TitleArtParityGpu, Effects) {
    // each effect at its defaults on the standard title layer, the whole document through the
    // renderer at 0.25 (Pachislop's harness: the standard title, one effect, GOLDEN_STILL)
    const std::map<std::string, const char*> references = {
        {"outer_glow", "effect/outerGlow"}, {"inner_glow", "effect/innerGlow"}, {"drop_shadow", "effect/dropShadow"},
        {"inner_shadow", "effect/innerShadow"}, {"outline", "effect/keyline"}, {"gradient_overlay", "effect/gradientOverlay"},
        {"gaussian_blur", "effect/gaussianBlur"}, {"adjust", "effect/adjust"}};
    std::string error;
    for (const auto& [type, reference] : references) {
        SCOPED_TRACE(type);
        Rgba8Image out;
        ASSERT_TRUE(renderDocument(standard::standardTitleDocument({standard::standardEffect("00000201", type.c_str())}), 0.25f, TitleOutputFormat::Srgb8, nullptr,
                                   &out, error))
            << error;
        expectParity(reference, out);
    }
    // red control: the metric tells effects apart (Outer Glow is not Drop Shadow's reference)
    Rgba8Image glow;
    ASSERT_TRUE(renderDocument(standard::standardTitleDocument({standard::standardEffect("00000201", "outer_glow")}), 0.25f, TitleOutputFormat::Srgb8, nullptr, &glow,
                               error))
        << error;
    Rgba8Image dropRef;
    ASSERT_TRUE(readImage(goldenPath("titleart/pachislop/effect/dropShadow.png"), dropRef));
    const BlockStats wrong = compareBlocks(dropRef, centred(trim(glow, 8), dropRef.width, dropRef.height));
    EXPECT_TRUE(wrong.fractionOver > 0.03 || wrong.histogramDistance > 6.0) << "the parity metric passes the wrong effect: " << wrong.text();
}

// Pachislop's Pattern Fill presets alone on its 1920 x 1080 canvas at 0.25
TEST_F(TitleArtParityGpu, PatternFillPresets) {
    const std::vector<std::pair<const char*, Json>> presets = {
        {"zebra", Json::object()},
        {"giraffe", Json{{"kind", "giraffe"}, {"color", "#7a3f16"}, {"background", "#f0d39a"}, {"scale", 160}}},
        {"leopard", Json{{"kind", "leopard"}, {"color", "#1e1208"}, {"color2", "#b5621c"}, {"background", "#e9b860"}, {"scale", 90}}},
        {"tiger", Json{{"kind", "tiger"}, {"color", "#140c06"}, {"background", "#f07f12"}, {"scale", 75}, {"rotation", 80}}},
        {"checker", Json{{"kind", "checker"}, {"color", "#111111"}, {"color2", "#f5f5f5"}, {"scale", 80}}},
        {"hazard", Json{{"kind", "hazard"}, {"color", "#141414"}, {"color2", "#ffd200"}, {"scale", 120}}},
        {"seigaiha", Json{{"kind", "seigaiha"}, {"color", "#e9c66b"}, {"background", "#0f2350"}, {"scale", 140}}},
        {"asanoha", Json{{"kind", "asanoha"}, {"color", "#ffffff"}, {"background", "#b3122e"}, {"scale", 160}}},
        {"shippo", Json{{"kind", "shippo"}, {"color", "#f1cf6e"}, {"background", "#3b1258"}, {"scale", 120}}},
    };
    for (const auto& [name, params] : presets) {
        SCOPED_TRACE(name);
        LinearImage out;
        std::string error;
        ASSERT_TRUE(rig->runGenerator("pattern_fill", params, 1920, 1080, 0.25f, out, error)) << error;
        expectParity((std::string("generator/patternFill-") + name).c_str(), toRgba8(out));
    }
}
