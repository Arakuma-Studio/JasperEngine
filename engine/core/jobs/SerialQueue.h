// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOBS_SERIALQUEUE_H
#define JOBS_SERIALQUEUE_H

#include "jobs/Task.h"

#include <cstddef>
#include <type_traits>

namespace jasperengine { namespace jobs {

    class JobSystem;

    namespace detail {
        struct StrandState;
        JASPERENGINE_API StrandState* createStrand(JobSystem* system, const char* name, Lane lane, GateClass gate);
        JASPERENGINE_API void destroyStrand(StrandState* strand);
        JASPERENGINE_API void strandPush(StrandState* strand, TaskState* task);
        JASPERENGINE_API void strandCancelPending(StrandState* strand);
        JASPERENGINE_API void strandWaitIdle(StrandState* strand);
        JASPERENGINE_API size_t strandPending(const StrandState* strand);
        JASPERENGINE_API Lane strandLane(const StrandState* strand);
        JASPERENGINE_API GateClass strandGate(const StrandState* strand);
    }

    // A strand: jobs run in FIFO order, at most one at a time, on the pool's workers of its
    // lane (never on a dedicated thread). For single-consumer work that used to own a thread:
    // thumbnail generation, deferred releases, import queues.
    //
    //     jobs::SerialQueue thumbnails{"thumbnails", jobs::Lane::Background};
    //     jobs::Task<Image> image = thumbnails.push([path]{ return render(path); });
    //
    // Waiting on a strand's task runs the strand's earlier tasks inline when no thread is
    // running the strand. The destructor waits for every pushed task (cancelPending() first
    // drops the ones not started).
    class JASPERENGINE_API SerialQueue {
    public:
        explicit SerialQueue(const char* name, Lane lane = Lane::Background,
                             GateClass gate = GateClass::Default, JobSystem* system = nullptr);
        ~SerialQueue();

        SerialQueue(const SerialQueue&) = delete;
        SerialQueue& operator=(const SerialQueue&) = delete;

        template<class F>
        auto push(F&& fn, const char* name = nullptr, uint32_t tags = 0)
            -> Task<std::invoke_result_t<std::decay_t<F>&>>{
            using R = std::invoke_result_t<std::decay_t<F>&>;
            SubmitOptions options;
            options.lane = detail::strandLane(strand);
            options.gate = detail::strandGate(strand);
            options.name = name ? name : queueName;
            options.tags = tags;
            detail::TaskState* state = detail::makeTask<F, R>(std::forward<F>(fn), options);
            state->flags |= detail::kFlagStrand;
            Task<R> handle(state);
            detail::strandPush(strand, state);
            return handle;
        }

        // Cancels every task not started yet.
        void cancelPending();
        // Returns once every pushed task is terminal (helps run them).
        void waitIdle();
        // Tasks pushed and not finished, the running one included.
        size_t pendingCount() const;

    private:
        const char* queueName;
        detail::StrandState* strand;
    };

}}

#endif // JOBS_SERIALQUEUE_H
