// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The colour maths built a second time, without optimisation (this file's flags,
// engine/tests/core/CMakeLists.txt), in a namespace of its own so it links beside the engine's
// optimised copy: PixelColorDeterminism.UnoptimisedBuildMatches compares the two (S43 AC5).

#define pixelart pixelart_o0
#include "pixelart/PixelColor.cpp"
#include "PixelColorHash.h"
#undef pixelart

uint64_t pixelColorHashUnoptimised() {
    return jasperengine::pixelart_o0::pixelColorHash();
}
