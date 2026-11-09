// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteRender.h"

#include "pixelart/aseprite/AsepriteBlend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        namespace ase = aseprite;
        namespace blend = aseprite::blend;

        constexpr int kGap = 2;
        constexpr int kLabelHeight = 7;
        const PixelRgba kSheetBackground{40, 40, 46, 255};
        const PixelRgba kMagenta{255, 0, 255, 255};

        // a 3x5 font (bit 2 = left column, rows top first), uppercase letters, digits and a dash
        const uint8_t* glyph(char c) {
            static const uint8_t kDigits[10][5] = {
                {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
                {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
            };
            static const uint8_t kLetters[26][5] = {
                {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4}, {3, 4, 5, 5, 3},
                {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5},
                {2, 5, 5, 5, 2}, {6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5}, {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2}, {5, 5, 5, 5, 7},
                {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5}, {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7},
            };
            static const uint8_t kDash[5] = {0, 0, 7, 0, 0};
            if (c >= '0' && c <= '9') return kDigits[c - '0'];
            if (c >= 'a' && c <= 'z') return kLetters[c - 'a'];
            if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
            if (c == '-' || c == '_') return kDash;
            return nullptr;
        }

        void fillSpan(std::span<uint8_t> px, int imageW, int imageH, int x, int y, int w, int h, const PixelRgba& c) {
            const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(imageW, x + w), y1 = std::min(imageH, y + h);
            for (int yy = y0; yy < y1; yy++) {
                uint8_t* row = px.data() + static_cast<size_t>(yy) * imageW * 4;
                for (int xx = x0; xx < x1; xx++) std::memcpy(row + xx * 4, c.data(), 4);
            }
        }

        void drawLabel(std::span<uint8_t> px, int imageW, int imageH, int x, int y, int maxW, const std::string& text, const PixelRgba& c) {
            int cursor = x;
            for (char ch : text) {
                if (cursor + 3 > x + maxW) break;
                if (const uint8_t* g = glyph(ch)) {
                    for (int row = 0; row < 5; row++) {
                        for (int col = 0; col < 3; col++) {
                            if (g[row] & (4 >> col)) fillSpan(px, imageW, imageH, cursor + col, y + row, 1, 1, c);
                        }
                    }
                }
                cursor += 4;
            }
        }

        std::string tagsOf(const ase::Document& doc, int frame) {
            for (const ase::Tag& tag : doc.tags) {
                if (frame >= tag.from && frame <= tag.to) return tag.name;
            }
            return "";
        }

        // src over dst (Aseprite's normal blend)
        void over(uint8_t* dst, const uint8_t* src, uint8_t opacity) {
            blend::unpack(blend::rgbaNormal(blend::pack(dst), blend::pack(src), opacity), dst);
        }

    }

    PixelImage renderSheet(const SpriteDoc& sprite, const SheetOptions& options, nlohmann::json& layout) {
        const ase::Document& doc = sprite.doc;
        const int n = static_cast<int>(options.frames.size());
        std::vector<SheetBackdrop> backdrops = options.backdrops;
        if (backdrops.empty()) {
            SheetBackdrop checker;
            checker.checker = true;
            checker.name = "checker";
            backdrops.push_back(checker);
        }
        const int columns = options.columns > 0 ? std::max(1, std::min(options.columns, std::max(1, n))) : std::max(1, std::min(8, n));
        const int perBackdropRows = n <= 0 ? 1 : (n + columns - 1) / columns;
        const int rows = perBackdropRows * static_cast<int>(backdrops.size());
        const int labelH = options.labels ? kLabelHeight : 0;
        int scale = options.scale;
        if (scale <= 0) {
            scale = std::max(1, (options.maxWidth - kGap * (columns + 1)) / std::max(1, columns * doc.width));
        }
        const int cellW = doc.width * scale;
        const int cellH = doc.height * scale;
        const int sheetW = kGap + columns * (cellW + kGap);
        const int sheetH = kGap + rows * (cellH + labelH + kGap);
        PixelImage out(sheetW, sheetH);
        std::span<uint8_t> px = out.editPixels();
        fillSpan(px, sheetW, sheetH, 0, 0, sheetW, sheetH, kSheetBackground);

        ase::RenderOptions render;
        render.includeHidden = options.allLayers || !options.layers.empty();
        render.layers = options.layers;
        std::vector<PixelImage> composed(static_cast<size_t>(doc.frameCount()));
        auto frameImage = [&](int frame) -> const PixelImage& {
            PixelImage& image = composed[static_cast<size_t>(frame)];
            if (image.empty()) image = ase::renderFrame(doc, frame, render);
            return image;
        };
        const uint8_t onionAlpha = static_cast<uint8_t>(std::floor(std::clamp(options.onionOpacity, 0.0, 1.0) * 255.0 + 0.5));

        nlohmann::json panels = nlohmann::json::array();
        for (int b = 0; b < static_cast<int>(backdrops.size()); b++) {
            const SheetBackdrop& backdrop = backdrops[static_cast<size_t>(b)];
            for (int i = 0; i < n; i++) {
                const int frame = options.frames[static_cast<size_t>(i)];
                const int ox = kGap + (i % columns) * (cellW + kGap);
                const int oy = kGap + (b * perBackdropRows + i / columns) * (cellH + labelH + kGap) + labelH;
                panels.push_back({{"frame", frame + 1}, {"backdrop", backdrop.name}, {"x", ox}, {"y", oy}, {"w", cellW}, {"h", cellH}});
                if (options.labels) {
                    std::string text = std::to_string(frame + 1) + " " + std::to_string(doc.frameDurations[static_cast<size_t>(frame)]) + "MS";
                    const std::string tag = tagsOf(doc, frame);
                    if (!tag.empty()) text += " " + tag;
                    drawLabel(px, sheetW, sheetH, ox, oy - labelH + 1, cellW, text, {235, 235, 235, 255});
                }
                // the backdrop, per sprite pixel so each block stays one colour
                for (int y = 0; y < doc.height; y++) {
                    for (int x = 0; x < doc.width; x++) {
                        PixelRgba c = backdrop.color;
                        if (backdrop.checker) {
                            const uint8_t v = ((x / 4 + y / 4) % 2) ? 200 : 160;
                            c = {v, v, v, 255};
                        }
                        fillSpan(px, sheetW, sheetH, ox + x * scale, oy + y * scale, scale, scale, c);
                    }
                }
                auto draw = [&](const PixelImage& image, uint8_t opacity) {
                    for (int y = 0; y < doc.height; y++) {
                        const uint8_t* row = image.row(y);
                        for (int x = 0; x < doc.width; x++) {
                            const uint8_t* s = row + x * 4;
                            if (s[3] == 0) continue;
                            // one blend per sprite pixel, copied: the block stays one colour
                            uint8_t* first = px.data() + (static_cast<size_t>(oy + y * scale) * sheetW + (ox + x * scale)) * 4;
                            over(first, s, opacity);
                            PixelRgba value{first[0], first[1], first[2], first[3]};
                            fillSpan(px, sheetW, sheetH, ox + x * scale, oy + y * scale, scale, scale, value);
                        }
                    }
                };
                // farthest onion first
                for (int k = options.onionBefore; k >= 1; k--) {
                    if (frame - k >= 0) draw(frameImage(frame - k), onionAlpha);
                }
                for (int k = options.onionAfter; k >= 1; k--) {
                    if (frame + k < doc.frameCount()) draw(frameImage(frame + k), onionAlpha);
                }
                draw(frameImage(frame), 255);
                if (options.grid && scale >= 6) {
                    const PixelRgba line{0, 0, 0, 90};
                    for (int y = 0; y < doc.height; y++) {
                        for (int x = 0; x < doc.width; x++) {
                            for (int t = 0; t < scale; t++) {
                                over(px.data() + (static_cast<size_t>(oy + y * scale) * sheetW + (ox + x * scale + t)) * 4, line.data(), 255);
                                if (t > 0) over(px.data() + (static_cast<size_t>(oy + y * scale + t) * sheetW + (ox + x * scale)) * 4, line.data(), 255);
                            }
                        }
                    }
                }
                // R28: a 1-pixel magenta frame around each rect of this frame (the ring just outside it)
                for (const DirtyRect& d : options.highlight) {
                    if (d.frame != frame + 1 || d.rect.empty()) continue;
                    const int rx = ox + d.rect.x * scale - 1, ry = oy + d.rect.y * scale - 1;
                    const int rw = d.rect.w * scale + 2, rh = d.rect.h * scale + 2;
                    fillSpan(px, sheetW, sheetH, rx, ry, rw, 1, kMagenta);
                    fillSpan(px, sheetW, sheetH, rx, ry + rh - 1, rw, 1, kMagenta);
                    fillSpan(px, sheetW, sheetH, rx, ry, 1, rh, kMagenta);
                    fillSpan(px, sheetW, sheetH, rx + rw - 1, ry, 1, rh, kMagenta);
                }
            }
        }
        layout = {{"scale", scale}, {"layout", std::move(panels)}};
        return out;
    }

    PixelImage renderDiffImage(const PixelImage& before, const PixelImage& after, const IntRect& bbox, int maxWidth) {
        const int w = std::max(before.width(), after.width()), h = std::max(before.height(), after.height());
        if (w == 0 || h == 0) return PixelImage();
        const int scale = std::max(1, (maxWidth - kGap * 3) / std::max(1, w * 2));
        const int cellW = w * scale, cellH = h * scale;
        const int sheetW = kGap * 3 + cellW * 2, sheetH = kGap * 2 + cellH;
        PixelImage out(sheetW, sheetH);
        std::span<uint8_t> px = out.editPixels();
        fillSpan(px, sheetW, sheetH, 0, 0, sheetW, sheetH, kSheetBackground);
        for (int side = 0; side < 2; side++) {
            const PixelImage& image = side == 0 ? before : after;
            const int ox = kGap + side * (cellW + kGap);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const uint8_t v = ((x / 4 + y / 4) % 2) ? 200 : 160;
                    PixelRgba c{v, v, v, 255};
                    const PixelRgba s = image.at(x, y);
                    if (s[3] > 0) over(c.data(), s.data(), 255);
                    fillSpan(px, sheetW, sheetH, ox + x * scale, kGap + y * scale, scale, scale, c);
                }
            }
            if (!bbox.empty()) {
                const int rx = ox + bbox.x * scale - 1, ry = kGap + bbox.y * scale - 1, rw = bbox.w * scale + 2, rh = bbox.h * scale + 2;
                fillSpan(px, sheetW, sheetH, rx, ry, rw, 1, kMagenta);
                fillSpan(px, sheetW, sheetH, rx, ry + rh - 1, rw, 1, kMagenta);
                fillSpan(px, sheetW, sheetH, rx, ry, 1, rh, kMagenta);
                fillSpan(px, sheetW, sheetH, rx + rw - 1, ry, 1, rh, kMagenta);
            }
        }
        return out;
    }

}
