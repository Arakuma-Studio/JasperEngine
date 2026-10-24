// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JoltJobAdapter.h"

#ifdef JASPERENGINE_PHYSICS_3D

#include "jobs/JobSystem.h"

#include <chrono>
#include <thread>

using namespace jasperengine;

JoltJobAdapter::JoltJobAdapter(JPH::uint maxJobs, JPH::uint maxBarriers): JPH::JobSystemWithBarrier(maxBarriers){
    jobPool.Init(maxJobs, maxJobs);
}

JoltJobAdapter::~JoltJobAdapter(){
    // (the engine's adapter is never destroyed; a test's or a tool's goes out of scope)
    while (queuedEnvelopes.load(std::memory_order_acquire) > 0){
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

int JoltJobAdapter::GetMaxConcurrency() const{
    // the workers and the thread that steps the world
    return jobs::system().getWorkerCount() + 1;
}

JPH::JobSystem::JobHandle JoltJobAdapter::CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 numDependencies){
    JPH::uint32 index;
    for (;;){
        index = jobPool.ConstructObject(name, color, this, function, numDependencies);
        if (index != AvailableJobs::cInvalidObjectIndex)
            break;
        // every job slot is taken: the ones running free theirs
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    Job* job = &jobPool.Get(index);
    // the handle keeps a reference: the job is queued below and may complete at once
    JobHandle handle(job);
    if (numDependencies == 0)
        QueueJob(job);
    return handle;
}

void JoltJobAdapter::QueueJob(Job* job){
    jobs::JobSystem& system = jobs::system();
    if (system.isSingleThreaded()){
        job->Execute();
        return;
    }
    // the envelope keeps the job alive until it ran; a barrier waiter may run it first
    // (Execute runs a job once)
    job->AddRef();
    queuedEnvelopes.fetch_add(1, std::memory_order_relaxed);
    system.enqueueDetached([this, job](){
        job->Execute();
        job->Release();
        queuedEnvelopes.fetch_sub(1, std::memory_order_release);
    }, jobs::Lane::High, "jolt");
}

void JoltJobAdapter::QueueJobs(Job** list, JPH::uint count){
    for (JPH::uint i = 0; i < count; i++)
        QueueJob(list[i]);
}

void JoltJobAdapter::FreeJob(Job* job){
    jobPool.DestructObject(job);
}

#endif // JASPERENGINE_PHYSICS_3D
