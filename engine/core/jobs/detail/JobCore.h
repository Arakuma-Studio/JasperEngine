// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOBS_DETAIL_JOBCORE_H
#define JOBS_DETAIL_JOBCORE_H

// The objects the scheduler queues, and the allocation of them. Templates in the public
// headers build them inline; everything with state (slab caches, queues, thread-locals) is
// behind exported functions in the engine library, so a module that instantiates a
// template (the editor, a game plugin) shares the engine's scheduler.

#include "Export.h"
#include "jobs/JobTypes.h"
#include "jobs/Primitives.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>

namespace jasperengine { namespace jobs {

    class JobSystem;

    namespace detail {

        class Scheduler;

        // Fixed-size blocks recycled through per-thread caches: no heap allocation once
        // warm. Bare job envelopes take 128 bytes (32-byte header + up to 96 bytes of
        // closure), task states 256 (header, closure and an inline result), dependency
        // links 32.
        enum class SlabClass : uint8_t {
            Link,
            Job,
            Task,
        };
        constexpr size_t kLinkSlabBytes = 32;
        constexpr size_t kJobSlabBytes = 128;
        constexpr size_t kTaskSlabBytes = 256;
        constexpr size_t kSlabAlignment = 16;
        // The largest closure a bare job keeps in its slab; a larger one goes to the heap
        // (decided at compile time, counted in Stats::totals.heapEnvelopes).
        constexpr size_t kMaxSlabClosureBytes = 96;

        JASPERENGINE_API void* acquireSlab(SlabClass slabClass);
        JASPERENGINE_API void releaseSlab(SlabClass slabClass, void* block) noexcept;
        JASPERENGINE_API void* acquireHeapEnvelope(size_t bytes);
        JASPERENGINE_API void releaseHeapEnvelope(void* block) noexcept;

        enum RunnableFlags : uint8_t {
            kFlagHeap = 1u << 0,         // allocated by acquireHeapEnvelope
            kFlagPreAdmitted = 1u << 1,  // its closure holds a BackgroundGate slot (I/O continuation)
            kFlagChannel = 1u << 2,      // runs on a JobChannel's blocking threads
            kFlagStrand = 1u << 3,       // runs through a SerialQueue
            kFlagIoGated = 1u << 4,      // released by an AssetIOService read
            kFlagDetached = 1u << 5,     // enqueueDetached: runs even in the shutdown drain
            kFlagInternal = 1u << 6,     // a counter visit or strand drain (not a job body itself)
        };

        enum class RunnableKind : uint8_t {
            Job,   // bare work: no handle, no status
            Task,  // a TaskState behind Task<T> handles
        };

        // What every queue holds. `next` links the overflow spill of a lane and the FIFO of
        // a counter, channel or strand.
        struct Runnable {
            Runnable* next = nullptr;
            RunnableKind kind = RunnableKind::Job;
            Lane lane = Lane::Normal;
            GateClass gate = GateClass::Default;
            uint8_t flags = 0;
            uint32_t tags = 0;
            const char* name = "job";
        };

        struct Job;
        struct JobOps {
            void (*run)(Job* job);
            void (*destroy)(Job* job);
        };

        struct Job : Runnable {
            const JobOps* ops = nullptr;
        };
        static_assert(sizeof(Job) <= kJobSlabBytes - kMaxSlabClosureBytes, "a bare job header leaves 96 bytes of closure");

        template<class F>
        struct JobImpl final : Job {
            F fn;

            template<class G>
            explicit JobImpl(G&& g): fn(std::forward<G>(g)) {}

            static void runImpl(Job* job){ static_cast<JobImpl*>(job)->fn(); }
            static void destroyImpl(Job* job){ static_cast<JobImpl*>(job)->~JobImpl(); }
            static const JobOps kOps;
        };

        template<class F>
        const JobOps JobImpl<F>::kOps = { &JobImpl<F>::runImpl, &JobImpl<F>::destroyImpl };

        template<class F>
        constexpr bool jobFitsSlab(){
            return sizeof(JobImpl<F>) <= kJobSlabBytes && alignof(JobImpl<F>) <= kSlabAlignment;
        }

        // A bare job around `fn`, from a slab when it fits.
        template<class F>
        Job* makeJob(F&& fn, Lane lane, GateClass gate, const char* name, uint32_t tags){
            using Impl = JobImpl<std::decay_t<F>>;
            constexpr bool inSlab = jobFitsSlab<std::decay_t<F>>();
            void* memory = inSlab ? acquireSlab(SlabClass::Job) : acquireHeapEnvelope(sizeof(Impl));
            Impl* job = nullptr;
            try{
                job = ::new (memory) Impl(std::forward<F>(fn));
            }catch(...){
                if (inSlab)
                    releaseSlab(SlabClass::Job, memory);
                else
                    releaseHeapEnvelope(memory);
                throw;
            }
            job->kind = RunnableKind::Job;
            job->lane = lane;
            job->gate = gate;
            job->flags = inSlab ? 0 : kFlagHeap;
            job->tags = tags;
            job->name = name ? name : "job";
            job->ops = &Impl::kOps;
            return job;
        }

        // Destroys a job's closure without running it and frees the envelope.
        JASPERENGINE_API void discardJob(Job* job) noexcept;

        // ---- task states (Task<T> handles)

        struct TaskState;
        struct DepLink;

        struct TaskOps {
            // Runs the closure, stores its result or exception, destroys the closure; false
            // when it threw.
            bool (*invoke)(TaskState* state);
            // Destroys the closure without running it.
            void (*discard)(TaskState* state);
            // Destroys the result (when the task completed) and the object, not the memory.
            void (*destroy)(TaskState* state);
            void* (*result)(TaskState* state);
        };

        struct TaskState : Runnable {
            const TaskOps* ops = nullptr;
            // Handles, the queue holding it, dependency links and a running thread.
            std::atomic<uint32_t> refs{1};
            std::atomic<uint8_t> status{(uint8_t)TaskStatus::Pending};
            std::atomic<uint8_t> depFailed{0};
            // Waiters parked on it (parking-lot protocol, see Scheduler.cpp).
            std::atomic<uint16_t> parked{0};
            DepPolicy policy = DepPolicy::RequireSuccess;
            // Dependencies not yet terminal, plus one held by the submit until it is done.
            std::atomic<uint32_t> pendingDeps{0};
            // Thread slot running it, -1 when none (wait-cycle detection).
            std::atomic<int32_t> runner{-1};
            Scheduler* scheduler = nullptr;
            CancelState* cancel = nullptr;
            // Links of the tasks that depend on this one; sealed when it ends.
            std::atomic<DepLink*> dependents{nullptr};
            // Links to this task's own dependencies (kept for waits that help them).
            DepLink* deps = nullptr;
            // The channel, strand or I/O request it runs through, by flag.
            void* owner = nullptr;
            std::exception_ptr error;
        };

        // One edge of the dependency graph: `dependent` waits on `dep`.
        struct DepLink {
            TaskState* dep = nullptr;
            TaskState* dependent = nullptr;
            DepLink* nextOfDep = nullptr;        // in dep->dependents
            DepLink* nextOfDependent = nullptr;  // in dependent->deps
        };
        static_assert(sizeof(DepLink) <= kLinkSlabBytes, "a dependency link fits its slab");

        template<class R>
        struct ResultStorage {
            alignas(R) unsigned char bytes[sizeof(R)];
            R* get(){ return std::launder(reinterpret_cast<R*>(bytes)); }
        };

        template<>
        struct ResultStorage<void> {
            void* get(){ return nullptr; }
        };

        template<class F, class R>
        struct TaskImpl final : TaskState {
            alignas(F) unsigned char fnBytes[sizeof(F)];
            ResultStorage<R> result;

            F& fn(){ return *std::launder(reinterpret_cast<F*>(fnBytes)); }

            static bool invokeImpl(TaskState* base){
                TaskImpl* self = static_cast<TaskImpl*>(base);
                bool ok = true;
                try{
                    if constexpr (std::is_void_v<R>){
                        self->fn()();
                    }else{
                        ::new ((void*)self->result.bytes) R(self->fn()());
                    }
                }catch(...){
                    self->error = std::current_exception();
                    ok = false;
                }
                self->fn().~F();
                return ok;
            }

            static void discardImpl(TaskState* base){
                static_cast<TaskImpl*>(base)->fn().~F();
            }

            static void destroyImpl(TaskState* base){
                TaskImpl* self = static_cast<TaskImpl*>(base);
                if constexpr (!std::is_void_v<R>){
                    if ((TaskStatus)self->status.load(std::memory_order_acquire) == TaskStatus::Completed)
                        self->result.get()->~R();
                }
                self->~TaskImpl();
            }

            static void* resultImpl(TaskState* base){
                return static_cast<void*>(static_cast<TaskImpl*>(base)->result.get());
            }

            static const TaskOps kOps;
        };

        template<class F, class R>
        const TaskOps TaskImpl<F, R>::kOps = {
            &TaskImpl<F, R>::invokeImpl, &TaskImpl<F, R>::discardImpl,
            &TaskImpl<F, R>::destroyImpl, &TaskImpl<F, R>::resultImpl };

        // A Pending task around `fn` with one reference (the handle's), not yet published.
        template<class F, class R>
        TaskState* makeTask(F&& fn, const SubmitOptions& options){
            using G = std::decay_t<F>;
            using Impl = TaskImpl<G, R>;
            constexpr bool inSlab = sizeof(Impl) <= kTaskSlabBytes && alignof(Impl) <= kSlabAlignment
                && sizeof(G) <= kMaxSlabClosureBytes;
            void* memory = inSlab ? acquireSlab(SlabClass::Task) : acquireHeapEnvelope(sizeof(Impl));
            Impl* task = ::new (memory) Impl;
            try{
                ::new ((void*)task->fnBytes) G(std::forward<F>(fn));
            }catch(...){
                task->~Impl();
                if (inSlab)
                    releaseSlab(SlabClass::Task, memory);
                else
                    releaseHeapEnvelope(memory);
                throw;
            }
            task->kind = RunnableKind::Task;
            task->lane = options.lane;
            task->gate = options.gate;
            task->flags = inSlab ? 0 : kFlagHeap;
            task->tags = options.tags;
            task->name = options.name ? options.name : "job";
            task->policy = options.deps;
            task->ops = &Impl::kOps;
            if (options.cancel && options.cancel->getState()){
                task->cancel = options.cancel->getState();
                retainCancel(task->cancel);
            }
            return task;
        }

        inline void retainTask(TaskState* state){
            state->refs.fetch_add(1, std::memory_order_relaxed);
        }
        JASPERENGINE_API void releaseTask(TaskState* state) noexcept;

        JASPERENGINE_API void waitTask(TaskState* state);
        JASPERENGINE_API bool cancelTask(TaskState* state);
        // Rethrows the job's exception (Failed) or throws TaskCancelled (Cancelled).
        [[noreturn]] JASPERENGINE_API void throwTaskOutcome(TaskState* state);

        inline TaskStatus statusOf(const TaskState* state){
            return (TaskStatus)state->status.load(std::memory_order_acquire);
        }

    }

}}

#endif // JOBS_DETAIL_JOBCORE_H
