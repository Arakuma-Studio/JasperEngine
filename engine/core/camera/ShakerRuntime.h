// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 5.6, 7.2, 7.3)
// - Purpose: the play state of one Camera Shaker (CameraShakerComponent::runtime): its shake
//   instances, its trauma, the offset it applied this frame and, in transform mode, the base the
//   additive offset is written over. Never serialized; a cloned component starts without one.
// - Lifetime: owned by the component (shared_ptr); ShakeInstance handles hold weak_ptrs, so a
//   destroyed shaker (GameObject destroyed, component removed, scene unloaded) leaves them
//   invalid.
// - Main thread (ECS).

#ifndef SHAKER_RUNTIME_H
#define SHAKER_RUNTIME_H

#include "Export.h"
#include "camera/ShakeInstance.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::camera{

    // at most this many live instances per shaker (the oldest one-shot goes first)
    constexpr size_t MAX_SHAKE_INSTANCES = 32;

    struct JASPERENGINE_API ShakerInstanceEntry{
        uint64_t id = 0;                // unique in the process (handles find it by id)
        std::string presetName;         // built-in name, .shake path or "(table)"
        ShakeInstance instance;
        int unscaled = -1;              // -1: the shaker's unscaledTime; 0 scaled; 1 unscaled
    };

    struct JASPERENGINE_API ShakerRuntime{
        std::vector<ShakerInstanceEntry> instances;
        TraumaState trauma;

        // the offset applied this frame, after the 2D rule, units and gains (rotation in degrees)
        Vector3 offsetPosition = Vector3(0, 0, 0);
        Vector3 offsetRotation = Vector3(0, 0, 0);
        // an offset was written last frame (the view offset set, or the transform-mode base moved)
        bool applied = false;

        // enabled and active in hierarchy at the last update (startPreset plays on the edges)
        bool wasActive = false;
        // the instance startPreset started (0: none): an enable replays it only once it is gone,
        // so disabling and enabling again never piles start shakes up
        uint64_t startInstanceId = 0;

        // transform mode: the base under the additive offset and what was written over it
        // (PoseBlendBone's rule: a value that is not what was written was written by someone else)
        bool transformActive = false;
        bool anchored = false;                  // the base is the RectTransform's positionOffset
        // the base was written back before this frame's scripts (CameraControlSystem::preUpdate):
        // what the GameObject holds now is the base, whatever wrote it this frame
        bool baseRestored = false;
        Vector3 basePosition = Vector3(0, 0, 0);
        Vector3 writtenPosition = Vector3(0, 0, 0);
        Vector2 baseAnchorOffset = Vector2(0, 0);
        Vector2 writtenAnchorOffset = Vector2(0, 0);
        Quaternion baseRotation;
        Quaternion writtenRotation;

        bool warnedCap = false;
        bool warnedBody = false;

        // S48 tickRate: the shake's clock since the last tick and the offset held until the next
        float tickClock = 0.0f;
        bool tickHeld = false;
        ShakeOffset heldRaw;

        ShakerRuntime();

        // a new id for an instance
        static uint64_t nextInstanceId();
        ShakerInstanceEntry* findInstance(uint64_t id);
        const ShakerInstanceEntry* findInstance(uint64_t id) const;
        // nothing to step or apply (the system's early-out)
        bool isIdle() const { return instances.empty() && trauma.trauma <= 0.0f && !applied; }
    };

}

#endif //SHAKER_RUNTIME_H
