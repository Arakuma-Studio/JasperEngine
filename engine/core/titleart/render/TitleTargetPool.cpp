// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleTargetPool.h"

#include <algorithm>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::titleart;

size_t titleart::colorFormatBytes(ColorFormat format) {
    switch (format) {
        case ColorFormat::RED: return 1;
        case ColorFormat::RGBA: return 4;
        case ColorFormat::RED16: return 2;
        case ColorFormat::SRGB8A8: return 4;
        case ColorFormat::RGBA16F: return 8;
        case ColorFormat::RGBA32F: return 16;
        case ColorFormat::R32F: return 4;
        case ColorFormat::RG16F: return 4;
        case ColorFormat::R16F: return 2;
    }
    return 4;
}

bool titleart::isFloatColorFormat(ColorFormat format) {
    switch (format) {
        case ColorFormat::RGBA16F:
        case ColorFormat::RGBA32F:
        case ColorFormat::R32F:
        case ColorFormat::RG16F:
        case ColorFormat::R16F: return true;
        case ColorFormat::RED:
        case ColorFormat::RGBA:
        case ColorFormat::RED16:
        case ColorFormat::SRGB8A8: return false;
    }
    return false;
}

size_t titleart::targetBytes(const TargetKey& key) {
    const size_t pixels = (size_t)std::max(0, key.width) * (size_t)std::max(0, key.height);
    const size_t samples = (size_t)std::max(1, key.samples);
    size_t bytes = pixels * colorFormatBytes(key.format) * samples;
    if (samples > 1) bytes += pixels * colorFormatBytes(key.format);
    if (key.depth != TitleDepthFormat::None) bytes += pixels * 4 * samples;
    return bytes;
}

// ---- TargetRef ----

TargetRef::TargetRef(TargetRef&& other) noexcept
    : pool(other.pool), slot(other.slot), generation(other.generation), targetKey(other.targetKey) {
    other.pool = nullptr;
}

TargetRef& TargetRef::operator=(TargetRef&& other) noexcept {
    if (this != &other) {
        reset();
        pool = other.pool;
        slot = other.slot;
        generation = other.generation;
        targetKey = other.targetKey;
        other.pool = nullptr;
    }
    return *this;
}

TargetRef::~TargetRef() {
    reset();
}

TitleTarget* TargetRef::get() const {
    return pool ? pool->targetOf(slot, generation) : nullptr;
}

void TargetRef::reset() {
    if (pool) pool->giveBack(slot, generation);
    pool = nullptr;
}

// ---- TitleTargetPool ----

TitleTargetPool::TitleTargetPool(TitleTargetAllocator& allocator, size_t maxBytes)
    : allocator(allocator), limit(maxBytes) {
}

TitleTargetPool::~TitleTargetPool() {
    clear();
}

TitleTarget* TitleTargetPool::targetOf(size_t slot, uint64_t refGeneration) const {
    if (refGeneration != generation || slot >= slots.size() || !slots[slot].lent) return nullptr;
    return slots[slot].target.get();
}

void TitleTargetPool::giveBack(size_t slot, uint64_t refGeneration) {
    if (refGeneration != generation || slot >= slots.size() || !slots[slot].lent) return;
    Slot& s = slots[slot];
    s.lent = false;
    s.lastUse = renders;
    s.useOrder = ++useClock;
    counters.bytesLive -= s.bytes;
    counters.bytesIdle += s.bytes;
}

void TitleTargetPool::freeSlot(size_t slot) {
    Slot& s = slots[slot];
    if (!s.target) return;
    if (s.lent) counters.bytesLive -= s.bytes;
    else counters.bytesIdle -= s.bytes;
    s.target.reset();
    s.lent = false;
    s.bytes = 0;
    counters.targets--;
    freeSlots.push_back(slot);
}

bool TitleTargetPool::evictIdleFor(size_t bytes) {
    while (counters.bytesLive + counters.bytesIdle + bytes > limit) {
        size_t oldest = std::numeric_limits<size_t>::max();
        uint64_t oldestOrder = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < slots.size(); i++) {
            if (slots[i].target && !slots[i].lent && slots[i].useOrder < oldestOrder) {
                oldest = i;
                oldestOrder = slots[i].useOrder;
            }
        }
        if (oldest == std::numeric_limits<size_t>::max()) return false;
        freeSlot(oldest);
    }
    return true;
}

TargetRef TitleTargetPool::acquire(const TargetKey& key, std::string* error) {
    counters.acquires++;
    if (key.depth != TitleDepthFormat::None || key.samples > 1) counters.depthOrMsaaAcquires++;
    if (isFloatColorFormat(key.format)) counters.floatAcquires++;
    TargetRef ref;
    if (key.width <= 0 || key.height <= 0) {
        counters.failures++;
        if (error) *error = "out_of_target_memory: an empty target was requested";
        return ref;
    }
    // the most recently returned idle target of the key (its texels are likeliest still cached)
    size_t found = std::numeric_limits<size_t>::max();
    uint64_t newest = 0;
    for (size_t i = 0; i < slots.size(); i++) {
        const Slot& s = slots[i];
        if (s.target && !s.lent && s.key == key && s.useOrder >= newest) {
            found = i;
            newest = s.useOrder;
        }
    }
    if (found != std::numeric_limits<size_t>::max()) {
        counters.reuses++;
    } else {
        const size_t bytes = allocator.bytes(key);
        if (bytes > limit || !evictIdleFor(bytes)) {
            counters.failures++;
            if (error) *error = "out_of_target_memory: " + std::to_string(key.width) + " x " + std::to_string(key.height) +
                                " needs " + std::to_string(bytes) + " bytes, " + std::to_string(counters.bytesLive) +
                                " of " + std::to_string(limit) + " are in use";
            return ref;
        }
        std::string allocationError;
        std::unique_ptr<TitleTarget> target = allocator.create(key, &allocationError);
        if (!target) {
            counters.failures++;
            if (error) *error = allocationError.empty() ? std::string("the render target could not be made") : allocationError;
            return ref;
        }
        if (!freeSlots.empty()) {
            found = freeSlots.back();
            freeSlots.pop_back();
        } else {
            found = slots.size();
            slots.emplace_back();
        }
        Slot& s = slots[found];
        s.target = std::move(target);
        s.key = key;
        s.bytes = bytes;
        s.lent = false;
        counters.targets++;
        counters.allocations++;
        counters.bytesIdle += bytes;
    }
    Slot& s = slots[found];
    s.lent = true;
    counters.bytesIdle -= s.bytes;
    counters.bytesLive += s.bytes;
    counters.peakBytes = std::max(counters.peakBytes, counters.bytesLive);
    ref.pool = this;
    ref.slot = found;
    ref.generation = generation;
    ref.targetKey = key;
    return ref;
}

void TitleTargetPool::endRender() {
    renders++;
    for (size_t i = 0; i < slots.size(); i++) {
        const Slot& s = slots[i];
        if (s.target && !s.lent && renders - s.lastUse >= (uint64_t)TITLE_TARGET_IDLE_RENDERS) freeSlot(i);
    }
}

void TitleTargetPool::trim() {
    for (size_t i = 0; i < slots.size(); i++) {
        if (slots[i].target && !slots[i].lent) freeSlot(i);
    }
}

void TitleTargetPool::clear() {
    slots.clear();
    freeSlots.clear();
    counters.bytesLive = 0;
    counters.bytesIdle = 0;
    counters.targets = 0;
    generation++;
}
