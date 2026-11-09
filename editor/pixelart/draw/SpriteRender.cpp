// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteRender.h"

#include "pixelart/aseprite/AsepriteBlend.h"

#include "miniz.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
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

    std::string encodeSheetPng(const PixelImage& image) {
        const int w = image.width(), h = image.height();
        if (w == 0 || h == 0) return std::string();
        const std::span<const uint8_t> pixels = image.pixels();
        // the palette, with the previous pixel as a fast path: sheets are long runs of one colour
        std::vector<uint32_t> colors;
        std::map<uint32_t, uint8_t> slots;
        std::vector<uint8_t> indices(static_cast<size_t>(w) * h);
        bool fits = true;
        uint32_t previous = 0;
        uint8_t previousSlot = 0;
        bool hasPrevious = false;
        for (size_t i = 0; i < indices.size() && fits; i++) {
            const uint8_t* p = pixels.data() + i * 4;
            const uint32_t key = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
            if (hasPrevious && key == previous) {
                indices[i] = previousSlot;
                continue;
            }
            auto it = slots.find(key);
            if (it == slots.end()) {
                if (colors.size() >= 256) {
                    fits = false;
                    break;
                }
                it = slots.emplace(key, static_cast<uint8_t>(colors.size())).first;
                colors.push_back(key);
            }
            previous = key;
            previousSlot = it->second;
            hasPrevious = true;
            indices[i] = previousSlot;
        }
        const size_t stride = fits ? static_cast<size_t>(w) : static_cast<size_t>(w) * 4;
        std::vector<uint8_t> raw((stride + 1) * h, 0);
        for (int y = 0; y < h; y++) {
            uint8_t* out = raw.data() + static_cast<size_t>(y) * (stride + 1) + 1;
            std::memcpy(out, fits ? indices.data() + static_cast<size_t>(y) * w : image.row(y), stride);
        }
        std::vector<uint8_t> z(mz_compressBound(static_cast<mz_ulong>(raw.size())));
        mz_ulong zLength = static_cast<mz_ulong>(z.size());
        if (mz_compress2(z.data(), &zLength, raw.data(), static_cast<mz_ulong>(raw.size()), 1) != MZ_OK) return std::string();
        std::string out("\x89PNG\r\n\x1a\n", 8);
        auto chunk = [&](const char* type, const uint8_t* data, size_t size) {
            const auto be32 = [&](uint32_t v) {
                for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>(static_cast<uint8_t>(v >> s)));
            };
            be32(static_cast<uint32_t>(size));
            const size_t at = out.size();
            out.append(type, 4);
            if (size > 0) out.append(reinterpret_cast<const char*>(data), size);
            be32(static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const uint8_t*>(out.data()) + at, size + 4)));
        };
        uint8_t ihdr[13] = {};
        for (int i = 0; i < 4; i++) {
            ihdr[i] = static_cast<uint8_t>(uint32_t(w) >> (24 - i * 8));
            ihdr[4 + i] = static_cast<uint8_t>(uint32_t(h) >> (24 - i * 8));
        }
        ihdr[8] = 8;
        ihdr[9] = fits ? 3 : 6;
        chunk("IHDR", ihdr, 13);
        if (fits) {
            std::vector<uint8_t> plte, trns;
            for (uint32_t c : colors) {
                plte.push_back(static_cast<uint8_t>(c >> 24));
                plte.push_back(static_cast<uint8_t>(c >> 16));
                plte.push_back(static_cast<uint8_t>(c >> 8));
                trns.push_back(static_cast<uint8_t>(c));
            }
            while (!trns.empty() && trns.back() == 255) trns.pop_back();
            chunk("PLTE", plte.data(), plte.size());
            if (!trns.empty()) chunk("tRNS", trns.data(), trns.size());
        }
        chunk("IDAT", z.data(), zLength);
        chunk("IEND", nullptr, 0);
        return out;
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
