// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemScheduler.h"

#include "Scene.h"
#include "SubSystem.h"
#include "Log.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;

namespace {

    // the access a declared system running on this thread has (D10)
    struct ThreadAccess {
        const ScheduledSystem* system = nullptr;
        Signature allowed;
    };

    thread_local ThreadAccess threadAccess;

    double nowUs(){
        using namespace std::chrono;
        return (double)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0;
    }

    constexpr uint32_t kExclusiveResources = SystemResource::Lua | SystemResource::GPU | SystemResource::Audio | SystemResource::Physics;

    // sets and clears the thread's system state even when the system throws
    struct RunGuard {
        bool declared;

        RunGuard(const ChangeClock* clock, const ScheduledSystem& system, Tick tick, bool declared): declared(declared) {
            if (declared){
                ChangeClock::setThreadSystemTick(clock, tick);
                threadAccess.system = &system;
                threadAccess.allowed = system.getDescriptor().reads | system.getDescriptor().writes;
            }
        }

        ~RunGuard(){
            if (declared){
                ChangeClock::clearThreadSystemTick();
                threadAccess.system = nullptr;
            }
        }
    };

    struct WaveLock {
        ChangeClock& clock;
        bool locked;

        WaveLock(ChangeClock& clock, bool locked): clock(clock), locked(locked) {
            if (locked) clock.setWaveLocked(true);
        }

        ~WaveLock(){
            if (locked) clock.setWaveLocked(false);
        }
    };

}

void ecs::checkComponentAccess(ComponentId id){
    const ScheduledSystem* system = threadAccess.system;
    if (system && !threadAccess.allowed.test(id)){
        Log::error("ECS: system '%s' accessed component type %u it did not declare in reads or writes", system->getName().c_str(), (unsigned)id);
        assert(false && "ECS: undeclared component access");
    }
}

const char* jasperengine::systemPhaseName(SystemPhase phase){
    switch (phase){
        case SystemPhase::FixedUpdate: return "FixedUpdate";
        case SystemPhase::Update: return "Update";
        case SystemPhase::PostUpdate: return "PostUpdate";
        case SystemPhase::Draw: return "Draw";
    }
    return "Update";
}

void SystemTiming::record(double ms){
    last = ms;
    samples[next] = (float)ms;
    next = (next + 1) % kWindow;
    if (count < kWindow){
        count++;
    }
    calls++;
}

double SystemTiming::averageMs() const {
    if (count == 0){
        return 0.0;
    }
    double total = 0.0;
    for (size_t i = 0; i < count; i++){
        total += samples[i];
    }
    return total / (double)count;
}

double SystemTiming::maxMs() const {
    double result = 0.0;
    for (size_t i = 0; i < count; i++){
        result = std::max(result, (double)samples[i]);
    }
    return result;
}

bool ScheduledSystem::isExclusive() const {
    return (descriptor.resources & (SystemResource::Exclusive | SystemResource::Structural)) != 0;
}

SystemScheduler::ProfileBegin SystemScheduler::profileBegin = nullptr;
SystemScheduler::ProfileEnd SystemScheduler::profileEnd = nullptr;

SystemScheduler::SystemScheduler(){
    if (const char* value = std::getenv("JE_ECS_SERIAL")){
        serialRequested = std::strcmp(value, "0") != 0;
    }
}

size_t SystemScheduler::workerCount(){
    // no job system to run a wave's systems on: every wave runs on the calling thread, in
    // its systems' order (which does not change the result)
    return 0;
}

void SystemScheduler::setProfilerHooks(ProfileBegin begin, ProfileEnd end){
    profileBegin = begin;
    profileEnd = end;
}

ScheduledSystem& SystemScheduler::add(SystemDescriptor descriptor, ScheduledSystem::Function function, SubSystem* owner){
    if (descriptor.everyNFrames == 0){
        descriptor.everyNFrames = 1;
    }
    auto system = std::make_unique<ScheduledSystem>();
    system->defaultEnabled = descriptor.enabled;
    system->defaultEveryNFrames = descriptor.everyNFrames;
    system->descriptor = std::move(descriptor);
    system->function = std::move(function);
    system->owner = owner;
    system->registration = systems.size();
    plans[(size_t)system->descriptor.phase].dirty = true;
    systems.push_back(std::move(system));
    return *systems.back();
}

bool SystemScheduler::conflicts(const ScheduledSystem& a, const ScheduledSystem& b){
    if (a.isExclusive() || b.isExclusive()){
        return true;
    }
    const SystemDescriptor& da = a.descriptor;
    const SystemDescriptor& db = b.descriptor;
    if (da.writes.intersects(db.writes) || da.writes.intersects(db.reads) || da.reads.intersects(db.writes)){
        return true;
    }
    if ((da.resources & db.resources & kExclusiveResources) != 0){
        return true;
    }
    auto dependsOn = [](const SystemDescriptor& d, const std::string& name){
        return std::find(d.dependsOn.begin(), d.dependsOn.end(), name) != d.dependsOn.end();
    };
    return dependsOn(da, db.name) || dependsOn(db, da.name);
}

void SystemScheduler::buildPlan(SystemPhase phase){
    PhasePlan& plan = plans[(size_t)phase];
    plan.waves.clear();
    plan.dirty = false;

    std::vector<ScheduledSystem*> remaining;
    for (const auto& system : systems){
        if (system->descriptor.phase == phase){
            remaining.push_back(system.get());
        }
    }

    // Registration order, moved only to come after what they depend on: each step takes the
    // first remaining system whose dependencies (in this phase) are all placed
    std::vector<ScheduledSystem*> order;
    order.reserve(remaining.size());
    auto placed = [&order](const std::string& name){
        return std::any_of(order.begin(), order.end(), [&name](const ScheduledSystem* s){ return s->descriptor.name == name; });
    };
    auto inPhase = [&](const std::string& name){
        return std::any_of(remaining.begin(), remaining.end(), [&name](const ScheduledSystem* s){ return s->descriptor.name == name; })
            || placed(name);
    };
    while (!remaining.empty()){
        auto next = std::find_if(remaining.begin(), remaining.end(), [&](const ScheduledSystem* s){
            for (const std::string& dependency : s->descriptor.dependsOn){
                if (dependency != s->descriptor.name && inPhase(dependency) && !placed(dependency)){
                    return false;
                }
            }
            return true;
        });
        if (next == remaining.end()){
            Log::error("ECS: the dependsOn of the %s systems form a cycle; '%s' runs in registration order",
                systemPhaseName(phase), remaining.front()->descriptor.name.c_str());
            next = remaining.begin();
        }
        order.push_back(*next);
        remaining.erase(next);
    }

    // Order-preserving wave packing: after the last earlier system it conflicts with
    for (size_t i = 0; i < order.size(); i++){
        uint32_t wave = 0;
        for (size_t j = 0; j < i; j++){
            if (conflicts(*order[i], *order[j])){
                wave = std::max(wave, order[j]->wave + 1);
            }
        }
        order[i]->wave = wave;
        if (plan.waves.size() <= wave){
            plan.waves.resize(wave + 1);
        }
        plan.waves[wave].push_back(order[i]);
    }
}

const std::vector<std::vector<ScheduledSystem*>>& SystemScheduler::getWaves(SystemPhase phase){
    if (plans[(size_t)phase].dirty){
        buildPlan(phase);
    }
    return plans[(size_t)phase].waves;
}

bool SystemScheduler::setEnabled(const std::string& name, bool enabled){
    bool found = false;
    for (const auto& system : systems){
        if (system->descriptor.name == name){
            found = true;
            if (system->descriptor.enabled != enabled){
                system->descriptor.enabled = enabled;
                system->framesSinceRun = 0;
                system->pendingDt = 0.0;
            }
        }
    }
    if (found){
        // a SubSystem hears it once, not once per phase
        for (const auto& system : systems){
            if (system->descriptor.name == name && system->owner){
                system->owner->onSchedulerEnabledChanged(enabled);
                break;
            }
        }
    }
    return found;
}

bool SystemScheduler::setEveryNFrames(const std::string& name, uint32_t everyNFrames){
    bool found = false;
    for (const auto& system : systems){
        if (system->descriptor.name == name){
            system->descriptor.everyNFrames = everyNFrames == 0 ? 1 : everyNFrames;
            found = true;
        }
    }
    return found;
}

void SystemScheduler::resetDebugOptions(){
    std::vector<std::string> reenabled;
    for (const auto& system : systems){
        system->descriptor.everyNFrames = system->defaultEveryNFrames;
        if (system->descriptor.enabled != system->defaultEnabled){
            if (std::find(reenabled.begin(), reenabled.end(), system->descriptor.name) == reenabled.end()){
                reenabled.push_back(system->descriptor.name);
            }
        }
    }
    for (const std::string& name : reenabled){
        for (const auto& system : systems){
            if (system->descriptor.name == name){
                setEnabled(name, system->defaultEnabled);
                break;
            }
        }
    }
}

bool SystemScheduler::oldestGate(Tick& tick) const {
    bool found = false;
    for (const auto& system : systems){
        if (system->descriptor.enabled && system->timing.callCount() > 0){
            if (!found || system->gate.last < tick){
                tick = system->gate.last;
                found = true;
            }
        }
    }
    return found;
}

void SystemScheduler::runSystem(Scene& scene, ScheduledSystem& system, double dt, bool declared, uint32_t wave){
    const double start = nowUs();
    {
        RunGuard guard(&scene.getChangeClock(), system, system.tick, declared);
        SystemContext context{scene, dt, system.commands, system.gate, system.tick};
        if (profileBegin){
            profileBegin(system.descriptor.name.c_str());
        }
        system.function(context);
        if (profileEnd){
            profileEnd();
        }
    }
    const double end = nowUs();
    // entry-sample contract: what it changed at its tick is not new to it next time
    system.gate.last = system.tick;
    system.timing.record((end - start) / 1000.0);
    currentTrace.push_back({&system.descriptor.name, system.descriptor.phase, wave, start - frameStartUs, end - frameStartUs});
}

void SystemScheduler::runPhase(Scene& scene, SystemPhase phase, double dt){
    if (!frameOpen){
        frameOpen = true;
        frameStartUs = nowUs();
        currentTrace.clear();
    }

    const auto& waves = getWaves(phase);
    ChangeClock& clock = scene.getChangeClock();
    std::vector<ScheduledSystem*> running;
    std::vector<double> runDt;

    for (uint32_t waveIndex = 0; waveIndex < (uint32_t)waves.size(); waveIndex++){
        running.clear();
        runDt.clear();
        for (ScheduledSystem* system : waves[waveIndex]){
            if (!system->descriptor.enabled){
                continue;
            }
            // every N frames, with the time of the frames it skipped
            system->pendingDt += dt;
            if (++system->framesSinceRun < system->descriptor.everyNFrames){
                continue;
            }
            running.push_back(system);
            runDt.push_back(system->pendingDt);
            system->framesSinceRun = 0;
            system->pendingDt = 0.0;
        }
        if (running.empty()){
            continue;
        }

        const bool declared = !(running.size() == 1 && running[0]->isExclusive());
        if (declared){
            // what the wave reads or writes is compacted and sorted before it starts
            Signature arrays;
            for (const ScheduledSystem* system : running){
                arrays |= system->descriptor.reads;
                arrays |= system->descriptor.writes;
            }
            arrays.forEachSet([&scene](size_t id){
                if (ComponentArrayBase* array = scene.getComponentArray((ComponentId)id)){
                    array->settle();
                }
            });
        }

        for (ScheduledSystem* system : running){
            system->tick = clock.advance();
        }

        {
            StructuralDeferScope scope(scene);
            WaveLock lock(clock, declared);
            for (size_t i = 0; i < running.size(); i++){
                runSystem(scene, *running[i], runDt[i], declared, waveIndex);
            }
            // the gates are samples: a change after the wave is newer than all of them
            clock.sample();
        }

        // after the wave, in registration order
        std::vector<ScheduledSystem*> byRegistration(running);
        std::sort(byRegistration.begin(), byRegistration.end(), [](const ScheduledSystem* a, const ScheduledSystem* b){
            return a->registration < b->registration;
        });
        for (ScheduledSystem* system : byRegistration){
            if (!system->commands.empty()){
                system->commands.playback(scene);
            }
        }
    }

    if (phase == SystemPhase::Draw){
        lastTrace.swap(currentTrace);
        currentTrace.clear();
        frameOpen = false;
    }
}
