// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_LINK_COMPONENT_H
#define NAV_MESH_LINK_COMPONENT_H

#include "Export.h"
#include "math/Vector3.h"
#include "navigation/NavTypes.h"

#include <cstdint>

namespace jasperengine{

    // Unity's NavMeshLink (spec S19 R7): an off-mesh connection between two points (in the
    // GameObject's space), baked into the tile holding its start. A width > 0 places
    // ceil(width / (2 * agent radius)) parallel connections across it.
    struct JASPERENGINE_API NavMeshLinkComponent{
        // component checkbox: a disabled link is left out of the bake
        bool enabled = true;

        int agentType = 0;
        Vector3 startPoint = Vector3(0.0f, 0.0f, -2.5f);
        Vector3 endPoint = Vector3(0.0f, 0.0f, 2.5f);
        float width = 0.0f;
        // the cost per metre of the link, -1: its area's
        float costModifier = -1.0f;
        bool bidirectional = true;
        // moving it at runtime moves its connections
        bool autoUpdatePosition = false;
        int area = nav::AREA_JUMP;
        // ---- S55: agents may use it (Unity 2.0's activated). Not a bake input: off, its baked
        // connections pass nothing until it is on again, in edit mode and in play
        bool activated = true;
    };

}

#endif //NAV_MESH_LINK_COMPONENT_H
