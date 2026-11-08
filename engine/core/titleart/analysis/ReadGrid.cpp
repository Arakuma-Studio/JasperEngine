// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/effects/b/common.ts (readGrid, gridDistance) and
// src/generators/particles.ts (readAlphaGrid) by SafEight, used with the author's permission.

#include "ReadGrid.h"

#include "render/FrameReadback.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtHash.h"
#include "titleart/generators/TitleGen.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    constexpr int GRID_ALIGN = 16;
    constexpr size_t GRID_CACHE = 64;

    struct GridCache {
        std::map<uint64_t, std::shared_ptr<const Grid>> grids;
        std::list<uint64_t> order;      // most recent first
        uint64_t readbacks = 0;
    };

    GridCache& cache() {
        static GridCache c;
        return c;
    }

    uint64_t specKey(uint64_t key, const GridSpec& s) {
        const double d[3] = {s.x0, s.y0, s.cellComp};
        const int i[5] = {s.w, s.h, s.maxTaps, s.ceilTaps ? 1 : 0, (int)s.packing};
        uint64_t h = hashCombine(key, fnv1a64(std::string_view(reinterpret_cast<const char*>(d), sizeof(d))));
        return hashCombine(h, fnv1a64(std::string_view(reinterpret_cast<const char*>(i), sizeof(i))));
    }

}

GridSpec analysis::frameGridSpec(const GeneratorContext& ctx, double cellComp) {
    // the whole render's half sizes in canvas px, in whole blocks of GRID_ALIGN cells each way
    const double halfW = (ctx.frame.empty() ? ctx.width : ctx.frame.width) / 2.0 / ctx.pxScale;
    const double halfH = (ctx.frame.empty() ? ctx.height : ctx.frame.height) / 2.0 / ctx.pxScale;
    const int nx = std::max(1, (int)std::ceil(halfW / cellComp / GRID_ALIGN - 1e-6)) * GRID_ALIGN;
    const int ny = std::max(1, (int)std::ceil(halfH / cellComp / GRID_ALIGN - 1e-6)) * GRID_ALIGN;
    GridSpec s;
    s.cellComp = cellComp;
    s.w = 2 * nx;
    s.h = 2 * ny;
    s.x0 = -nx * cellComp;
    s.y0 = -ny * cellComp;
    return s;
}

float analysis::decodeGridLuma(uint8_t hi, uint8_t lo) {
    return (float)(hi * 256 + lo) / 65535.0f * 4.0f;
}

void analysis::encodeGridLuma(float lum, uint8_t& hi, uint8_t& lo) {
    const int v = (int)std::lround(std::clamp(lum / 4.0f, 0.0f, 1.0f) * 65535.0f);
    hi = (uint8_t)(v >> 8);
    lo = (uint8_t)(v & 255);
}

std::shared_ptr<const Grid> analysis::readGrid(const GeneratorContext& ctx, const TargetRef& src, uint64_t key, const GridSpec& spec) {
    if (spec.w <= 0 || spec.h <= 0 || !src || !src.get()) return nullptr;
    GridCache& c = cache();
    const uint64_t k = key ? specKey(key, spec) : 0;
    if (k) {
        auto hit = c.grids.find(k);
        if (hit != c.grids.end()) {
            c.order.remove(k);
            c.order.push_front(k);
            return hit->second;
        }
    }
    TargetKey tk;
    tk.width = spec.w;
    tk.height = spec.h;
    tk.format = ColorFormat::RGBA;
    tk.filter = TextureFilter::NEAREST;
    TargetRef* out = ctx.runner.acquire(tk);
    if (!out) return nullptr;
    const double cellPx = spec.cellComp * ctx.pxScale;
    const int taps = std::min(spec.maxTaps, std::max(1, spec.ceilTaps ? (int)std::ceil(cellPx) : (int)std::lround(cellPx)));
    TitlePass pass = gen::genPass(ctx, gen::GenProgram::Grid, "analysis grid", *out, ctx.documentToTarget);
    pass.textures[0].target = &src;
    pass.set(6, (float)spec.x0, (float)spec.y0, (float)spec.cellComp, (float)taps);
    pass.set(7, (float)spec.w, (float)spec.h, spec.packing == GridPacking::Color ? 1.0f : 0.0f, 0.0f);
    pass.set(8, (float)src.key().width, (float)src.key().height);
    const bool drawn = ctx.runner.run(pass) && !ctx.runner.skippedPasses();
    std::vector<uint8_t> bytes;
    TextureRender* texture = drawn ? out->get()->colorTexture() : nullptr;
    const bool read = texture && FrameReadback::readTexture(*texture, spec.w, spec.h, 4, bytes);
    ctx.runner.release(out);
    if (!read) return nullptr;
    c.readbacks++;

    auto grid = std::make_shared<Grid>();
    grid->w = spec.w;
    grid->h = spec.h;
    grid->cellComp = spec.cellComp;
    grid->cell = cellPx;
    grid->x0 = spec.x0;
    grid->y0 = spec.y0;
    const size_t n = (size_t)spec.w * spec.h;
    grid->alpha.resize(n);
    if (spec.packing == GridPacking::Luma) {
        grid->lum.resize(n);
        grid->sat.resize(n);
    } else {
        grid->rgba.resize(n * 4);
    }
    for (int r = 0; r < spec.h; r++) {
        // the readback's rows run top first; the grid's bottom first (the Grid program wrote
        // grid row h-1-r into memory row r)
        const int j = spec.h - 1 - r;
        for (int i = 0; i < spec.w; i++) {
            const uint8_t* px = &bytes[((size_t)r * spec.w + i) * 4];
            const size_t at = (size_t)j * spec.w + i;
            if (spec.packing == GridPacking::Luma) {
                grid->alpha[at] = px[0] / 255.0f;
                grid->lum[at] = decodeGridLuma(px[1], px[2]);
                grid->sat[at] = px[3] / 255.0f;
            } else {
                const float a = px[3] / 255.0f;
                grid->alpha[at] = a;
                for (int ch = 0; ch < 3; ch++) grid->rgba[at * 4 + ch] = srgbToLinear(px[ch] / 255.0f) * a;
                grid->rgba[at * 4 + 3] = a;
            }
        }
    }
    if (k) {
        c.grids[k] = grid;
        c.order.push_front(k);
        while (c.order.size() > GRID_CACHE) {
            c.grids.erase(c.order.back());
            c.order.pop_back();
        }
    }
    return grid;
}

Grid analysis::gridFromImage(const std::vector<uint8_t>& rgba, int width, int height, const TitleRectI& frame, double scale, int canvasWidth,
                             int canvasHeight, double cellComp) {
    Grid g;
    if (width <= 0 || height <= 0 || scale <= 0.0 || rgba.size() < (size_t)width * height * 4) return g;
    const int nx = std::max(1, (int)std::ceil(canvasWidth / 2.0 / cellComp / GRID_ALIGN - 1e-6)) * GRID_ALIGN;
    const int ny = std::max(1, (int)std::ceil(canvasHeight / 2.0 / cellComp / GRID_ALIGN - 1e-6)) * GRID_ALIGN;
    g.w = 2 * nx;
    g.h = 2 * ny;
    g.cellComp = cellComp;
    g.cell = cellComp * scale;
    g.x0 = -nx * cellComp;
    g.y0 = -ny * cellComp;
    const size_t n = (size_t)g.w * g.h;
    g.alpha.assign(n, 0.0f);
    g.lum.assign(n, 0.0f);
    g.sat.assign(n, 0.0f);
    std::vector<float> satWeight(n, 0.0f);
    // each pixel adds its share of a cell's area (a cell holds (cell px)^2 pixels)
    const float share = (float)(1.0 / (g.cell * g.cell));
    for (int j = 0; j < height; j++) {
        // the pixel centre in canvas px about the centre, +y up
        const double cy = canvasHeight / 2.0 - (frame.y + (j + 0.5) / scale);
        const int gy = (int)std::floor((cy - g.y0) / cellComp);
        if (gy < 0 || gy >= g.h) continue;
        for (int i = 0; i < width; i++) {
            const double cx = frame.x + (i + 0.5) / scale - canvasWidth / 2.0;
            const int gx = (int)std::floor((cx - g.x0) / cellComp);
            if (gx < 0 || gx >= g.w) continue;
            const uint8_t* px = &rgba[((size_t)j * width + i) * 4];
            const float a = px[3] / 255.0f;
            if (a <= 0.0f) continue;
            const float r = srgbToLinear(px[0] / 255.0f), gr = srgbToLinear(px[1] / 255.0f), b = srgbToLinear(px[2] / 255.0f);
            const size_t at = (size_t)gy * g.w + gx;
            g.alpha[at] += a * share;
            g.lum[at] += (0.2126f * r + 0.7152f * gr + 0.0722f * b) * a * share;
            const float mx = std::max(r, std::max(gr, b));
            if (mx > 1e-5f) g.sat[at] += (mx - std::min(r, std::min(gr, b))) / mx * a * share;
            satWeight[at] += a * share;
        }
    }
    for (size_t i = 0; i < n; i++) {
        g.alpha[i] = std::min(1.0f, g.alpha[i]);
        g.sat[i] = satWeight[i] > 0.0f ? std::min(1.0f, g.sat[i] / satWeight[i]) : 0.0f;
    }
    return g;
}

uint64_t analysis::gridReadbacks() {
    return cache().readbacks;
}

void analysis::clearGridCache() {
    GridCache& c = cache();
    c.grids.clear();
    c.order.clear();
}

std::vector<float> analysis::gridDistance(int w, int h, const std::vector<float>& alpha) {
    std::vector<float> d((size_t)w * h);
    const float INF = 1e9f;
    for (size_t i = 0; i < d.size(); i++) d[i] = alpha[i] >= 0.5f ? 0.0f : INF;
    const float D = std::sqrt(2.0f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t i = (size_t)y * w + x;
            float v = d[i];
            if (x > 0) v = std::min(v, d[i - 1] + 1.0f);
            if (y > 0) {
                v = std::min(v, d[i - w] + 1.0f);
                if (x > 0) v = std::min(v, d[i - w - 1] + D);
                if (x < w - 1) v = std::min(v, d[i - w + 1] + D);
            }
            d[i] = v;
        }
    }
    for (int y = h - 1; y >= 0; y--) {
        for (int x = w - 1; x >= 0; x--) {
            const size_t i = (size_t)y * w + x;
            float v = d[i];
            if (x < w - 1) v = std::min(v, d[i + 1] + 1.0f);
            if (y < h - 1) {
                v = std::min(v, d[i + w] + 1.0f);
                if (x < w - 1) v = std::min(v, d[i + w + 1] + D);
                if (x > 0) v = std::min(v, d[i + w - 1] + D);
            }
            d[i] = v;
        }
    }
    return d;
}
