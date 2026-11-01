// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoColorConvert.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

VideoColor::Matrix VideoColor::resolved(int height) const{
    if (matrix != Matrix::UNSPECIFIED)
        return matrix;
    return height < 720 ? Matrix::BT601 : Matrix::BT709;
}

bool VideoColor::isHdr() const{
    return transfer == 16 || transfer == 18;
}

void VideoColorConvert::matrix(const VideoColor& color, int height, float out[16]){
    for (int i = 0; i < 16; i++)
        out[i] = 0.0f;

    // y, u, v in [0, 1] (texel / 255) to Y' in [0, 1] and Cb, Cr in [-0.5, 0.5]
    double yScale, yOffset, cScale, cOffset;
    if (color.fullRange){
        yScale = 1.0;
        yOffset = 0.0;
        cScale = 1.0;
        cOffset = -128.0 / 255.0;
    }else{
        yScale = 255.0 / 219.0;
        yOffset = -16.0 / 219.0;
        cScale = 255.0 / 224.0;
        cOffset = -128.0 / 224.0;
    }

    // rgb = A * (Y', Cb, Cr)
    double a[3][3];
    const VideoColor::Matrix kind = color.resolved(height);
    if (kind == VideoColor::Matrix::IDENTITY){
        // GBR: the Y plane holds G, U holds B, V holds R (full values, no chroma offset)
        const double scale = color.fullRange ? 1.0 : 255.0 / 219.0;
        const double offset = color.fullRange ? 0.0 : -16.0 / 219.0;
        // column 0 (y) -> g, column 1 (u) -> b, column 2 (v) -> r
        out[0 * 4 + 1] = (float)scale;
        out[1 * 4 + 2] = (float)scale;
        out[2 * 4 + 0] = (float)scale;
        out[3 * 4 + 0] = (float)offset;
        out[3 * 4 + 1] = (float)offset;
        out[3 * 4 + 2] = (float)offset;
        return;
    }
    double kr = 0.2126, kb = 0.0722;
    if (kind == VideoColor::Matrix::BT601){
        kr = 0.299;
        kb = 0.114;
    }else if (kind == VideoColor::Matrix::BT2020){
        kr = 0.2627;
        kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;
    a[0][0] = 1.0; a[0][1] = 0.0;                              a[0][2] = 2.0 * (1.0 - kr);
    a[1][0] = 1.0; a[1][1] = -2.0 * kb * (1.0 - kb) / kg;      a[1][2] = -2.0 * kr * (1.0 - kr) / kg;
    a[2][0] = 1.0; a[2][1] = 2.0 * (1.0 - kb);                 a[2][2] = 0.0;

    // M = A * diag(yScale, cScale, cScale), offset = A * (yOffset, cOffset, cOffset)
    const double scales[3] = {yScale, cScale, cScale};
    const double offsets[3] = {yOffset, cOffset, cOffset};
    for (int row = 0; row < 3; row++){
        double offset = 0.0;
        for (int column = 0; column < 3; column++){
            out[column * 4 + row] = (float)(a[row][column] * scales[column]);
            offset += a[row][column] * offsets[column];
        }
        out[3 * 4 + row] = (float)offset;
    }
}

void VideoColorConvert::chromaScale(const VideoFrame& frame, float out[2]){
    out[0] = 1.0f;
    out[1] = 1.0f;
    if (!frame.hasChroma())
        return;
    out[0] = (float)frame.width / (float)(frame.chromaWidth() << frame.chromaShiftX);
    out[1] = (float)frame.height / (float)(frame.chromaHeight() << frame.chromaShiftY);
}

namespace {

    // a texture sampled with linear filtering and clamp-to-edge at normalized (s, t)
    float sampleLinear(const std::vector<uint8_t>& plane, int width, int height, float s, float t){
        const float x = s * (float)width - 0.5f;
        const float y = t * (float)height - 0.5f;
        const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        const float fx = x - (float)x0, fy = y - (float)y0;
        auto at = [&](int px, int py){
            px = std::clamp(px, 0, width - 1);
            py = std::clamp(py, 0, height - 1);
            return (float)plane[(size_t)py * (size_t)width + (size_t)px];
        };
        const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
        const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
        return (top + (bottom - top) * fy) / 255.0f;
    }

}

void VideoColorConvert::toRgba(const VideoFrame& frame, std::vector<uint8_t>& rgba){
    rgba.resize((size_t)frame.width * (size_t)frame.height * 4);
    float m[16];
    matrix(frame.color, frame.height, m);
    float scale[2];
    chromaScale(frame, scale);
    const int cw = frame.chromaWidth(), ch = frame.chromaHeight();
    for (int y = 0; y < frame.height; y++){
        const float t = ((float)y + 0.5f) / (float)frame.height;
        for (int x = 0; x < frame.width; x++){
            const float s = ((float)x + 0.5f) / (float)frame.width;
            const float Y = (float)frame.y[(size_t)y * (size_t)frame.width + (size_t)x] / 255.0f;
            // monochrome: neutral chroma (128)
            float U = 128.0f / 255.0f, V = 128.0f / 255.0f;
            if (frame.hasChroma()){
                U = sampleLinear(frame.u, cw, ch, s * scale[0], t * scale[1]);
                V = sampleLinear(frame.v, cw, ch, s * scale[0], t * scale[1]);
            }
            uint8_t* out = &rgba[((size_t)y * (size_t)frame.width + (size_t)x) * 4];
            for (int c = 0; c < 3; c++){
                const float value = m[0 * 4 + c] * Y + m[1 * 4 + c] * U + m[2 * 4 + c] * V + m[3 * 4 + c];
                out[c] = (uint8_t)std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
            }
            out[3] = 255;
        }
    }
}
