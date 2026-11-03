// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_CHANGETRACKING_H
#define ECS_CHANGETRACKING_H

#include <cstddef>
#include <cstdint>
#include "Export.h"
#include "PagedStorage.h"

namespace jasperengine {

    // Monotonic per-registry change counter. 64 bits: it never wraps in practice.
    using Tick = uint64_t;

    // What a consumer has seen: changes stamped after `last` are new to it. Entry-sample
    // contract: a scheduled system's gate is written back with the tick it ran at, so its
    // own writes of that run are not reported to it again; what it records into its command
    // buffer is stamped later, at playback, and is.
    struct ChangeGate {
        Tick last = 0;
    };

    // The change tick of one registry.
    //
    // It advances before each scheduled system runs, at each command-buffer playback and at
    // the end of each editor command. A consumer outside the scheduler samples it
    // (EntityRegistry::changeTick, MCP, Lua): the next stamp after a sample advances it first,
    // so a change made after a sample is always newer than the sample.
    //
    // While a parallel wave runs, each system stamps with the tick it was given (a
    // thread-local slot), never touching the shared counter; the wave lock forbids index
    // observations that would compact or sort an array.
    class JASPERENGINE_API ChangeClock {
    private:
        Tick current = 1;
        mutable bool sampled = false;
        bool waveLocked = false;

    public:
        Tick sample() const {
            sampled = true;
            return current;
        }

        // the current value without the sample's effect (statistics)
        Tick peek() const {
            return current;
        }

        Tick advance(){
            sampled = false;
            return ++current;
        }

        // The tick a change made now is stamped with
        Tick stamp();

        void setWaveLocked(bool locked){ waveLocked = locked; }
        bool isWaveLocked() const { return waveLocked; }

        // While a scheduled system runs on this thread, its changes to `clock`'s registry are
        // stamped with `tick` (SystemScheduler)
        static void setThreadSystemTick(const ChangeClock* clock, Tick tick);
        static void clearThreadSystemTick();
    };

    namespace ecs {

        // Per-slot ticks of a component array: 32-bit offsets from a 64-bit base (8 bytes per
        // component for both ticks instead of 16). When a tick passes base + 2^32 - 1 the
        // column rebases to half the range below it; offsets older than the new base become
        // 0, so a very old change can be reported again after billions of ticks, never missed.
        class TickColumn {
        private:
            static constexpr Tick kMaxOffset = 0xFFFFFFFFull;
            PagedArray<uint32_t> offsets;
            Tick base = 0;

            void rebase(Tick tick, size_t count){
                const Tick newBase = tick - kMaxOffset / 2;
                for (size_t i = 0; i < count; i++){
                    const Tick old = base + offsets[i];
                    offsets[i] = old > newBase ? (uint32_t)(old - newBase) : 0u;
                }
                base = newBase;
            }

        public:
            Tick get(size_t index) const {
                return base + offsets[index];
            }

            // `count`: the slots in use, which a rebase rewrites
            void set(size_t index, Tick tick, size_t count){
                if (tick < base){
                    tick = base;
                }
                if (tick - base > kMaxOffset){
                    rebase(tick, count);
                }
                offsets[index] = (uint32_t)(tick - base);
            }

            void copy(size_t to, size_t from){
                offsets[to] = offsets[from];
            }

            uint32_t raw(size_t index) const { return offsets[index]; }
            void setRaw(size_t index, uint32_t value){ offsets[index] = value; }

            PagedArray<uint32_t>& storage(){ return offsets; }

            void reserve(size_t count){ offsets.reserve(count); }
            void shrinkTo(size_t count){ offsets.shrinkTo(count); }
            size_t bytes() const { return offsets.bytes(); }
        };

    }

}

#endif // ECS_CHANGETRACKING_H
