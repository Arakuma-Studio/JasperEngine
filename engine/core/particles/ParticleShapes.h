// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLE_SHAPES_H
#define PARTICLES_PARTICLE_SHAPES_H

// The emitter shapes of version 2 (particles spec 6.3.1): where a particle starts and the
// direction its start speed goes, in the emitter's frame (the shape's own position, rotation
// and scale applied). Axes: cones, hemispheres and the arc turn around local +Y; circles,
// rectangles and donuts lie in the XZ plane. Gtested (ParticleShapeTest).

#include "Export.h"
#include "ParticleModules.h"
#include "ParticleRandom.h"

namespace jasperengine {
namespace particles {

    struct JASPERENGINE_API ShapeSample {
        Vector3 position = Vector3(0, 0, 0);
        Vector3 direction = Vector3(0, 1, 0);   // unit
    };

    // what the Loop / PingPong / BurstSpread arc and radius modes read
    struct JASPERENGINE_API ShapeContext {
        float arcPhase = 0.0f;      // degrees moved along the arc (arcSpeed x 360 x time)
        float radiusPhase = 0.0f;   // radius lengths moved (radiusSpeed x time)
        int spreadIndex = 0;        // this particle in its burst (BurstSpread)
        int spreadCount = 0;        // the burst's count; 0: not a burst (BurstSpread picks at random)
    };

    // the arc angle in degrees (0 .. arc) by the arc mode
    JASPERENGINE_API float arcAngle(const ParticleShapeModule& shape, const ShapeContext& context, Pcg32& rng);
    // the position along the radius (0 .. 1) by the radius mode (Edge, the radial fraction)
    JASPERENGINE_API float radiusFraction(const ParticleShapeModule& shape, const ShapeContext& context, Pcg32& rng);

    // a sample of the shape (shape disabled: the origin, straight up); randomDirectionAmount,
    // sphericalDirectionAmount and randomPositionAmount applied
    JASPERENGINE_API ShapeSample sampleShape(const ParticleShapeModule& shape, const ShapeContext& context, Pcg32& rng);

    // a random unit vector, uniform on the sphere
    JASPERENGINE_API Vector3 randomUnitVector(Pcg32& rng);

}
}

#endif //PARTICLES_PARTICLE_SHAPES_H
