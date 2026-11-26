// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef OUTLINE_COMPONENT_H
#define OUTLINE_COMPONENT_H

#include "Export.h"
#include "math/Vector4.h"

namespace jasperengine{

    // QuickOutline's five modes (docs/specs/S94-rendering-anim-gaps.md F22): which parts of the
    // outline band and of the object's hidden silhouette draw. (int: the reflection and the
    // cooked scenes read an enum field as an int)
    enum class OutlineMode{
        OUTLINE_ALL,            // the band, also where something covers it
        OUTLINE_VISIBLE,        // the band where nothing covers it
        OUTLINE_HIDDEN,         // the band only where something covers it
        OUTLINE_AND_SILHOUETTE, // the band everywhere, and the object's covered parts filled
        SILHOUETTE_ONLY         // the object's covered parts filled, no band
    };

    enum class OutlineWidthUnits{
        PIXELS, // a constant width on screen
        WORLD   // world units along the smoothed normal
    };

    // Unity's QuickOutline as a component (S94 F22, F23): outlines the GameObject's Mesh, and with
    // Include Children every Mesh below it (skinned and instanced ones included), after the opaque
    // queue: a stencil mask of each silhouette, then a hull pushed out along the smoothed normals
    // outside every mask (library:outline_stencil). The nearest enabled Outline at or above a Mesh
    // decides its look. Disabled or absent: no pass, no draw, no allocation.
    struct JASPERENGINE_API OutlineComponent{
        bool enabled = true;
        OutlineMode mode = OutlineMode::OUTLINE_ALL;
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f); // linear, straight alpha (HDR above 1)
        float width = 2.0f;                               // in widthUnits
        OutlineWidthUnits widthUnits = OutlineWidthUnits::PIXELS;
        bool includeChildren = true;
    };

}

#endif //OUTLINE_COMPONENT_H
