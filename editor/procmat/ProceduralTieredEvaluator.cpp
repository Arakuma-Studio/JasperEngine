// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralTieredEvaluator.h"

using namespace jasperengine::editor::procmat;

ProceduralTieredEvaluator::ProceduralTieredEvaluator(std::unique_ptr<ProceduralEvaluator> device) : device(std::move(device)) {
    onCpu = !this->device;
}

EvaluatorTier ProceduralTieredEvaluator::tier() const {
    return onCpu ? EvaluatorTier::Cpu : device->tier();
}

std::string ProceduralTieredEvaluator::deviceName() const {
    return onCpu ? cpu.deviceName() : device->deviceName();
}

void ProceduralTieredEvaluator::fallBack(const std::string& why) {
    onCpu = true;
    warning = why + " Continuing on the CPU tier (" + cpu.deviceName() + "), which is slower.";
    // whatever it had started stops; its images are not shown again
    device->cancel();
}

bool ProceduralTieredEvaluator::prepare(const PassPlan& newPlan, std::string& error) {
    plan = newPlan;
    if (!onCpu) {
        std::string deviceError;
        if (device->prepare(newPlan, deviceError)) return true;
        fallBack("The " + device->deviceName() + " device cannot run Procedural Material passes: " + deviceError + ".");
    }
    return cpu.prepare(newPlan, error);
}

void ProceduralTieredEvaluator::setUniforms(uint32_t pass, const UniformBlock& block) {
    if (plan && pass < plan->passes.size()) plan->passes[pass].uniforms = block;
    if (onCpu) cpu.setUniforms(pass, block);
    else device->setUniforms(pass, block);
}

void ProceduralTieredEvaluator::cancel() {
    if (onCpu) cpu.cancel();
    else device->cancel();
}

bool ProceduralTieredEvaluator::advance(double budgetMs, EvaluationResult& result) {
    if (!onCpu) {
        if (!device->advance(budgetMs, result)) return false;
        // ok, or an empty result after a cancel: nothing to recover from
        if (result.ok || result.error.empty() || !plan) return true;
        const std::string name = device->deviceName();
        std::string why;
        if (result.failedPass >= 0 && (size_t)result.failedPass < plan->passes.size()) {
            why = "Pass " + std::to_string(result.failedPass) + " (" + plan->passes[(size_t)result.failedPass].label + ") failed on " + name + ": " + result.error + ".";
        } else {
            why = "The " + name + " device failed: " + result.error + ".";
        }
        if (!result.log.empty()) why += " Compiler log: " + result.log;
        fallBack(why);
        std::string error;
        if (!cpu.prepare(*plan, error)) {
            result = EvaluationResult();
            result.error = warning + " " + error;
            return true;
        }
        result = EvaluationResult();
        // the CPU tier's first slice waits for the next call: this one spent its budget on the device
        return false;
    }
    if (!cpu.advance(budgetMs, result)) return false;
    if (!warning.empty() && (result.ok || !result.error.empty())) result.warnings.push_back(warning);
    return true;
}
