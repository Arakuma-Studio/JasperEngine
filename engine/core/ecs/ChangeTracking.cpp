// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ChangeTracking.h"

using namespace jasperengine;

namespace {

    // the registry and tick of the scheduled system running on this thread
    struct ThreadSystemTick {
        const ChangeClock* clock = nullptr;
        Tick tick = 0;
    };

    thread_local ThreadSystemTick threadSystemTick;

}

Tick ChangeClock::stamp(){
    if (threadSystemTick.clock == this){
        return threadSystemTick.tick;
    }
    if (sampled){
        current++;
        sampled = false;
    }
    return current;
}

void ChangeClock::setThreadSystemTick(const ChangeClock* clock, Tick tick){
    threadSystemTick.clock = clock;
    threadSystemTick.tick = tick;
}

void ChangeClock::clearThreadSystemTick(){
    threadSystemTick.clock = nullptr;
    threadSystemTick.tick = 0;
}
