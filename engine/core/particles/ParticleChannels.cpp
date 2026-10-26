// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleChannels.h"

#include <algorithm>
#include <cstring>
#include <new>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr size_t kAlignment = 64;
    constexpr size_t kSmallestClass = 4096;

    // the class of a request: the power of two at or above it (from 4 KiB)
    size_t classIndex(size_t bytes, size_t& size){
        size = kSmallestClass;
        size_t index = 0;
        while (size < bytes){
            size <<= 1;
            index++;
        }
        return index;
    }

}

StoragePool::~StoragePool(){
    clear();
}

void* StoragePool::acquire(size_t bytes, size_t& granted){
    const size_t index = classIndex(bytes, granted);
    std::lock_guard<std::mutex> lock(mutex);
    void* block = nullptr;
    std::vector<FreeBlock>& list = freeLists[index];
    if (!list.empty()){
        block = list.back().block;
        list.pop_back();
        free -= granted;
    }else{
        block = ::operator new(granted, std::align_val_t(kAlignment));
        allocations++;
    }
    inUse += granted;
    std::memset(block, 0, granted);
    return block;
}

void StoragePool::release(void* block, size_t granted, double now){
    if (!block)
        return;
    size_t size = 0;
    const size_t index = classIndex(granted, size);
    std::lock_guard<std::mutex> lock(mutex);
    freeLists[index].push_back({block, now});
    inUse -= granted;
    free += granted;
}

void StoragePool::trim(double now, double keepSeconds){
    std::lock_guard<std::mutex> lock(mutex);
    size_t size = kSmallestClass;
    for (std::vector<FreeBlock>& list : freeLists){
        list.erase(std::remove_if(list.begin(), list.end(), [&](const FreeBlock& b){
            if (now - b.releasedAt <= keepSeconds)
                return false;
            ::operator delete(b.block, std::align_val_t(kAlignment));
            free -= size;
            return true;
        }), list.end());
        size <<= 1;
    }
}

void StoragePool::clear(){
    std::lock_guard<std::mutex> lock(mutex);
    for (std::vector<FreeBlock>& list : freeLists){
        for (const FreeBlock& b : list)
            ::operator delete(b.block, std::align_val_t(kAlignment));
        list.clear();
    }
    free = 0;
}

size_t StoragePool::bytesFree() const{
    std::lock_guard<std::mutex> lock(mutex);
    return free;
}

size_t StoragePool::bytesInUse() const{
    std::lock_guard<std::mutex> lock(mutex);
    return inUse;
}

size_t StoragePool::blocksFree() const{
    std::lock_guard<std::mutex> lock(mutex);
    size_t n = 0;
    for (const std::vector<FreeBlock>& list : freeLists)
        n += list.size();
    return n;
}

uint64_t StoragePool::systemAllocations() const{
    std::lock_guard<std::mutex> lock(mutex);
    return allocations;
}

StoragePool& StoragePool::shared(){
    static StoragePool pool;
    return pool;
}

ParticleChannels::~ParticleChannels(){
    release(0.0);
}

void ParticleChannels::configure(ChannelMask newMask, uint32_t capacity, StoragePool& pool, double now){
    const uint32_t padded = (capacity + 7u) & ~7u;
    // each channel starts on a 64-byte line: the stride is rounded up to 16 floats
    const uint32_t stride = (padded + 15u) & ~15u;
    size_t channelCount = 0;
    for (int c = 0; c < ChCount; c++){
        if (newMask & channelBit((Channel)c))
            channelCount++;
    }
    const size_t bytes = std::max<size_t>(channelCount * stride * sizeof(float), 64);
    if (block && (owner != &pool || bytes > blockBytes)){
        release(now);
    }
    if (!block){
        block = pool.acquire(bytes, blockBytes);
        owner = &pool;
    }else{
        std::memset(block, 0, blockBytes);
    }
    mask = newMask;
    cap = capacity;
    paddedCapacity = stride;
    count = 0;
    float* cursor = static_cast<float*>(block);
    for (int c = 0; c < ChCount; c++){
        if (newMask & channelBit((Channel)c)){
            channels[c] = cursor;
            cursor += stride;
        }else{
            channels[c] = nullptr;
        }
    }
}

void ParticleChannels::release(double now){
    if (block && owner)
        owner->release(block, blockBytes, now);
    block = nullptr;
    blockBytes = 0;
    owner = nullptr;
    for (float*& c : channels)
        c = nullptr;
    mask = 0;
    cap = 0;
    paddedCapacity = 0;
    count = 0;
}

void ParticleChannels::move(uint32_t from, uint32_t to){
    for (float* c : channels){
        if (c)
            c[to] = c[from];
    }
}

void ParticleChannels::copyFrom(float* const* src, uint32_t srcFirst, uint32_t dstFirst, uint32_t n){
    for (int c = 0; c < ChCount; c++){
        if (channels[c] && src[c])
            std::memcpy(channels[c] + dstFirst, src[c] + srcFirst, n * sizeof(float));
    }
}

uint64_t ParticleChannels::hash() const{
    uint64_t h = 1469598103934665603ull;
    for (int c = 0; c < ChCount; c++){
        if (!channels[c])
            continue;
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(channels[c]);
        for (size_t b = 0; b < (size_t)count * sizeof(float); b++){
            h ^= bytes[b];
            h *= 1099511628211ull;
        }
    }
    return h;
}

size_t ParticleChannels::bytesPerParticle() const{
    size_t n = 0;
    for (float* c : channels){
        if (c)
            n += sizeof(float);
    }
    return n;
}

void ParticleChannels::save(Snapshot& out) const{
    out.mask = mask;
    out.count = count;
    size_t channelCount = 0;
    for (float* c : channels){
        if (c)
            channelCount++;
    }
    out.data.resize(channelCount * count);
    uint32_t* cursor = out.data.data();
    for (float* c : channels){
        if (!c)
            continue;
        std::memcpy(cursor, c, count * sizeof(float));
        cursor += count;
    }
}

void ParticleChannels::restore(const Snapshot& in){
    if (in.mask != mask || in.count > cap)
        return;
    count = in.count;
    const uint32_t* cursor = in.data.data();
    for (float* c : channels){
        if (!c)
            continue;
        std::memcpy(c, cursor, count * sizeof(float));
        cursor += count;
    }
}
