// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISESWITCH_H
#define NOISE_NOISESWITCH_H

// Whether the JASPERENGINE_NOISE module is in this build (T2 R42). OFF compiles every module file to
// nothing. Undefined means on, like navigation's switch: the golden tool has no module wiring, and the
// Xcode project is told JASPERENGINE_NOISE=0 only when an export leaves it out.

#if !defined(JASPERENGINE_NOISE) || JASPERENGINE_NOISE
    #define JASPERENGINE_NOISE_ENABLED 1
#else
    #define JASPERENGINE_NOISE_ENABLED 0
#endif

#endif // NOISE_NOISESWITCH_H
