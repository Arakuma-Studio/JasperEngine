// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/effects/lib.ts (blur's pyramid, sdf's jump flooding) and
// src/engine/layer2d/materialPass.ts (updateSdf) by SafEight, used with the author's permission.

#include "TitleFields.h"
#include "TitlePlanner.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

// ---- plans and set-ups ----

std::vector<float> titleart::floodSteps(float reachPx, int width, int height) {
    const double side = std::max(1, std::max(width, height));
    const double reach = reachPx > 0.0f ? std::min((double)reachPx, side) : side;
    const int k = std::max(1, (int)std::ceil(std::log2(reach + 2.0)));
    std::vector<float> out;
    for (int step = 1 << (k - 1); step >= 1; step /= 2) out.push_back((float)step);
    out.push_back(1.0f);
    return out;
}

TitleBlurPlan titleart::blurPlan(float radiusPx, int width, int height) {
    TitleBlurPlan plan;
    const float sigma = std::max(0.0f, radiusPx / 3.0f);
    plan.copy = sigma < 0.3f;
    if (plan.copy) return plan;
    while (sigma / (float)(1 << plan.levels) > 6.0f && plan.levels < 5) plan.levels++;
    plan.sigma = sigma / (float)(1 << plan.levels);
    for (int l = 1; l <= plan.levels; l++) {
        plan.sizes.push_back({std::max(1, (width + (1 << l) - 1) >> l), std::max(1, (height + (1 << l) - 1) >> l)});
    }
    return plan;
}

void titleart::setupResample(TitleGpuPass& pass, TextureRender* src, int toW, int toH, float opacity) {
    pass.program = TitleRenderProgram::Copy;
    pass.tex[0] = src;
    pass.params.set(2, 1.0f / (float)std::max(1, toW), 1.0f / (float)std::max(1, toH), 0.0f, 0.0f);
    pass.params.set(3, opacity);
}

void titleart::setupBlur(TitleGpuPass& pass, TextureRender* src, int width, int height, bool horizontal, float sigma) {
    pass.program = TitleRenderProgram::Blur;
    pass.tex[0] = src;
    const float dx = 1.0f / (float)std::max(1, width), dy = 1.0f / (float)std::max(1, height);
    pass.params.set(2, horizontal ? dx : 0.0f, horizontal ? 0.0f : dy, sigma);
    pass.params.set(3, dx, dy, 0.0f, 0.0f);
}

void titleart::setupSeed(TitleGpuPass& pass, TextureRender* src, bool alpha, float threshold, const std::array<float, 4>& seedsOut) {
    pass.program = alpha ? TitleRenderProgram::SeedAlpha : TitleRenderProgram::Seed;
    pass.tex[0] = src;
    pass.params.set(TITLE_PARAM_OUT_ENCODING, seedsOut[0], seedsOut[1], seedsOut[2], seedsOut[3]);
    pass.params.set(2, threshold);
}

void titleart::setupFlood(TitleGpuPass& pass, TextureRender* seeds, float step, const std::array<float, 4>& enc) {
    pass.program = TitleRenderProgram::Flood;
    pass.raw[0] = seeds;
    pass.params.set(TITLE_PARAM_OUT_ENCODING, enc[0], enc[1], enc[2], enc[3]);
    pass.params.set(TITLE_PARAM_RAW_ENCODING, enc[0], enc[1], enc[2], enc[3]);
    pass.params.set(2, step);
}

void titleart::setupResolve(TitleGpuPass& pass, TextureRender* seeds, TextureRender* coverage, bool alpha, float reachPx, float threshold,
                            const std::array<float, 4>& seedsEnc, const std::array<float, 4>& out) {
    pass.program = alpha ? TitleRenderProgram::ResolveAlpha : TitleRenderProgram::Resolve;
    pass.raw[0] = seeds;
    pass.tex[0] = coverage;
    pass.params.set(TITLE_PARAM_OUT_ENCODING, out[0], out[1], out[2], out[3]);
    pass.params.set(TITLE_PARAM_RAW_ENCODING, seedsEnc[0], seedsEnc[1], seedsEnc[2], seedsEnc[3]);
    pass.params.set(2, reachPx, threshold);
}

// ---- recorded: the planner's steps ----

TitlePx TitlePlanner::blur(TitlePx src, float radiusPx, const std::string& layerId) {
    const TitleJobState::Slot& from = st->slots[(size_t)src.slot];
    const TargetKey srcKey = from.entry && from.entry->target ? from.entry->target.key() : from.key;
    auto keyOf = [&srcKey](int w, int h) {
        TargetKey k = srcKey;
        k.width = w;
        k.height = h;
        return k;
    };
    const int W = src.rect.width, H = src.rect.height;
    const TitleBlurPlan plan = blurPlan(radiusPx, W, H);
    TitlePx out{newSlot(keyOf(W, H)), src.rect};
    auto resample = [this, &layerId](int fromSlot, int toSlot, int toW, int toH) {
        pass("blur level", layerId, toSlot, {0, 0, toW, toH}, TitleRenderProgram::Copy, [fromSlot, toW, toH](TitleJobState& s, TitleGpuPass& p, std::string&) {
            setupResample(p, s.texture(fromSlot), toW, toH);
            return true;
        });
    };
    if (plan.copy) {
        resample(src.slot, out.slot, W, H);
        return out;
    }
    // down by bilinear halvings, the separable kernel there, back up
    int cur = src.slot;
    int w = W, h = H;
    for (const auto& size : plan.sizes) {
        const int level = newSlot(keyOf(size.first, size.second));
        resample(cur, level, size.first, size.second);
        if (cur != src.slot) release(cur);
        cur = level;
        w = size.first;
        h = size.second;
    }
    const float sigma = plan.sigma;
    const int across = newSlot(keyOf(w, h));
    const int x = cur;
    pass("blur x", layerId, across, {0, 0, w, h}, TitleRenderProgram::Blur, [x, w, h, sigma](TitleJobState& s, TitleGpuPass& p, std::string&) {
        setupBlur(p, s.texture(x), w, h, true, sigma);
        return true;
    });
    if (cur != src.slot) release(cur);
    const int down = plan.levels > 0 ? newSlot(keyOf(w, h)) : out.slot;
    pass("blur y", layerId, down, {0, 0, w, h}, TitleRenderProgram::Blur, [across, w, h, sigma](TitleJobState& s, TitleGpuPass& p, std::string&) {
        setupBlur(p, s.texture(across), w, h, false, sigma);
        return true;
    });
    release(across);
    if (plan.levels > 0) {
        resample(down, out.slot, W, H);
        release(down);
    }
    return out;
}

TitlePx TitlePlanner::distanceField(TitlePx src, bool alphaSource, float threshold, float reachPx, const std::string& layerId,
                                    std::array<float, 4>* encoding) {
    const int w = src.rect.width, h = src.rect.height;
    const std::array<float, 4> seedsEnc = titleEncoding(svc.caps.format(TitleBuffer::Seeds), TitleValueKind::Seeds);
    const std::array<float, 4> distEnc = titleEncoding(svc.caps.format(TitleBuffer::Distance), TitleValueKind::Distance);
    if (encoding) *encoding = distEnc;
    int a = newSlot(TitleBuffer::Seeds, w, h);
    int b = newSlot(TitleBuffer::Seeds, w, h);
    const int from = src.slot;
    pass("seeds", layerId, a, {0, 0, w, h}, alphaSource ? TitleRenderProgram::SeedAlpha : TitleRenderProgram::Seed, [from, alphaSource, threshold, seedsEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
        setupSeed(p, s.texture(from), alphaSource, threshold, seedsEnc);
        return true;
    });
    const std::vector<float> steps = floodSteps(reachPx, w, h);
    floodPasses += (uint32_t)steps.size();
    unboundedFloodPasses += (uint32_t)floodSteps(0.0f, w, h).size();
    for (float stepPx : steps) {
        const int in = a;
        pass("flood", layerId, b, {0, 0, w, h}, TitleRenderProgram::Flood, [in, stepPx, seedsEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
            setupFlood(p, s.texture(in), stepPx, seedsEnc);
            return true;
        });
        std::swap(a, b);
    }
    release(b);
    TitlePx out{newSlot(TitleBuffer::Distance, w, h), src.rect};
    const float reach = reachPx > 0.0f ? reachPx : (float)std::max(w, h);
    const int seeds = a;
    pass("distance", layerId, out.slot, {0, 0, w, h}, alphaSource ? TitleRenderProgram::ResolveAlpha : TitleRenderProgram::Resolve,
         [seeds, from, alphaSource, reach, threshold, seedsEnc, distEnc](TitleJobState& s, TitleGpuPass& p, std::string&) {
             setupResolve(p, s.texture(seeds), s.texture(from), alphaSource, reach, threshold, seedsEnc, distEnc);
             return true;
         });
    release(a);
    return out;
}
