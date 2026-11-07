// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALCPUEVALUATOR_H
#define PROCEDURALCPUEVALUATOR_H

// Tier 3 of S90 R8: the passes a GPU tier draws, run per pixel by SpirvInterpreter (R11). It needs no
// window and no graphics device, only the shader compiler, so it is also what a bake falls back to
// when no editor can start (R39) and what a device failure continues on (R9).
//
// - Each pass is compiled to SPIR-V and loaded inside advance(), a pass a unit of work. An instruction
//   the interpreter does not run is refused there, before any pixel, naming the pass and its node.
// - Each step is one job on the S06 job system's Background lane, its rows shared with helpers. advance()
//   only polls it, so a slice costs the caller nearly nothing; cancel() and a new plan stop a running
//   step within a row.
// - Buffers are stored as RGBA16F stores them (R12), with a box mip chain when the plan gives one; the
//   outputs go through the same clamp and 8-bit rounding as the quantize pass. A sampler is bilinear and
//   repeats, as a GPU tier's is.
// - The same plan gives the same bytes on every run and with any number of threads (R13, AC6): a pixel
//   depends only on its own coordinates and on what earlier steps stored.
//
// Unlike a device tier, a result always holds the outputs' pixels, even for a plan that does not read
// back: the CPU tier has no device image to show instead.

#include "PassPlanner.h"
#include "ProceduralEvaluator.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::procmat {

    class SpirvImage;
    class SpirvProgram;

    // How the CPU tier is named in the UI and in results (device: {tier: "cpu", name})
    std::string cpuDeviceName();

    class ProceduralCpuEvaluator : public ProceduralEvaluator {
    public:
        ProceduralCpuEvaluator();
        // A running step stops at its next row; the job keeps what it reads alive until then
        ~ProceduralCpuEvaluator() override;

        EvaluatorTier tier() const override { return EvaluatorTier::Cpu; }
        std::string deviceName() const override { return cpuDeviceName(); }
        bool prepare(const PassPlan& plan, std::string& error) override;
        void setUniforms(uint32_t pass, const UniformBlock& block) override;
        bool advance(double budgetMs, EvaluationResult& result) override;
        void cancel() override;

        // The share of the evaluation done, 0 to 1, for a progress bar
        float progress() const;

    private:
        struct Step;
        enum class Phase { Idle, Compile, Draw, Quantize };

        bool compilePass(size_t pass, EvaluationResult& result);
        bool startStep(size_t step, EvaluationResult& result);
        void startQuantize();
        void stopRunning();
        void fail(EvaluationResult& result, const std::string& error, int pass, const std::string& log);

        PassPlan plan;
        Phase phase = Phase::Idle;
        size_t nextPass = 0;
        size_t nextStep = 0;
        // by fragment text: a rebuild keeps what did not change
        std::map<std::string, std::shared_ptr<const SpirvProgram>> programs;
        std::vector<std::shared_ptr<const SpirvProgram>> passPrograms;
        // what each Buffer holds now, and its size at this point of the plan
        std::vector<std::shared_ptr<const SpirvImage>> buffers;
        std::vector<int> bufferSizes;
        std::shared_ptr<Step> running;
    };

    // The whole evaluation of a plan on the CPU tier, on the calling thread with the job system's
    // workers: the entry point of a bake that runs with no window and no device (R39). `cancelled` is
    // polled between steps and rows; `progress` gets 0 to 1. False with `result.error` on failure.
    bool evaluateOnCpu(const PassPlan& plan, EvaluationResult& result, const std::function<bool()>& cancelled = {},
                       const std::function<void(float)>& progress = {});

    // Which node of a pass the CPU tier refuses (R11), when the pass inlines several: each of them is
    // planned alone with what it reads, and the smallest of those plans that does not load names it.
    // Empty when every node loads alone.
    std::string refusedNode(const graph::Graph& graph, const std::shared_ptr<const NodeLibrary>& library, const PlannedPass& pass,
                            const PlanOptions& options);

}

#endif
