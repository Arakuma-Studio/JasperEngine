// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_SYSTEMSCHEDULER_H
#define ECS_SYSTEMSCHEDULER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Export.h"
#include "Signature.h"
#include "ChangeTracking.h"
#include "CommandBuffer.h"

namespace jasperengine {

    class Scene;
    class SubSystem;

    enum class SystemPhase : uint8_t { FixedUpdate, Update, PostUpdate, Draw };
    inline constexpr size_t kSystemPhaseCount = 4;

    JASPERENGINE_API const char* systemPhaseName(SystemPhase phase);

    // What a system uses besides its component arrays. Two systems that share an exclusive
    // resource (Lua, GPU, Audio, Physics) never run at the same time; MainThread systems run
    // on the thread that runs the phase; an Exclusive system (every SubSystem) or a
    // Structural one (it changes entities directly) runs alone in its wave.
    namespace SystemResource {
        enum : uint32_t {
            MainThread = 1u << 0,
            Lua        = 1u << 1,
            GPU        = 1u << 2,
            Audio      = 1u << 3,
            Physics    = 1u << 4,
            Structural = 1u << 5,
            Exclusive  = 1u << 6
        };
    }

    struct SystemDescriptor {
        std::string name;
        SystemPhase phase = SystemPhase::Update;
        Signature reads;
        Signature writes;
        uint32_t resources = 0;
        // systems of the same phase this one runs after
        std::vector<std::string> dependsOn;
        // runs once every N frames (fixed steps in FixedUpdate) with the time of all of them
        uint32_t everyNFrames = 1;
        bool enabled = true;
    };

    // What a system's function gets: its scene, the time since its last run, its own command
    // buffer (played back after its wave, in registration order), its change gate (changes
    // newer than gate.last are new to it) and the tick its changes are stamped with
    struct SystemContext {
        Scene& scene;
        double dt;
        EntityCommandBuffer& cmd;
        ChangeGate& gate;
        Tick tick;
    };

    // Milliseconds of one system in one phase: the last run, the average and maximum of the
    // last 120 runs, and the number of runs
    class JASPERENGINE_API SystemTiming {
    public:
        static constexpr size_t kWindow = 120;

    private:
        std::array<float, kWindow> samples{};
        size_t count = 0;
        size_t next = 0;
        double last = 0.0;
        uint64_t calls = 0;

    public:
        void record(double ms);
        double lastMs() const { return last; }
        double averageMs() const;
        double maxMs() const;
        uint64_t callCount() const { return calls; }
    };

    // One system's span in the wave trace of a frame (microseconds from the frame start)
    struct WaveTraceEntry {
        const std::string* system;
        SystemPhase phase;
        uint32_t wave;
        double startUs;
        double endUs;
    };

    // A registered system: its descriptor, its place in the schedule and its statistics
    class JASPERENGINE_API ScheduledSystem {
        friend class SystemScheduler;

    public:
        using Function = std::function<void(SystemContext&)>;

    private:
        SystemDescriptor descriptor;
        Function function;
        SubSystem* owner = nullptr; // the SubSystem whose phase method this runs
        size_t registration = 0;

        uint32_t wave = 0;
        EntityCommandBuffer commands;
        ChangeGate gate;
        Tick tick = 0;
        uint32_t framesSinceRun = 0;
        double pendingDt = 0.0;
        SystemTiming timing;

        // the descriptor's values before debug changes (resetDebugOptions)
        bool defaultEnabled = true;
        uint32_t defaultEveryNFrames = 1;

    public:
        const SystemDescriptor& getDescriptor() const { return descriptor; }
        const std::string& getName() const { return descriptor.name; }
        SystemPhase getPhase() const { return descriptor.phase; }
        uint32_t getWave() const { return wave; }
        const SystemTiming& getTiming() const { return timing; }
        const ChangeGate& getGate() const { return gate; }
        bool isEnabled() const { return descriptor.enabled; }
        uint32_t getEveryNFrames() const { return descriptor.everyNFrames; }
        bool isExclusive() const;
        SubSystem* getOwner() const { return owner; }
    };

    // Runs a scene's systems phase by phase.
    //
    // A phase is a sequence of waves. Systems are taken in registration order (moved only
    // after what they dependsOn); each goes into the wave after the last earlier system it
    // conflicts with: writes against reads or writes of the same component types, a shared
    // exclusive resource, a dependsOn, or either one Exclusive/Structural. Conflicting systems
    // therefore keep their registration order; systems of one wave are provably independent.
    //
    // Before a wave its systems' arrays are settled (compacted and sorted). Its systems run
    // inside a StructuralDeferScope; during a wave of declared (non-Exclusive) systems the
    // registry refuses structural changes and index observations that would reorder an
    // array, and every system stamps its changes with its own tick. Each system's command
    // buffer is played back after the wave, in registration order. Waves run their systems
    // one after the other on the calling thread (also with JE_ECS_SERIAL=1 or without
    // worker threads), which gives the same result as running them concurrently.
    class JASPERENGINE_API SystemScheduler {
    public:
        // S15's CPU profiler: called around each system run with "System/Phase"
        using ProfileBegin = void (*)(const char* name);
        using ProfileEnd = void (*)();

    private:
        struct PhasePlan {
            std::vector<std::vector<ScheduledSystem*>> waves;
            bool dirty = true;
        };

        std::vector<std::unique_ptr<ScheduledSystem>> systems;
        std::array<PhasePlan, kSystemPhaseCount> plans;
        bool serialRequested = false;

        std::vector<WaveTraceEntry> currentTrace;
        std::vector<WaveTraceEntry> lastTrace;
        double frameStartUs = 0.0;
        bool frameOpen = false;

        static ProfileBegin profileBegin;
        static ProfileEnd profileEnd;

        void buildPlan(SystemPhase phase);
        static bool conflicts(const ScheduledSystem& a, const ScheduledSystem& b);
        void runSystem(Scene& scene, ScheduledSystem& system, double dt, bool declared, uint32_t wave);

    public:
        SystemScheduler();
        SystemScheduler(const SystemScheduler&) = delete;
        SystemScheduler& operator=(const SystemScheduler&) = delete;

        // A system function; `owner` when it runs a SubSystem's phase method
        ScheduledSystem& add(SystemDescriptor descriptor, ScheduledSystem::Function function, SubSystem* owner = nullptr);

        // Runs the phase's waves with `dt` (each system gets the time since its own last run)
        void runPhase(Scene& scene, SystemPhase phase, double dt);

        // Play-mode debug state: every phase entry of the named system. False when there is
        // no system of that name.
        bool setEnabled(const std::string& name, bool enabled);
        bool setEveryNFrames(const std::string& name, uint32_t everyNFrames);
        // back to the descriptors' values
        void resetDebugOptions();

        // Whether a wave's systems run one after the other on the calling thread. Always so
        // without job-system workers; JE_ECS_SERIAL=1 (or setSerial(true)) asks for it anyway.
        bool isSerial() const { return serialRequested || workerCount() == 0; }
        void setSerial(bool serial){ serialRequested = serial; }
        // the job-system workers a wave can use besides the calling thread
        static size_t workerCount();

        const std::vector<std::unique_ptr<ScheduledSystem>>& getSystems() const { return systems; }
        // the waves of a phase (the plan is rebuilt when a system is added or changed)
        const std::vector<std::vector<ScheduledSystem*>>& getWaves(SystemPhase phase);
        // the wave trace of the last complete frame (its Draw phase ended it)
        const std::vector<WaveTraceEntry>& getLastTrace() const { return lastTrace; }

        // the oldest gate of an enabled system; false when there is none
        bool oldestGate(Tick& tick) const;

        static void setProfilerHooks(ProfileBegin begin, ProfileEnd end);
    };

}

#endif // ECS_SYSTEMSCHEDULER_H
