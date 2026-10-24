// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_MODIFIER_COMPONENT_H
#define LATTICE_MODIFIER_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include <vector>

namespace jasperengine{

    // Where a binding deforms a skinned mesh (spec §2.1); ignored for meshes without skinning.
    enum class LatticeStage{
        BEFORE_SKINNING, // the bind pose, in mesh space
        AFTER_SKINNING   // the posed mesh
    };

    // Vertex colour channel scaling a binding per vertex (spec §3.4).
    enum class LatticeMaskChannel{
        NONE,
        RED,
        GREEN,
        BLUE,
        ALPHA,
        INVERTED_RED,
        INVERTED_GREEN,
        INVERTED_BLUE,
        INVERTED_ALPHA
    };

    struct JASPERENGINE_API LatticeBinding{
        Entity lattice = NULL_ENTITY;     // entity with a LatticeComponent
        float weight = 1.0f;              // 0..2, scales the deformation
        LatticeStage stage = LatticeStage::AFTER_SKINNING;
        int submesh = -1;                 // only this submesh (material slot), -1 = all
        LatticeMaskChannel mask = LatticeMaskChannel::NONE;
    };

    // Makes this entity's mesh (and with includeChildren its descendants' meshes) follow
    // the listed lattices, in order. At most LATTICE_MAX_PER_MESH apply per mesh.
    struct JASPERENGINE_API LatticeModifierComponent{
        bool enabled = true;
        std::vector<LatticeBinding> bindings;
        bool includeChildren = false;
        int subdivisions = 8; // sprite quads become an n x n grid while deformed, 1..64

        // --- runtime only (not serialized) ---
        bool warnedOverflow = false;
    };

}

#endif //LATTICE_MODIFIER_COMPONENT_H
