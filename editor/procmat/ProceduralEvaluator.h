// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALEVALUATOR_H
#define PROCEDURALEVALUATOR_H

// How a Procedural Material's passes run on a device (docs/specs/S90-procedural-material.md, D-section).
//
// A PassPlan (PassPlanner.h) is what to draw; a ProceduralEvaluator draws it on one device. There is
// one implementation per way of running shaders: ProceduralGpuEvaluator for the tiers on the editor's
// context, and the SPIR-V interpreter's for the CPU. Windows, the bake, previews and tools all talk to
// this interface and to ProceduralSession, so they never know which tier they are on.
//
// The contract, for every implementation:
// - prepare() takes a new plan: anything from the previous plan may be reused or dropped. It only
//   refuses a plan the device can never run; compiling happens inside advance(), so a failed pass is
//   reported there with its index and log (R9).
// - setUniforms() changes the block of one pass of the prepared plan, with no compile (R7). The next
//   advance() redraws from the first step that pass affects. The text of the plan must not change: a
//   plan with other text or other sizes goes through prepare().
// - advance() does some work and returns false while more remains. A slice stops once budgetMs is used,
//   after the unit of work in hand: a compile or a draw is not split. On true, `result` holds the outcome
//   and the evaluator is idle until the next prepare() or setUniforms().
// - cancel() drops the evaluation in progress: nothing more is drawn, and advance() returns true with an
//   empty result. The prepared plan stays, so setUniforms() can restart it.
// - All calls come from one thread. A GPU implementation needs that to be the render thread, outside any
//   render pass, once per frame: its read back waits for the frame that drew to be committed.

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::procmat {

    struct PassPlan;

    // The names shown in the UI and in results (R8)
    enum class EvaluatorTier : uint8_t { Gpu, Software, Cpu };

    inline const char* evaluatorTierName(EvaluatorTier tier) {
        switch (tier) {
            case EvaluatorTier::Gpu: return "gpu";
            case EvaluatorTier::Software: return "software";
            case EvaluatorTier::Cpu: return "cpu";
        }
        return "gpu";
    }

    // A pass's uniform block: vec4s, four floats each. Shorter than the block is padded with zeros.
    struct UniformBlock {
        std::vector<float> values;

        bool operator==(const UniformBlock&) const = default;
    };

    // One output as RGBA8 (R12): width * height * 4 bytes, the row at v = 0 first
    struct OutputImage {
        std::string name;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };

    struct EvaluationResult {
        bool ok = false;
        // not ok: what failed. A device failure names its pass (R9): failedPass and the compiler's log
        std::string error;
        int failedPass = -1;
        std::string log;
        // In plan order; empty when the plan does not read back (PassPlan::readBack) or on failure
        std::vector<OutputImage> outputs;
    };

    class ProceduralEvaluator {
    public:
        virtual ~ProceduralEvaluator() = default;
        virtual EvaluatorTier tier() const = 0;
        virtual std::string deviceName() const = 0;
        virtual bool prepare(const PassPlan& plan, std::string& error) = 0;
        virtual void setUniforms(uint32_t pass, const UniformBlock& block) = 0;
        // false when more work remains; a slice stays within budgetMs
        virtual bool advance(double budgetMs, EvaluationResult& result) = 0;
        virtual void cancel() = 0;
    };

}

#endif
