// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor tests' one definition of System's test seam: a unity build merges the sources that
// would each define it

#include "System.h"

namespace jasperengine {
    // TEST-ONLY seam (System.h befriends it): detach / restore the instance
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}
