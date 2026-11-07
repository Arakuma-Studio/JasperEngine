// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PASSPLANNER_H
#define PASSPLANNER_H

// From a graph to what an evaluator draws (S90 R5, R6): one fragment shader per pass, where a pass is
// the upstream subgraph inlined and cut at a Buffer. The planner inserts the Buffers a graph did not
// place, so users never manage them for performance.

#include "ProceduralEvaluator.h"
#include "ProcMatExpander.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::procmat {

    // M0's measured limits (R6). Size and uniforms are soft: a single node over them still runs.
    struct PlanLimits {
        int textures = 16;
        size_t glslBytes = 64 * 1024;
        int uniformVec4s = 200;
        // distinct coordinates one output may be expanded at before it is worth a Buffer
        int repeatedSamples = 4;
    };

    // GL 4.1 guarantees 256 fragment vec4s, and the engine's budget is 16 textures on every backend
    inline constexpr int PLAN_HARD_UNIFORM_VEC4S = 256;

    struct PlannedTexture {
        std::string name;   // t_<name> and s_<name> in the fragment
        int buffer = 0;
    };

    // A float of a pass's block the evaluator sets to the size the buffer it reads has at that step
    struct PlannedSize {
        uint32_t index = 0;
        int buffer = 0;
    };

    struct PlannedPass {
        std::string label;              // what a failure names (R9)
        std::vector<std::string> nodes; // the graph nodes inlined in it
        std::string fragment;
        UniformBlock uniforms;
        std::vector<PlannedTexture> textures;
        std::vector<PlannedSize> sizes;
    };

    struct PlannedBuffer {
        std::string label;
        int size = 0;
        bool mips = false;  // sampled with a level of detail: box-filtered mips (R11)
    };

    // Draws a pass into a buffer, in this order. An Iterate Buffer is several steps on one buffer, each
    // reading what the one before wrote; a shrinking one changes size as it goes.
    struct PlannedStep {
        int pass = -1;      // -1: the buffer is cleared to zero (a Buffer with nothing wired)
        int buffer = 0;
        int size = 0;
    };

    struct PlannedOutput {
        std::string name;
        int buffer = 0;
    };

    struct PassPlan {
        int size = 0;   // of the outputs: square, power of two
        // false: results hold no pixels and the images stay on the device (a live preview, which must
        // not stall a frame on a read back)
        bool readBack = true;
        std::vector<PlannedPass> passes;
        std::vector<PlannedBuffer> buffers;
        std::vector<PlannedStep> steps;
        std::vector<PlannedOutput> outputs;

        // Equal plans differ at most in their uniform values, so setUniforms() is enough (R7)
        bool sameStructure(const PassPlan& other) const;
    };

    struct PlanOptions {
        int size = 1024;
        bool readBack = true;
        // the graph's seed: changes uniforms only
        double seed = 0.0;
        // the largest Buffer, 0 for what the graph asks: a preview caps a 2048 Buffer at its own size
        int maxBufferSize = 0;
        PlanLimits limits;
    };

    // An output of a plan: `port` of `node` in the graph, by the ids a file uses
    struct PlanRoot {
        std::string name;
        std::string node;
        std::string port;
    };

    struct PlanSource {
        std::string name;
        SourceRef source;
    };

    // The first step to draw again after `pass`'s uniforms change: its first step, or earlier when a buffer
    // drawn from there on was drawn before it too (an Iterate Buffer's loop starts over from its initial
    // step, whatever pass of the loop changed)
    size_t firstStepToRedraw(const PassPlan& plan, uint32_t pass);

    // The roots of a material's bake: each connected port of its material_output node, and "orm" when
    // any of occlusion, roughness and metallic is, in the order R14 lists them
    std::vector<PlanRoot> materialOutputRoots(const graph::Graph& graph);
    // What a bake writes and a preview shows (R14): materialOutputRoots, and "normal" made from the float
    // height when Height is connected and Normal is not. A connected Normal is turned from the library's
    // convention into the engine's either way.
    std::vector<PlanRoot> materialBakeRoots(const graph::Graph& graph);

    // Plans generators already wired in `expander`: what a test does to wire small sources
    bool planPasses(Expander& expander, const std::vector<PlanSource>& roots, const PlanOptions& options, PassPlan& plan, std::string& error);
    // The whole path from a graph file. Every parameter is checked first (R3); an invalid graph plans
    // nothing and says why. Empty roots mean materialOutputRoots().
    bool planGraph(const graph::Graph& graph, const std::shared_ptr<const NodeLibrary>& library, const std::vector<PlanRoot>& roots,
                   const PlanOptions& options, PassPlan& plan, std::string& error);

}

#endif
