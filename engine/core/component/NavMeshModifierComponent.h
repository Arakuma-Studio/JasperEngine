// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_MODIFIER_COMPONENT_H
#define NAV_MESH_MODIFIER_COMPONENT_H

#include "Export.h"
#include "math/Vector3.h"
#include "navigation/NavTypes.h"

#include <cstdint>

namespace jasperengine{

    // Whether a bake collects the GameObject (and its subtree with applyToChildren)
    enum class NavModifierInclusion{
        AUTO,       // the surface's policy: moving bodies, agents and obstacles are left out
        INCLUDE,    // collected even if the policy leaves it out
        EXCLUDE     // never collected
    };

    // Unity's NavMeshModifier and NavMeshModifierVolume (spec S19 R5): the area of what a bake
    // collects from this GameObject (and its subtree), its inclusion, or with Use Volume a box
    // that marks the area of whatever navmesh is inside it. Not Walkable removes it.
    struct JASPERENGINE_API NavMeshModifierComponent{
        // component checkbox: a disabled modifier changes nothing
        bool enabled = true;

        NavModifierInclusion overrideInclusion = NavModifierInclusion::AUTO;
        bool overrideArea = false;
        int area = nav::AREA_WALKABLE;
        bool applyToChildren = true;
        // a modifier volume: the box (GameObject space) marks its area on the navmesh
        bool useVolume = false;
        Vector3 volumeCenter = Vector3(0.0f, 1.0f, 0.0f);
        Vector3 volumeSize = Vector3(4.0f, 3.0f, 4.0f);
        // ---- S76: with useVolume, the bake only splits the navmesh along the volume, and while
        // the modifier is enabled the polygons inside take `area` without a rebake (a door, a bridge)
        bool applyAtRuntime = false;
    };

}

#endif //NAV_MESH_MODIFIER_COMPONENT_H
