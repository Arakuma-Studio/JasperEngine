// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALSESSION_H
#define PROCEDURALSESSION_H

// Keeps one graph's image up to date while it is edited, on any ProceduralEvaluator (S90 R7):
// - an edit that only changes values (a float, a colour, a same-size ramp, the seed) reaches the prepared
//   passes as uniforms, with no compile;
// - an edit that changes the generated text or a size (structure, an enum, a boolean) rebuilds, 0.2 s
//   after the last such edit;
// - until a new image is ready, the previous one stays and isStale() says so. A graph that cannot be
//   planned keeps it stale too, with the reason in error().
//
// Which edit is which is decided by generating the plan and comparing it, never by a list of parameter
// kinds, so the two cannot drift apart. Time comes from the caller: the tests and the window pass theirs.

#include "PassPlanner.h"

#include <memory>
#include <optional>
#include <string>

namespace jasperengine::editor::procmat {

    class ProceduralSession {
    public:
        static constexpr double REBUILD_DEBOUNCE_SECONDS = 0.2;

        ProceduralSession(ProceduralEvaluator& evaluator, std::shared_ptr<const NodeLibrary> library, PlanOptions options);

        // The graph as it is now, at `now` seconds. Cheap enough for every change of a drag.
        void setGraph(const graph::Graph& graph, double now);
        // The same graph at another seed or size: a seed is a value edit, a size rebuilds
        void setOptions(const PlanOptions& options, double now);
        // Once per frame: fires a debounced rebuild and gives the evaluator its slice
        void update(double now, double budgetMs);

        // the latest finished image, whatever the graph has become since
        const EvaluationResult& latest() const { return image; }
        bool hasImage() const { return image.ok; }
        // the graph has changed since latest() was drawn, or its evaluation failed
        bool isStale() const { return stale; }
        // why the latest edit has no image: a graph that cannot be planned, a device failure, a node the
        // CPU tier cannot run (named)
        const std::string& error() const { return problem; }
        // the plan the evaluator holds, null before the first one
        const PassPlan* activePlan() const { return active ? &*active : nullptr; }
        // a rebuild waits for its debounce
        bool isRebuildPending() const { return pending.has_value(); }

    private:
        void replan(double now);

        ProceduralEvaluator& evaluator;
        std::shared_ptr<const NodeLibrary> library;
        PlanOptions options;
        std::optional<graph::Graph> current;
        std::optional<PassPlan> active;
        std::optional<PassPlan> pending;
        double rebuildAt = 0.0;
        bool running = false;
        bool stale = false;
        std::string problem;
        EvaluationResult image;
    };

}

#endif
