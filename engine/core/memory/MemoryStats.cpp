// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The registry is a fixed array of records that is never freed, so a reader can walk it
// while owners register and unregister: a slot's seqlock tells the reader whether what it
// copied is one consistent version. Owner-serialized records (arenas, pools, ranges)
// publish their values as one seqlock write; TrackedBytes, written from any thread,
// update each value with an atomic and never touch the sequence.

#include "MemoryStats.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::memory;

namespace jasperengine { namespace memory { namespace detail {

    constexpr size_t kNameWords = 6;   // 48 bytes: 47 characters and the terminator
    constexpr size_t kMaxNameLength = kNameWords * 8 - 1;

    struct StatsRecord {
        // 0 free, 1 being (un)registered, 2 live
        std::atomic<uint32_t> state{0};
        // odd while a version is being written
        std::atomic<uint32_t> sequence{0};
        // the name, as relaxed-atomic words: readers copy it while a slot may be reused
        std::atomic<uint64_t> name[kNameWords] = {};
        std::atomic<uint8_t> kind{0};
        std::atomic<uint8_t> residency{0};
        std::atomic<uint64_t> current{0};
        std::atomic<uint64_t> peak{0};
        std::atomic<uint64_t> capacity{0};
        std::atomic<uint64_t> count{0};
    };

}}}

using detail::StatsRecord;

namespace{
    constexpr size_t kMaxRecords = 1024;

    StatsRecord* records(){
        // immortal: owners with static storage unregister during static destruction
        static StatsRecord* all = new StatsRecord[kMaxRecords];
        return all;
    }

    StatsRecord* overflowRecord(){
        static StatsRecord* overflow = new StatsRecord();
        return overflow;
    }

    std::atomic<size_t> searchStart{0};

    void beginWrite(StatsRecord* record){
        const uint32_t sequence = record->sequence.load(std::memory_order_relaxed);
        record->sequence.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    void endWrite(StatsRecord* record){
        const uint32_t sequence = record->sequence.load(std::memory_order_relaxed);
        record->sequence.store(sequence + 1, std::memory_order_release);
    }

    void subtractClamped(std::atomic<uint64_t>& value, uint64_t amount){
        uint64_t current = value.load(std::memory_order_relaxed);
        for (;;){
            const uint64_t next = current > amount ? current - amount : 0;
            if (value.compare_exchange_weak(current, next, std::memory_order_relaxed))
                return;
        }
    }

    void raisePeak(std::atomic<uint64_t>& peak, uint64_t value){
        uint64_t seen = peak.load(std::memory_order_relaxed);
        while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed)){
        }
    }
}

const char* memory::residencyName(Residency residency){
    switch (residency){
        case Residency::Cpu: return "Cpu";
        case Residency::Gpu: return "Gpu";
        case Residency::Mapped: return "Mapped";
    }
    return "Cpu";
}

const char* memory::kindName(StatsKind kind){
    switch (kind){
        case StatsKind::Arena: return "Arena";
        case StatsKind::Pool: return "Pool";
        case StatsKind::Range: return "Range";
        case StatsKind::Tracked: return "Tracked";
    }
    return "Tracked";
}

StatsRecord* detail::registerRecord(std::string_view name, StatsKind kind, Residency residency){
    StatsRecord* all = records();
    const size_t start = searchStart.load(std::memory_order_relaxed);
    for (size_t k = 0; k < kMaxRecords; k++){
        const size_t index = (start + k) % kMaxRecords;
        StatsRecord* record = &all[index];
        uint32_t expected = 0;
        if (record->state.load(std::memory_order_relaxed) != 0
            || !record->state.compare_exchange_strong(expected, 1, std::memory_order_acquire))
            continue;
        searchStart.store((index + 1) % kMaxRecords, std::memory_order_relaxed);

        beginWrite(record);
        char buffer[kNameWords * 8] = {};
        const size_t length = std::min(name.size(), kMaxNameLength);
        std::memcpy(buffer, name.data(), length);
        for (size_t w = 0; w < kNameWords; w++){
            uint64_t word;
            std::memcpy(&word, buffer + w * 8, 8);
            record->name[w].store(word, std::memory_order_relaxed);
        }
        record->kind.store((uint8_t)kind, std::memory_order_relaxed);
        record->residency.store((uint8_t)residency, std::memory_order_relaxed);
        record->current.store(0, std::memory_order_relaxed);
        record->peak.store(0, std::memory_order_relaxed);
        record->capacity.store(0, std::memory_order_relaxed);
        record->count.store(0, std::memory_order_relaxed);
        endWrite(record);
        record->state.store(2, std::memory_order_release);
        return record;
    }
    return overflowRecord();
}

void detail::unregisterRecord(StatsRecord* record){
    if (!record || record == overflowRecord())
        return;
    record->state.store(1, std::memory_order_relaxed);
    beginWrite(record);
    record->current.store(0, std::memory_order_relaxed);
    record->peak.store(0, std::memory_order_relaxed);
    record->capacity.store(0, std::memory_order_relaxed);
    record->count.store(0, std::memory_order_relaxed);
    endWrite(record);
    record->state.store(0, std::memory_order_release);
}

void detail::publish(StatsRecord* record, uint64_t current, uint64_t peak, uint64_t capacity, uint64_t count){
    beginWrite(record);
    record->current.store(current, std::memory_order_relaxed);
    record->peak.store(peak, std::memory_order_relaxed);
    record->capacity.store(capacity, std::memory_order_relaxed);
    record->count.store(count, std::memory_order_relaxed);
    endWrite(record);
}

std::vector<StatsEntry> memory::snapshot(){
    std::vector<StatsEntry> entries;
    StatsRecord* all = records();
    for (size_t index = 0; index < kMaxRecords; index++){
        StatsRecord* record = &all[index];
        if (record->state.load(std::memory_order_acquire) != 2)
            continue;
        for (int attempt = 0; attempt < 64; attempt++){
            const uint32_t before = record->sequence.load(std::memory_order_acquire);
            if (before & 1u)
                continue;
            if (record->state.load(std::memory_order_acquire) != 2)
                break;
            char buffer[detail::kNameWords * 8];
            for (size_t w = 0; w < detail::kNameWords; w++){
                const uint64_t word = record->name[w].load(std::memory_order_relaxed);
                std::memcpy(buffer + w * 8, &word, 8);
            }
            buffer[sizeof(buffer) - 1] = '\0';
            StatsEntry entry;
            entry.kind = (StatsKind)record->kind.load(std::memory_order_relaxed);
            entry.residency = (Residency)record->residency.load(std::memory_order_relaxed);
            entry.current = record->current.load(std::memory_order_relaxed);
            entry.peak = record->peak.load(std::memory_order_relaxed);
            entry.capacity = record->capacity.load(std::memory_order_relaxed);
            entry.count = record->count.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (record->sequence.load(std::memory_order_relaxed) != before)
                continue;
            entry.name = buffer;
            // a TrackedBytes' peak is raised after its count: never report it below
            entry.peak = std::max(entry.peak, entry.current);
            entries.push_back(std::move(entry));
            break;
        }
    }
    return entries;
}

// ---- TrackedBytes

TrackedBytes::TrackedBytes(std::string_view name, Residency residency):
    record(detail::registerRecord(name, StatsKind::Tracked, residency)){
}

TrackedBytes::~TrackedBytes(){
    detail::unregisterRecord(record);
}

void TrackedBytes::add(uint64_t bytes, uint64_t items){
    const uint64_t now = record->current.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    record->count.fetch_add(items, std::memory_order_relaxed);
    raisePeak(record->peak, now);
}

void TrackedBytes::subtract(uint64_t bytes, uint64_t items){
    subtractClamped(record->current, bytes);
    subtractClamped(record->count, items);
}

void TrackedBytes::setCapacity(uint64_t bytes){
    record->capacity.store(bytes, std::memory_order_relaxed);
}

uint64_t TrackedBytes::current() const{
    return record->current.load(std::memory_order_relaxed);
}
