// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The tests compile the math sources without the engine library. Angle (used by the Matrix3/
// Matrix4 rotation helpers) asks the engine for its angle unit: the engine default, degrees.

#include "Engine.h"

using namespace jasperengine;

bool Engine::isUseDegrees(){
    return true;
}
