// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISESWITCH_H
#define NOISE_NOISESWITCH_H

// Whether the JASPERENGINE_NOISE module is in this build (T2 R42). OFF compiles every module file to
// nothing. The standalone golden tool has no engine and no module wiring, and always wants it.

#if defined(JASPERENGINE_NOISE) || defined(JASPERENGINE_NOISE_SERIAL)
    #define JASPERENGINE_NOISE_ENABLED 1
#else
    #define JASPERENGINE_NOISE_ENABLED 0
#endif

#endif // NOISE_NOISESWITCH_H
