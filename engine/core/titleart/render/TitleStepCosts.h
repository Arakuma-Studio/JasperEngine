// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R45, §6.3, §6.13, Deviation 54)
// - Purpose: what render steps really cost on the main thread, learned per kind of step, so a
//   scheduler that slices jobs by a time budget (TitleRenderJob::step with a model, the editor's
//   TitlePreviewQueue, S81's scheduler) predicts from measurements instead of trusting the static
//   estimates (pixels x costWeight, cpuUs). Those are desktop-GPU figures: on a software
//   rasterizer the GPU passes and the readback run on the main thread far above them.
// - Model: per kind a ratio of measured to static estimate, so a kind learned at one size predicts
//   another; a kind never measured uses the ratio of every kind (the machine's), a fresh model 1.
//   The ratio rises fast on a slower measurement and falls slower, so one quick run does not
//   undo what slow ones taught. Kinds are capped (MAX_KINDS); past the cap only the machine's
//   ratio learns.
// - Kind of a step: its StepKind and its label without the " tile i/n" a split adds (labels name
//   the pass, not the document: "flood", "outer_glow blur x"). Callers may also measure work of
//   their own under a name of their own (the preview's "preview build", "preview readback").
// - Clock: nowUs() is the clock jobs measure with (steady_clock; tests pass a fake one).
// - GPU gate: a software rasterizer draws a pass on its own threads, but the main thread waits for
//   it when a later pass samples its target (or at the readback): a 128 px thumbnail's stack passes
//   cost 150-230 ms there. With a gate (busy: the GPU has not passed the fence; fence: one after
//   what was submitted) a job fences after each GPU step and does not start one, nor the
//   readback, while the GPU is busy: it waits for a later frame instead of blocking this one.
//   No gate elsewhere (a hardware GPU orders its own work without the CPU waiting).
// - Thread-safety: main thread (the jobs that use it run there).

#pragma once

#include "Export.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace jasperengine::titleart {

    struct TitleRenderStep;

    class JASPERENGINE_API TitleStepCosts {
    public:
        static constexpr size_t MAX_KINDS = 512;

        explicit TitleStepCosts(std::function<double()> clockUs = {});

        double nowUs() const { return clock(); }
        void setGpuGate(std::function<bool()> busyFn, std::function<void()> fenceFn) {
            busy = std::move(busyFn);
            fence = std::move(fenceFn);
        }
        bool gpuBusy() const { return busy && busy(); }
        void fenceGpu() const { if (fence) fence(); }

        // the predicted main-thread cost in us of `estimateUs` of static estimate (at least 1)
        uint32_t predictUs(const std::string& kind, uint32_t estimateUs) const;
        uint32_t predictUs(const TitleRenderStep& step) const;
        // a measurement of work whose static estimate was `estimateUs`
        void observe(const std::string& kind, uint32_t estimateUs, double measuredUs);
        void observe(const TitleRenderStep& step, double measuredUs);

        static std::string kindOf(const TitleRenderStep& step);

        // the learned ratio of a kind (the machine's when it has none), and the machine's
        double ratio(const std::string& kind) const;
        double machineRatio() const { return machine.ratio; }
        uint64_t observations() const { return machine.count; }
        size_t kinds() const { return byKind.size(); }

    private:
        struct Ratio {
            double ratio = 1.0;
            uint64_t count = 0;
            void learn(double observed);
        };
        std::function<double()> clock;
        std::function<bool()> busy;
        std::function<void()> fence;
        Ratio machine;
        std::map<std::string, Ratio> byKind;
    };

}
