// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HIT_STOP_EXEMPT_COMPONENT_H
#define HIT_STOP_EXEMPT_COMPONENT_H

#include "Export.h"

namespace jasperengine{

    // Hit Stop Exempt (docs/specs/S48-pixel-fx.md R21): while a hit-stop freezes the game, this
    // GameObject (and, with includeChildren, its descendants) keeps running at the unfrozen delta
    // in its Animator, Animation Player, Pose Animator, Sprite Animation, Actions, Tweens,
    // Particle Systems, Camera Shaker, Camera Follow, PixelFX effects and its scripts
    // (Time.deltaTime inside its callbacks). Rigidbodies run with the physics world.
    struct JASPERENGINE_API HitStopExemptComponent{
        bool enabled = true;
        bool includeChildren = true;
    };

}

#endif //HIT_STOP_EXEMPT_COMPONENT_H
