// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// get_frame_times' hint (docs/specs/release-performance-and-gpu-quirks.md R34, AC10): when the
// frame thread's CPU (its thread clock) is under 60 % of the frame's CPU time, the frame waits more
// than it computes, and the agent is told where to look; nothing otherwise, nothing on the web.

#include <gtest/gtest.h>

#include "mcp/tools/ProfilingHints.h"

namespace ProfilingHintsTest_cpp {

using namespace jasperengine::editor::mcp;

TEST(ProfilingHints, WaitingWhenThreadCpuIsUnder60Percent){
    EXPECT_EQ(frameTimesHint(30.0, 10.0),
              "The frame thread waits more than it computes (driver, locks, I/O): see get_gpu_passes and the Timeline's Wait band.");
    EXPECT_EQ(frameTimesHint(30.0, 25.0), "");
    EXPECT_EQ(frameTimesHint(30.0, 18.0), "") << "60 % exactly computes enough";
    // the web has no thread clock
    EXPECT_EQ(frameTimesHint(30.0, -1.0), "");
}

}  // namespace ProfilingHintsTest_cpp
