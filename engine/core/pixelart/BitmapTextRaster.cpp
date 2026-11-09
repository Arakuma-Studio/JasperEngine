// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/BitmapTextRaster.h"

#include "pixelart/BitmapText.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jasperengine::pixelart {

    RasterImage::RasterImage(int width, int height): width(std::max(width, 0)), height(std::max(height, 0)){
        rgba.assign((size_t)this->width * (size_t)this->height * 4, 0);
    }

    void RasterImage::fill(uint32_t color){
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            for (int c = 0; c < 4; c++) rgba[i + (size_t)c] = (uint8_t)(color >> (8 * c));
    }

    void RasterImage::checker(uint32_t a, uint32_t b, int cell){
        const int size = std::max(cell, 1);
        for (int y = 0; y < height; y++){
            for (int x = 0; x < width; x++){
                const uint32_t color = (((x / size) + (y / size)) % 2) ? b : a;
                uint8_t* px = &rgba[((size_t)y * (size_t)width + (size_t)x) * 4];
                for (int c = 0; c < 4; c++) px[c] = (uint8_t)(color >> (8 * c));
            }
        }
    }

    void RasterImage::blend(int x, int y, const uint8_t* src){
        if (x < 0 || y < 0 || x >= width || y >= height || src[3] == 0)
            return;
        uint8_t* dst = &rgba[((size_t)y * (size_t)width + (size_t)x) * 4];
        const float sa = src[3] / 255.0f, da = dst[3] / 255.0f;
        const float oa = sa + da * (1.0f - sa);
        for (int c = 0; c < 3; c++)
            dst[c] = oa > 0.0f ? (uint8_t)std::lround((src[c] * sa + dst[c] * da * (1.0f - sa)) / oa) : 0;
        dst[3] = (uint8_t)std::lround(oa * 255.0f);
    }

    void rasterBitmapText(const BitmapFont& font, const std::string& text, int scale, uint32_t color, RasterImage& image, int x, int y){
        const int k = std::max(scale, 1);
        bool invalid = false;
        const std::vector<uint32_t> cp = StringUtils::decodeUtf8ToCodepoints(text, invalid);
        const std::vector<BitmapLine> lines = BitmapText::layoutLines(font, cp, false, 0);
        uint8_t tint[4];
        for (int c = 0; c < 4; c++) tint[c] = (uint8_t)(color >> (8 * c));
        std::vector<uint8_t> glyphRgba;
        for (size_t l = 0; l < lines.size(); l++){
            const BitmapLine& line = lines[l];
            const int lineTop = (int)l * font.getLineHeight();
            int pen = 0;
            for (size_t i = line.first; i < line.first + line.count; i++){
                if (i > line.first) pen += font.getSpacing() + font.kerning(cp[i - 1], cp[i]);
                const BitmapGlyph* g = font.glyph(cp[i]);
                font.copyGlyphRgba(*g, glyphRgba);
                for (int gy = 0; gy < g->h; gy++){
                    for (int gx = 0; gx < g->w; gx++){
                        const uint8_t* src = &glyphRgba[((size_t)gy * g->w + (size_t)gx) * 4];
                        if (src[3] == 0)
                            continue;
                        uint8_t px[4];
                        for (int c = 0; c < 4; c++) px[c] = (uint8_t)((src[c] * tint[c] + 127) / 255);
                        const int ox = x + (pen + g->offsetX + gx) * k;
                        const int oy = y + (lineTop + g->offsetY + gy) * k;
                        for (int sy = 0; sy < k; sy++)
                            for (int sx = 0; sx < k; sx++)
                                image.blend(ox + sx, oy + sy, px);
                    }
                }
                pen += g->advance;
            }
        }
    }

    void rasterStyledBitmapText(const BitmapFont& font, const std::string& text, int scale, uint32_t color, const BitmapTextStyle& style,
                                RasterImage& image, int x, int y){
        if (!style.hasBack()){
            rasterBitmapText(font, text, scale, color, image, x, y);
            return;
        }
        // the text at 1x with a margin for the outline and the shadow
        int w = 0, h = 0;
        BitmapText::measure(font, text, w, h);
        const int margin = style.outlineWidth + std::max(std::abs(style.shadowX), std::abs(style.shadowY)) + 1;
        RasterImage mask(w + 2 * margin, h + 2 * margin + font.getLineHeight());
        rasterBitmapText(font, text, 1, rgbaColor(255, 255, 255), mask, margin, margin);
        // the silhouette grown through 4 or 8 neighbours
        std::vector<uint8_t> grown((size_t)mask.width * (size_t)mask.height, 0);
        for (size_t i = 0; i < grown.size(); i++) grown[i] = mask.rgba[i * 4 + 3] ? 1 : 0;
        for (int step = 0; step < style.outlineWidth; step++){
            std::vector<uint8_t> next = grown;
            for (int py = 0; py < mask.height; py++){
                for (int px = 0; px < mask.width; px++){
                    if (grown[(size_t)py * (size_t)mask.width + (size_t)px]) continue;
                    for (int dy = -1; dy <= 1; dy++){
                        for (int dx = -1; dx <= 1; dx++){
                            if ((dx == 0 && dy == 0) || (style.outlineNeighbors == 4 && dx != 0 && dy != 0)) continue;
                            const int nx = px + dx, ny = py + dy;
                            if (nx >= 0 && ny >= 0 && nx < mask.width && ny < mask.height && grown[(size_t)ny * (size_t)mask.width + (size_t)nx])
                                next[(size_t)py * (size_t)mask.width + (size_t)px] = 1;
                        }
                    }
                }
            }
            grown.swap(next);
        }
        uint8_t outline[4], shadow[4];
        for (int c = 0; c < 4; c++){
            outline[c] = (uint8_t)(style.outlineColor >> (8 * c));
            shadow[c] = (uint8_t)(style.shadowColor >> (8 * c));
        }
        const int k = std::max(scale, 1);
        auto plot = [&](int fx, int fy, const uint8_t* px){
            const int ox = x + (fx - margin) * k, oy = y + (fy - margin) * k;
            for (int sy = 0; sy < k; sy++)
                for (int sx = 0; sx < k; sx++)
                    image.blend(ox + sx, oy + sy, px);
        };
        for (int py = 0; py < mask.height; py++)
            for (int px = 0; px < mask.width; px++)
                if (style.shadow && grown[(size_t)py * (size_t)mask.width + (size_t)px])
                    plot(px + style.shadowX, py + style.shadowY, shadow);
        if (style.outlineWidth > 0){
            for (int py = 0; py < mask.height; py++)
                for (int px = 0; px < mask.width; px++)
                    if (grown[(size_t)py * (size_t)mask.width + (size_t)px])
                        plot(px, py, outline);
        }
        rasterBitmapText(font, text, k, color, image, x, y);
    }

}
