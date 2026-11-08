// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteContactSheet.h"

#include "AsepriteBlend.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace jasperengine::editor::pixelart::aseprite {

    namespace {

        // digits 0-9, 3x5, rows top first, bit 2 = left column
        constexpr uint8_t kDigits[10][5] = {
            {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
            {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
        };

        void drawNumber(PixelImage& image, int number, int x, int y, int scale, PixelRgba color) {
            const std::string text = std::to_string(number);
            for (size_t i = 0; i < text.size(); i++) {
                const int digit = text[i] - '0';
                for (int row = 0; row < 5; row++) {
                    for (int col = 0; col < 3; col++) {
                        if (!(kDigits[digit][row] & (4 >> col))) continue;
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                image.set(x + (static_cast<int>(i) * 4 + col) * scale + sx, y + row * scale + sy, color);
                            }
                        }
                    }
                }
            }
        }

        void fillRect(PixelImage& image, int x, int y, int w, int h, PixelRgba color) {
            for (int yy = std::max(0, y); yy < std::min(image.height(), y + h); yy++) {
                for (int xx = std::max(0, x); xx < std::min(image.width(), x + w); xx++) image.set(xx, yy, color);
            }
        }

    }

    PixelImage contactSheet(const SpriteSheetData& sheet, const std::vector<PixelImage>& pages, int maxSize) {
        const int n = static_cast<int>(sheet.frames.size());
        if (n == 0) return PixelImage();
        int cellW = 1, cellH = 1;
        for (const SpriteFrame& f : sheet.frames) {
            cellW = std::max(cellW, static_cast<int>(f.sourceSize.x));
            cellH = std::max(cellH, static_cast<int>(f.sourceSize.y));
        }
        const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
        const int rows = (n + columns - 1) / columns;
        const int gap = 2, label = 7, band = 3;
        const int unitW = cellW + gap, unitH = cellH + label + band + gap;
        // the largest integer scale that keeps the sheet within maxSize
        int scale = std::max(1, std::min((maxSize - gap) / std::max(1, columns * unitW), (maxSize - gap) / std::max(1, rows * unitH)));
        scale = std::min(scale, 16);
        PixelImage out(gap * scale + columns * unitW * scale, gap * scale + rows * unitH * scale);
        out.fill({40, 40, 46, 255});
        for (int i = 0; i < n; i++) {
            const SpriteFrame& f = sheet.frames[static_cast<size_t>(i)];
            const int ox = (gap + (i % columns) * unitW) * scale;
            const int oy = (gap + (i / columns) * unitH) * scale;
            // checkerboard behind the frame
            for (int y = 0; y < static_cast<int>(f.sourceSize.y); y++) {
                for (int x = 0; x < static_cast<int>(f.sourceSize.x); x++) {
                    const uint8_t v = ((x / 4 + y / 4) % 2) ? 200 : 160;
                    fillRect(out, ox + x * scale, oy + y * scale, scale, scale, {v, v, v, 255});
                }
            }
            if (f.page < pages.size()) {
                const PixelImage& page = pages[f.page];
                for (int y = 0; y < static_cast<int>(f.rect.getHeight()); y++) {
                    for (int x = 0; x < static_cast<int>(f.rect.getWidth()); x++) {
                        const PixelRgba c = page.at(static_cast<int>(f.rect.getX()) + x, static_cast<int>(f.rect.getY()) + y);
                        if (c[3] == 0) continue;
                        const int dx = ox + (static_cast<int>(f.offset.x) + x) * scale;
                        const int dy = oy + (static_cast<int>(f.offset.y) + y) * scale;
                        std::span<uint8_t> px = out.editPixels();
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                const int tx = dx + sx, ty = dy + sy;
                                if (tx < 0 || ty < 0 || tx >= out.width() || ty >= out.height()) continue;
                                uint8_t* d = px.data() + (static_cast<size_t>(ty) * out.width() + tx) * 4;
                                blend::unpack(blend::rgbaNormal(blend::pack(d), blend::pack(c.data()), 255), d);
                            }
                        }
                    }
                }
            }
            // the tags' bands, then the frame index
            int bandY = oy + cellH * scale;
            for (const SpriteTag& tag : sheet.tags) {
                if (i < tag.from || i > tag.to) continue;
                const PixelRgba color{static_cast<uint8_t>(tag.color.x * 255.0f + 0.5f), static_cast<uint8_t>(tag.color.y * 255.0f + 0.5f),
                                      static_cast<uint8_t>(tag.color.z * 255.0f + 0.5f), 255};
                fillRect(out, ox, bandY, cellW * scale, std::max(1, scale), color);
                bandY += std::max(1, scale);
                if (bandY >= oy + (cellH + band) * scale) break;
            }
            drawNumber(out, i, ox, oy + (cellH + band + 1) * scale, std::max(1, scale / 2), {235, 235, 235, 255});
        }
        return out;
    }

}
