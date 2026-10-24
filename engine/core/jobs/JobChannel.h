// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Blocking channels are adapted from OpenEngine's JobChannel and BlockingThreads
// (Modules/JobSystem/Include/JobSystem/JobChannel.h, Source/BlockingThreads.{h,cpp}),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef JOBS_JOBCHANNEL_H
#define JOBS_JOBCHANNEL_H

#include "jobs/Task.h"

#include <cstdint>
#include <type_traits>

namespace jasperengine { namespace jobs {

    class JobSystem;

    namespace detail {
        struct ChannelState;
        JASPERENGINE_API ChannelState* createChannel(JobSystem* system, const char* name, uint32_t maxRunning);
        JASPERENGINE_API void destroyChannel(ChannelState* channel);
        JASPERENGINE_API void channelSubmit(ChannelState* channel, TaskState* task);
        JASPERENGINE_API const char* channelName(const ChannelState* channel);
        JASPERENGINE_API uint32_t channelCap(const ChannelState* channel);
        JASPERENGINE_API uint32_t channelQueued(const ChannelState* channel);
        JASPERENGINE_API uint32_t channelRunning(const ChannelState* channel);
    }

    // Work that blocks: waiting on another process (cmake, FFmpeg, the compiler), the network
    // or a long disk write. Channel jobs run on blocking threads that never run compute jobs,
    // created on demand up to the sum of the channels' caps (the scheduler's blocking budget:
    // 8 on desktop, 1 on threaded web), at most `maxRunning` of one channel at a time. In
    // single-thread mode they run from pump().
    //
    // The process-wide channels are JobSystem::processChannel() (cap 2), ioWriteChannel() (1)
    // and netChannel() (2). A job must not wait on a job of its own channel (asserted when
    // the scheduler checks waits): with the cap reached that is a deadlock.
    class JASPERENGINE_API JobChannel {
    public:
        // `name` is a static string. null system: the process JobSystem.
        JobChannel(const char* name, uint32_t maxRunning, JobSystem* system = nullptr);
        // Cancels the jobs not started and waits for the running ones.
        ~JobChannel();

        JobChannel(const JobChannel&) = delete;
        JobChannel& operator=(const JobChannel&) = delete;

        // `options.lane` and `options.gate` do not apply; name, tags, cancel do.
        template<class F>
        auto submit(F&& fn, const SubmitOptions& options = {})
            -> Task<std::invoke_result_t<std::decay_t<F>&>>{
            using R = std::invoke_result_t<std::decay_t<F>&>;
            SubmitOptions channelOptions = options;
            channelOptions.lane = Lane::Normal;
            if (!options.name || options.name[0] == '\0' || options.name == SubmitOptions().name)
                channelOptions.name = detail::channelName(channel);
            detail::TaskState* state = detail::makeTask<F, R>(std::forward<F>(fn), channelOptions);
            state->flags |= detail::kFlagChannel;
            Task<R> handle(state);
            detail::channelSubmit(channel, state);
            return handle;
        }

        const char* getName() const { return detail::channelName(channel); }
        uint32_t getCap() const { return detail::channelCap(channel); }
        uint32_t getQueued() const { return detail::channelQueued(channel); }
        uint32_t getRunning() const { return detail::channelRunning(channel); }

    private:
        detail::ChannelState* channel;
    };

}}

#endif // JOBS_JOBCHANNEL_H
