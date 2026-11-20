// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What get_frame_times says next about its numbers (docs/specs/release-performance-and-gpu-quirks.md
// R34), apart from the tool so the rule is tested alone (ProfilingHints.*).

#include <string>

namespace jasperengine::editor::mcp {

    // cpuMs: the frames' CPU ms (median); mainThreadCpuMs: the frame thread's CPU per frame from its
    // thread clock (< 0: unavailable, the web). "" when there is nothing to say.
    std::string frameTimesHint(double cpuMs, double mainThreadCpuMs);

}
