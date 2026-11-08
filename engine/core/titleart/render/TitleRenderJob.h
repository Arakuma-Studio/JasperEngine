// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R18, R18a, §6.3)
// - Purpose: one render of a document as a flat ordered list of steps (no graph: the
//   do-not-copy list), run whole steps at a time within a budget and resumed on the next call:
//   the time-sliced preview path, S81's scheduler and the bake (which runs it to the end in one
//   call) share it, and the result equals a one-shot render byte for byte.
// - Steps are CPU stages (shaping, outlines, masks, S83's analysis, S86's mesh units) and GPU
//   passes. Each carries its cost estimate: a GPU pass its output pixels times its pass's
//   costWeight (ns per output pixel on the desktop reference; S83's model supplies the weights
//   of its definitions, S81 scales them per platform), a CPU stage its estimate in us. The
//   renderer (TitleArtRenderer::begin) builds the list; this class only schedules it.
// - Splitting: splitOversizedSteps(maxUs) cuts every splittable GPU pass whose estimate exceeds
//   maxUs into scissor tiles of at least TITLE_MIN_TILE x TITLE_MIN_TILE; the step's run is
//   called once per tile with the tile as its scissor, so a pass that writes only its own
//   scissor gives the same image split or not.
// - Double buffering is the owner's: keep the previous finished job (and its image) until the
//   new one is done.
// - Thread-safety: main thread (GPU steps record draw calls); a CPU step may hand work to the
//   job system and finish in a later step.

#pragma once

#include "Export.h"
#include "shader/ShaderBuildTypes.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/render/TitleTargetPool.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    constexpr int TITLE_MIN_TILE = 256;

    enum class StepKind : uint8_t { Cpu, Gpu };

    struct TitleRenderStep {
        StepKind kind = StepKind::Gpu;
        std::string label;              // "outer_glow blur x" (traces, debugging)
        std::string layerId;            // the layer it belongs to ("" for document passes)
        // GPU: the output pixels it draws and its pass's cost weight (ns per output pixel)
        TitleRectI scissor;
        float costWeight = 1.0f;
        bool splittable = false;        // writes only its scissor from inputs it does not write
        ShaderKey shader;               // the program variant it binds
        bool countsAsPass = true;       // a GPU draw pass (passesDone / passesTotal)
        uint32_t passes = 1;            // the draw passes it runs (an effect's kernel may run several)
        // CPU: its estimate
        uint32_t cpuUs = 0;
        // runs the step (a GPU pass inside `scissor`, which a split narrows to its tile); false
        // with `error` fails the job
        std::function<bool(const TitleRectI& scissor, std::string& error)> run;
        // null, or false while the step cannot run yet (its program still compiles): step()
        // then returns without running it and the job waits. False right after the step ran
        // (an effect's pass met a program not built yet) runs the step again once it is true
        std::function<bool()> ready;
    };

    // the renderer's list under construction (S86's 3D leaf appends its mesh units and passes)
    class JASPERENGINE_API TitleStepList {
    public:
        void add(TitleRenderStep step) { list.push_back(std::move(step)); }
        size_t size() const { return list.size(); }
        bool empty() const { return list.empty(); }
        const std::vector<TitleRenderStep>& steps() const { return list; }

    private:
        friend class TitleRenderJob;
        std::vector<TitleRenderStep> list;
    };

    // 0 = unlimited
    struct StepBudget {
        uint32_t budgetUs = 0;
        uint32_t maxPasses = 0;
    };

    // what one step() call did
    struct StepReport {
        uint32_t steps = 0;
        uint32_t passes = 0;
        uint32_t estimatedUs = 0;
        uint32_t measuredUs = 0;
    };

    // per layer, for S82's cost readout (R71)
    struct NodeCost {
        std::string layerId;
        uint32_t passes = 0;
        uint32_t estimatedUs = 0;
        uint32_t measuredUs = 0;
    };

    // the finished image: a target of the output format, its frame (canvas pixels, image space)
    // and the bounds of its alpha within it (pixels of the image, alpha >= 1/255)
    struct TitleImage {
        TargetRef target;
        int width = 0, height = 0;
        ColorFormat format = ColorFormat::RGBA;
        TitleRectI frame;
        TitleRectI pixels;              // the output pixels it covers (the scaled canvas's, exact where frame rounds)
        TitleRectI reach;               // a glyph cell's: the output pixels its content and reach can cover (within pixels)
        TitleRectI alphaBounds;
        bool premultiplied = true;      // intermediates are; the output pass writes straight sRGB
    };

    // what a request asked to keep besides the image (R18a)
    struct TitleSplits {
        std::map<std::string, TitleImage> layers;   // RenderRequest::keepSplits, by layer id
        TitleImage glow;                            // the light of the document (OutputContent::Glow)
        TitleImage glyphBack, glyphFront;           // RenderRequest::glyphCell's two parts
    };

    class JASPERENGINE_API TitleRenderJob {
    public:
        explicit TitleRenderJob(TitleStepList steps);
        TitleRenderJob(const TitleRenderJob&) = delete;
        TitleRenderJob& operator=(const TitleRenderJob&) = delete;

        // Runs whole steps while their estimates fit the budget (always at least one that is
        // ready); true when the job is complete, false while steps remain (waiting() when the
        // next one is not ready) or after a failure (error() says which)
        bool step(const StepBudget& budget, StepReport* report = nullptr);
        bool done() const;
        bool failed() const { return !failure.empty(); }
        // the next step is not ready (TitleRenderStep::ready): step() ran nothing for it
        bool waiting() const { return blocked; }
        // estimated cost done / total, 0..1 (rises monotonically to 1)
        float progress() const;
        uint32_t passesDone() const { return passesRun; }
        uint32_t passesTotal() const { return passCount; }
        // the next step's estimate in us (0 when done)
        uint32_t nextStepCostUs() const;
        // cuts the remaining splittable GPU steps estimated above maxUs into scissor tiles >= 256 x 256
        void splitOversizedSteps(uint32_t maxUs);
        // every program variant the job binds, in first-use order (S81 R18 warm-up, R47)
        std::vector<ShaderKey> shaderKeys() const;
        std::vector<NodeCost> nodeCosts() const;
        const std::string& error() const { return failure; }

        const TitleImage& image() const { return result; }
        const TitleSplits& splits() const { return kept; }
        const std::vector<TitleProblem>& problems() const { return found; }

        // ---- the renderer's side ----
        TitleImage& resultImage() { return result; }
        TitleSplits& resultSplits() { return kept; }
        void addProblem(TitleProblem problem) { found.push_back(std::move(problem)); }
        // state the steps share that must live as long as the job (CPU stage outputs, targets)
        void keepAlive(std::shared_ptr<void> state) { held.push_back(std::move(state)); }
        // the estimate of one step in us (GPU: pixels x costWeight / 1000, at least 1)
        static uint32_t estimateUs(const TitleRenderStep& step);

    private:
        std::vector<TitleRenderStep> steps;
        size_t next = 0;
        uint64_t totalUs = 0;
        uint64_t doneUs = 0;
        uint32_t passCount = 0;
        uint32_t passesRun = 0;
        std::map<std::string, NodeCost> costs;
        std::vector<std::string> costOrder;
        std::string failure;
        TitleImage result;
        TitleSplits kept;
        std::vector<TitleProblem> found;
        std::vector<std::shared_ptr<void>> held;
        bool blocked = false;

        NodeCost& costOf(const std::string& layerId);
    };

}
