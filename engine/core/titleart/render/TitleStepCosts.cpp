// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleStepCosts.h"

#include "TitleRenderJob.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // a measurement above the ratio moves it half way, one below a quarter of the way
    constexpr double RISE = 0.5;
    constexpr double FALL = 0.25;
    // a ratio outside these is a broken clock or estimate, not a machine
    constexpr double MIN_RATIO = 1.0e-3;
    constexpr double MAX_RATIO = 1.0e5;

}

TitleStepCosts::TitleStepCosts(std::function<double()> clockUs) : clock(std::move(clockUs)) {
    if (!clock) {
        clock = [] {
            using namespace std::chrono;
            return duration<double, std::micro>(steady_clock::now().time_since_epoch()).count();
        };
    }
}

void TitleStepCosts::Ratio::learn(double observed) {
    observed = std::clamp(observed, MIN_RATIO, MAX_RATIO);
    // the first measurement of a kind is all it knows
    if (count == 0) ratio = observed;
    else ratio += (observed > ratio ? RISE : FALL) * (observed - ratio);
    count++;
}

std::string TitleStepCosts::kindOf(const TitleRenderStep& step) {
    std::string label = step.label;
    const size_t tile = label.find(" tile ");
    if (tile != std::string::npos) label.resize(tile);
    return (step.kind == StepKind::Cpu ? "cpu:" : "gpu:") + label;
}

double TitleStepCosts::ratio(const std::string& kind) const {
    auto it = byKind.find(kind);
    return it != byKind.end() ? it->second.ratio : machine.ratio;
}

uint32_t TitleStepCosts::predictUs(const std::string& kind, uint32_t estimateUs) const {
    const double us = std::max<uint32_t>(1, estimateUs) * ratio(kind);
    return (uint32_t)std::clamp(std::ceil(us), 1.0, 4.0e9);
}

uint32_t TitleStepCosts::predictUs(const TitleRenderStep& step) const {
    return predictUs(kindOf(step), TitleRenderJob::estimateUs(step));
}

namespace {

    double mean(const std::vector<double>& values) {
        if (values.empty()) return 0.0;
        double sum = 0.0;
        for (double v : values) sum += v;
        return sum / (double)values.size();
    }

}

double TitleStepCosts::firstError() const {
    return mean(firstErrors);
}

double TitleStepCosts::recentError() const {
    return mean(recentErrors);
}

void TitleStepCosts::observe(const std::string& kind, uint32_t estimateUs, double measuredUs) {
    const double predicted = predictUs(kind, estimateUs);
    const double measured = std::max(0.0, measuredUs);
    const double error = std::abs(measured - predicted) / std::max({measured, predicted, 1.0});
    if (firstErrors.size() < ERROR_WINDOW) firstErrors.push_back(error);
    if (recentErrors.size() < ERROR_WINDOW) {
        recentErrors.push_back(error);
    } else {
        recentErrors[recentNext] = error;
        recentNext = (recentNext + 1) % ERROR_WINDOW;
    }
    const double observed = std::max(0.0, measuredUs) / (double)std::max<uint32_t>(1, estimateUs);
    machine.learn(observed);
    auto it = byKind.find(kind);
    if (it == byKind.end()) {
        if (byKind.size() >= MAX_KINDS) return;
        it = byKind.emplace(kind, Ratio{}).first;
    }
    it->second.learn(observed);
}

void TitleStepCosts::observe(const TitleRenderStep& step, double measuredUs) {
    observe(kindOf(step), TitleRenderJob::estimateUs(step), measuredUs);
}
