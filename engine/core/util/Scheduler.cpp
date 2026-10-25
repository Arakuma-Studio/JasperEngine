// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Scheduler.h"
#include "CrashHook.h"
#include "log/Logger.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>

using namespace jasperengine;

namespace je_scheduler {}
using namespace je_scheduler;

namespace je_scheduler {

    // the slot state word: generation << 2 | running | cancelled
    constexpr uint64_t kCancelled = 1;
    constexpr uint64_t kRunning = 2;
    constexpr int kGenerationShift = 2;
    // a handle id: generation (40 bits) << 24 | slot index
    constexpr int kSlotBits = 24;
    constexpr uint64_t kSlotMask = (1ull << kSlotBits) - 1;
    constexpr uint64_t kHandleGenerationMask = (1ull << 40) - 1;
    constexpr int kMaxCatchUp = 8;

    uint64_t generationOf(uint64_t state) {
        return state >> kGenerationShift;
    }

    uint64_t handleId(uint32_t slot, uint64_t generation) {
        return ((generation & kHandleGenerationMask) << kSlotBits) | slot;
    }

    struct SchedulerRegistry {
        std::mutex mutex;
        std::vector<Scheduler*> schedulers;
    };

    SchedulerRegistry& schedulerRegistry() {
        static SchedulerRegistry* instance = new SchedulerRegistry();
        return *instance;
    }

    bool greaterEntry(const auto& a, const auto& b) {
        if (a.due != b.due) return a.due > b.due;
        return a.seq > b.seq;
    }

}

struct Scheduler::Slot {
    std::atomic<uint64_t> state{1ull << kGenerationShift};
    Task task;
    double due = 0.0;
    double period = 0.0;
    uint32_t remaining = 0;     // runs left for a counted repeat
    bool repeating = false;
    bool counted = false;
    bool catchUp = false;
    bool inUse = false;
    bool queued = false;        // in a heap or the inbox
    TimeDomain domain = TimeDomain::Game;
    uint64_t ownerKey = 0;
    uint64_t seq = 0;
    const char* debugName = nullptr;
};

// ------------------------------------------------------------ names

const char* jasperengine::timeDomainName(TimeDomain domain) {
    switch (domain) {
        case TimeDomain::Game: return "game";
        case TimeDomain::Unscaled: return "unscaled";
        case TimeDomain::Real: return "real";
        case TimeDomain::Frames: return "frames";
        case TimeDomain::FixedSteps: return "fixed_steps";
    }
    return "game";
}

bool jasperengine::parseTimeDomain(const std::string& name, TimeDomain& out) {
    for (size_t i = 0; i < TIME_DOMAIN_COUNT; i++) {
        if (name == timeDomainName(static_cast<TimeDomain>(i))) {
            out = static_cast<TimeDomain>(i);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------ handles

bool TimerHandle::cancel() const noexcept {
    return scheduler && scheduler->cancel(*this);
}

bool TimerHandle::active() const noexcept {
    return scheduler && scheduler->isActive(*this);
}

TimerGroup::TimerGroup(TimerGroup&& other) noexcept: handles(std::move(other.handles)) {
}

TimerGroup& TimerGroup::operator=(TimerGroup&& other) noexcept {
    if (this != &other) {
        cancelAll();
        handles = std::move(other.handles);
    }
    return *this;
}

TimerGroup::~TimerGroup() {
    cancelAll();
}

TimerHandle TimerGroup::add(TimerHandle handle) {
    // what already finished goes, so a long-lived group stays small
    handles.erase(std::remove_if(handles.begin(), handles.end(), [](const TimerHandle& h) { return !h.active(); }),
                  handles.end());
    if (handle) handles.push_back(handle);
    return handle;
}

void TimerGroup::cancelAll() {
    for (const TimerHandle& handle : handles) handle.cancel();
    handles.clear();
}

size_t TimerGroup::size() const {
    return static_cast<size_t>(std::count_if(handles.begin(), handles.end(), [](const TimerHandle& h) { return h.active(); }));
}

// ------------------------------------------------------------ the scheduler

Scheduler::Scheduler(const char* name): schedulerName(name), ownerThread(std::this_thread::get_id()) {
    for (auto& chunk : chunks) chunk.store(nullptr, std::memory_order_relaxed);
    SchedulerRegistry& r = schedulerRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.schedulers.push_back(this);
}

Scheduler::~Scheduler() {
    {
        SchedulerRegistry& r = schedulerRegistry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.schedulers.erase(std::remove(r.schedulers.begin(), r.schedulers.end(), this), r.schedulers.end());
    }
    clear();
    for (auto& chunk : chunks) delete[] chunk.load(std::memory_order_relaxed);
}

Scheduler& Scheduler::game() {
    // Never destroyed: tasks may hold Lua references that must not be released after exit
    static Scheduler* instance = new Scheduler("game");
    return *instance;
}

size_t Scheduler::cancelOwnerEverywhere(uint64_t ownerKey) {
    std::vector<Scheduler*> schedulers;
    {
        SchedulerRegistry& r = schedulerRegistry();
        std::lock_guard<std::mutex> lock(r.mutex);
        schedulers = r.schedulers;
    }
    size_t cancelled = 0;
    for (Scheduler* scheduler : schedulers) cancelled += scheduler->cancelOwner(ownerKey);
    return cancelled;
}

void Scheduler::setOwnerThread() {
    ownerThread = std::this_thread::get_id();
}

bool Scheduler::isOwnerThread() const {
    return std::this_thread::get_id() == ownerThread;
}

Scheduler::Slot& Scheduler::slotAt(uint32_t index) const {
    return chunks[index / CHUNK_SLOTS].load(std::memory_order_acquire)[index % CHUNK_SLOTS];
}

uint32_t Scheduler::allocateSlot() {
    // mutex held; what the owner thread freed is its own to reuse first
    if (!ownerFreed.empty() && isOwnerThread()) {
        const uint32_t index = ownerFreed.back();
        ownerFreed.pop_back();
        return index;
    }
    if (freeSlots.empty()) {
        const size_t count = chunkCount.load(std::memory_order_relaxed);
        if (count == MAX_CHUNKS) return UINT32_MAX;
        Slot* chunk = new Slot[CHUNK_SLOTS];
        const uint32_t base = static_cast<uint32_t>(count * CHUNK_SLOTS);
        chunks[count].store(chunk, std::memory_order_release);
        chunkCount.store(count + 1, std::memory_order_release);
        freeSlots.reserve(freeSlots.size() + CHUNK_SLOTS);
        for (size_t i = CHUNK_SLOTS; i-- > 0;) freeSlots.push_back(base + static_cast<uint32_t>(i));
    }
    const uint32_t index = freeSlots.back();
    freeSlots.pop_back();
    return index;
}

void Scheduler::freeSlot(uint32_t index, Slot& slot) {
    // owner thread: the task (and what it captured) goes now, stale handles become inert
    slot.inUse = false;
    slot.queued = false;
    // heap entries of its last push are stale from now on, even once the slot is reused
    slot.seq = 0;
    const uint64_t state = slot.state.load(std::memory_order_acquire);
    slot.state.store((generationOf(state) + 1) << kGenerationShift, std::memory_order_release);
    slot.task.reset();
    // back to the free list in batches (returnFreed): no lock per task
    ownerFreed.push_back(index);
}

void Scheduler::returnFreed() {
    if (ownerFreed.empty()) return;
    std::lock_guard<std::mutex> lock(mutex);
    freeSlots.insert(freeSlots.end(), ownerFreed.begin(), ownerFreed.end());
    ownerFreed.clear();
}

TimerHandle Scheduler::schedule(TimeDomain domain, double dueOrDelay, bool absolute, double period, Task task,
                                const TimerOpts& opts) {
    if (!task) return {};
    const bool owner = isOwnerThread();
    std::unique_lock<std::mutex> lock(mutex);
    const uint32_t index = allocateSlot();
    if (index == UINT32_MAX) {
        lock.unlock();
        JE_LOG_ERROR(logCategoryEngine(), "Scheduler '%s': too many pending tasks", schedulerName);
        return {};
    }
    Slot& slot = slotAt(index);
    slot.task = std::move(task);
    slot.period = period;
    slot.repeating = period > 0.0;
    slot.counted = slot.repeating && opts.count > 0;
    slot.remaining = opts.count;
    slot.catchUp = opts.catchUp;
    slot.domain = domain;
    slot.ownerKey = opts.ownerKey;
    slot.debugName = opts.debugName;
    slot.inUse = true;
    slot.queued = true;
    const uint64_t generation = generationOf(slot.state.load(std::memory_order_acquire));
    if (owner && !absolute) {
        lock.unlock();
        slot.due = time[static_cast<size_t>(domain)] + dueOrDelay;
        push(domain, index, slot);
    } else if (owner) {
        lock.unlock();
        slot.due = dueOrDelay;
        push(domain, index, slot);
    } else {
        // the owner thread places it (its clocks are its own)
        slot.due = absolute ? dueOrDelay : 0.0;
        inbox.push_back(InboxEntry{index, generation, false, absolute ? -1.0 : dueOrDelay});
        inboxSize.store(inbox.size(), std::memory_order_release);
    }
    return TimerHandle{this, handleId(index, generation)};
}

void Scheduler::push(TimeDomain domain, uint32_t index, Slot& slot) {
    slot.seq = nextSeq++;
    std::vector<HeapEntry>& heap = heaps[static_cast<size_t>(domain)];
    heap.push_back(HeapEntry{slot.due, slot.seq, index});
    std::push_heap(heap.begin(), heap.end(), [](const HeapEntry& a, const HeapEntry& b) { return greaterEntry(a, b); });
}

TimerHandle Scheduler::post(Task task, TimerOpts opts) {
    if (!task) return {};
    std::lock_guard<std::mutex> lock(mutex);
    const uint32_t index = allocateSlot();
    if (index == UINT32_MAX) return {};
    Slot& slot = slotAt(index);
    slot.task = std::move(task);
    slot.period = 0.0;
    slot.repeating = false;
    slot.counted = false;
    slot.catchUp = false;
    slot.domain = TimeDomain::Real;
    slot.ownerKey = opts.ownerKey;
    slot.debugName = opts.debugName ? opts.debugName : "main-thread task";
    slot.inUse = true;
    slot.queued = true;
    const uint64_t generation = generationOf(slot.state.load(std::memory_order_acquire));
    inbox.push_back(InboxEntry{index, generation, true, 0.0});
    inboxSize.store(inbox.size(), std::memory_order_release);
    return TimerHandle{this, handleId(index, generation)};
}

TimerHandle Scheduler::after(double delay, Task task, TimeDomain domain, TimerOpts opts) {
    return schedule(domain, std::max(0.0, delay), false, 0.0, std::move(task), opts);
}

TimerHandle Scheduler::every(double period, Task task, TimeDomain domain, TimerOpts opts) {
    if (!(period > 0.0)) return {};
    return schedule(domain, period, false, period, std::move(task), opts);
}

TimerHandle Scheduler::nextFrame(Task task, TimerOpts opts) {
    return schedule(TimeDomain::Frames, 1.0, false, 0.0, std::move(task), opts);
}

TimerHandle Scheduler::afterFrames(uint32_t frames, Task task, TimerOpts opts) {
    return schedule(TimeDomain::Frames, static_cast<double>(std::max<uint32_t>(frames, 1)), false, 0.0, std::move(task), opts);
}

TimerHandle Scheduler::afterFixedSteps(uint32_t steps, Task task, TimerOpts opts) {
    return schedule(TimeDomain::FixedSteps, static_cast<double>(std::max<uint32_t>(steps, 1)), false, 0.0, std::move(task), opts);
}

TimerHandle Scheduler::at(TimeDomain domain, double timeValue, Task task, TimerOpts opts) {
    return schedule(domain, timeValue, true, 0.0, std::move(task), opts);
}

bool Scheduler::cancel(TimerHandle handle) noexcept {
    if (handle.scheduler != this || handle.id == 0) return false;
    const uint64_t index = handle.id & kSlotMask;
    if (index >= chunkCount.load(std::memory_order_acquire) * CHUNK_SLOTS) return false;
    Slot& slot = slotAt(static_cast<uint32_t>(index));
    const uint64_t wanted = handle.id >> kSlotBits;
    uint64_t state = slot.state.load(std::memory_order_acquire);
    while (true) {
        if ((generationOf(state) & kHandleGenerationMask) != wanted || (state & kCancelled)) return false;
        if (slot.state.compare_exchange_weak(state, state | kCancelled, std::memory_order_acq_rel)) break;
    }
    cancelledCount.fetch_add(1, std::memory_order_relaxed);
    if (isOwnerThread()) {
        // freed right away (unless it is the task running now: after its run)
        if (!(state & kRunning)) {
            freeSlot(static_cast<uint32_t>(index), slot);
        }
    } else {
        remoteCancels.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool Scheduler::isActive(TimerHandle handle) const noexcept {
    if (handle.scheduler != this || handle.id == 0) return false;
    const uint64_t index = handle.id & kSlotMask;
    if (index >= chunkCount.load(std::memory_order_acquire) * CHUNK_SLOTS) return false;
    const uint64_t state = slotAt(static_cast<uint32_t>(index)).state.load(std::memory_order_acquire);
    return (generationOf(state) & kHandleGenerationMask) == (handle.id >> kSlotBits) && !(state & kCancelled);
}

size_t Scheduler::cancelOwner(uint64_t ownerKey) {
    if (ownerKey == 0) return 0;
    // found under the lock (other threads fill slots under it), cancelled outside it
    std::vector<TimerHandle> owned;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t slots = chunkCount.load(std::memory_order_acquire) * CHUNK_SLOTS;
        for (size_t i = 0; i < slots; i++) {
            const Slot& slot = slotAt(static_cast<uint32_t>(i));
            if (!slot.inUse || slot.ownerKey != ownerKey) continue;
            owned.push_back(TimerHandle{this, handleId(static_cast<uint32_t>(i), generationOf(slot.state.load(std::memory_order_acquire)))});
        }
    }
    size_t cancelled = 0;
    for (const TimerHandle& handle : owned) {
        if (cancel(handle)) cancelled++;
    }
    returnFreed();
    return cancelled;
}

void Scheduler::advance(TimeDomain domain, double delta) {
    if (delta > 0.0) time[static_cast<size_t>(domain)] += delta;
}

double Scheduler::now(TimeDomain domain) const {
    return time[static_cast<size_t>(domain)];
}

bool Scheduler::runSlot(uint32_t index, Slot& slot, bool guarded) {
    uint64_t state = slot.state.load(std::memory_order_acquire);
    // started unless cancelled first: after this a cancel only stops repeats
    while (true) {
        if (state & kCancelled) {
            freeSlot(index, slot);
            return false;
        }
        if (slot.state.compare_exchange_weak(state, state | kRunning, std::memory_order_acq_rel)) break;
    }
    slot.queued = false;
    const TimerHandle previous = running;
    running = TimerHandle{this, handleId(index, generationOf(state))};

    const char* name = slot.debugName ? slot.debugName : "timer";
    auto run = [&slot, name]() {
        try {
            slot.task();
        } catch (const std::exception& e) {
            JE_LOG_ERROR(logCategoryEngine(), "A %s failed: %s", name, e.what());
        } catch (...) {
            JE_LOG_ERROR(logCategoryEngine(), "A %s failed with an unknown error.", name);
        }
    };
    if (guarded) {
        std::string errorInfo;
        const bool ok = CrashHook::runGuarded([](void* data) { (*static_cast<decltype(run)*>(data))(); }, &run, errorInfo);
        if (!ok) {
            slot.state.fetch_or(kCancelled, std::memory_order_acq_rel);
            CrashHook::report(name, errorInfo);
        }
    } else {
        run();
    }
    running = previous;
    ran++;
    state = slot.state.fetch_and(~kRunning, std::memory_order_acq_rel) & ~kRunning;
    return !(state & kCancelled);
}

size_t Scheduler::process(TimeDomain domain) {
    std::vector<HeapEntry>& heap = heaps[static_cast<size_t>(domain)];
    if (heap.empty()) return 0;
    const auto compare = [](const HeapEntry& a, const HeapEntry& b) { return greaterEntry(a, b); };
    // a little slack: 30 frames of 1/60 s add up to 0.5 give or take the last bit
    const double nowTime = time[static_cast<size_t>(domain)] + 1e-9 * std::max(1.0, std::abs(time[static_cast<size_t>(domain)]));
    // only what was scheduled before this call, and due now
    const uint64_t seqLimit = nextSeq;
    const bool guarded = CrashHook::active();
    size_t count = 0;
    while (!heap.empty() && heap.front().due <= nowTime) {
        std::pop_heap(heap.begin(), heap.end(), compare);
        const HeapEntry entry = heap.back();
        heap.pop_back();
        Slot& slot = slotAt(entry.slot);
        if (!slot.inUse || entry.seq != slot.seq) continue;   // freed (cancelled) or pushed again since
        if (entry.seq >= seqLimit) {
            deferred.push_back(entry);
            continue;
        }
        // periods that elapsed: one run (up to 8 with catchUp), the next due keeps the phase
        uint64_t missed = 1;
        if (slot.repeating) {
            missed = static_cast<uint64_t>(std::floor((nowTime - slot.due) / slot.period)) + 1;
        }
        int runs = slot.catchUp ? static_cast<int>(std::min<uint64_t>(missed, kMaxCatchUp)) : 1;
        bool keep = true;
        while (runs-- > 0 && keep) {
            keep = runSlot(entry.slot, slot, guarded);
            count++;
            if (!keep) break;
            if (slot.counted && --slot.remaining == 0) keep = false;
            if (!slot.repeating) keep = false;
        }
        if (!slot.inUse) continue;        // freed while running (cancelled run of its own)
        if (keep) {
            slot.due += static_cast<double>(missed) * slot.period;
            slot.queued = true;
            push(domain, entry.slot, slot);   // seq >= seqLimit: not again in this call
        } else {
            freeSlot(entry.slot, slot);
        }
    }
    for (const HeapEntry& entry : deferred) {
        heap.push_back(entry);
        std::push_heap(heap.begin(), heap.end(), compare);
    }
    deferred.clear();
    returnFreed();
    return count;
}

size_t Scheduler::processPosted() {
    if (remoteCancels.exchange(0, std::memory_order_acq_rel) > 0) sweepCancelled();
    if (inboxSize.load(std::memory_order_acquire) == 0) return 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        inboxWork.swap(inbox);
        inboxSize.store(0, std::memory_order_release);
    }
    size_t count = 0;
    const bool guarded = CrashHook::active();
    for (const InboxEntry& entry : inboxWork) {
        Slot& slot = slotAt(entry.slot);
        if (!slot.inUse || generationOf(slot.state.load(std::memory_order_acquire)) != entry.generation) continue;
        if (entry.posted) {
            runSlot(entry.slot, slot, guarded);
            count++;
            if (slot.inUse) freeSlot(entry.slot, slot);
            continue;
        }
        if (entry.delay >= 0.0) slot.due = time[static_cast<size_t>(slot.domain)] + entry.delay;
        push(slot.domain, entry.slot, slot);
    }
    inboxWork.clear();
    returnFreed();
    return count;
}

bool Scheduler::hasPosted() const {
    return inboxSize.load(std::memory_order_acquire) > 0;
}

void Scheduler::sweepCancelled() {
    std::vector<uint32_t> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t slots = chunkCount.load(std::memory_order_acquire) * CHUNK_SLOTS;
        for (size_t i = 0; i < slots; i++) {
            const Slot& slot = slotAt(static_cast<uint32_t>(i));
            if (!slot.inUse) continue;
            const uint64_t state = slot.state.load(std::memory_order_acquire);
            if ((state & kCancelled) && !(state & kRunning)) cancelled.push_back(static_cast<uint32_t>(i));
        }
    }
    for (uint32_t index : cancelled) freeSlot(index, slotAt(index));
    returnFreed();
}

void Scheduler::clear() {
    std::vector<uint32_t> live;
    {
        std::lock_guard<std::mutex> lock(mutex);
        inbox.clear();
        inboxSize.store(0, std::memory_order_release);
        const size_t slots = chunkCount.load(std::memory_order_acquire) * CHUNK_SLOTS;
        for (size_t i = 0; i < slots; i++) {
            Slot& slot = slotAt(static_cast<uint32_t>(i));
            if (!slot.inUse) continue;
            const uint64_t state = slot.state.fetch_or(kCancelled, std::memory_order_acq_rel);
            // the task running now (clear() called from it) is freed after its run
            if (!(state & kRunning)) live.push_back(static_cast<uint32_t>(i));
        }
    }
    for (auto& heap : heaps) heap.clear();
    deferred.clear();
    for (uint32_t index : live) {
        cancelledCount.fetch_add(1, std::memory_order_relaxed);
        freeSlot(index, slotAt(index));
    }
    returnFreed();
}

double Scheduler::nextDueRealTime() const {
    if (hasPosted()) return 0.0;
    const std::vector<HeapEntry>& heap = heaps[static_cast<size_t>(TimeDomain::Real)];
    if (heap.empty()) return -1.0;
    return std::max(0.0, heap.front().due - time[static_cast<size_t>(TimeDomain::Real)]);
}

SchedulerStats Scheduler::stats() const {
    SchedulerStats result;
    for (size_t d = 0; d < TIME_DOMAIN_COUNT; d++) {
        result.now[d] = time[d];
        for (const HeapEntry& entry : heaps[d]) {
            const Slot& slot = slotAt(entry.slot);
            const uint64_t state = slot.state.load(std::memory_order_acquire);
            if (slot.inUse && entry.seq == slot.seq && !(state & kCancelled)) {
                result.pending[d]++;
            }
        }
    }
    result.posted = inboxSize.load(std::memory_order_acquire);
    result.ran = ran;
    result.cancelled = cancelledCount.load(std::memory_order_relaxed);
    return result;
}

std::vector<TimerInfo> Scheduler::pending(const TimeDomain* domain, size_t max) const {
    std::vector<TimerInfo> result;
    for (size_t d = 0; d < TIME_DOMAIN_COUNT; d++) {
        if (domain && static_cast<size_t>(*domain) != d) continue;
        for (const HeapEntry& entry : heaps[d]) {
            const Slot& slot = slotAt(entry.slot);
            const uint64_t state = slot.state.load(std::memory_order_acquire);
            if (!slot.inUse || entry.seq != slot.seq || (state & kCancelled)) continue;
            TimerInfo info;
            info.id = handleId(entry.slot, generationOf(state));
            info.domain = static_cast<TimeDomain>(d);
            info.due = slot.due;
            info.remaining = slot.due - time[d];
            info.period = slot.period;
            info.remainingRuns = slot.counted ? slot.remaining : 0;
            info.ownerKey = slot.ownerKey;
            info.debugName = slot.debugName ? slot.debugName : "timer";
            result.push_back(std::move(info));
        }
    }
    std::sort(result.begin(), result.end(), [](const TimerInfo& a, const TimerInfo& b) { return a.remaining < b.remaining; });
    if (result.size() > max) result.resize(max);
    return result;
}
