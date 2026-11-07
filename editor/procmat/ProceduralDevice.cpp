// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralDevice.h"

#include "ProceduralCpuEvaluator.h"
#include "ProceduralGpuEvaluator.h"
#include "ProceduralTieredEvaluator.h"

#include "render/SystemRender.h"

#include <algorithm>
#include <cctype>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    // GL_RENDERER (and device names) of the software renderers a desktop may provide: Mesa's llvmpipe,
    // softpipe and swrast, lavapipe, SwiftShader, WARP and Apple's software renderer
    const char* const SOFTWARE_RENDERERS[] = {
        "llvmpipe", "softpipe", "swrast", "lavapipe", "swiftshader", "microsoft basic render", "apple software renderer",
    };

    bool hasContext() {
        const char* backend = SystemRender::getBackendName();
        return std::strcmp(backend, "none") != 0 && std::strcmp(backend, "dummy") != 0;
    }

}

EvaluatorTier jasperengine::editor::procmat::tierOfContext(const std::string& rendererName, bool cpuDeviceType) {
    if (cpuDeviceType) return EvaluatorTier::Software;
    std::string name = rendererName;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (const char* software : SOFTWARE_RENDERERS) {
        if (name.find(software) != std::string::npos) return EvaluatorTier::Software;
    }
    return EvaluatorTier::Gpu;
}

EvaluatorDevice jasperengine::editor::procmat::editorDevice() {
    if (!hasContext()) return {EvaluatorTier::Cpu, cpuDeviceName()};
    const std::string name = SystemRender::getDeviceName();
    return {tierOfContext(name, SystemRender::isCpuDevice()), name};
}

std::unique_ptr<ProceduralEvaluator> jasperengine::editor::procmat::makeEditorEvaluator() {
    const EvaluatorDevice device = editorDevice();
    if (device.tier == EvaluatorTier::Cpu) return std::make_unique<ProceduralTieredEvaluator>(nullptr);
    return std::make_unique<ProceduralTieredEvaluator>(std::make_unique<ProceduralGpuEvaluator>(device.tier));
}
