// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The parallelFor contract is adapted from OpenEngine's JobSystem (ParallelForOptions.h,
// ParallelAlgorithms.h, WorkStealingThreadPool.h), Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License. See NOTICE.md.

#ifndef JOBS_PARALLELFOR_H
#define JOBS_PARALLELFOR_H

#include "Export.h"
#include "jobs/JobTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace jasperengine { namespace jobs {

    struct ParallelForOptions {
        // maxHelpers asking for one helper per worker.
        static constexpr size_t kAllWorkers = SIZE_MAX;

        // Units claimed at a time: a body of ~1 µs wants a grain of 16 or more.
        size_t grain = 1;
        // Helper jobs published beside the caller; clamped to min(claims - 1, 64) and to
        // the helpers the lane may hold unstarted. 0 runs every unit on the caller.
        size_t maxHelpers = kAllWorkers;
        // The helpers' lane, clamped by the body running on the calling thread: a
        // Background body forks Background whatever is asked, a Normal body never forks High.
        Lane lane = Lane::Normal;
        // Polled by the caller before each claim; true stops the run (no further unit
        // starts, units in flight finish) and parallelFor returns false.
        const std::function<bool()>* stop = nullptr;
    };

    namespace detail {
        class Scheduler;
        // Runs units [begin, end) of the body; false when the body asked to stop.
        typedef bool (*ParallelForInvoke)(void* body, size_t begin, size_t end);
        JASPERENGINE_API bool runParallelFor(Scheduler* scheduler, ParallelForInvoke invoke, void* body,
                                             size_t count, const ParallelForOptions& options);

        template<class Body>
        bool invokeParallelBody(void* bodyPointer, size_t begin, size_t end){
            Body& body = *static_cast<Body*>(bodyPointer);
            for (size_t unit = begin; unit < end; unit++){
                if constexpr (std::is_same<decltype(body(unit)), bool>::value){
                    if (!body(unit))
                        return false;
                }else{
                    body(unit);
                }
            }
            return true;
        }
    }

}}

#endif // JOBS_PARALLELFOR_H
