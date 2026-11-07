// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What S83's generator tests share (docs/specs/S83-title-effects-generators.md §7): the preset
// files read with the JSON parser (they are YAML in its JSON-compatible form, Deviations), a
// generator preset's document (Pachislop's generatorProject: the standard title and the
// generator following it, above or below as the preset places it), a whole-document render
// through the renderer, and the image metrics of S80's goldens and AC12's parity (premultiplied
// 8-bit pixels; 4 x 4 block means and 16-bin histograms).

#pragma once

#include "TitleStandardTitle.h"

#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "shader/RuntimeShaderCompiler.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/render/TitleArtRenderer.h"

#include "ImageCompare.h"
#include "Readback.h"
#include "TestPaths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace jasperengine::titleart::test {

    // straight sRGB 8-bit RGBA, rows top first (a PNG's pixels)
    struct GenImage {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    inline std::filesystem::path repositoryRoot() {
        return std::filesystem::path(jasperengine::test::testPaths().sourceDir) / ".." / ".." / "..";
    }

    // editor/resources/titleart/fx-presets/<type>.yaml's preset list (empty when it has none)
    inline Json loadGeneratorPresets(const std::string& type) {
        std::ifstream in(repositoryRoot() / "editor" / "resources" / "titleart" / "fx-presets" / (type + ".yaml"));
        if (!in) return Json::array();
        std::stringstream text;
        text << in.rdbuf();
        Json file = Json::parse(text.str(), nullptr, false);
        if (!file.is_object() || !file.contains("presets") || !file["presets"].is_array()) return Json::array();
        return file["presets"];
    }

    // the preset's document: the standard title and the generator layer (following the title
    // when its definition can follow), placed above or below it as the preset says
    inline TitleArtDoc generatorPresetDocument(const std::string& type, const Json& preset, const Json& paramOverrides = Json::object()) {
        const GeneratorDef* def = findGenerator(type);
        Json params = preset.value("params", Json::object());
        for (auto it = paramOverrides.begin(); it != paramOverrides.end(); ++it) params[it.key()] = it.value();
        const Json look = preset.value("layer", Json::object());
        Json layer{{"id", "00000301"}, {"type", "generator"}, {"name", preset.value("name", std::string(type))},
                   {"generator", {{"type", type}, {"params", params}}}};
        layer["blend"] = look.value("blend", std::string("normal"));
        layer["opacity"] = look.value("opacity", 1.0);
        Json effects = Json::array();
        int n = 0;
        for (const Json& e : look.value("effects", Json::array())) {
            Json fx = e;
            char id[16];
            std::snprintf(id, sizeof(id), "%08x", 0x400 + n++);
            fx["id"] = id;
            fx["enabled"] = true;
            effects.push_back(fx);
        }
        layer["effects"] = effects;
        const bool follows = def && def->follow == FollowMode::Required;
        if (!follows) return standardDocument({layer});
        layer["follow"] = "00000001";
        Json title = standardTitleLayer();
        title["effects"] = Json::array();
        if (look.value("place", std::string("above")) == "below") return standardDocument({layer, title});
        return standardDocument({title, layer});
    }

    // a job run to the end frame by frame (its programs compile on the way)
    inline bool runGenJob(TitleRenderJob& job, std::string& error) {
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

    // the whole document at `scale` as straight sRGB bytes, through the renderer, its generators
    // drawing for `tier` (S83 D12)
    inline bool renderGenDocument(const TitleArtDoc& doc, float scale, GenImage& out, std::string& error, uint32_t testFlags = 0,
                                  std::vector<TitleProblem>* problems = nullptr, FxTier tier = FxTier::Desktop) {
        static TestAssetResolver assets;
        TitleArtRenderer renderer;
        RenderRequest request;
        request.doc = std::make_shared<const TitleArtDoc>(doc);
        request.pxScale = scale;
        request.output = TitleOutputFormat::Srgb8;
        request.assets = &assets;
        request.testFlags = testFlags;
        request.fxTier = tier;
        std::unique_ptr<TitleRenderJob> job = renderer.begin(request);
        if (!job || !runGenJob(*job, error)) return false;
        if (problems) *problems = job->problems();
        const TitleImage& image = job->image();
        TextureRender* texture = image.target.get() ? image.target.get()->colorTexture() : nullptr;
        if (!texture) {
            error = "the render has no image";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!jasperengine::test::readTextureRaw(*texture, image.width, image.height, image.format, bytes)) {
            error = "readback failed";
            return false;
        }
        out.width = image.width;
        out.height = image.height;
        out.rgba = std::move(bytes);
        return true;
    }

    inline double genPremul(const GenImage& image, size_t i, int c) {
        return c == 3 ? image.rgba[i + 3] : image.rgba[i + c] * image.rgba[i + 3] / 255.0;
    }

    struct GenPixelStats {
        int max = 0;
        double mean = 0.0;
        double fractionOver8 = 0.0;
        std::string text() const {
            char t[160];
            std::snprintf(t, sizeof(t), "max %d/255, mean %.3f/255, %.3f %% of px > 8/255", max, mean, fractionOver8 * 100.0);
            return t;
        }
    };

    // premultiplied, as S80's ImageDiff
    inline GenPixelStats compareGenImages(const GenImage& a, const GenImage& b) {
        GenPixelStats s;
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
                const double d = std::fabs(genPremul(a, i, c) - genPremul(b, i, c));
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

    struct GenBlockStats {
        double fractionOver = 0.0;
        double histogramDistance = 0.0;
        std::string text() const {
            char t[160];
            std::snprintf(t, sizeof(t), "%.2f %% of 4 x 4 blocks over the threshold, histogram distance %.2f %%", fractionOver * 100.0,
                          histogramDistance);
            return t;
        }
    };

    // S80 AC12's metric: 4 x 4 block means differing by more than `threshold` in any channel, and
    // the largest total-variation distance of the 16-bin channel histograms (premultiplied)
    inline GenBlockStats compareGenBlocks(const GenImage& a, const GenImage& b, int threshold = 24) {
        GenBlockStats s;
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
                            ma[c] += genPremul(a, i, c);
                            mb[c] += genPremul(b, i, c);
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
                ha[std::min(15, (int)(genPremul(a, i, c) / 16.0))] += 1.0;
                hb[std::min(15, (int)(genPremul(b, i, c) / 16.0))] += 1.0;
            }
            double tv = 0;
            for (int k = 0; k < 16; k++) tv += std::fabs(ha[k] - hb[k]);
            s.histogramDistance = std::max(s.histogramDistance, 50.0 * tv / (double)std::max<size_t>(1, pixels));
        }
        return s;
    }

    // the box of pixels with alpha >= threshold (empty image when none)
    inline GenImage trimGen(const GenImage& image, int threshold) {
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
        GenImage out;
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
    inline GenImage centredGen(const GenImage& image, int width, int height) {
        GenImage out;
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

}
