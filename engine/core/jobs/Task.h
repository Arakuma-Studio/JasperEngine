// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOBS_TASK_H
#define JOBS_TASK_H

#include "jobs/detail/JobCore.h"

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace jasperengine { namespace jobs {

    // Thrown by Task<T>::get() on a task that ended Cancelled.
    class JASPERENGINE_API TaskCancelled : public std::runtime_error {
    public:
        TaskCancelled();
    };

    // A type-erased handle: status, wait and cancel of any task. Copies share the task;
    // destroying the last handle neither cancels nor blocks (the job still runs).
    class TaskRef {
    public:
        TaskRef() = default;
        TaskRef(const TaskRef& other): state(other.state){ if (state) detail::retainTask(state); }
        TaskRef(TaskRef&& other) noexcept: state(other.state){ other.state = nullptr; }
        TaskRef& operator=(const TaskRef& other){
            if (this != &other){
                if (other.state)
                    detail::retainTask(other.state);
                reset();
                state = other.state;
            }
            return *this;
        }
        TaskRef& operator=(TaskRef&& other) noexcept{
            if (this != &other){
                reset();
                state = other.state;
                other.state = nullptr;
            }
            return *this;
        }
        ~TaskRef(){ reset(); }

        bool isValid() const { return state != nullptr; }
        explicit operator bool() const { return state != nullptr; }

        // Terminal: Completed, Failed or Cancelled. An invalid handle is never ready.
        bool isReady() const { return state && isTerminal(detail::statusOf(state)); }
        // Cancelled for an invalid handle.
        TaskStatus status() const { return state ? detail::statusOf(state) : TaskStatus::Cancelled; }

        // Returns once the task is terminal, on any thread. It runs the task inline when
        // nobody has started it (helping its dependencies first), and parks only on a task
        // running on another thread, so a wait never deadlocks on an acyclic graph.
        void wait() const { if (state) detail::waitTask(state); }

        // A Pending task never runs: it ends Cancelled, and its RequireSuccess dependents
        // with it. A running task sees jobs::cancelRequested() only through a CancelToken.
        // True when this call cancelled it.
        bool cancel() const { return state && detail::cancelTask(state); }

        // Drops this handle's reference.
        void reset(){
            if (state){
                detail::releaseTask(state);
                state = nullptr;
            }
        }

        bool operator==(const TaskRef& other) const { return state == other.state; }
        bool operator!=(const TaskRef& other) const { return state != other.state; }

        detail::TaskState* getState() const { return state; }

    protected:
        // Adopts the reference `state` carries.
        explicit TaskRef(detail::TaskState* adopted): state(adopted) {}

        detail::TaskState* state = nullptr;

        friend class JobSystem;
    };

    // The dependencies of a submit: a view of TaskRefs that must outlive the call.
    class DepList {
    public:
        DepList(): items(nullptr), count(0) {}
        // the list's array lives until the end of the submit expression, which is all a DepList needs
        DepList(std::initializer_list<TaskRef> list): count(list.size()) { items = list.begin(); }
        DepList(const std::vector<TaskRef>& list): items(list.data()), count(list.size()) {}
        template<size_t N>
        DepList(const std::array<TaskRef, N>& list): items(list.data()), count(N) {}
        DepList(const TaskRef* data, size_t size): items(data), count(size) {}

        const TaskRef* data() const { return items; }
        size_t size() const { return count; }

    private:
        const TaskRef* items = nullptr;
        size_t count;
    };

    // The handle of a job returning T. get() waits, then returns the result, rethrows the
    // job's exception, or throws TaskCancelled.
    template<class T>
    class Task : public TaskRef {
        static_assert(!std::is_reference<T>::value, "a job returns a value, not a reference");

    public:
        using ValueType = T;

        Task() = default;

        T& get() const {
            if (!state)
                throw TaskCancelled();
            detail::waitTask(state);
            if (detail::statusOf(state) != TaskStatus::Completed)
                detail::throwTaskOutcome(state);
            return *static_cast<T*>(state->ops->result(state));
        }

        // Submits `fn` to run once this task is terminal (SubmitOptions::deps decides what a
        // failure does). `fn` takes this task's result by reference, or nothing.
        template<class F>
        auto then(F&& fn, const SubmitOptions& options = {}) const;

    private:
        explicit Task(detail::TaskState* adopted): TaskRef(adopted) {}

        friend class JobSystem;
        friend class SerialQueue;
        friend class JobChannel;
        friend class TaskFactory;
    };

    template<>
    class Task<void> : public TaskRef {
    public:
        using ValueType = void;

        Task() = default;

        void get() const {
            if (!state)
                throw TaskCancelled();
            detail::waitTask(state);
            if (detail::statusOf(state) != TaskStatus::Completed)
                detail::throwTaskOutcome(state);
        }

        template<class F>
        auto then(F&& fn, const SubmitOptions& options = {}) const;

    private:
        explicit Task(detail::TaskState* adopted): TaskRef(adopted) {}

        friend class JobSystem;
        friend class SerialQueue;
        friend class JobChannel;
        friend class TaskFactory;
    };

    // Builds handles from task states made outside JobSystem::submit (the I/O service).
    class TaskFactory {
    public:
        template<class T>
        static Task<T> adopt(detail::TaskState* state){ return Task<T>(state); }
    };

}}

#endif // JOBS_TASK_H
