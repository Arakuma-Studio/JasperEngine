// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MEMORY_MEMORYSTATS_H
#define MEMORY_MEMORYSTATS_H

// One registry of named memory counters: every Arena, ObjectPool, RangeAllocator and
// TrackedBytes registers a record under its name for its lifetime. snapshot() copies them
// for tools (the Jobs & Memory panel, get_memory_stats) without taking a lock: each record
// is a seqlock, so a reader never blocks a writer and never sees a torn set of values.

#include "Export.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine { namespace memory {

    // Where the counted bytes live.
    enum class Residency : uint8_t {
        Cpu,     // process memory
        Gpu,     // device memory
        Mapped,  // host memory the device reads (staging, mapped buffers)
    };

    // What registered a record.
    enum class StatsKind : uint8_t {
        Arena,   // an Arena or a thread's scratch arena: bytes in use, high water, block bytes, blocks
        Pool,    // an ObjectPool: live slot bytes, peak, block bytes, live objects
        Range,   // a RangeAllocator, in bytes: live ranges, peak, capacity, live ranges
        Tracked, // a TrackedBytes: what its owner reported
    };

    JASPERENGINE_API const char* residencyName(Residency residency);
    JASPERENGINE_API const char* kindName(StatsKind kind);

    struct StatsEntry {
        std::string name;
        StatsKind kind = StatsKind::Tracked;
        Residency residency = Residency::Cpu;
        uint64_t current = 0;
        uint64_t peak = 0;
        uint64_t capacity = 0;
        uint64_t count = 0;
    };

    // Every record registered at the moment of the call, in registration-slot order.
    // Lock-free for the reader; allocates the returned vector.
    JASPERENGINE_API std::vector<StatsEntry> snapshot();

    namespace detail {
        struct StatsRecord;

        // A record named `name` (copied, at most 47 characters). Never null: past the
        // registry's capacity, records share one unlisted overflow record.
        JASPERENGINE_API StatsRecord* registerRecord(std::string_view name, StatsKind kind, Residency residency);
        JASPERENGINE_API void unregisterRecord(StatsRecord* record);
        // Owner-serialized records (arena, pool, range): a consistent set of values.
        JASPERENGINE_API void publish(StatsRecord* record, uint64_t current, uint64_t peak, uint64_t capacity, uint64_t count);
    }

    // A named byte count that a subsystem keeps current for memory the allocators do not
    // see: texture and mesh pools, the Lua heap, Jolt's temp allocator, RmlUi.
    //
    //     static memory::TrackedBytes textures{"Texture.Pool", memory::Residency::Gpu};
    //     textures.add(bytes);        // one more texture of `bytes`
    //     textures.subtract(bytes);
    //
    // add and subtract are relaxed atomics, safe from any thread, with no lock and no
    // allocation. Registered for its whole life, so neither copyable nor movable.
    class JASPERENGINE_API TrackedBytes {
    public:
        TrackedBytes(std::string_view name, Residency residency);
        ~TrackedBytes();

        TrackedBytes(const TrackedBytes&) = delete;
        TrackedBytes& operator=(const TrackedBytes&) = delete;

        // `items`: what the record's count counts (allocations, textures...).
        void add(uint64_t bytes, uint64_t items = 1);
        // Subtracts at most the current values: an over-reported release leaves 0.
        void subtract(uint64_t bytes, uint64_t items = 1);
        void setCapacity(uint64_t bytes);
        uint64_t current() const;

    private:
        detail::StatsRecord* record;
    };

}}

#endif // MEMORY_MEMORYSTATS_H
