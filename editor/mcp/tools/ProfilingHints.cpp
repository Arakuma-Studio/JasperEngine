// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProfilingHints.h"

namespace jasperengine::editor::mcp {

std::string frameTimesHint(double cpuMs, double mainThreadCpuMs){
    // the frame's CPU time counts its waits; the thread clock only what the thread computed
    if (mainThreadCpuMs < 0.0 || cpuMs <= 0.0 || mainThreadCpuMs >= 0.6 * cpuMs)
        return std::string();
    return "The frame thread waits more than it computes (driver, locks, I/O): see get_gpu_passes and the Timeline's Wait band.";
}

}
