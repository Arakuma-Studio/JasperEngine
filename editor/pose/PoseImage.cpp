// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseImage.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace jasperengine;

namespace {

    // A bounding-box edge as a pixel index, clamped as a float into [-1, size] before the int
    // cast: a huge coordinate on either side of the image (finite, e.g. 1e30) is out of int
    // range and the cast would be undefined. -1 and size are both off-image, so a box entirely
    // outside the image comes out empty after the caller's clamp to [0, size - 1].
    int boxIndex(float edge, int size){
        return (int)std::clamp(edge, -1.0f, (float)size);
    }

    // Printable ASCII 32..126, 10 rows each, bit (0x20 >> column) per pixel (column 5 is the gap)
    const uint8_t FONT_6X10[95][10] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
        {0x00,0x08,0x08,0x08,0x08,0x08,0x00,0x08,0x00,0x00}, // !
        {0x00,0x14,0x14,0x14,0x00,0x00,0x00,0x00,0x00,0x00}, // "
        {0x00,0x14,0x14,0x3E,0x14,0x3E,0x14,0x14,0x00,0x00}, // #
        {0x00,0x08,0x1E,0x28,0x1C,0x0A,0x3C,0x08,0x00,0x00}, // $
        {0x00,0x30,0x32,0x04,0x08,0x10,0x26,0x06,0x00,0x00}, // %
        {0x00,0x18,0x24,0x28,0x10,0x2A,0x24,0x1A,0x00,0x00}, // &
        {0x00,0x08,0x08,0x10,0x00,0x00,0x00,0x00,0x00,0x00}, // '
        {0x00,0x04,0x08,0x10,0x10,0x10,0x08,0x04,0x00,0x00}, // (
        {0x00,0x10,0x08,0x04,0x04,0x04,0x08,0x10,0x00,0x00}, // )
        {0x00,0x00,0x08,0x2A,0x1C,0x2A,0x08,0x00,0x00,0x00}, // *
        {0x00,0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00,0x00}, // +
        {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x08,0x10,0x00}, // ,
        {0x00,0x00,0x00,0x00,0x3E,0x00,0x00,0x00,0x00,0x00}, // -
        {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00}, // .
        {0x00,0x02,0x02,0x04,0x08,0x10,0x20,0x20,0x00,0x00}, // /
        {0x00,0x1C,0x22,0x26,0x2A,0x32,0x22,0x1C,0x00,0x00}, // 0
        {0x00,0x08,0x18,0x08,0x08,0x08,0x08,0x1C,0x00,0x00}, // 1
        {0x00,0x1C,0x22,0x02,0x04,0x08,0x10,0x3E,0x00,0x00}, // 2
        {0x00,0x3E,0x04,0x08,0x04,0x02,0x22,0x1C,0x00,0x00}, // 3
        {0x00,0x04,0x0C,0x14,0x24,0x3E,0x04,0x04,0x00,0x00}, // 4
        {0x00,0x3E,0x20,0x3C,0x02,0x02,0x22,0x1C,0x00,0x00}, // 5
        {0x00,0x0C,0x10,0x20,0x3C,0x22,0x22,0x1C,0x00,0x00}, // 6
        {0x00,0x3E,0x02,0x04,0x08,0x10,0x10,0x10,0x00,0x00}, // 7
        {0x00,0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C,0x00,0x00}, // 8
        {0x00,0x1C,0x22,0x22,0x1E,0x02,0x04,0x18,0x00,0x00}, // 9
        {0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00}, // :
        {0x00,0x00,0x18,0x18,0x00,0x18,0x08,0x10,0x00,0x00}, // ;
        {0x00,0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00,0x00}, // <
        {0x00,0x00,0x00,0x3E,0x00,0x3E,0x00,0x00,0x00,0x00}, // =
        {0x00,0x10,0x08,0x04,0x02,0x04,0x08,0x10,0x00,0x00}, // >
        {0x00,0x1C,0x22,0x02,0x04,0x08,0x00,0x08,0x00,0x00}, // ?
        {0x00,0x1C,0x22,0x02,0x1A,0x2A,0x2A,0x1C,0x00,0x00}, // @
        {0x00,0x1C,0x22,0x22,0x3E,0x22,0x22,0x22,0x00,0x00}, // A
        {0x00,0x3C,0x22,0x22,0x3C,0x22,0x22,0x3C,0x00,0x00}, // B
        {0x00,0x1C,0x22,0x20,0x20,0x20,0x22,0x1C,0x00,0x00}, // C
        {0x00,0x38,0x24,0x22,0x22,0x22,0x24,0x38,0x00,0x00}, // D
        {0x00,0x3E,0x20,0x20,0x3C,0x20,0x20,0x3E,0x00,0x00}, // E
        {0x00,0x3E,0x20,0x20,0x3C,0x20,0x20,0x20,0x00,0x00}, // F
        {0x00,0x1C,0x22,0x20,0x2E,0x22,0x22,0x1E,0x00,0x00}, // G
        {0x00,0x22,0x22,0x22,0x3E,0x22,0x22,0x22,0x00,0x00}, // H
        {0x00,0x1C,0x08,0x08,0x08,0x08,0x08,0x1C,0x00,0x00}, // I
        {0x00,0x0E,0x04,0x04,0x04,0x04,0x24,0x18,0x00,0x00}, // J
        {0x00,0x22,0x24,0x28,0x30,0x28,0x24,0x22,0x00,0x00}, // K
        {0x00,0x20,0x20,0x20,0x20,0x20,0x20,0x3E,0x00,0x00}, // L
        {0x00,0x22,0x36,0x2A,0x2A,0x22,0x22,0x22,0x00,0x00}, // M
        {0x00,0x22,0x22,0x32,0x2A,0x26,0x22,0x22,0x00,0x00}, // N
        {0x00,0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0x00,0x00}, // O
        {0x00,0x3C,0x22,0x22,0x3C,0x20,0x20,0x20,0x00,0x00}, // P
        {0x00,0x1C,0x22,0x22,0x22,0x2A,0x24,0x1A,0x00,0x00}, // Q
        {0x00,0x3C,0x22,0x22,0x3C,0x28,0x24,0x22,0x00,0x00}, // R
        {0x00,0x1E,0x20,0x20,0x1C,0x02,0x02,0x3C,0x00,0x00}, // S
        {0x00,0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0x00,0x00}, // T
        {0x00,0x22,0x22,0x22,0x22,0x22,0x22,0x1C,0x00,0x00}, // U
        {0x00,0x22,0x22,0x22,0x22,0x22,0x14,0x08,0x00,0x00}, // V
        {0x00,0x22,0x22,0x22,0x2A,0x2A,0x2A,0x14,0x00,0x00}, // W
        {0x00,0x22,0x22,0x14,0x08,0x14,0x22,0x22,0x00,0x00}, // X
        {0x00,0x22,0x22,0x14,0x08,0x08,0x08,0x08,0x00,0x00}, // Y
        {0x00,0x3E,0x02,0x04,0x08,0x10,0x20,0x3E,0x00,0x00}, // Z
        {0x00,0x1C,0x10,0x10,0x10,0x10,0x10,0x1C,0x00,0x00}, // [
        {0x00,0x20,0x20,0x10,0x08,0x04,0x02,0x02,0x00,0x00}, // backslash
        {0x00,0x1C,0x04,0x04,0x04,0x04,0x04,0x1C,0x00,0x00}, // ]
        {0x00,0x08,0x14,0x22,0x00,0x00,0x00,0x00,0x00,0x00}, // ^
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3E,0x00}, // _
        {0x00,0x10,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // `
        {0x00,0x00,0x00,0x1C,0x02,0x1E,0x22,0x1E,0x00,0x00}, // a
        {0x00,0x20,0x20,0x3C,0x22,0x22,0x22,0x3C,0x00,0x00}, // b
        {0x00,0x00,0x00,0x1C,0x20,0x20,0x22,0x1C,0x00,0x00}, // c
        {0x00,0x02,0x02,0x1E,0x22,0x22,0x22,0x1E,0x00,0x00}, // d
        {0x00,0x00,0x00,0x1C,0x22,0x3E,0x20,0x1C,0x00,0x00}, // e
        {0x00,0x0C,0x12,0x10,0x38,0x10,0x10,0x10,0x00,0x00}, // f
        {0x00,0x00,0x00,0x1E,0x22,0x22,0x22,0x1E,0x02,0x1C}, // g
        {0x00,0x20,0x20,0x2C,0x32,0x22,0x22,0x22,0x00,0x00}, // h
        {0x00,0x08,0x00,0x18,0x08,0x08,0x08,0x1C,0x00,0x00}, // i
        {0x00,0x04,0x00,0x0C,0x04,0x04,0x04,0x04,0x24,0x18}, // j
        {0x00,0x20,0x20,0x24,0x28,0x30,0x28,0x24,0x00,0x00}, // k
        {0x00,0x18,0x08,0x08,0x08,0x08,0x08,0x1C,0x00,0x00}, // l
        {0x00,0x00,0x00,0x34,0x2A,0x2A,0x2A,0x2A,0x00,0x00}, // m
        {0x00,0x00,0x00,0x2C,0x32,0x22,0x22,0x22,0x00,0x00}, // n
        {0x00,0x00,0x00,0x1C,0x22,0x22,0x22,0x1C,0x00,0x00}, // o
        {0x00,0x00,0x00,0x3C,0x22,0x22,0x22,0x3C,0x20,0x20}, // p
        {0x00,0x00,0x00,0x1E,0x22,0x22,0x22,0x1E,0x02,0x02}, // q
        {0x00,0x00,0x00,0x2C,0x32,0x20,0x20,0x20,0x00,0x00}, // r
        {0x00,0x00,0x00,0x1E,0x20,0x1C,0x02,0x3C,0x00,0x00}, // s
        {0x00,0x10,0x10,0x38,0x10,0x10,0x12,0x0C,0x00,0x00}, // t
        {0x00,0x00,0x00,0x22,0x22,0x22,0x26,0x1A,0x00,0x00}, // u
        {0x00,0x00,0x00,0x22,0x22,0x22,0x14,0x08,0x00,0x00}, // v
        {0x00,0x00,0x00,0x22,0x22,0x2A,0x2A,0x14,0x00,0x00}, // w
        {0x00,0x00,0x00,0x22,0x14,0x08,0x14,0x22,0x00,0x00}, // x
        {0x00,0x00,0x00,0x22,0x22,0x22,0x22,0x1E,0x02,0x1C}, // y
        {0x00,0x00,0x00,0x3E,0x04,0x08,0x10,0x3E,0x00,0x00}, // z
        {0x00,0x06,0x08,0x08,0x10,0x08,0x08,0x06,0x00,0x00}, // {
        {0x00,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00}, // |
        {0x00,0x30,0x08,0x08,0x04,0x08,0x08,0x30,0x00,0x00}, // }
        {0x00,0x00,0x00,0x10,0x2A,0x04,0x00,0x00,0x00,0x00}, // ~
    };

    uint8_t toByte(float v){
        return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
    }

    void appendBytes(void* context, void* data, int size){
        std::vector<uint8_t>* out = static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    }

    // distance from p to the segment ab
    float segmentDistance(float px, float py, float ax, float ay, float bx, float by){
        const float dx = bx - ax;
        const float dy = by - ay;
        const float lengthSq = dx * dx + dy * dy;
        float t = 0.0f;
        if (lengthSq > 0.0f){
            t = std::clamp(((px - ax) * dx + (py - ay) * dy) / lengthSq, 0.0f, 1.0f);
        }
        const float cx = ax + t * dx - px;
        const float cy = ay + t * dy - py;
        return std::sqrt(cx * cx + cy * cy);
    }

}

editor::PoseColor editor::PoseColor::fromFloat(float r, float g, float b, float a){
    return PoseColor(toByte(r), toByte(g), toByte(b), toByte(a));
}

// ---- PoseImage ----

editor::PoseImage::PoseImage(int width, int height, PoseColor fill){
    create(width, height, fill);
}

void editor::PoseImage::create(int width, int height, PoseColor fill){
    if (width <= 0 || height <= 0){
        clear();
        return;
    }
    this->width = width;
    this->height = height;
    pixels.assign((size_t)width * height * 4, 0);
    this->fill(fill);
}

void editor::PoseImage::clear(){
    width = 0;
    height = 0;
    pixels.clear();
}

void editor::PoseImage::fill(PoseColor color){
    for (size_t i = 0; i + 3 < pixels.size(); i += 4){
        pixels[i + 0] = color.r;
        pixels[i + 1] = color.g;
        pixels[i + 2] = color.b;
        pixels[i + 3] = color.a;
    }
}

bool editor::PoseImage::assign(int width, int height, std::vector<uint8_t>&& rgba){
    if (width <= 0 || height <= 0 || rgba.size() != (size_t)width * height * 4){
        return false;
    }
    this->width = width;
    this->height = height;
    pixels = std::move(rgba);
    return true;
}

editor::PoseColor editor::PoseImage::getPixel(int x, int y) const{
    if (x < 0 || y < 0 || x >= width || y >= height){
        return PoseColor(0, 0, 0, 0);
    }
    const uint8_t* p = &pixels[((size_t)y * width + x) * 4];
    return PoseColor(p[0], p[1], p[2], p[3]);
}

void editor::PoseImage::setPixel(int x, int y, PoseColor color){
    if (x < 0 || y < 0 || x >= width || y >= height){
        return;
    }
    uint8_t* p = &pixels[((size_t)y * width + x) * 4];
    p[0] = color.r;
    p[1] = color.g;
    p[2] = color.b;
    p[3] = color.a;
}

void editor::PoseImage::blendPixel(int x, int y, PoseColor color, float coverage){
    if (x < 0 || y < 0 || x >= width || y >= height || coverage <= 0.0f || color.a == 0){
        return;
    }
    uint8_t* p = &pixels[((size_t)y * width + x) * 4];
    const float sa = (color.a / 255.0f) * std::min(coverage, 1.0f);
    if (sa >= 1.0f){
        p[0] = color.r;
        p[1] = color.g;
        p[2] = color.b;
        p[3] = 255;
        return;
    }
    // straight alpha source-over
    const float da = p[3] / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0f){
        p[0] = p[1] = p[2] = p[3] = 0;
        return;
    }
    const float dw = da * (1.0f - sa);
    p[0] = toByte((color.r / 255.0f * sa + p[0] / 255.0f * dw) / oa);
    p[1] = toByte((color.g / 255.0f * sa + p[1] / 255.0f * dw) / oa);
    p[2] = toByte((color.b / 255.0f * sa + p[2] / 255.0f * dw) / oa);
    p[3] = toByte(oa);
}

void editor::PoseImage::blit(const PoseImage& src, int x, int y){
    blit(src, 0, 0, src.width, src.height, x, y);
}

void editor::PoseImage::blit(const PoseImage& src, int sx, int sy, int sw, int sh, int x, int y){
    // clip the source rectangle to the source, then the destination to this image
    if (sx < 0){ sw += sx; x -= sx; sx = 0; }
    if (sy < 0){ sh += sy; y -= sy; sy = 0; }
    sw = std::min(sw, src.width - sx);
    sh = std::min(sh, src.height - sy);
    if (x < 0){ sw += x; sx -= x; x = 0; }
    if (y < 0){ sh += y; sy -= y; y = 0; }
    sw = std::min(sw, width - x);
    sh = std::min(sh, height - y);
    if (sw <= 0 || sh <= 0){
        return;
    }
    for (int row = 0; row < sh; row++){
        std::memcpy(&pixels[((size_t)(y + row) * width + x) * 4],
                    &src.pixels[((size_t)(sy + row) * src.width + sx) * 4], (size_t)sw * 4);
    }
}

void editor::PoseImage::blendImage(const PoseImage& src, int x, int y){
    for (int row = 0; row < src.height; row++){
        for (int col = 0; col < src.width; col++){
            blendPixel(x + col, y + row, src.getPixel(col, row));
        }
    }
}

editor::PoseImage editor::PoseImage::crop(int x, int y, int w, int h) const{
    PoseImage out(w, h);
    out.blit(*this, x, y, w, h, 0, 0);
    return out;
}

void editor::PoseImage::fillRect(int x, int y, int w, int h, PoseColor color){
    const int x0 = std::max(x, 0);
    const int y0 = std::max(y, 0);
    const int x1 = std::min(x + w, width);
    const int y1 = std::min(y + h, height);
    for (int py = y0; py < y1; py++){
        for (int px = x0; px < x1; px++){
            blendPixel(px, py, color);
        }
    }
}

void editor::PoseImage::strokeRect(int x, int y, int w, int h, PoseColor color){
    if (w <= 0 || h <= 0){
        return;
    }
    fillRect(x, y, w, 1, color);
    if (h > 1){
        fillRect(x, y + h - 1, w, 1, color);
    }
    if (h > 2){
        fillRect(x, y + 1, 1, h - 2, color);
        if (w > 1){
            fillRect(x + w - 1, y + 1, 1, h - 2, color);
        }
    }
}

void editor::PoseImage::drawLine(float x0, float y0, float x1, float y1, float lineWidth, PoseColor color){
    // NaN or infinite input would reach float-to-int casts (undefined): nothing is drawn
    if (isEmpty() || !std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1) ||
            !std::isfinite(lineWidth) || !(lineWidth > 0.0f)){
        return;
    }
    const float half = lineWidth * 0.5f;
    // coverage falls off over one pixel across the edge, so the box grows by one more
    const int minX = std::max(0, boxIndex(std::floor(std::min(x0, x1) - half - 1.0f), width));
    const int minY = std::max(0, boxIndex(std::floor(std::min(y0, y1) - half - 1.0f), height));
    const int maxX = std::min(width - 1, boxIndex(std::ceil(std::max(x0, x1) + half + 1.0f), width));
    const int maxY = std::min(height - 1, boxIndex(std::ceil(std::max(y0, y1) + half + 1.0f), height));
    for (int py = minY; py <= maxY; py++){
        for (int px = minX; px <= maxX; px++){
            const float d = segmentDistance(px + 0.5f, py + 0.5f, x0, y0, x1, y1);
            const float coverage = std::clamp(half + 0.5f - d, 0.0f, 1.0f);
            if (coverage > 0.0f){
                blendPixel(px, py, color, coverage);
            }
        }
    }
}

void editor::PoseImage::fillCircle(float cx, float cy, float diameter, PoseColor color){
    if (isEmpty() || !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(diameter) || !(diameter > 0.0f)){
        return;
    }
    const float radius = diameter * 0.5f;
    const int minX = std::max(0, boxIndex(std::floor(cx - radius - 1.0f), width));
    const int minY = std::max(0, boxIndex(std::floor(cy - radius - 1.0f), height));
    const int maxX = std::min(width - 1, boxIndex(std::ceil(cx + radius + 1.0f), width));
    const int maxY = std::min(height - 1, boxIndex(std::ceil(cy + radius + 1.0f), height));
    for (int py = minY; py <= maxY; py++){
        for (int px = minX; px <= maxX; px++){
            const float dx = px + 0.5f - cx;
            const float dy = py + 0.5f - cy;
            const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            if (coverage > 0.0f){
                blendPixel(px, py, color, coverage);
            }
        }
    }
}

const uint8_t* editor::PoseImage::glyphRows(char c){
    const unsigned char code = (unsigned char)c;
    if (code < 32 || code > 126){
        return FONT_6X10['?' - 32];
    }
    return FONT_6X10[code - 32];
}

int editor::PoseImage::drawText(int x, int y, const std::string& text, PoseColor color){
    for (char c : text){
        const uint8_t* rows = glyphRows(c);
        for (int row = 0; row < GLYPH_HEIGHT; row++){
            const uint8_t bits = rows[row];
            if (!bits){
                continue;
            }
            for (int col = 0; col < GLYPH_WIDTH; col++){
                if (bits & (0x20 >> col)){
                    blendPixel(x + col, y + row, color);
                }
            }
        }
        x += GLYPH_WIDTH;
    }
    return x;
}

int editor::PoseImage::textWidth(const std::string& text){
    return (int)text.size() * GLYPH_WIDTH;
}

std::string editor::PoseImage::fitText(const std::string& text, int maxWidth){
    if (textWidth(text) <= maxWidth){
        return text;
    }
    const int maxChars = maxWidth / GLYPH_WIDTH;
    if (maxChars <= 2){
        // not even one character and the dots fit: only the dots that do
        return std::string((size_t)std::max(maxChars, 0), '.');
    }
    return text.substr(0, (size_t)(maxChars - 2)) + "..";
}

bool editor::PoseImage::encodePng(std::vector<uint8_t>& out) const{
    out.clear();
    if (isEmpty()){
        return false;
    }
    if (!stbi_write_png_to_func(appendBytes, &out, width, height, 4, pixels.data(), width * 4)){
        out.clear();
        return false;
    }
    return !out.empty();
}

bool editor::PoseImage::writePng(const std::string& path) const{
    std::vector<uint8_t> png;
    if (!encodePng(png)){
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file){
        return false;
    }
    file.write(reinterpret_cast<const char*>(png.data()), (std::streamsize)png.size());
    return (bool)file;
}

bool editor::PoseImage::decodePng(const uint8_t* bytes, size_t size, PoseImage& out){
    out.clear();
    if (!bytes || size == 0 || size > (size_t)INT32_MAX){
        return false;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* data = stbi_load_from_memory(bytes, (int)size, &w, &h, &channels, 4);
    if (!data){
        return false;
    }
    std::vector<uint8_t> rgba(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return out.assign(w, h, std::move(rgba));
}

bool editor::PoseImage::readPng(const std::string& path, PoseImage& out){
    std::ifstream file(path, std::ios::binary);
    if (!file){
        out.clear();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return decodePng(bytes.data(), bytes.size(), out);
}

// ---- PoseSheetLayout ----

int editor::PoseSheetLayout::getWidth() const{
    return columns * cellWidth + std::max(columns - 1, 0) * SEPARATOR;
}

int editor::PoseSheetLayout::getHeight() const{
    const int title = titleHeight > 0 ? titleHeight + SEPARATOR : 0;
    return title + rows * (cellHeight + labelHeight) + std::max(rows - 1, 0) * SEPARATOR;
}

int editor::PoseSheetLayout::cellX(int column) const{
    return column * (cellWidth + SEPARATOR);
}

int editor::PoseSheetLayout::cellY(int row) const{
    const int title = titleHeight > 0 ? titleHeight + SEPARATOR : 0;
    return title + row * (cellHeight + labelHeight + SEPARATOR);
}

int editor::PoseSheetLayout::labelY(int row) const{
    return cellY(row) + cellHeight;
}

// ---- PoseContactSheet ----

editor::PoseContactSheet::PoseContactSheet(const PoseSheetLayout& layout, const std::string& title): layout(layout){
    this->layout.columns = std::max(this->layout.columns, 1);
    this->layout.rows = std::max(this->layout.rows, 1);
    this->layout.cellWidth = std::max(this->layout.cellWidth, 1);
    this->layout.cellHeight = std::max(this->layout.cellHeight, 1);
    this->layout.labelHeight = std::max(this->layout.labelHeight, 0);
    this->layout.titleHeight = std::max(this->layout.titleHeight, 0);

    // separators are what is left between the cells and strips
    image.create(this->layout.getWidth(), this->layout.getHeight(), SEPARATOR_COLOR);
    for (int row = 0; row < this->layout.rows; row++){
        for (int column = 0; column < this->layout.columns; column++){
            if (this->layout.labelHeight > 0){
                drawStrip(this->layout.cellX(column), this->layout.labelY(row),
                          this->layout.cellWidth, this->layout.labelHeight, "");
            }
        }
    }
    if (this->layout.titleHeight > 0){
        setTitle(title);
    }
}

void editor::PoseContactSheet::setCell(int column, int row, const PoseImage& cell){
    if (column < 0 || row < 0 || column >= layout.columns || row >= layout.rows){
        return;
    }
    const int w = std::min(cell.getWidth(), layout.cellWidth);
    const int h = std::min(cell.getHeight(), layout.cellHeight);
    image.blit(cell, 0, 0, w, h, layout.cellX(column), layout.cellY(row));
}

void editor::PoseContactSheet::setLabel(int column, int row, const std::string& text){
    if (column < 0 || row < 0 || column >= layout.columns || row >= layout.rows || layout.labelHeight <= 0){
        return;
    }
    drawStrip(layout.cellX(column), layout.labelY(row), layout.cellWidth, layout.labelHeight, text);
}

void editor::PoseContactSheet::setTitle(const std::string& text){
    if (layout.titleHeight <= 0){
        return;
    }
    drawStrip(0, 0, image.getWidth(), layout.titleHeight, text);
}

void editor::PoseContactSheet::drawStrip(int x, int y, int w, int h, const std::string& text){
    image.fillRect(x, y, w, h, LABEL_BACKGROUND);
    if (text.empty()){
        return;
    }
    const std::string fitted = PoseImage::fitText(text, w - 2 * LABEL_PADDING);
    const int textY = y + std::max((h - PoseImage::GLYPH_HEIGHT) / 2, 0);
    image.drawText(x + LABEL_PADDING, textY, fitted, LABEL_TEXT);
}

std::string editor::PoseContactSheet::poseLabel(int index, const std::string& name){
    return name.empty() ? std::to_string(index) : std::to_string(index) + " " + name;
}

std::string editor::PoseContactSheet::timeLabel(float seconds, bool leftContact, bool rightContact){
    // no "-0.00" for a time that rounds to zero
    if (std::fabs(seconds) < 0.005f){
        seconds = 0.0f;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "t=%.2f", seconds);
    std::string label = buffer;
    if (leftContact || rightContact){
        label += leftContact && rightContact ? " [LR]" : (leftContact ? " [L]" : " [R]");
    }
    return label;
}
