// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLE_NOISE_H
#define PARTICLES_PARTICLE_NOISE_H

// The Noise module's field (particles spec 6.3.9): 3D gradient noise (Perlin's improved
// noise, its fade curve and gradient set, a fixed hashed lattice) summed over octaves; each
// axis samples the field at its own offset so the three are uncorrelated. Values stay in
// [-1, 1] per axis (the octave sum is divided by the weights' sum). Unity's noise pattern is
// not reproduced (its algorithm is unpublished); the parameters mean the same.

#include "Export.h"
#include "math/Vector3.h"

namespace jasperengine {
namespace particles {

    // gradient noise at p, in [-1, 1]
    JASPERENGINE_API float gradientNoise3(const Vector3& p);

    // the three-axis fractal sum: `octaves` 1..4, each `octaveScale` times the frequency and
    // `octaveMultiplier` times the weight of the one before
    JASPERENGINE_API Vector3 fbm3(const Vector3& p, int octaves, float octaveMultiplier, float octaveScale);

}
}

#endif //PARTICLES_PARTICLE_NOISE_H
