// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Portions adapted from OpenEngine's AssetDecodeGate (Engine/Source/Assets/AssetDecodeGate.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "BackgroundGate.h"

#include <algorithm>
#include <cassert>

using namespace jasperengine;
using namespace jasperengine::jobs;

namespace{
    // Gate slots Heavy work never takes: the decodes the frame waits on run there.
    constexpr int kSlotsKeptFromHeavy = 2;

    inline uint32_t takenOf(uint64_t word){ return (uint32_t)(word & 0xffffffffu); }
    inline uint32_t heavyOf(uint64_t word){ return (uint32_t)(word >> 32); }
    inline uint64_t pack(uint32_t taken, uint32_t heavy){ return ((uint64_t)heavy << 32) | taken; }
}

BackgroundGate::BackgroundGate(int workers, int reserve, std::function<void()> onRelease):
    workers(std::max(workers, 0)), reserve(std::max(reserve, 0)), onRelease(std::move(onRelease)){
}

int BackgroundGate::maxBackgroundFor(int workers, int reserve, int maxOverride){
    if (maxOverride > 0)
        return maxOverride;
    return std::max(1, workers - std::max(reserve, 0));
}

int BackgroundGate::heavyMaxFor(int maxBackground){
    return std::max(1, maxBackground - kSlotsKeptFromHeavy);
}

int BackgroundGate::maxBackground() const{
    return maxBackgroundFor(workers, reserve.load(std::memory_order_relaxed), maxOverride.load(std::memory_order_relaxed));
}

int BackgroundGate::heavyMax() const{
    return heavyMaxFor(maxBackground());
}

int BackgroundGate::taken() const{
    return (int)takenOf(slots.load(std::memory_order_relaxed));
}

int BackgroundGate::heavyTaken() const{
    return (int)heavyOf(slots.load(std::memory_order_relaxed));
}

void BackgroundGate::setReserve(int value){
    reserve.store(std::max(value, 0), std::memory_order_relaxed);
    // a larger maximum may admit queued work now
    if (onRelease)
        onRelease();
}

void BackgroundGate::setMaxOverride(int value){
    maxOverride.store(std::max(value, 0), std::memory_order_relaxed);
    if (onRelease)
        onRelease();
}

bool BackgroundGate::canAcquire(GateClass gateClass) const{
    const uint64_t word = slots.load(std::memory_order_relaxed);
    const int maxSlots = maxBackground();
    if ((int)takenOf(word) >= maxSlots)
        return false;
    return gateClass != GateClass::Heavy || (int)heavyOf(word) < heavyMaxFor(maxSlots);
}

bool BackgroundGate::tryAcquire(GateClass gateClass){
    const int maxSlots = maxBackground();
    const int maxHeavy = heavyMaxFor(maxSlots);
    uint64_t word = slots.load(std::memory_order_relaxed);
    for (;;){
        const uint32_t taken = takenOf(word);
        const uint32_t heavy = heavyOf(word);
        if ((int)taken >= maxSlots)
            return false;
        if (gateClass == GateClass::Heavy && (int)heavy >= maxHeavy)
            return false;
        const uint64_t next = pack(taken + 1, gateClass == GateClass::Heavy ? heavy + 1 : heavy);
        if (slots.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_relaxed)){
            uint64_t peak = peaks.load(std::memory_order_relaxed);
            for (;;){
                const uint64_t raised = pack(std::max(takenOf(peak), takenOf(next)), std::max(heavyOf(peak), heavyOf(next)));
                if (raised == peak || peaks.compare_exchange_weak(peak, raised, std::memory_order_relaxed))
                    break;
            }
            return true;
        }
    }
}

BackgroundGate::Peaks BackgroundGate::takePeaks(){
    const uint64_t peak = peaks.exchange(slots.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return {(int)takenOf(peak), (int)heavyOf(peak)};
}

BackgroundGate::Peaks BackgroundGate::peeks() const{
    const uint64_t peak = peaks.load(std::memory_order_relaxed);
    return {(int)takenOf(peak), (int)heavyOf(peak)};
}

void BackgroundGate::release(GateClass gateClass){
    uint64_t word = slots.load(std::memory_order_relaxed);
    for (;;){
        const uint32_t taken = takenOf(word);
        const uint32_t heavy = heavyOf(word);
        assert(taken > 0 && "BackgroundGate::release without a slot tryAcquire took");
        assert((gateClass != GateClass::Heavy || heavy > 0) && "BackgroundGate::release of a Heavy slot never taken");
        if (taken == 0)
            return;
        const uint64_t next = pack(taken - 1, gateClass == GateClass::Heavy && heavy > 0 ? heavy - 1 : heavy);
        if (slots.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_relaxed))
            break;
    }
    if (onRelease)
        onRelease();
}
