// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISETYPES_H
#define NOISE_NOISETYPES_H

// What the kernels, the plan and the runtime API all name. No kernel code here: anything may include it.

#include <cstdint>

namespace jasperengine { namespace noise {

    // Which back end a batch runs on. Scalar is its own instantiation only under
    // JASPERENGINE_NOISE_ALL_BACKENDS (editor and tests): players carry Native alone (R51).
    enum class Backend : uint8_t { Native, Scalar };

    struct Interval {
        float min = 0.0f;
        float max = 0.0f;
    };

}}

#endif // NOISE_NOISETYPES_H
