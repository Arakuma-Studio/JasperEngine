// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISECOMPAT_H
#define NOISE_NOISECOMPAT_H

// The value noise the Noise and Erode brushes and fill_noise have always drawn (T2 R36, N13), with
// their outputs preserved. Outside the JASPERENGINE_NOISE module: it is under 1 KiB, and the terrain
// code that calls it is in every player.

#include "Export.h"

#include <cstdint>

namespace jasperengine { namespace noise { namespace compat {

    // One value in [0, 1] per integer cell
    JASPERENGINE_API float hash01(int x, int y, uint32_t seed);
    // Value noise on the integer lattice, in [0, 1]
    JASPERENGINE_API float value(float x, float y, uint32_t seed);
    // Octaves at doubling frequency and halving weight, normalized to [-1, 1]
    JASPERENGINE_API float fractal(float x, float y, uint32_t seed, int octaves);

}}}

#endif // NOISE_NOISECOMPAT_H
