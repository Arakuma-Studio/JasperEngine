// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleGenMobile.h"

#include "titleart/TitleArtHash.h"
#include "titleart/generators/TitleGen.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::gen;

namespace {

    TitleAffine scaledRows(const TitleAffine& m, double sx, double sy) {
        TitleAffine r = m;
        r.a *= sx;
        r.b *= sx;
        r.tx *= sx;
        r.c *= sy;
        r.d *= sy;
        r.ty *= sy;
        return r;
    }

    // the noise texture's size, its period in texels and its octaves' lattice cells per period
    constexpr int NOISE_SIZE = 256;
    constexpr double NOISE_PERIOD = 255.0;
    constexpr int NOISE_OCTAVES = 4;
    constexpr int NOISE_BASE_CELLS = 16;

    double smooth(double t) {
        return t * t * (3.0 - 2.0 * t);
    }

    // one channel: four octaves of value noise on lattices of 16, 32, 64 and 128 cells per
    // period (amplitudes halving), normalised to 0..1 with mean 0.5, and wrapping at the period
    std::vector<double> noiseChannel(int channel) {
        std::vector<double> out((size_t)NOISE_SIZE * NOISE_SIZE, 0.0);
        double amp = 0.5, total = 0.0;
        for (int o = 0; o < NOISE_OCTAVES; o++) {
            const int cells = NOISE_BASE_CELLS << o;
            std::vector<double> lattice((size_t)cells * cells);
            for (int j = 0; j < cells; j++) {
                for (int i = 0; i < cells; i++) lattice[(size_t)j * cells + i] = hashN({(double)i, (double)j, (double)o, (double)channel});
            }
            auto at = [&](int i, int j) { return lattice[(size_t)(j % cells) * cells + (size_t)(i % cells)]; };
            for (int y = 0; y < NOISE_SIZE; y++) {
                const double v = y * cells / NOISE_PERIOD;
                const int j = (int)std::floor(v);
                const double fy = smooth(v - j);
                for (int x = 0; x < NOISE_SIZE; x++) {
                    const double u = x * cells / NOISE_PERIOD;
                    const int i = (int)std::floor(u);
                    const double fx = smooth(u - i);
                    const double top = at(i, j) + (at(i + 1, j) - at(i, j)) * fx;
                    const double bottom = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * fx;
                    out[(size_t)y * NOISE_SIZE + x] += amp * (top + (bottom - top) * fy);
                }
            }
            total += amp;
            amp *= 0.5;
        }
        for (double& v : out) v /= total;
        return out;
    }

}

GeneratorContext gen::reducedContext(const GeneratorContext& ctx, int divisor) {
    const int d = std::max(1, divisor);
    GeneratorContext r(ctx);
    r.width = std::max(1, (ctx.width + d - 1) / d);
    r.height = std::max(1, (ctx.height + d - 1) / d);
    const double sx = (double)r.width / std::max(1, ctx.width), sy = (double)r.height / std::max(1, ctx.height);
    r.pxScale = (float)(ctx.pxScale * 0.5 * (sx + sy));
    const int x0 = (int)std::lround(ctx.frame.x * sx), y0 = (int)std::lround(ctx.frame.y * sy);
    const int x1 = (int)std::lround((ctx.frame.x + ctx.frame.width) * sx), y1 = (int)std::lround((ctx.frame.y + ctx.frame.height) * sy);
    r.frame = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    r.documentToTarget = scaledRows(ctx.documentToTarget, sx, sy);
    r.layerToTarget = scaledRows(ctx.layerToTarget, sx, sy);
    r.follow = nullptr;
    r.followContent = nullptr;
    // the reduced inputs are other pixels than the full ones the keys stand for
    const uint64_t reduced = fnv1a64("reduced") + (uint64_t)d;
    r.followKey = ctx.followKey ? hashCombine(ctx.followKey, reduced) : 0;
    r.followContentKey = ctx.followContentKey ? hashCombine(ctx.followContentKey, reduced) : 0;
    r.usesCanvasPosition = false;
    r.problems.clear();
    return r;
}

void gen::drawReduced(GeneratorContext& ctx, TargetRef& target, int divisor, const std::function<void(GeneratorContext& reduced, TargetRef& out)>& draw) {
    GeneratorContext reduced = reducedContext(ctx, divisor);
    EffectContext small = effectContext(reduced);
    fx::Scratch content;
    if (ctx.followContent) {
        content = fx::scratch(small);
        if (!content || !fx::copy(small, *ctx.followContent, *content)) return;
        reduced.followContent = content.get();
    }
    fx::Scratch out = fx::scratch(small);
    if (!out) return;
    draw(reduced, *out);
    for (TitleProblem& p : reduced.problems) ctx.problems.push_back(std::move(p));
    ctx.usesCanvasPosition = ctx.usesCanvasPosition || reduced.usesCanvasPosition;
    // the copy samples the reduced target at each full pixel's uv: bilinear, aligned by construction
    EffectContext full = effectContext(ctx);
    fx::copy(full, *out, target);
}

const TitleLutData& gen::mobileNoiseData() {
    static const TitleLutData data = [] {
        TitleLutData d;
        d.format = ColorFormat::RGBA;
        d.width = NOISE_SIZE;
        d.height = NOISE_SIZE;
        d.filter = TextureFilter::LINEAR;
        const std::vector<double> r = noiseChannel(0), g = noiseChannel(1);
        d.bytes.resize((size_t)NOISE_SIZE * NOISE_SIZE * 4);
        for (size_t i = 0; i < r.size(); i++) {
            d.bytes[i * 4 + 0] = (uint8_t)std::lround(std::clamp(r[i], 0.0, 1.0) * 255.0);
            d.bytes[i * 4 + 1] = (uint8_t)std::lround(std::clamp(g[i], 0.0, 1.0) * 255.0);
            d.bytes[i * 4 + 2] = 0;
            d.bytes[i * 4 + 3] = 255;
        }
        return d;
    }();
    return data;
}

fx::Scratch gen::upstreamField(const GeneratorContext& ctx, const TargetRef& field, const TitleVec2& flow, double lengthPx, double side) {
    fx::Scratch out = fx::scratchOf(ctx.runner, TitleBuffer::Distance, ctx.width, ctx.height);
    if (!out) return {};
    TargetRef& target = *out;
    TitlePass pass = genPass(ctx, GenProgram::UpstreamField, "upstream field", target, ctx.documentToTarget);
    pass.raw[0].target = &field;
    pass.set(6, (float)flow.x, (float)flow.y, (float)std::max(1.0, lengthPx), (float)side);
    if (!ctx.runner.run(pass)) return {};
    return out;
}
