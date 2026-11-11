// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/demo.ts and packages/tools/test/synthetic.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupSynthetic.h"

#include "CleanupCommon.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace jasperengine::editor::pixelart::cleanup {

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace demo_ts {

        // Uint8ClampedArray's store of an integer value
        uint8_t clampByte(double value) { return uint8_t(value < 0 ? 0 : value > 255 ? 255 : value); }

        Rgba8 hexColor(const char* hex) {
            Rgba8 color{0, 0, 0, 255};
            jasperengine::pixelart::parseHex(hex, color);
            color.a = 255;
            return color;
        }

        void paintPixel(std::span<uint8_t> data, int width, int x, int y, int r, int g, int b) {
            uint8_t* p = data.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
            p[0] = clampByte(r);
            p[1] = clampByte(g);
            p[2] = clampByte(b);
            p[3] = 255;
        }

    }

    std::vector<double> boundaries(int count, const Upscale& upscale) {
        using namespace demo_ts;
        std::vector<double> cuts{upscale.offset};
        for (int k = 0; k < count; k++) cuts.push_back(cuts[size_t(k)] + upscale.scale * (1 + (upscale.drift * k) / std::max(1, count - 1)));
        return cuts;
    }

    RgbaImage upscale(const PixelArt& sprite, const Upscale& options) {
        using namespace demo_ts;
        const std::vector<double> xs = boundaries(sprite.width, options);
        const std::vector<double> ys = boundaries(sprite.height, options);
        // The picture ends where the last cell does; before `offset`, the first cell is cut off
        const int width = int(jsRound(xs.back()));
        const int height = int(jsRound(ys.back()));
        auto cellOf = [](const std::vector<double>& cuts, double position) {
            size_t i = 0;
            while (i + 2 < cuts.size() && position >= cuts[i + 1]) i++;
            return i;
        };
        std::vector<uint8_t> data(size_t(width) * size_t(height) * 4, 0);
        for (int y = 0; y < height; y++) {
            const size_t sy = cellOf(ys, y + 0.5);
            for (int x = 0; x < width; x++) {
                const size_t sx = cellOf(xs, x + 0.5);
                const int32_t index = sprite.indices[sy * size_t(sprite.width) + sx];
                if (index == TRANSPARENT) continue;
                const Rgba8 color = sprite.palette[size_t(index)];
                uint8_t* p = data.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
                p[0] = color.r;
                p[1] = color.g;
                p[2] = color.b;
                p[3] = 255;
            }
        }
        for (int pass = 0; pass < options.blur; pass++) {
            std::vector<uint8_t> blurred(data.size());
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    for (int c = 0; c < 4; c++) {
                        int sum = 0, count = 0;
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                                sum += data[(size_t(ny) * size_t(width) + size_t(nx)) * 4 + size_t(c)];
                                count++;
                            }
                        }
                        blurred[(size_t(y) * size_t(width) + size_t(x)) * 4 + size_t(c)] = uint8_t(jsRound(double(sum) / count));
                    }
                }
            }
            data = std::move(blurred);
        }
        if (options.noise > 0) {
            Random next(options.seed);
            for (size_t i = 0; i < data.size(); i += 4) {
                for (size_t c = 0; c < 3; c++) data[i + c] = clampByte(data[i + c] + jsRound((next() * 2 - 1) * options.noise));
            }
        }
        RgbaImage image(width, height);
        if (!image.empty()) std::copy(data.begin(), data.end(), image.editPixels().begin());
        return image;
    }

    PixelArt demoSprite() {
        using namespace demo_ts;
        static const char* const mushroom[16] = {
            "................", ".....KKKKKK.....", "...KKRRRRWWKK...", "..KRRRRRWWWWRK..", ".KRWWRRRRWWRRRK.", ".KWWWWRRRRRRRRK.",
            "KRWWWWRRRRRWWRRK", "KRRWWRRRRRWWWWRK", "KDRRRRRRRRWWWWDK", "KDDRRRRRRRRWWDDK", ".KKDDDDDDDDDDKK.", "...KKSSSSSSKK...",
            "....KSKSSKSK....", "....KSSSSSSsK...", "....KsSSSSssK...", ".....KKKKKKK....",
        };
        // demo.ts's mushroomColors, in its key order
        const std::string keys = ".KRDWSs";
        static const char* const colors[7] = {"#333c57", "#1a1c2c", "#b13e53", "#5d275d", "#f4f4f4", "#ffcd75", "#ef7d57"};
        PixelArt art;
        art.width = 16;
        art.height = 16;
        for (const char* hex : colors) art.palette.push_back(hexColor(hex));
        for (const char* row : mushroom) {
            for (int x = 0; x < 16; x++) art.indices.push_back(int32_t(keys.find(row[x])));
        }
        return art;
    }

    RgbaImage demoImage() {
        using namespace demo_ts;
        PixelArt sprite = demoSprite();
        Random next(42);
        auto off = [&](int value) { return uint8_t(std::min(255.0, std::max(0.0, value + jsRound((next() * 2 - 1) * 8)))); };
        for (Rgba8& color : sprite.palette) {
            const uint8_t r = off(color.r);
            const uint8_t g = off(color.g);
            const uint8_t b = off(color.b);
            color = Rgba8{r, g, b, 255};
        }
        Upscale options;
        options.scale = 23.3;
        options.offset = 6;
        options.drift = 0.04;
        options.blur = 2;
        options.noise = 7;
        options.seed = 3;
        return upscale(sprite, options);
    }

    std::vector<Rgba8> sweetie16() {
        using namespace demo_ts;
        static const char* const hexes[16] = {"1a1c2c", "5d275d", "b13e53", "ef7d57", "ffcd75", "a7f070", "38b764", "257179",
                                              "29366f", "3b5dc9", "41a6f6", "73eff7", "f4f4f4", "94b0c2", "566c86", "333c57"};
        std::vector<Rgba8> out;
        for (const char* hex : hexes) out.push_back(hexColor(hex));
        return out;
    }

    PixelArt makeSprite(int width, int height, const std::vector<Rgba8>& palette, uint32_t seed, bool transparentBorder) {
        using namespace demo_ts;
        Random next(seed);
        PixelArt art;
        art.width = width;
        art.height = height;
        art.palette = palette;
        art.indices.assign(size_t(width) * size_t(height), 0);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                // Mostly copies a neighbour, so the sprite has areas, like real art, and some edges vanish
                const double roll = next();
                const bool hasLeft = x > 0;
                const bool hasUp = y > 0;
                const int32_t left = hasLeft ? art.indices[size_t(y) * size_t(width) + size_t(x) - 1] : 0;
                const int32_t up = hasUp ? art.indices[size_t(y - 1) * size_t(width) + size_t(x)] : 0;
                int32_t value = int32_t(std::floor(next() * double(palette.size())));
                if (roll < 0.35 && hasLeft && left != TRANSPARENT) value = left;
                else if (roll < 0.55 && hasUp && up != TRANSPARENT) value = up;
                art.indices[size_t(y) * size_t(width) + size_t(x)] = value;
            }
        }
        if (transparentBorder) {
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    if (x == 0 || y == 0 || x == width - 1 || y == height - 1) art.indices[size_t(y) * size_t(width) + size_t(x)] = TRANSPARENT;
                }
            }
        }
        return art;
    }

    PixelArt pad(const PixelArt& sprite, int margin) {
        using namespace demo_ts;
        PixelArt out;
        out.width = sprite.width + 2 * margin;
        out.height = sprite.height + 2 * margin;
        out.palette = sprite.palette;
        out.indices.assign(size_t(out.width) * size_t(out.height), TRANSPARENT);
        for (int y = 0; y < sprite.height; y++) {
            for (int x = 0; x < sprite.width; x++) {
                out.indices[size_t(y + margin) * size_t(out.width) + size_t(x + margin)] = sprite.indices[size_t(y) * size_t(sprite.width) + size_t(x)];
            }
        }
        return out;
    }

    PixelArt place(const PixelArt& sprite, int width, int height, int x, int y) {
        using namespace demo_ts;
        PixelArt out;
        out.width = width;
        out.height = height;
        out.palette = sprite.palette;
        out.indices.assign(size_t(width) * size_t(height), TRANSPARENT);
        for (int sy = 0; sy < sprite.height; sy++) {
            for (int sx = 0; sx < sprite.width; sx++) {
                const int32_t index = sprite.indices[size_t(sy) * size_t(sprite.width) + size_t(sx)];
                if (index != TRANSPARENT && sx + x >= 0 && sy + y >= 0 && sx + x < width && sy + y < height) {
                    out.indices[size_t(sy + y) * size_t(width) + size_t(sx + x)] = index;
                }
            }
        }
        return out;
    }

    FakeSheet fakeSheet(const std::vector<PixelArt>& frames, double scale, uint32_t seed, bool jitter) {
        using namespace demo_ts;
        Random next(seed);
        const PixelArt& first = frames.front();
        const int frameWidth = int(std::ceil(first.width * scale)) + 8;
        const int frameHeight = int(std::ceil(first.height * scale)) + 8;
        const int gap = int(jsRound(frameWidth * 0.3));
        const int top = int(jsRound(frameHeight * 0.4));
        const int width = gap + int(frames.size()) * (frameWidth + gap);
        const int height = top + frameHeight + int(jsRound(frameHeight * 0.6));
        FakeSheet sheet;
        sheet.image = RgbaImage(width, height);
        std::span<uint8_t> data = sheet.image.editPixels();
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                // A little noise, as a JPEG's flat areas have
                const int n = int(jsRound((next() - 0.5) * 6));
                if (x % 40 < 2 || y % 40 < 2) paintPixel(data, width, x, y, 4 + std::max(0, n), 6 + std::max(0, n), 16 + n);
                else paintPixel(data, width, x, y, 29 + n, 36 + n, 52 + n);
            }
        }
        for (size_t f = 0; f < frames.size(); f++) {
            const int left = gap + int(f) * (frameWidth + gap);
            // The box around the frame, in the grid's dark colour
            for (int x = left; x < left + frameWidth; x++) {
                paintPixel(data, width, x, top, 4, 6, 16);
                paintPixel(data, width, x, top + frameHeight - 1, 4, 6, 16);
            }
            for (int y = top; y < top + frameHeight; y++) {
                paintPixel(data, width, left, y, 4, 6, 16);
                paintPixel(data, width, left + frameWidth - 1, y, 4, 6, 16);
            }
            Upscale options;
            options.scale = scale;
            options.offset = jitter ? jsRound(next() * scale * 0.8) : 0;
            options.blur = 1;
            options.noise = 4;
            options.seed = seed + uint32_t(f);
            const RgbaImage picture = upscale(frames[f], options);
            const int x0 = left + 4;
            const int y0 = top + 4;
            for (int y = 0; y < picture.height() && y0 + y < height; y++) {
                for (int x = 0; x < picture.width() && x0 + x < width; x++) {
                    const uint8_t* s = picture.row(y) + size_t(x) * 4;
                    const double alpha = s[3] / 255.0;
                    if (alpha == 0) continue;
                    uint8_t* p = data.data() + (size_t(y0 + y) * size_t(width) + size_t(x0 + x)) * 4;
                    for (int c = 0; c < 3; c++) p[c] = clampByte(jsRound(s[c] * alpha + p[c] * (1 - alpha)));
                }
            }
            sheet.frames.push_back({x0, y0, picture.width(), picture.height()});
            // A label under the box: a light bar, like text
            for (int y = top + frameHeight + 10; y < top + frameHeight + 16; y++) {
                for (int x = left + int(jsRound(frameWidth * 0.3)); x < left + int(jsRound(frameWidth * 0.7)); x++) {
                    if ((x >> 2) % 2 == 0) paintPixel(data, width, x, y, 220, 220, 225);
                }
            }
        }
        return sheet;
    }

    RgbaImage warped(const PixelArt& sprite, double scale, double offset, double bend, double period) {
        using namespace demo_ts;
        const int width = int(jsRound(2 * offset + sprite.width * scale));
        const int height = int(jsRound(2 * offset + sprite.height * scale));
        auto column = [&](int k, double y) { return offset + k * scale + bend * scale * std::sin((2 * std::numbers::pi * y) / (period * scale)); };
        auto row = [&](int k, double x) { return offset + k * scale + bend * scale * std::sin((2 * std::numbers::pi * x) / (period * scale) + 1); };
        RgbaImage image(width, height);
        std::span<uint8_t> data = image.editPixels();
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int u = -1;
                while (u < sprite.width && column(u + 1, y + 0.5) <= x + 0.5) u++;
                int v = -1;
                while (v < sprite.height && row(v + 1, x + 0.5) <= y + 0.5) v++;
                const int32_t index = u >= 0 && v >= 0 && u < sprite.width && v < sprite.height ? sprite.indices[size_t(v) * size_t(sprite.width) + size_t(u)] : TRANSPARENT;
                if (index == TRANSPARENT) continue;
                const Rgba8 color = sprite.palette[size_t(index)];
                uint8_t* p = data.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
                p[0] = color.r;
                p[1] = color.g;
                p[2] = color.b;
                p[3] = 255;
            }
        }
        return image;
    }

}
