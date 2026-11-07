// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALDEVICE_H
#define PROCEDURALDEVICE_H

// Which tier of S90 R8 the editor's process is on, and the evaluator that goes with it.
//
// A context on hardware is `gpu`, one on a software renderer `software`, and no context `cpu`. Vulkan
// says which it is (a CPU device type); GL only names its renderer, so the name is matched against the
// software renderers we know of, and a name we do not know counts as `gpu`.

#include "ProceduralEvaluator.h"

#include <memory>
#include <string>

namespace jasperengine::editor::procmat {

    // What a result, the UI and the bake's recipe show: device: {tier, name}
    struct EvaluatorDevice {
        EvaluatorTier tier = EvaluatorTier::Cpu;
        std::string name;
    };

    // The tier of a context from what it reports. `cpuDeviceType`: the API says the device is the CPU.
    EvaluatorTier tierOfContext(const std::string& rendererName, bool cpuDeviceType);

    // The editor's context now; the CPU tier when there is none
    EvaluatorDevice editorDevice();

    // An evaluator in R8's order for one graph: the context's tier, then the CPU's after a device failure
    // (ProceduralTieredEvaluator). Main thread; with no context, the CPU tier alone.
    std::unique_ptr<ProceduralEvaluator> makeEditorEvaluator();

}

#endif
