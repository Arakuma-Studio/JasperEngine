// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCMAT_PLANINTERPRETER_H
#define PROCMAT_PLANINTERPRETER_H

// A plan run on SpirvInterpreter with plain threads, as R11 and R12 say a CPU tier runs one: the
// reference the node image test holds the GPU tiers to. A step's Buffer rounds to half precision, so
// nothing accumulates differently across passes. The product's CPU tier, with jobs and cancelling, is
// the evaluator of M5, not this.

#include "../../procmat/PassPlanner.h"
#include "../../procmat/SpirvInterpreter.h"

#include "shader/ShaderVariantCompiler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::procmat::test {

    inline float roundedToHalf(float value) {
        if (!std::isfinite(value) || value == 0.0f) return value;
        const float magnitude = std::fabs(value);
        // past the largest half, what rounds up to 65536 is infinity
        if (magnitude >= 65520.0f) return std::copysign(HUGE_VALF, value);
        int exponent = 0;
        std::frexp(magnitude, &exponent);
        // 11 significant bits, and subnormals stop at 2^-24
        const float step = std::ldexp(1.0f, std::max(exponent - 11, -24));
        return std::copysign(std::nearbyint(magnitude / step) * step, value);
    }

    inline uint8_t quantized(float value) {
        const float clamped = value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f; // NaN goes to 0
        return (uint8_t)std::floor(clamped * 255.0f + 0.5f);
    }

    inline bool runPlanOnInterpreter(const PassPlan& plan, int threads, std::vector<OutputImage>& outputs, std::string& error) {
        std::vector<std::unique_ptr<SpirvProgram>> programs;
        for (const PlannedPass& pass : plan.passes) {
            std::vector<uint32_t> words;
            std::string log;
            if (!ShaderVariantCompiler::compileFragmentToSpirv(pass.fragment, words, &error, &log)) {
                error = pass.label + ": " + error + "\n" + log;
                return false;
            }
            programs.push_back(SpirvProgram::load(words.data(), words.size(), error));
            if (!programs.back()) {
                error = pass.label + ": " + error;
                return false;
            }
        }
        std::vector<std::shared_ptr<const SpirvImage>> buffers(plan.buffers.size());
        std::vector<int> sizes(plan.buffers.size(), 0);
        for (const PlannedStep& step : plan.steps) {
            const size_t texels = (size_t)step.size * step.size;
            std::vector<float> rgba(texels * 4, 0.0f);
            if (step.pass >= 0) {
                const PlannedPass& pass = plan.passes[(size_t)step.pass];
                const SpirvProgram& program = *programs[(size_t)step.pass];
                std::vector<float> block = pass.uniforms.values;
                for (const PlannedSize& size : pass.sizes) {
                    if (size.index < block.size() && sizes[(size_t)size.buffer] > 0) block[size.index] = (float)sizes[(size_t)size.buffer];
                }
                std::atomic<int> nextRow{0};
                const auto worker = [&]() {
                    SpirvMachine machine(program);
                    machine.setUniforms(block.data(), block.size());
                    for (const PlannedTexture& texture : pass.textures) {
                        const int slot = program.textureSlot("t_" + texture.name);
                        if (slot >= 0) machine.setTexture(slot, buffers[(size_t)texture.buffer].get());
                    }
                    for (;;) {
                        const int y = nextRow.fetch_add(1);
                        if (y >= step.size) return;
                        const float v = ((float)y + 0.5f) / (float)step.size;
                        for (int x = 0; x < step.size; x++) {
                            machine.shade(((float)x + 0.5f) / (float)step.size, v, &rgba[((size_t)y * step.size + x) * 4]);
                        }
                    }
                };
                std::vector<std::thread> pool;
                for (int i = 0; i < threads; i++) pool.emplace_back(worker);
                for (std::thread& thread : pool) thread.join();
                for (float& value : rgba) value = roundedToHalf(value);
            }
            buffers[(size_t)step.buffer] = std::make_shared<SpirvImage>(step.size, step.size, std::move(rgba));
            sizes[(size_t)step.buffer] = step.size;
        }
        outputs.clear();
        for (const PlannedOutput& output : plan.outputs) {
            OutputImage image;
            image.name = output.name;
            image.width = plan.size;
            image.height = plan.size;
            image.rgba.resize((size_t)plan.size * plan.size * 4);
            for (int y = 0; y < plan.size; y++) {
                for (int x = 0; x < plan.size; x++) {
                    float texel[4];
                    buffers[(size_t)output.buffer]->sampleBilinear(((float)x + 0.5f) / (float)plan.size, ((float)y + 0.5f) / (float)plan.size, 0, texel);
                    for (int k = 0; k < 4; k++) image.rgba[((size_t)y * plan.size + x) * 4 + k] = quantized(texel[k]);
                }
            }
            outputs.push_back(std::move(image));
        }
        return true;
    }

    // AC8's numbers for one output, the worst channel's, in 8-bit steps
    struct ImageDifference {
        double mean = 0.0;
        int p99 = 0;
        int maximum = 0;

        bool withinAc8() const { return mean <= 2.0 && p99 <= 4; }
    };

    inline ImageDifference differenceOf(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        ImageDifference result;
        const size_t pixels = a.size() / 4;
        for (int channel = 0; channel < 4; channel++) {
            int histogram[256] = {};
            for (size_t i = (size_t)channel; i < a.size() && i < b.size(); i += 4) histogram[std::abs((int)a[i] - (int)b[i])]++;
            size_t sum = 0, seen = 0;
            int p99 = -1, maximum = 0;
            for (int d = 0; d < 256; d++) {
                sum += (size_t)histogram[d] * d;
                seen += histogram[d];
                if (histogram[d]) maximum = d;
                if (p99 < 0 && seen * 100 >= pixels * 99) p99 = d;
            }
            result.mean = std::max(result.mean, (double)sum / (double)pixels);
            result.p99 = std::max(result.p99, p99);
            result.maximum = std::max(result.maximum, maximum);
        }
        return result;
    }

}

#endif
