// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolVramTracker_h
#define SokolVramTracker_h

// The sokol side of the VRAM tracker (spec S15 R6-R9, core/profiler/GpuMemory.h): sokol's
// make/init and destroy/uninit trace hooks (SokolTraceHub) of images and buffers become
// gpumem entries with their estimated bytes, category and owner. Compiled with the profiler.

#include <cstdint>

struct sg_image_desc;
struct sg_buffer_desc;

namespace jasperengine{

    class SokolVramTracker{
    public:
        static void onImageCreated(uint32_t image, const sg_image_desc* desc);
        static void onImageDestroyed(uint32_t image);
        static void onBufferCreated(uint32_t buffer, const sg_buffer_desc* desc);
        static void onBufferDestroyed(uint32_t buffer);

        // the label an image was made with ("" when unknown)
        static const char* imageLabel(uint32_t image);

        // the estimated bytes of an image as sokol made it (every mip, slice, sample and
        // in-flight copy) and of a buffer
        static uint64_t imageBytes(const sg_image_desc& desc);
        static uint64_t bufferBytes(const sg_buffer_desc& desc);
    };

}

#endif //SokolVramTracker_h
