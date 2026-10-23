// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "jobs/JobSystem.h"
#include "jobs/detail/Scheduler.h"

using namespace jasperengine;
using namespace jasperengine::jobs;
using namespace jasperengine::jobs::detail;

// ---- strands

StrandState* detail::createStrand(JobSystem* system, const char* name, Lane lane, GateClass gate){
    auto* strand = new StrandState();
    strand->scheduler = schedulerOf(system);
    strand->name = name ? name : "serial";
    strand->lane = lane;
    strand->gate = gate;
    return strand;
}

void detail::destroyStrand(StrandState* strand){
    strand->scheduler->strandWaitIdle(strand);
    // a drain job still queued holds a reference; the last one deletes the state
    if (strand->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete strand;
}

void detail::strandPush(StrandState* strand, TaskState* task){
    strand->scheduler->strandPush(strand, task);
}

void detail::strandCancelPending(StrandState* strand){
    strand->scheduler->strandCancelPending(strand);
}

void detail::strandWaitIdle(StrandState* strand){
    strand->scheduler->strandWaitIdle(strand);
}

size_t detail::strandPending(const StrandState* strand){
    std::lock_guard<std::mutex> lock(const_cast<StrandState*>(strand)->mutex);
    return strand->pending;
}

Lane detail::strandLane(const StrandState* strand){
    return strand->lane;
}

GateClass detail::strandGate(const StrandState* strand){
    return strand->gate;
}

SerialQueue::SerialQueue(const char* name, Lane lane, GateClass gate, JobSystem* system):
    queueName(name ? name : "serial"), strand(createStrand(system, queueName, lane, gate)){
}

SerialQueue::~SerialQueue(){
    destroyStrand(strand);
}

void SerialQueue::cancelPending(){
    strandCancelPending(strand);
}

void SerialQueue::waitIdle(){
    strandWaitIdle(strand);
}

size_t SerialQueue::pendingCount() const{
    return strandPending(strand);
}

// ---- channels

ChannelState* detail::createChannel(JobSystem* system, const char* name, uint32_t maxRunning){
    auto* channel = new ChannelState();
    channel->scheduler = schedulerOf(system);
    channel->name = name ? name : "channel";
    channel->cap = maxRunning > 0 ? maxRunning : 1;
    channel->scheduler->channelRegister(channel);
    return channel;
}

void detail::destroyChannel(ChannelState* channel){
    channel->scheduler->channelUnregister(channel);
    delete channel;
}

void detail::channelSubmit(ChannelState* channel, TaskState* task){
    channel->scheduler->channelSubmit(channel, task);
}

const char* detail::channelName(const ChannelState* channel){
    return channel->name;
}

uint32_t detail::channelCap(const ChannelState* channel){
    return channel->cap;
}

uint32_t detail::channelQueued(const ChannelState* channel){
    std::lock_guard<std::mutex> lock(channel->scheduler->blockingMutex);
    return channel->queued;
}

uint32_t detail::channelRunning(const ChannelState* channel){
    std::lock_guard<std::mutex> lock(channel->scheduler->blockingMutex);
    return channel->running;
}

JobChannel::JobChannel(const char* name, uint32_t maxRunning, JobSystem* system):
    channel(createChannel(system, name, maxRunning)){
}

JobChannel::~JobChannel(){
    destroyChannel(channel);
}
