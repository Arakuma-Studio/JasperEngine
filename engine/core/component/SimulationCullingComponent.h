// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SIMULATION_CULLING_COMPONENT_H
#define SIMULATION_CULLING_COMPONENT_H

#include <cstdint>

namespace jasperengine{

    enum class SimulationState : uint8_t { Full, Reduced, Culled };
    enum class SimulationCullReason : uint8_t { None, Distance, GroundNotLoaded };

    // Simulation Culling (docs/specs/culling-group-and-simulation-distance.md R10-R18): while the
    // GameObject is beyond Simulation Distance from every simulation focus, or over ground that is
    // not loaded, its scripts' update callbacks are skipped, its Animator stops and its dynamic
    // bodies freeze (kinematic, in place); between Reduced Rate Distance and Simulation Distance its
    // scripts' onUpdate and onPostUpdate run every Reduced Update Interval frames. It applies to
    // the GameObject and the descendants whose nearest Simulation Culling is this one. Evaluated by
    // CullingSystem while the scene runs; a disabled component is Full and acts on nothing.
    struct SimulationCullingComponent{
        bool enabled = true;

        float simulationDistance = 64.0f;   // metres from the nearest focus to the bounds' surface
        float reducedRateDistance = 0.0f;   // 0: no reduced band
        int reducedUpdateInterval = 4;      // frames, 2..60
        float hysteresis = 2.0f;            // metres past an edge before moving outward
        float boundsRadius = 0.0f;          // 0: the rendered or collider bounds when it starts, else 0.5 m
        bool simulateWhenVisible = true;    // a visible GameObject beyond the distance is Reduced (Full without a band)
        bool freezeWhenGroundNotLoaded = true;
        bool cullScripts = true;
        bool cullAnimator = true;
        bool cullPhysics = true;

        // runtime, not saved
        SimulationState state = SimulationState::Full;
        SimulationCullReason reason = SimulationCullReason::None;
        float focusDistance = 0.0f;         // metres to the nearest focus, from the bounds' surface

        // runtime bookkeeping of CullingSystem
        float resolvedRadius = -1.0f;       // < 0: not resolved from bounds yet
        float groundCheckX = 0.0f;          // where the ground was last checked
        float groundCheckZ = 0.0f;
        bool groundChecked = false;
        bool groundLoaded = true;
        bool evaluated = false;             // once evaluated, a disabled component resumes what it froze
    };

}

#endif //SIMULATION_CULLING_COMPONENT_H
