// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleRenderJob.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

TitleRenderJob::TitleRenderJob(TitleStepList list) : steps(std::move(list.list)) {
    for (const TitleRenderStep& s : steps) {
        const uint32_t estimate = estimateUs(s);
        totalUs += estimate;
        if (s.kind == StepKind::Gpu && s.countsAsPass) passCount += s.passes;
        NodeCost& cost = costOf(s.layerId);
        cost.estimatedUs += estimate;
        if (s.kind == StepKind::Gpu && s.countsAsPass) cost.passes += s.passes;
    }
}

uint32_t TitleRenderJob::estimateUs(const TitleRenderStep& step) {
    if (step.kind == StepKind::Cpu) return std::max<uint32_t>(1, step.cpuUs);
    const double pixels = (double)std::max(0, step.scissor.width) * (double)std::max(0, step.scissor.height);
    const double us = pixels * std::max(0.0f, step.costWeight) / 1000.0;
    return (uint32_t)std::clamp(std::ceil(us), 1.0, 4.0e9);
}

NodeCost& TitleRenderJob::costOf(const std::string& layerId) {
    auto it = costs.find(layerId);
    if (it == costs.end()) {
        costOrder.push_back(layerId);
        it = costs.emplace(layerId, NodeCost{}).first;
        it->second.layerId = layerId;
    }
    return it->second;
}

bool TitleRenderJob::done() const {
    return failure.empty() && next >= steps.size();
}

float TitleRenderJob::progress() const {
    if (next >= steps.size()) return 1.0f;
    if (totalUs == 0) return 0.0f;
    return (float)std::min(1.0, (double)doneUs / (double)totalUs);
}

uint32_t TitleRenderJob::predictUs(const TitleRenderStep& step) const {
    return costModel ? costModel->predictUs(step) : estimateUs(step);
}

double TitleRenderJob::nowUs() const {
    if (costModel) return costModel->nowUs();
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t TitleRenderJob::nextStepCostUs() const {
    return next < steps.size() && failure.empty() ? predictUs(steps[next]) : 0;
}

bool TitleRenderJob::step(const StepBudget& budget, StepReport* report) {
    StepReport local;
    blocked = false;
    const double callStart = nowUs();
    while (failure.empty() && next < steps.size()) {
        TitleRenderStep& s = steps[next];
        const uint32_t estimate = estimateUs(s);
        const uint32_t predicted = predictUs(s);
        const bool isPass = s.kind == StepKind::Gpu && s.countsAsPass;
        if (local.steps > 0) {
            // what the call took so far, readiness checks included, or what its steps were
            // predicted to take when that is more (a clock too coarse for tiny steps)
            const double spent = std::max(nowUs() - callStart, (double)local.estimatedUs);
            if (budget.budgetUs > 0 && spent + predicted > budget.budgetUs) break;
            if (budget.maxPasses > 0 && isPass && local.passes >= budget.maxPasses) break;
        }
        // a software GPU still drawing what this step may sample: a later call runs it
        if (costModel && s.kind == StepKind::Gpu && costModel->gpuBusy()) {
            blocked = true;
            break;
        }
        // the readiness check may build the step's program: part of what the step costs
        const double start = nowUs();
        if (s.ready && !s.ready()) {
            blocked = true;
            break;
        }
        std::string error;
        const bool ok = !s.run || s.run(s.scissor, error);
        const double measuredUs = std::max(0.0, nowUs() - start);
        if (costModel && s.kind == StepKind::Gpu) costModel->fenceGpu();
        const uint32_t measured = (uint32_t)std::min(measuredUs, 4.0e9);
        local.steps++;
        local.estimatedUs += predicted;
        local.measuredUs += measured;
        if (measured >= local.longestUs) {
            local.longestUs = measured;
            local.longestLabel = s.label;
        }
        NodeCost& cost = costOf(s.layerId);
        cost.measuredUs += measured;
        if (!ok) {
            failure = error.empty() ? std::string("render step '") + s.label + "' failed" : error;
            break;
        }
        // a step that met a program still being compiled runs again once it is built (what it
        // took then is not what it costs)
        if (s.ready && !s.ready()) {
            blocked = true;
            break;
        }
        if (costModel) costModel->observe(s, measuredUs);
        if (isPass) {
            local.passes += s.passes;
            passesRun += s.passes;
        }
        doneUs += estimate;
        next++;
    }
    if (report) *report = local;
    return done();
}

void TitleRenderJob::splitOversizedSteps(uint32_t maxUs) {
    if (maxUs == 0) return;
    std::vector<TitleRenderStep> out(std::make_move_iterator(steps.begin()), std::make_move_iterator(steps.begin() + (long)next));
    for (size_t i = next; i < steps.size(); i++) {
        TitleRenderStep& s = steps[i];
        const uint32_t estimate = predictUs(s);
        const TitleRectI r = s.scissor;
        if (s.kind != StepKind::Gpu || !s.splittable || estimate <= maxUs ||
            (r.width < 2 * TITLE_MIN_TILE && r.height < 2 * TITLE_MIN_TILE)) {
            out.push_back(std::move(s));
            continue;
        }
        // the fewest tiles that bring each under maxUs, none narrower than TITLE_MIN_TILE
        const int maxX = std::max(1, r.width / TITLE_MIN_TILE);
        const int maxY = std::max(1, r.height / TITLE_MIN_TILE);
        const int wanted = (int)std::ceil((double)estimate / (double)maxUs);
        int nx = 1, ny = 1;
        while (nx * ny < wanted && (nx < maxX || ny < maxY)) {
            // split the longer tile side
            const double tileW = (double)r.width / nx, tileH = (double)r.height / ny;
            if ((tileW >= tileH && nx < maxX) || ny >= maxY) nx++;
            else ny++;
        }
        const bool pass = s.countsAsPass;
        for (int ty = 0; ty < ny; ty++) {
            for (int tx = 0; tx < nx; tx++) {
                TitleRenderStep tile = s;
                const int x0 = r.x + r.width * tx / nx, x1 = r.x + r.width * (tx + 1) / nx;
                const int y0 = r.y + r.height * ty / ny, y1 = r.y + r.height * (ty + 1) / ny;
                tile.scissor = {x0, y0, x1 - x0, y1 - y0};
                tile.label = s.label + " tile " + std::to_string(ty * nx + tx + 1) + "/" + std::to_string(nx * ny);
                out.push_back(std::move(tile));
            }
        }
        if (pass) {
            passCount += (uint32_t)(nx * ny - 1) * s.passes;
            costOf(s.layerId).passes += (uint32_t)(nx * ny - 1) * s.passes;
        }
    }
    steps = std::move(out);
    // the estimates change by rounding only; keep progress consistent with the new list
    uint64_t total = 0, finished = 0;
    for (size_t i = 0; i < steps.size(); i++) {
        const uint32_t e = estimateUs(steps[i]);
        total += e;
        if (i < next) finished += e;
    }
    totalUs = total;
    doneUs = finished;
}

std::vector<ShaderKey> TitleRenderJob::shaderKeys() const {
    std::vector<ShaderKey> keys;
    for (const TitleRenderStep& s : steps) {
        if (s.kind != StepKind::Gpu || s.shader == ShaderKey()) continue;
        if (std::find(keys.begin(), keys.end(), s.shader) == keys.end()) keys.push_back(s.shader);
    }
    return keys;
}

std::vector<NodeCost> TitleRenderJob::nodeCosts() const {
    std::vector<NodeCost> out;
    for (const std::string& id : costOrder) {
        if (id.empty()) continue;
        out.push_back(costs.at(id));
    }
    return out;
}
