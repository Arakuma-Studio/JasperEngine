// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

namespace jasperengine::diag::crash::detail {
// PC is emitted separately. All operations here are allocation/lock free for signal use.
inline size_t crashCallerStart(void* const* frames, size_t count, uintptr_t pc,
                              size_t handlerFrames, uintptr_t addressMask = ~uintptr_t{0},
                              uintptr_t handlerReturnPc = 0) {
    size_t fallback = handlerFrames < count ? handlerFrames : count;
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(frames[i]) & addressMask;
        if (pc && address == (pc & addressMask)) return i + 1;
        if (handlerReturnPc && address == (handlerReturnPc & addressMask)) fallback = i + 1;
    }
    // Apple unwinders can omit the exact interrupted PC. Keep the surviving callers
    // rather than reducing a useful trace to just the separately saved PC.
    return fallback;
}
}
