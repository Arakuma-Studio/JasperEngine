// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOBS_JOBSYSTEM_H
#define JOBS_JOBSYSTEM_H

// The engine's job system: a work-stealing pool with High, Normal and Background lanes,
// task handles with dependencies and cancellation, fork-join counters, parallelFor,
// strands (SerialQueue), blocking channels (JobChannel) and frame protection
// (BackgroundGate). See docs/jobs-memory.md.
//
//     auto mesh = jobs::submit([path]{ return parseMesh(path); },
//                              {jobs::Lane::Background, jobs::GateClass::Default, "parse-mesh"});
//     ...
//     if (mesh.isReady()) use(mesh.get());          // the main thread polls, or
//     jobs::parallelFor(count, [&](size_t i){ out[i] = f(in[i]); });   // from any thread
//
// Rules:
//  - Jobs never touch sokol, Lua or ImGui. Results come back by polling Task::isReady()
//    on the main thread, or through postToMain().
//  - Every wait is legal on every thread, a worker included: it runs the awaited work
//    inline when nobody has started it and parks only on work running elsewhere.
//  - Do not wait while holding a mutex that the awaited jobs take.
//  - With 0 workers (single-thread mode, the web build without pthreads) nothing runs at
//    submit: the Engine pumps the queues once per frame.

#include "Export.h"
#include "jobs/BackgroundGate.h"
#include "jobs/JobChannel.h"
#include "jobs/JobCounter.h"
#include "jobs/JobStats.h"
#include "jobs/JobTypes.h"
#include "jobs/ParallelFor.h"
#include "jobs/Primitives.h"
#include "jobs/SerialQueue.h"
#include "jobs/Task.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

namespace jasperengine { namespace jobs {

    namespace detail {
        class Scheduler;
        JASPERENGINE_API void publishTask(Scheduler* scheduler, TaskState* task, const TaskRef* deps, size_t depCount);
        JASPERENGINE_API void publishJob(Scheduler* scheduler, Job* job);
        JASPERENGINE_API Scheduler* schedulerOf(JobSystem* system);
        JASPERENGINE_API JobSystem* systemOf(TaskState* task);
    }

    class JASPERENGINE_API JobSystem {
    public:
        explicit JobSystem(const Config& config = {});
        // shutdown()
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        // Cancels every Pending submit() task, runs the queued enqueueDetached() jobs, joins
        // the threads. A submit afterwards returns a Cancelled handle and never runs; an
        // enqueueDetached afterwards runs on the caller. Idempotent.
        void shutdown();
        bool isShuttingDown() const;

        int getWorkerCount() const;
        bool isSingleThreaded() const;
        // Jobs tagged Tag::Asset that are queued or running (gated reads included).
        uint32_t getAssetJobCount() const;

        template<class F>
        auto submit(F&& fn, const SubmitOptions& options = {})
            -> Task<std::invoke_result_t<std::decay_t<F>&>>;

        // Runs `fn` once every task of `deps` is terminal (see DepPolicy).
        template<class F>
        auto submit(F&& fn, const SubmitOptions& options, DepList deps)
            -> Task<std::invoke_result_t<std::decay_t<F>&>>;

        // Must-run work without a handle: never dropped, even by shutdown (which runs it).
        // A throw is logged and swallowed.
        template<class F>
        void enqueueDetached(F&& fn, Lane lane = Lane::Normal, const char* name = "job");

        // Runs body(i) for every i in [0, count) on the caller and up to options.maxHelpers
        // helpers; legal on any thread, nested at any depth. The caller claims units until
        // none is left, then waits only for units in progress elsewhere. A body returning
        // false, or options.stop, stops the run (returns false); the first exception is
        // rethrown once the units in flight have finished. No heap allocation once warm.
        template<class Body>
        bool parallelFor(size_t count, Body&& body, const ParallelForOptions& options = {});

        // Runs fn(chunkBegin, chunkEnd) over [begin, end) in chunks of `chunk`.
        template<class F>
        void parallelForChunked(size_t begin, size_t end, size_t chunk, F&& fn, const ParallelForOptions& options = {});

        // Sorts [first, last): chunks sorted in parallel, then merged on the caller. Ranges
        // under `threshold` elements sort sequentially.
        template<class Iter, class Compare>
        void parallelSort(Iter first, Iter last, Compare comp, size_t threshold = 5000);
        template<class Iter>
        void parallelSort(Iter first, Iter last, size_t threshold = 5000){
            parallelSort(first, last, std::less<typename std::iterator_traits<Iter>::value_type>(), threshold);
        }

        // Single-thread mode: runs all queued High and Normal work, then Background work
        // until `budgetMs` is spent, then the channel jobs. A no-op with workers.
        void pump(double budgetMs);

        BackgroundGate& getGate();
        // The process-wide blocking channels.
        JobChannel& processChannel();
        JobChannel& ioWriteChannel();
        JobChannel& netChannel();

        // Records one frame of per-worker busy time (the Engine calls it every frame);
        // stats() averages the last `windowFrames` of them.
        void sampleFrame();
        Stats stats(int windowFrames = 60) const;
        // Busy % of `worker` over the last frames, oldest first (up to 120 values).
        std::vector<float> workerHistory(int worker) const;

        detail::Scheduler* getScheduler() const { return scheduler; }

    private:
        detail::Scheduler* scheduler;
        std::unique_ptr<JobChannel> channels[3];
    };

    // ---- the process instance (owned by Engine: created in systemInit, shut down in
    // systemShutdown). Before that, the first use creates it with the default Config.

    JASPERENGINE_API JobSystem& system();
    JASPERENGINE_API bool hasSystem();
    // Creates the process instance with `config` (keeps the existing one, logging, when it
    // was already created with another worker count).
    JASPERENGINE_API JobSystem& initialize(const Config& config);
    // Shuts the process instance down (it stays allocated: later submits are Cancelled,
    // detached jobs run on the caller); initialize() then starts a new one.
    JASPERENGINE_API void shutdown();

    template<class F>
    auto submit(F&& fn, const SubmitOptions& options = {}){
        return system().submit(std::forward<F>(fn), options);
    }
    template<class F>
    auto submit(F&& fn, const SubmitOptions& options, DepList deps){
        return system().submit(std::forward<F>(fn), options, deps);
    }
    template<class F>
    void enqueueDetached(F&& fn, Lane lane = Lane::Normal, const char* name = "job"){
        system().enqueueDetached(std::forward<F>(fn), lane, name);
    }
    template<class Body>
    bool parallelFor(size_t count, Body&& body, const ParallelForOptions& options = {}){
        return system().parallelFor(count, std::forward<Body>(body), options);
    }
    template<class F>
    void parallelForChunked(size_t begin, size_t end, size_t chunk, F&& fn, const ParallelForOptions& options = {}){
        system().parallelForChunked(begin, end, chunk, std::forward<F>(fn), options);
    }
    template<class Iter, class Compare>
    void parallelSort(Iter first, Iter last, Compare comp, size_t threshold = 5000){
        system().parallelSort(first, last, comp, threshold);
    }
    template<class Iter>
    void parallelSort(Iter first, Iter last, size_t threshold = 5000){
        system().parallelSort(first, last, threshold);
    }

    // Single-thread mode pump of the process instance (a no-op with workers).
    JASPERENGINE_API void pump(double budgetMs);
    JASPERENGINE_API Stats stats(int windowFrames = 60);

    // Queues `fn` for the main thread; Engine runs the queue at the start of every frame.
    JASPERENGINE_API void postToMain(std::function<void()> fn);
    // Runs what postToMain queued (main thread). Returns the number run.
    JASPERENGINE_API size_t drainMainQueue();

    // Inside a running job: whether its CancelToken was cancelled. False outside a job.
    JASPERENGINE_API bool cancelRequested();
    // The thread that called markMainThread() (Engine::systemInit).
    JASPERENGINE_API bool isMainThread();
    JASPERENGINE_API void markMainThread();
    // A worker of any JobSystem.
    JASPERENGINE_API bool isWorker();
    // Running a job body (any thread: a worker, a helping waiter, a blocking thread).
    JASPERENGINE_API bool isExecutingJob();
    // Running a JobChannel job: blocking work may run inline here instead of being submitted
    // to a channel and waited on (a channel job must not wait on its own channel).
    JASPERENGINE_API bool isInChannelJob();

    // ---- templates

    template<class F>
    auto JobSystem::submit(F&& fn, const SubmitOptions& options)
        -> Task<std::invoke_result_t<std::decay_t<F>&>>{
        using R = std::invoke_result_t<std::decay_t<F>&>;
        detail::TaskState* state = detail::makeTask<F, R>(std::forward<F>(fn), options);
        Task<R> handle(state);
        detail::publishTask(scheduler, state, nullptr, 0);
        return handle;
    }

    template<class F>
    auto JobSystem::submit(F&& fn, const SubmitOptions& options, DepList deps)
        -> Task<std::invoke_result_t<std::decay_t<F>&>>{
        using R = std::invoke_result_t<std::decay_t<F>&>;
        detail::TaskState* state = detail::makeTask<F, R>(std::forward<F>(fn), options);
        Task<R> handle(state);
        detail::publishTask(scheduler, state, deps.data(), deps.size());
        return handle;
    }

    template<class F>
    void JobSystem::enqueueDetached(F&& fn, Lane lane, const char* name){
        detail::Job* job = detail::makeJob(std::forward<F>(fn), lane, GateClass::Default, name, 0);
        job->flags |= detail::kFlagDetached;
        detail::publishJob(scheduler, job);
    }

    template<class Body>
    bool JobSystem::parallelFor(size_t count, Body&& body, const ParallelForOptions& options){
        using B = std::remove_reference_t<Body>;
        return detail::runParallelFor(scheduler, &detail::invokeParallelBody<B>,
                                      (void*)std::addressof(body), count, options);
    }

    template<class F>
    void JobSystem::parallelForChunked(size_t begin, size_t end, size_t chunk, F&& fn, const ParallelForOptions& options){
        if (end <= begin)
            return;
        chunk = std::max<size_t>(chunk, 1);
        const size_t total = end - begin;
        const size_t chunks = (total + chunk - 1) / chunk;
        parallelFor(chunks, [&](size_t index){
            const size_t chunkBegin = begin + index * chunk;
            fn(chunkBegin, std::min(chunkBegin + chunk, end));
        }, options);
    }

    template<class Iter, class Compare>
    void JobSystem::parallelSort(Iter first, Iter last, Compare comp, size_t threshold){
        const size_t count = (size_t)std::distance(first, last);
        if (count < 2)
            return;
        if (count < threshold || getWorkerCount() == 0){
            std::sort(first, last, comp);
            return;
        }
        const size_t chunkCount = std::min<size_t>({(size_t)getWorkerCount() + 1, count, 64});
        const size_t chunkSize = (count + chunkCount - 1) / chunkCount;
        const size_t sortedChunks = (count + chunkSize - 1) / chunkSize;
        parallelFor(sortedChunks, [&](size_t chunk){
            const size_t offset = chunk * chunkSize;
            std::sort(std::next(first, (std::ptrdiff_t)offset),
                      std::next(first, (std::ptrdiff_t)std::min(offset + chunkSize, count)), comp);
        });
        // merge adjacent sorted runs on the caller
        for (size_t stride = chunkSize; stride < count; stride *= 2){
            for (size_t i = 0; i + stride < count; i += stride * 2){
                Iter low = std::next(first, (std::ptrdiff_t)i);
                Iter mid = std::next(first, (std::ptrdiff_t)std::min(i + stride, count));
                Iter high = std::next(first, (std::ptrdiff_t)std::min(i + stride * 2, count));
                std::inplace_merge(low, mid, high, comp);
            }
        }
    }

    namespace detail {
        // then(): the continuation gets the antecedent's result when it takes one.
        template<class T, class F>
        auto continuationOf(const Task<T>& antecedent, F&& fn){
            if constexpr (!std::is_void<T>::value && std::is_invocable<std::decay_t<F>&, std::add_lvalue_reference_t<T>>::value){
                return [antecedent, f = std::forward<F>(fn)]() mutable { return f(antecedent.get()); };
            }else{
                return [f = std::forward<F>(fn)]() mutable { return f(); };
            }
        }
    }

    template<class T>
    template<class F>
    auto Task<T>::then(F&& fn, const SubmitOptions& options) const{
        JobSystem* owner = state ? detail::systemOf(state) : &system();
        TaskRef self(*this);
        return owner->submit(detail::continuationOf(*this, std::forward<F>(fn)), options, DepList(&self, 1));
    }

    template<class F>
    auto Task<void>::then(F&& fn, const SubmitOptions& options) const{
        JobSystem* owner = state ? detail::systemOf(state) : &system();
        TaskRef self(*this);
        return owner->submit(detail::continuationOf(*this, std::forward<F>(fn)), options, DepList(&self, 1));
    }

}}

#endif // JOBS_JOBSYSTEM_H
