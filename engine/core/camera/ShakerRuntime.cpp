// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShakerRuntime.h"

#include <atomic>

using namespace jasperengine;
using namespace jasperengine::camera;

ShakerRuntime::ShakerRuntime(){
    // the trauma noise of each shaker starts somewhere else
    trauma.seed = (float)randomShakeSeed();
}

uint64_t ShakerRuntime::nextInstanceId(){
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1);
}

ShakerInstanceEntry* ShakerRuntime::findInstance(uint64_t id){
    for (ShakerInstanceEntry& entry : instances){
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

const ShakerInstanceEntry* ShakerRuntime::findInstance(uint64_t id) const{
    for (const ShakerInstanceEntry& entry : instances){
        if (entry.id == id) return &entry;
    }
    return nullptr;
}
