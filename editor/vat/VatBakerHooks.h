// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 5.2, first bullet)
// - Purpose: the Inspector's "Bake ▾ › Vertex Animation…" entries (Animation Player, Pose Animator
//   and Lattice Modifier sections, and each clip row of the model asset view) reach the Vertex
//   Animation Baker window through these hooks, so InspectorWindow includes no window and no baker
//   (the PoseInspectorHooks pattern). Each entry shows only once `bake` is set (the baker window
//   sets it when it is constructed and clears it in its destructor).
// - Thread-safety: main thread; set once at startup.
// - Invariants: the entries are disabled with the reason while the scene plays (the baker poses the
//   scene); `bake` reports its own problems (the window's status line and the Console).

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor{

    class Project;

    // what an entry bakes
    struct VatBakeEntry{
        enum class Kind{
            AnimationPlayer,    // the GameObject `entity` (its player states, entity Animations, Pose Animator)
            PoseAnimator,       // the Pose Animator of `entity` playing `asset` (.poseanim, as the component stores it)
            LatticeModifier,    // the lattice tracks moving `entity`'s lattices: the nearest animated GameObject at or above it
            ModelClip           // the clip `clip` of the model file `asset` (the model asset view; no scene)
        };
        Kind kind = Kind::AnimationPlayer;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        std::string asset;      // PoseAnimator: the animation; ModelClip: the model (asset-relative or absolute)
        std::string clip;       // ModelClip: the animation's name in the model
    };

    struct VatBakerHooks{
        // Opens the Vertex Animation Baker prefilled with the entry's source (and clip). `now`
        // (Shift-click): bakes at once with the last-used settings, placed beside the source
        // (one undo step), the window showing the progress.
        std::function<void(Project* project, const VatBakeEntry& entry, bool now)> bake;
    };

    // the editor-wide hooks (set by the baker window)
    inline VatBakerHooks& vatBakerHooks(){
        static VatBakerHooks hooks;
        return hooks;
    }

    // the tooltip of the entries
    inline const char* vatBakeEntryTooltip(){
        return "Bake to a Vertex Animation (.vat): the animation plays on the GPU, for crowds.\n"
               "Opens the Vertex Animation Baker with this source; Shift-click bakes now with the last settings\n"
               "and places the result beside the source (one undo step).";
    }

}
