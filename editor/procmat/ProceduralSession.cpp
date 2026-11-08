// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralSession.h"

#include "ProceduralCpuEvaluator.h"

#include <algorithm>

using namespace jasperengine::editor::procmat;

ProceduralSession::ProceduralSession(ProceduralEvaluator& evaluator, std::shared_ptr<const NodeLibrary> library, PlanOptions options)
    : evaluator(evaluator), library(std::move(library)), options(std::move(options)) {}

void ProceduralSession::setGraph(const graph::Graph& graph, double now) {
    current = graph;
    replan(now);
}

void ProceduralSession::setOptions(const PlanOptions& newOptions, double now) {
    options = newOptions;
    if (current) replan(now);
}

void ProceduralSession::setRoots(const std::vector<PlanRoot>& newRoots, double now) {
    const auto same = [](const PlanRoot& a, const PlanRoot& b) { return a.name == b.name && a.node == b.node && a.port == b.port; };
    if (newRoots.size() == roots.size() && std::equal(newRoots.begin(), newRoots.end(), roots.begin(), same)) return;
    roots = newRoots;
    if (current) replan(now);
}

void ProceduralSession::replan(double now) {
    stale = true;
    PassPlan plan;
    std::string planError;
    if (!planGraph(*current, library, roots, options, plan, planError)) {
        problem = std::move(planError);
        // what was on its way was for a graph that no longer is
        pending.reset();
        if (running) {
            evaluator.cancel();
            EvaluationResult dropped;
            evaluator.advance(0.0, dropped);
            running = false;
            // what it holds was never drawn: an edit back to it must draw it, not take it as the image
            active.reset();
        }
        return;
    }
    problem.clear();
    if (active && plan.sameStructure(*active)) {
        pending.reset();
        for (size_t i = 0; i < plan.passes.size(); i++) {
            if (!(plan.passes[i].uniforms == active->passes[i].uniforms)) {
                evaluator.setUniforms((uint32_t)i, plan.passes[i].uniforms);
                running = true;
            }
        }
        active = std::move(plan);
        // edited back to what is drawn, or an edit no value depends on
        if (!running) stale = false;
        return;
    }
    // the first image has nothing to keep on screen meanwhile: only an edit waits, and the wait runs from
    // the last structural edit, so dragging a slider meanwhile does not hold the rebuild back
    if (!pending || !plan.sameStructure(*pending)) rebuildAt = active ? now + REBUILD_DEBOUNCE_SECONDS : now;
    pending = std::move(plan);
}

void ProceduralSession::update(double now, double budgetMs) {
    if (pending && now >= rebuildAt) {
        std::string prepareError;
        const bool prepared = evaluator.prepare(*pending, prepareError);
        if (prepared) {
            active = std::move(pending);
            running = true;
        } else {
            problem = std::move(prepareError);
        }
        pending.reset();
    }
    if (!running) return;
    EvaluationResult result;
    if (!evaluator.advance(budgetMs, result)) return;
    running = false;
    if (!result.ok) {
        problem = result.error;
        // the CPU tier failed after the device did: both are what went wrong (R9)
        for (const std::string& warning : result.warnings) problem = warning + " " + problem;
        // a pass inlines many nodes; the session has the graph to say which one the CPU tier refused (R11)
        if (result.unsupported && result.failedNode.empty() && active && current && result.failedPass >= 0 &&
            (size_t)result.failedPass < active->passes.size()) {
            const std::string node = refusedNode(*current, library, active->passes[(size_t)result.failedPass], options);
            if (!node.empty()) problem += " The node is " + node + ".";
        }
        return;
    }
    image = std::move(result);
    // an edit that arrived meanwhile is still waiting for its debounce
    stale = pending.has_value();
}
