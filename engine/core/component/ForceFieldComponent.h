// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FORCE_FIELD_COMPONENT_H
#define FORCE_FIELD_COMPONENT_H

// A force field (docs/specs/S20-particles.md R6): particle systems whose External Forces
// module shares one of its layers feel it inside its shape. The shape is the GameObject's
// transform times the unit shape (Sphere: radius 1; Box: half extents 1; Cylinder: radius 1,
// half height 1 along Y; Infinite: everywhere); strength falls off over the distance from
// the centre (0) to the shape's edge (1) by the falloff curve. It is also the wind source
// other systems may read (Directional).

#include "Engine.h"
#include "math/Curve.h"
#include "math/Vector3.h"

#include <cstdint>

namespace jasperengine{

    enum class ForceFieldShapeType{
        Sphere,
        Box,
        Cylinder,
        Infinite
    };

    enum class ForceFieldKind{
        Directional,    // wind along the direction, with turbulence and pulsing
        Radial,         // towards the centre (negative strength: away from it)
        Vortex,         // around the axis
        Turbulence,     // curl noise
        Drag            // slows the particles down
    };

    struct JASPERENGINE_API ForceFieldComponent{
        // component checkbox: a disabled field is not gathered
        bool enabled = true;

        ForceFieldShapeType shape = ForceFieldShapeType::Sphere;
        // the shape's size in the GameObject's space: Sphere x the radius, Box xyz the half
        // extents, Cylinder x the radius and y the half height
        Vector3 size = Vector3(5.0f, 5.0f, 5.0f);
        ForceFieldKind kind = ForceFieldKind::Directional;
        // world units per second squared (Drag: per second)
        float strength = 1.0f;
        // Directional: the wind direction, Vortex: the axis (the GameObject's space)
        Vector3 direction = Vector3(0.0f, 0.0f, 1.0f);
        // Directional: the noise added to the wind (a share of the strength)
        float turbulence = 0.0f;
        // Directional: pulses per second (0: steady); Turbulence and the Directional
        // turbulence: the noise frequency (per world unit)
        float pulseFrequency = 0.0f;
        float frequency = 1.0f;
        // strength over the distance from the centre (0) to the edge (1)
        Curve falloff = Curve::constant(1.0f);
        // the External Forces layers it acts on (bit n: layer n)
        uint32_t layers = 1;
    };

}

#endif //FORCE_FIELD_COMPONENT_H
