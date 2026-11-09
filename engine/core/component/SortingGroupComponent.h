// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef SORTING_GROUP_COMPONENT_H
#define SORTING_GROUP_COMPONENT_H

#include "Export.h"

#include <cstdint>

namespace jasperengine{

    // Unity's Sorting Group (spec S52 R5): the GameObject's subtree sorts as one item at the
    // group's Sorting Layer and Order in Layer (and its pivot along the Transparency Sort Axis);
    // inside it the renderers sort among themselves. Groups nest; a disabled group is transparent
    // to sorting
    struct JASPERENGINE_API SortingGroupComponent{
        bool enabled = true;
        int32_t sortingLayer = 0;   // a SortingLayers id
        int32_t sortingOrder = 0;   // Order in Layer, -32768..32767
    };

}

#endif //SORTING_GROUP_COMPONENT_H
