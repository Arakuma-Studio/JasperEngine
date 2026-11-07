// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralCpuEvaluator.h"

#include "ProceduralMaterialKind.h"
#include "SpirvInterpreter.h"

#include "jobs/JobSystem.h"
#include "shader/ShaderVariantCompiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    uint8_t quantizedByte(float value) {
        // the quantize pass clamps in the shader (R12); NaN went to 0 in jp_finite already, and here too
        const float clamped = value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
        return (uint8_t)std::floor(clamped * 255.0f + 0.5f);
    }

    std::string nodesOf(const PlannedPass& pass) {
        if (pass.nodes.empty()) return pass.label;
        if (pass.nodes.size() == 1) return "node " + pass.nodes.front();
        std::string text = "one of nodes ";
        for (size_t i = 0; i < pass.nodes.size(); i++) text += (i ? ", " : "") + pass.nodes[i];
        return text;
    }

    bool loadPass(const PlannedPass& pass, std::shared_ptr<const SpirvProgram>& program, std::string& error, std::string& log, bool& refused) {
        refused = false;
        std::vector<uint32_t> words;
        if (!ShaderVariantCompiler::compileFragmentToSpirv(pass.fragment, words, &error, &log)) return false;
        std::unique_ptr<SpirvProgram> loaded = SpirvProgram::load(words.data(), words.size(), error);
        if (!loaded) {
            refused = true;
            return false;
        }
        program = std::move(loaded);
        return true;
    }

}

// One step in flight. The job holds it too, so a step a new plan dropped finishes on its own data.
struct ProceduralCpuEvaluator::Step {
    size_t index = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> rowsDone{0};
    int rows = 0;
    // filled by the job, read once `task` is ready
    std::shared_ptr<const SpirvImage> image;
    std::vector<OutputImage> outputs;
    std::string failure;
    jobs::TaskRef task;
};

std::string jasperengine::editor::procmat::cpuDeviceName() {
    return "SPIR-V interpreter on the CPU";
}

ProceduralCpuEvaluator::ProceduralCpuEvaluator() = default;

ProceduralCpuEvaluator::~ProceduralCpuEvaluator() {
    stopRunning();
}

void ProceduralCpuEvaluator::stopRunning() {
    if (running) running->stop = true;
    running.reset();
}

bool ProceduralCpuEvaluator::prepare(const PassPlan& newPlan, std::string& error) {
    // the jobs index with these, so a plan that would read out of bounds is refused here
    const auto bufferOk = [&](int buffer) { return buffer >= 0 && (size_t)buffer < newPlan.buffers.size(); };
    if (newPlan.size <= 0) {
        error = "the plan has no size";
        return false;
    }
    for (const PlannedStep& step : newPlan.steps) {
        if (!bufferOk(step.buffer) || step.size <= 0 || step.pass >= (int)newPlan.passes.size()) {
            error = "the plan has a step outside its passes or buffers";
            return false;
        }
    }
    for (const PlannedPass& pass : newPlan.passes) {
        for (const PlannedTexture& texture : pass.textures) {
            if (!bufferOk(texture.buffer)) {
                error = pass.label + " reads a buffer the plan does not have";
                return false;
            }
        }
        for (const PlannedSize& size : pass.sizes) {
            if (!bufferOk(size.buffer)) {
                error = pass.label + " reads the size of a buffer the plan does not have";
                return false;
            }
        }
    }
    for (const PlannedOutput& output : newPlan.outputs) {
        if (!bufferOk(output.buffer)) {
            error = "output " + output.name + " reads a buffer the plan does not have";
            return false;
        }
    }
    stopRunning();
    plan = newPlan;
    std::map<std::string, std::shared_ptr<const SpirvProgram>> kept;
    for (const PlannedPass& pass : plan.passes) {
        auto it = programs.find(pass.fragment);
        if (it != programs.end()) kept[pass.fragment] = it->second;
    }
    programs = std::move(kept);
    passPrograms.assign(plan.passes.size(), nullptr);
    buffers.assign(plan.buffers.size(), nullptr);
    bufferSizes.assign(plan.buffers.size(), 0);
    nextPass = 0;
    nextStep = 0;
    phase = Phase::Compile;
    return true;
}

void ProceduralCpuEvaluator::setUniforms(uint32_t pass, const UniformBlock& block) {
    if (pass >= plan.passes.size()) return;
    plan.passes[pass].uniforms = block;
    const size_t first = firstStepToRedraw(plan, pass);
    if (first == plan.steps.size()) return;
    // a step before the first to redraw stays valid; anything later reads what is about to change
    if (running && (phase == Phase::Quantize || running->index >= first)) stopRunning();
    nextStep = std::min(nextStep, first);
    phase = nextPass < plan.passes.size() ? Phase::Compile : Phase::Draw;
}

void ProceduralCpuEvaluator::cancel() {
    stopRunning();
    phase = Phase::Idle;
}

float ProceduralCpuEvaluator::progress() const {
    const size_t units = plan.passes.size() + plan.steps.size() + 1;
    double done = 0.0;
    switch (phase) {
        case Phase::Idle: return 1.0f;
        case Phase::Compile: done = (double)nextPass; break;
        case Phase::Draw:
            done = (double)plan.passes.size() + (double)nextStep;
            if (running && running->rows > 0) done += (double)running->rowsDone.load() / running->rows;
            break;
        case Phase::Quantize: done = (double)(plan.passes.size() + plan.steps.size()); break;
    }
    return (float)std::min(1.0, done / (double)units);
}

void ProceduralCpuEvaluator::fail(EvaluationResult& result, const std::string& error, int pass, const std::string& log) {
    result = EvaluationResult();
    result.error = error;
    result.failedPass = pass;
    result.log = log;
    stopRunning();
    phase = Phase::Idle;
}

bool ProceduralCpuEvaluator::compilePass(size_t index, EvaluationResult& result) {
    const PlannedPass& pass = plan.passes[index];
    std::shared_ptr<const SpirvProgram>& program = programs[pass.fragment];
    if (!program) {
        std::string error, log;
        bool refused = false;
        if (!loadPass(pass, program, error, log, refused)) {
            programs.erase(pass.fragment);
            if (refused) {
                // found before any pixel runs (R11), with what the user can act on: the node
                fail(result, pass.label + ": the CPU tier cannot run " + nodesOf(pass) + ": " + error, (int)index, log);
                result.unsupported = true;
                if (pass.nodes.size() == 1) result.failedNode = pass.nodes.front();
            } else {
                fail(result, pass.label + ": " + error, (int)index, log);
            }
            return false;
        }
    }
    passPrograms[index] = program;
    return true;
}

bool ProceduralCpuEvaluator::startStep(size_t index, EvaluationResult& result) {
    const PlannedStep& planned = plan.steps[index];
    auto step = std::make_shared<Step>();
    step->index = index;
    step->rows = planned.size;
    const int size = planned.size;
    const bool mips = plan.buffers[(size_t)planned.buffer].mips;

    std::shared_ptr<const SpirvProgram> program;
    std::vector<float> block;
    // the images a pass reads, by texture slot: a job never touches the evaluator's own table
    std::vector<std::pair<int, std::shared_ptr<const SpirvImage>>> textures;
    if (planned.pass >= 0) {
        const PlannedPass& pass = plan.passes[(size_t)planned.pass];
        program = passPrograms[(size_t)planned.pass];
        block = pass.uniforms.values;
        // the size each Buffer read here has now: a shrinking Iterate Buffer's is not the one generated
        for (const PlannedSize& sized : pass.sizes) {
            if (sized.index < block.size() && bufferSizes[(size_t)sized.buffer] > 0) block[sized.index] = (float)bufferSizes[(size_t)sized.buffer];
        }
        for (const PlannedTexture& texture : pass.textures) {
            const int slot = program->textureSlot("t_" + texture.name);
            // a texture a pass names only for its size is optimized out of the shader: nothing to bind
            if (slot < 0) continue;
            if (!buffers[(size_t)texture.buffer]) {
                fail(result, pass.label + " reads a buffer that was never drawn", planned.pass, {});
                return false;
            }
            textures.push_back({slot, buffers[(size_t)texture.buffer]});
        }
    }

    const auto body = [step, program, block = std::move(block), textures = std::move(textures), size, mips]() {
        std::vector<float> rgba((size_t)size * size * 4, 0.0f);
        if (program) {
            std::mutex failureLock;
            const std::function<bool()> stopped = [&]() { return step->stop.load(std::memory_order_relaxed) || jobs::system().isShuttingDown(); };
            jobs::ParallelForOptions options;
            options.lane = jobs::Lane::Background;
            options.stop = &stopped;
            // a row is one unit: a pixel depends on nothing another row computes, so how rows are
            // shared between threads never changes a byte
            jobs::parallelFor((size_t)size, [&](size_t y) {
                SpirvMachine machine(*program);
                machine.setUniforms(block.data(), block.size());
                for (const auto& [slot, image] : textures) machine.setTexture(slot, image.get());
                const float v = ((float)y + 0.5f) / (float)size;
                float* row = &rgba[y * (size_t)size * 4];
                for (int x = 0; x < size; x++) {
                    if (machine.shade(((float)x + 0.5f) / (float)size, v, row + (size_t)x * 4) != SpirvMachine::Result::Ok) {
                        std::lock_guard<std::mutex> lock(failureLock);
                        if (step->failure.empty()) {
                            step->failure = "a loop ran past the interpreter's limit at pixel (" + std::to_string(x) + ", " + std::to_string(y) + ")";
                        }
                        step->stop = true;
                        return false;
                    }
                }
                step->rowsDone.fetch_add(1, std::memory_order_relaxed);
                return true;
            }, options);
        }
        if (step->stop.load()) return;
        step->image = std::make_shared<SpirvImage>(size, size, std::move(rgba), mips, true);
    };
    jobs::SubmitOptions submit;
    submit.lane = jobs::Lane::Background;
    submit.gate = jobs::GateClass::Heavy;
    submit.name = "procmat-cpu-step";
    step->task = jobs::submit(body, submit);
    running = std::move(step);
    return true;
}

void ProceduralCpuEvaluator::startQuantize() {
    auto step = std::make_shared<Step>();
    step->index = plan.steps.size();
    std::vector<std::pair<std::string, std::shared_ptr<const SpirvImage>>> sources;
    for (const PlannedOutput& output : plan.outputs) sources.push_back({output.name, buffers[(size_t)output.buffer]});
    const int size = plan.size;
    const auto body = [step, sources = std::move(sources), size]() {
        for (const auto& [name, image] : sources) {
            if (step->stop.load()) return;
            OutputImage out;
            out.name = name;
            out.width = size;
            out.height = size;
            out.rgba.resize((size_t)size * size * 4);
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    // the quantize pass samples its source at texel centres, bilinear at level 0
                    float texel[4];
                    image->sampleBilinear(((float)x + 0.5f) / (float)size, ((float)y + 0.5f) / (float)size, 0, texel);
                    for (int k = 0; k < 4; k++) out.rgba[((size_t)y * size + x) * 4 + k] = quantizedByte(texel[k]);
                }
            }
            step->outputs.push_back(std::move(out));
        }
    };
    jobs::SubmitOptions submit;
    submit.lane = jobs::Lane::Background;
    submit.name = "procmat-cpu-quantize";
    step->task = jobs::submit(body, submit);
    running = std::move(step);
}

bool ProceduralCpuEvaluator::advance(double budgetMs, EvaluationResult& result) {
    const auto start = std::chrono::steady_clock::now();
    const auto outOfBudget = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs;
    };
    // with no workers nothing runs at submit: the job system runs it here, within the slice
    const auto ready = [&]() {
        if (!running->task.isReady() && jobs::system().isSingleThreaded()) jobs::pump(std::max(budgetMs, 1.0));
        return running->task.isReady();
    };
    for (;;) {
        switch (phase) {
            case Phase::Idle:
                result = EvaluationResult();
                return true;
            case Phase::Compile:
                if (nextPass >= plan.passes.size()) {
                    phase = Phase::Draw;
                    continue;
                }
                if (!compilePass(nextPass, result)) return true;
                nextPass++;
                break;
            case Phase::Draw:
                if (running) {
                    if (!ready()) return false;
                    const std::shared_ptr<Step> done = std::move(running);
                    const PlannedStep& step = plan.steps[done->index];
                    if (!done->image) {
                        const std::string label = step.pass >= 0 ? plan.passes[(size_t)step.pass].label : "a cleared buffer";
                        fail(result, label + ": " + (done->failure.empty() ? std::string("the job system stopped it") : done->failure), step.pass, {});
                        return true;
                    }
                    buffers[(size_t)step.buffer] = done->image;
                    bufferSizes[(size_t)step.buffer] = step.size;
                    nextStep = done->index + 1;
                    break;
                }
                if (nextStep >= plan.steps.size()) {
                    phase = Phase::Quantize;
                    startQuantize();
                    continue;
                }
                if (!startStep(nextStep, result)) return true;
                break;
            case Phase::Quantize: {
                if (!ready()) return false;
                const std::shared_ptr<Step> done = std::move(running);
                result = EvaluationResult();
                result.ok = true;
                result.outputs = std::move(done->outputs);
                phase = Phase::Idle;
                return true;
            }
        }
        if (outOfBudget()) return false;
    }
}

bool jasperengine::editor::procmat::evaluateOnCpu(const PassPlan& plan, EvaluationResult& result, const std::function<bool()>& cancelled,
                                                  const std::function<void(float)>& progress) {
    ProceduralCpuEvaluator evaluator;
    std::string error;
    if (!evaluator.prepare(plan, error)) {
        result = EvaluationResult();
        result.error = error;
        return false;
    }
    while (!evaluator.advance(50.0, result)) {
        if (cancelled && cancelled()) {
            evaluator.cancel();
            result = EvaluationResult();
            result.error = "Cancelled.";
            return false;
        }
        if (progress) progress(evaluator.progress());
        // the work runs on the workers: this thread only waits for the step in hand
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (progress && result.ok) progress(1.0f);
    return result.ok;
}

std::string jasperengine::editor::procmat::refusedNode(const graph::Graph& graph, const std::shared_ptr<const NodeLibrary>& library,
                                                       const PlannedPass& pass, const PlanOptions& options) {
    std::string culprit;
    size_t smallest = SIZE_MAX;
    for (const std::string& id : pass.nodes) {
        const graph::Node* node = nullptr;
        for (const graph::Node& candidate : graph.nodes) {
            if (candidate.id == id) node = &candidate;
        }
        const Json* descriptor = node ? library->descriptor(node->type) : nullptr;
        if (!descriptor) continue;
        std::string port;
        for (const graph::Port& candidate : nodePorts(*descriptor, 1)) {
            if (candidate.direction == graph::PortDirection::Out) {
                port = candidate.id;
                break;
            }
        }
        if (port.empty()) continue;
        PlanOptions alone = options;
        alone.readBack = false;
        PassPlan plan;
        std::string error;
        if (!planGraph(graph, library, {{"probe", id, port}}, alone, plan, error)) continue;
        // a node's plan holds what it reads, so the smallest refused one is the node itself
        for (const PlannedPass& planned : plan.passes) {
            std::shared_ptr<const SpirvProgram> program;
            std::string log;
            bool refused = false;
            if (!loadPass(planned, program, error, log, refused) && refused && planned.nodes.size() < smallest) {
                smallest = planned.nodes.size();
                culprit = id;
            }
        }
    }
    return culprit;
}
