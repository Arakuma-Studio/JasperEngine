// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCMAT_TEST_IMAGEEVALUATOR_H
#define PROCMAT_TEST_IMAGEEVALUATOR_H

// An evaluator of the tests' own, on no device: it counts what it is asked and, `slices` advance()
// calls after a prepare or a uniform edit, gives every output of the plan as pixels whose red is the
// number of the evaluation (so a new image is told from the one before). Its tier is the test's.

#include "../../procmat/PassPlanner.h"
#include "../../procmat/ProceduralEvaluator.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::procmat::test {

    class ImageEvaluator : public ProceduralEvaluator {
    public:
        // what every evaluator one factory makes adds to
        struct Counts {
            int prepares = 0;
            int uniformEdits = 0;
            int evaluations = 0;
            int cancels = 0;
            int made = 0;
        };

        ImageEvaluator(std::shared_ptr<Counts> counts, EvaluatorTier tier, int slices) : counts(std::move(counts)), level(tier), slices(slices) {}

        EvaluatorTier tier() const override { return level; }
        std::string deviceName() const override { return "test device"; }
        bool prepare(const PassPlan& plan, std::string&) override {
            counts->prepares++;
            outputs.clear();
            for (const PlannedOutput& output : plan.outputs) outputs.push_back(output.name);
            size = plan.size;
            remaining = slices;
            running = true;
            return true;
        }
        void setUniforms(uint32_t, const UniformBlock&) override {
            counts->uniformEdits++;
            remaining = slices;
            running = true;
        }
        bool advance(double, EvaluationResult& result) override {
            result = EvaluationResult();
            if (!running) {
                // cancelled, or idle: an empty result
                return true;
            }
            if (--remaining > 0) return false;
            running = false;
            counts->evaluations++;
            result.ok = true;
            for (const std::string& name : outputs) {
                OutputImage image;
                image.name = name;
                image.width = size;
                image.height = size;
                image.rgba.assign((size_t)size * (size_t)size * 4, 255);
                image.rgba[0] = (uint8_t)counts->evaluations;
                result.outputs.push_back(std::move(image));
            }
            return true;
        }
        void cancel() override {
            if (running) counts->cancels++;
            running = false;
        }

    private:
        std::shared_ptr<Counts> counts;
        EvaluatorTier level;
        int slices = 1;
        int remaining = 0;
        bool running = false;
        int size = 0;
        std::vector<std::string> outputs;
    };

}

#endif
