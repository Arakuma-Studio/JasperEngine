// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISEPARALLEL_H
#define NOISE_NOISEPARALLEL_H

// The one parallel loop of the noise library and erosion (T2 R7, N17). JASPERENGINE_NOISE_SERIAL makes
// it a plain loop, for the golden tool alone: that one links no job system. Results never depend on
// which thread runs which unit, so both forms give the same bits.

#include <cstddef>

#if !defined(JASPERENGINE_NOISE_SERIAL)
    #include "jobs/JobSystem.h"
    #include <functional>
#endif

namespace jasperengine { namespace jobs { class CancelToken; } }

namespace jasperengine { namespace noise { namespace detail {

#if defined(JASPERENGINE_NOISE_SERIAL)

    inline bool cancelled(const jobs::CancelToken*) { return false; }

    template<class Body>
    bool parallelChunks(size_t count, bool, jobs::CancelToken*, Body&& body){
        for (size_t i = 0; i < count; i++) body(i);
        return true;
    }

#else

    inline bool cancelled(const jobs::CancelToken* cancel) { return cancel && cancel->isCancelled(); }

    // False when cancel stopped it: units in flight finish, and no further unit starts
    template<class Body>
    bool parallelChunks(size_t count, bool parallel, jobs::CancelToken* cancel, Body&& body){
        if (!parallel || count <= 1){
            for (size_t i = 0; i < count; i++){
                if (cancelled(cancel)) return false;
                body(i);
            }
            return true;
        }
        // A helper claims units without asking: nothing starts on a run already cancelled
        if (cancelled(cancel)) return false;
        jobs::ParallelForOptions options;
        std::function<bool()> stop;
        if (cancel){
            stop = [cancel]{ return cancel->isCancelled(); };
            options.stop = &stop;
        }
        return jobs::parallelFor(count, body, options);
    }

#endif

}}}

#endif // NOISE_NOISEPARALLEL_H
