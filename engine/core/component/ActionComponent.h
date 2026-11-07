// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ACTION_COMPONENT_H
#define ACTION_COMPONENT_H

#include "Engine.h"

namespace jasperengine{

    enum class ActionState{
        Running,
        Paused,
        Stopped
    };

    struct JASPERENGINE_API ActionComponent{
        ActionState state = ActionState::Stopped;
        float timecount = 0;

        bool startTrigger = false;
        bool stopTrigger = false;
        bool pauseTrigger = false;

        bool ownedTarget = false;

        Entity target = NULL_ENTITY;

        float speed = 1;

        // Seconds into the action (its own time, before speed) a new run starts at, in Play mode
        // and builds; editor previews start at 0. Clamped to >= 0.
        float startOffset = 0;
        // A new run starts at a random point of one cycle instead (ActionSystem::getDuration),
        // the same every playthrough (seed: the entity id and the run). Overrides startOffset.
        bool randomStart = false;
        // Runtime: where the current run started (0, startOffset or the random point) and how
        // many runs started (Random Start's seed). Not saved.
        float phase = 0;
        uint32_t runs = 0;

        // Blend weight (0..1) this action contributes when its output is combined
        // with other actions on the same target. Propagated from the owning
        // AnimationComponent during crossfades. Runtime-only (not serialized).
        float weight = 1;

        FunctionSubscribe<void()> onStart;
        FunctionSubscribe<void()> onPause;
        FunctionSubscribe<void()> onStop;

        FunctionSubscribe<void()> onStep;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onStart); f(onPause); f(onStop); f(onStep); }
    };

    
}

#endif //ACTION_COMPONENT_H