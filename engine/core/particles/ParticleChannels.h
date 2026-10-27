// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The idea (channels allocated per declared mask, fixed capacity, swap-back removal) is
// OpenEngine's ParticleChannels (Engine/Modules/Particles/Include/Particles/ParticleChannels.h),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef PARTICLES_PARTICLECHANNELS_H
#define PARTICLES_PARTICLECHANNELS_H

// SoA particle storage (docs/specs/S20-particles.md D3, R16): one array per channel the program
// declares, each 64-byte aligned, capacity padded to a multiple of 8, all in one block from the
// capacity-class pool. Removing a particle moves the last one into its slot.

#include "Export.h"
#include "particles/ParticleProgram.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace jasperengine { namespace particles {

    // Blocks of power-of-two sizes, reused by emitters that bind, restart or replay (R16). A
    // released block waits on its class's free list; trim() frees what waited longer than the
    // keep time. Thread-safe.
    class JASPERENGINE_API StoragePool {
    public:
        static constexpr double kKeepSeconds = 10.0;

        StoragePool() = default;
        ~StoragePool();
        StoragePool(const StoragePool&) = delete;
        StoragePool& operator=(const StoragePool&) = delete;

        // at least `bytes`, 64-byte aligned, zeroed; `granted` is the class size
        void* acquire(size_t bytes, size_t& granted);
        void release(void* block, size_t granted, double now);
        // frees the blocks released before now - keepSeconds
        void trim(double now, double keepSeconds = kKeepSeconds);
        void clear();

        size_t bytesFree() const;
        size_t bytesInUse() const;
        size_t blocksFree() const;
        // allocations made from the system (a steady state makes none)
        uint64_t systemAllocations() const;

        // the process pool the emitters use
        static StoragePool& shared();

    private:
        struct FreeBlock {
            void* block;
            double releasedAt;
        };
        mutable std::mutex mutex;
        std::vector<FreeBlock> freeLists[48];
        size_t inUse = 0;
        size_t free = 0;
        uint64_t allocations = 0;
    };

    class JASPERENGINE_API ParticleChannels {
    public:
        ParticleChannels() = default;
        ~ParticleChannels();
        ParticleChannels(const ParticleChannels&) = delete;
        ParticleChannels& operator=(const ParticleChannels&) = delete;

        // storage for `capacity` particles of the channels in `mask`; keeps the block when it
        // is already large enough (a restart); clears the count
        void configure(ChannelMask mask, uint32_t capacity, StoragePool& pool, double now);
        // returns the block to the pool
        void release(double now);

        float* get(Channel c) const { return channels[c]; }
        uint32_t* getU(Channel c) const { return reinterpret_cast<uint32_t*>(channels[c]); }
        bool has(Channel c) const { return (mask & channelBit(c)) != 0; }
        ChannelMask channelMask() const { return mask; }
        uint32_t capacity() const { return cap; }
        // the allocated stride of every channel (capacity padded to 8)
        uint32_t stride() const { return paddedCapacity; }
        uint32_t count = 0;

        // moves particle `from` into slot `to` (every channel)
        void move(uint32_t from, uint32_t to);
        // copies `n` particles from `src` (same mask, any stride) starting at srcFirst into dstFirst
        void copyFrom(float* const* src, uint32_t srcFirst, uint32_t dstFirst, uint32_t n);
        // FNV-1a over every channel of the live particles, in channel order (determinism tests)
        uint64_t hash() const;
        size_t bytesPerParticle() const;

        // a full copy of the live particles (seek checkpoints)
        struct Snapshot {
            ChannelMask mask = 0;
            uint32_t count = 0;
            std::vector<uint32_t> data;   // channel after channel, `count` values each
        };
        void save(Snapshot& out) const;
        void restore(const Snapshot& in);

    private:
        float* channels[ChCount] = {};
        ChannelMask mask = 0;
        uint32_t cap = 0;
        uint32_t paddedCapacity = 0;
        void* block = nullptr;
        size_t blockBytes = 0;
        StoragePool* owner = nullptr;
    };

}}

#endif // PARTICLES_PARTICLECHANNELS_H
