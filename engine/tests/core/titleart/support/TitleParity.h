// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the Pachislop parity tests share (docs/specs/S80-title-art-core.md AC12): a document
// rendered whole by the renderer with the module's own text and geometry and the test fonts,
// Pachislop's GOLDEN_STILL framing (tests/harness/main.ts: scale 0.25, trimmed at alpha 8, no
// padding) centred on a reference, and AC12's loose structural metric. The effect cases
// (effects/TitleArtEffectsGpuTest.cpp) and the scenes (render/TitleArtScenesParityGpuTest.cpp)
// use it; the references are golden/titleart/pachislop/ (PROVENANCE.md).

#pragma once

#include "TitleStandardTitle.h"

#include "ImageCompare.h"
#include "Readback.h"

#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "shader/RuntimeShaderCompiler.h"
#include "texture/TexturePrepare.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/render/TitleArtRenderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart::test::parity {

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

    inline uint8_t toByte(double v) {
        return (uint8_t)std::lround(std::min(1.0, std::max(0.0, v)) * 255.0);
    }

    inline Rgba8Image toRgba8(const LinearImage& image) {
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

    inline Rgba8Image trim(const Rgba8Image& image, int threshold) {
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
    inline Rgba8Image centred(const Rgba8Image& image, int width, int height) {
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

    inline double premul(const Rgba8Image& image, size_t i, int c) {
        return c == 3 ? image.rgba[i + 3] : image.rgba[i + c] * image.rgba[i + 3] / 255.0;
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
    inline BlockStats compareBlocks(const Rgba8Image& a, const Rgba8Image& b, int threshold = 24) {
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

    // AC12's limits: at most 3 % of the blocks over, a histogram distance of at most 6 %
    inline bool withinParity(const BlockStats& s) {
        return s.fractionOver <= 0.03 && s.histogramDistance <= 6.0;
    }

    // ---- rendering ----

    // a job run to the end, frame by frame (its programs compile on the way)
    inline bool runJob(TitleRenderJob& job, std::string& error) {
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
    inline bool renderDocument(const TitleArtDoc& doc, float scale, TitleOutputFormat format, LinearImage* linear, Rgba8Image* srgb,
                               std::string& error) {
        static TestAssetResolver assets;
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
            if (!::jasperengine::test::readTextureRaw(*texture, image.width, image.height, image.format, bytes)) {
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
            if (!::jasperengine::test::readTexture(*texture, image.width, image.height, bytes)) {
                error = "readback failed";
                return false;
            }
            srgb->width = image.width;
            srgb->height = image.height;
            srgb->rgba = std::move(bytes);
        }
        return true;
    }

    // ---- references ----

    inline std::filesystem::path goldenPath(const std::string& relative) {
        return std::filesystem::path(::jasperengine::test::getGoldenDirectory()) / relative;
    }

    inline bool readImage(const std::filesystem::path& path, Rgba8Image& out) {
        return ::jasperengine::test::readPng(path.string(), out.rgba, out.width, out.height);
    }

    // `rendered` trimmed and centred on Pachislop's reference `reference`
    // (golden/titleart/pachislop/<reference>.png), held to AC12's metric; a failing render (or
    // any, with JASPER_TEST_UPDATE_GOLDEN=1) is written to the output directory to look at
    inline void expectParity(const char* reference, const Rgba8Image& rendered) {
        Rgba8Image ref;
        ASSERT_TRUE(readImage(goldenPath(std::string("titleart/pachislop/") + reference + ".png"), ref)) << reference;
        // Pachislop's goldens are trimmed (alpha >= 8, no padding): ours the same, centred on theirs
        const Rgba8Image ours = centred(trim(rendered, 8), ref.width, ref.height);
        const BlockStats stats = compareBlocks(ref, ours);
        const bool ok = withinParity(stats);
        if (!ok || ::jasperengine::test::isGoldenUpdateMode()) {
            const std::filesystem::path actual = std::filesystem::path(::jasperengine::test::getOutputDirectory()) /
                                                 ("titleart-parity-" + std::string(reference).substr(std::string(reference).find('/') + 1) + ".png");
            std::filesystem::create_directories(actual.parent_path());
            ::jasperengine::test::writePng(actual.string(), ours.rgba, ours.width, ours.height);
        }
        std::printf("[parity] %s: %s\n", reference, stats.text().c_str());
        EXPECT_TRUE(ok) << reference << ": " << stats.text();
    }

}
