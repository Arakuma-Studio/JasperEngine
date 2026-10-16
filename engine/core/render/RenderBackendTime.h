// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RENDERBACKENDTIME_H
#define RENDERBACKENDTIME_H

#include <chrono>
#include <cstdint>

namespace jasperengine {

    // The time the calling thread spends in the graphics backend (sokol and the driver under it:
    // passes, pipelines, bindings, uniforms, draws, buffer updates, the frame commit), measured only
    // while enabled (tests; off, a scope costs one branch). Phase 5 (p5-tooling): EffectPerfTest
    // gates the engine's own main-thread work on glcore, the frame's time less this, since on
    // llvmpipe the driver's per-pass scene setup is most of a frame with layers (phase-5 spec 5.4).
    // Measured on the steady clock (a few ns a read: the thread's CPU clock is a system call, which
    // cost more than many of the calls it timed), so waits for the driver count as the driver's.
    // Nested scopes count once (the outermost).
    class RenderBackendTime {
    public:
        static void setEnabled(bool enabled){ state().enabled = enabled; }
        static bool isEnabled(){ return state().enabled; }
        // the calling thread's total since the last reset, ms
        static double totalMs(){ return (double)state().totalNs / 1.0e6; }
        static void reset(){ state().totalNs = 0; state().scopes = 0; }
        // the outermost scopes since the reset (each one's own clock reads cost the thread
        // time outside the total too: a caller subtracts scopes x that cost, see EffectPerfTest)
        static uint64_t scopeCount(){ return state().scopes; }

        class Scope {
        public:
            Scope(){
                State& s = state();
                if (s.enabled && s.depth++ == 0){
                    s.scopes++;
                    start = now();
                }
            }
            ~Scope(){
                State& s = state();
                if (s.enabled && s.depth > 0 && --s.depth == 0 && start >= 0)
                    s.totalNs += now() - start;
            }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;

        private:
            int64_t start = -1;
        };

    private:
        struct State {
            bool enabled = false;
            int depth = 0;
            int64_t totalNs = 0;
            uint64_t scopes = 0;
        };
        static State& state(){
            static thread_local State s;
            return s;
        }
        static int64_t now(){
            return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    };

}

#endif //RENDERBACKENDTIME_H
