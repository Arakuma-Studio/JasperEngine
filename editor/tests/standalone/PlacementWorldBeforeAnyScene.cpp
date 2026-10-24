// SPDX-License-Identifier: MIT
//
// A PlacementWorld made before anything else in the process has set Jolt up (no Scene, so no
// PhysicsSystem, has run): its members allocate through Jolt as they are constructed, so the
// world must set Jolt up first. The editor test binary can't check this itself: its static
// editor App builds a Scene before main. Run by PlacementWorldReview.WorldBeforeAnySceneInAFreshProcess.
// Exit 0: fine; 2: Jolt was already set up, so nothing was proven; a crash: the bug.

#include "physics/PlacementWorld.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Core/Factory.h"
#endif

#include <cstdio>

int main() {
#ifdef JASPERENGINE_PHYSICS_3D
    if (JPH::Factory::sInstance != nullptr) {
        std::fprintf(stderr, "Jolt was set up before main: nothing to check\n");
        return 2;
    }
#endif
    jasperengine::PlacementWorld world(nullptr, jasperengine::PlacementWorldSettings());
    world.step();
    if (world.getStepCount() != 1 || !world.allAsleep()) return 1;
    std::printf("placement world before any scene: ok\n");
    return 0;
}
