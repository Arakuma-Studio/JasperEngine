// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOCOLORCONVERT_H
#define VIDEOCOLORCONVERT_H

#include "Export.h"
#include "VideoTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    // YUV -> RGB of <video> frames (docs/html-ui/phase-6-video.md §3.6.5): the matrix the
    // HTMLUI_POST Yuv pass applies on the GPU, and the same conversion on the CPU (render
    // interfaces other than RmlRenderer, and the tests' reference).
    class JASPERENGINE_API VideoColorConvert {
    public:
        // Column-major 4x4 (a GLSL mat4): rgb = M * vec4(y, u, v, 1) with y, u, v the R8 texels
        // in [0, 1]; the range offsets and scales are folded in. Rows 3 (alpha) are 0 (the pass
        // writes alpha 1).
        static void matrix(const VideoColor& color, int height, float out[16]);
        // the texture-coordinate scale of the chroma planes: luma width / (2^shift x chroma width)
        // (1 for even sizes; the chroma planes cover a little more than the picture for odd ones)
        static void chromaScale(const VideoFrame& frame, float out[2]);
        // premultiplied (opaque) RGBA8 of a frame, rows top first; chroma sampled bilinearly at
        // the GPU's texture coordinates (clamped to the edge)
        static void toRgba(const VideoFrame& frame, std::vector<uint8_t>& rgba);
    };

}

#endif //VIDEOCOLORCONVERT_H
