// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Portions adapted from OpenEngine's AssetDecodeGate (Engine/Include/Assets/AssetDecodeGate.h,
// Engine/Source/Assets/AssetDecodeGate.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#ifndef JOBS_BACKGROUNDGATE_H
#define JOBS_BACKGROUNDGATE_H

#include "Export.h"
#include "jobs/JobTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>

namespace jasperengine { namespace jobs {

    // Frame protection: the workers that may run Background bodies at once. A running body
    // cannot be preempted, so the gate bounds what runs, not what is queued.
    //  - at most maxBackground() = max(1, workers - reserve) slots are taken, which keeps
    //    `reserve` workers free for the frame (2 on desktop, 1 on mobile and web by default);
    //  - Heavy work (texture cooks, mesh simplification, shader compiles, nav bakes) takes at
    //    most heavyMax() = max(1, maxBackground() - 2) of them, so the short decodes the frame
    //    waits on always find a slot no cook holds. A gate of one slot shares it.
    // The reserve and an explicit maximum (Engine::setMaxResourceLoadingThreads) change live:
    // a smaller maximum stops admitting until enough running bodies end.
    //
    // Thread-safe; tryAcquire and release are one compare-exchange on a packed word.
    class JASPERENGINE_API BackgroundGate {
    public:
        // `onRelease` runs after every release, outside any lock (wakes the workers and the
        // asset readers waiting for a slot).
        BackgroundGate(int workers, int reserve, std::function<void()> onRelease = nullptr);

        BackgroundGate(const BackgroundGate&) = delete;
        BackgroundGate& operator=(const BackgroundGate&) = delete;

        int getWorkers() const { return workers; }
        int getReserve() const { return reserve.load(std::memory_order_relaxed); }
        // 0 = derived from the reserve.
        int getMaxOverride() const { return maxOverride.load(std::memory_order_relaxed); }
        int maxBackground() const;
        int heavyMax() const;
        int taken() const;
        int heavyTaken() const;

        // The most slots taken at once (all, Heavy) since the last takePeaks(), which starts
        // the next interval at what is taken now. peeks() reads without resetting.
        struct Peaks { int taken = 0; int heavy = 0; };
        Peaks takePeaks();
        Peaks peeks() const;

        void setReserve(int reserve);
        // Replaces maxBackground() when > 0 (heavyMax follows it); 0 returns to the reserve.
        void setMaxOverride(int maxBackground);

        // Takes a slot for `gateClass` when one is free; never blocks.
        bool tryAcquire(GateClass gateClass);
        // Whether tryAcquire(gateClass) would succeed now (a snapshot).
        bool canAcquire(GateClass gateClass) const;
        // Gives back a slot tryAcquire took for the same class.
        void release(GateClass gateClass);

        // The slot counts for `workers` and `reserve` (and an override), as documented above.
        static int maxBackgroundFor(int workers, int reserve, int maxOverride);
        static int heavyMaxFor(int maxBackground);

    private:
        const int workers;
        std::atomic<int> reserve;
        std::atomic<int> maxOverride{0};
        // taken in the low 32 bits, of them Heavy in the high 32
        std::atomic<uint64_t> slots{0};
        // packed like `slots`: the high-water marks of both counts
        std::atomic<uint64_t> peaks{0};
        std::function<void()> onRelease;
    };

}}

#endif // JOBS_BACKGROUNDGATE_H
