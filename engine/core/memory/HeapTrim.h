// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MEMORY_HEAPTRIM_H
#define MEMORY_HEAPTRIM_H

#include "Export.h"

#include <cstdint>

namespace jasperengine { namespace memory {

    // Gives the allocator's free pages back to the operating system where it can: glibc Linux
    // (malloc_trim), macOS and iOS (malloc_zone_pressure_relief), Android 9+ (mallopt M_PURGE).
    // False where it does nothing (the web: linear memory cannot shrink; Windows; other
    // allocators). Thread-safe; it walks the heap, so the engine asks for it with requestHeapTrim
    JASPERENGINE_API bool releaseFreeHeap();

    // One Background-lane job runs releaseFreeHeap; a request while one is queued or running is
    // merged into one more run after it. Nothing where releaseFreeHeap does nothing. Asked after
    // a scene load (SceneManager), a Play stop and a scene close (the editor)
    JASPERENGINE_API void requestHeapTrim();

    // trims completed since start (tests, diagnostics)
    JASPERENGINE_API uint64_t heapTrimCount();

}}

#endif // MEMORY_HEAPTRIM_H
