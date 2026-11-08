// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALTIEREDEVALUATOR_H
#define PROCEDURALTIEREDEVALUATOR_H

// The order of S90 R8 for one graph: the device's evaluator while it works, the CPU tier from the
// first pass it fails to compile or read back (R9).
//
// - The failed evaluation starts over on the CPU tier from the plan as it is now, so no pixel of the
//   device's attempt, nor of an earlier image, reaches the result.
// - Its result, and every later one, carries a warning naming the pass, the device and the compiler log.
// - It stays on the CPU tier: a live editor cannot change its renderer, so it never tries a software
//   tier instead (D4). A tier is the property of a process; a new graph gets a new evaluator.
// - Without a device (no context), it is the CPU tier from the start, with no warning.

#include "PassPlanner.h"
#include "ProceduralCpuEvaluator.h"
#include "ProceduralEvaluator.h"

#include <memory>
#include <optional>
#include <string>

namespace jasperengine::editor::procmat {

    class ProceduralTieredEvaluator : public ProceduralEvaluator {
    public:
        // `device`: tier 1 or 2 on the editor's context, null when there is none
        explicit ProceduralTieredEvaluator(std::unique_ptr<ProceduralEvaluator> device);

        EvaluatorTier tier() const override;
        std::string deviceName() const override;
        bool prepare(const PassPlan& plan, std::string& error) override;
        void setUniforms(uint32_t pass, const UniformBlock& block) override;
        bool advance(double budgetMs, EvaluationResult& result) override;
        void cancel() override;

        // The device's evaluator while it is the one drawing, for a preview that shows its images without
        // reading them back; null once on the CPU tier, whose results hold the pixels
        ProceduralEvaluator* deviceEvaluator() const { return onCpu ? nullptr : device.get(); }
        // why the CPU tier is drawing, empty while the device is
        const std::string& fallbackWarning() const { return warning; }
        float progress() const { return onCpu ? cpu.progress() : 0.0f; }

    private:
        void fallBack(const std::string& why);

        std::unique_ptr<ProceduralEvaluator> device;
        ProceduralCpuEvaluator cpu;
        bool onCpu = false;
        std::string warning;
        // the plan as it is now, uniform edits included: what the CPU tier starts over from
        std::optional<PassPlan> plan;
    };

}

#endif
