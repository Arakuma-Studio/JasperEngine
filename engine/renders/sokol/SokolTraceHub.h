// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolTraceHub_h
#define SokolTraceHub_h

// The one owner of sokol's trace hooks (spec S15 §6.4). It installs them once at setup, chains
// to the table that was installed before (sg_install_trace_hooks returns it) and feeds the pass
// recorder (SokolPassRecorder) and, with the profiler compiled in, the VRAM tracker
// (SokolVramTracker). Release games (JASPERENGINE_PROFILER=0) install only the pass recorder's
// subset, and only when JASPERENGINE_FRAME_STATS asks for the pass list.

namespace jasperengine{

    class SokolTraceHub{
    public:
        // after sg_setup
        static void install(bool frameStats);
        // before sg_shutdown: restores the previous table
        static void uninstall();
        static bool isInstalled();
    };

}

#endif //SokolTraceHub_h
