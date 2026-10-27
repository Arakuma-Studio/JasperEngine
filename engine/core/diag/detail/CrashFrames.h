// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

namespace jasperengine::diag::crash::detail {
// PC is emitted separately. All operations here are allocation/lock free for signal use.
inline size_t crashCallerStart(void* const* frames, size_t count, uintptr_t pc,
                              size_t handlerFrames, uintptr_t addressMask = ~uintptr_t{0}) {
    (void)handlerFrames;
    for (size_t i = 0; i < count; ++i) {
        if (pc && (reinterpret_cast<uintptr_t>(frames[i]) & addressMask) == (pc & addressMask)) return i + 1;
    }
    return count;
}
}
