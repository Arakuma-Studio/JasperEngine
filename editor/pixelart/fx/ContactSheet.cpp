// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The backdrops' colours and layout follow pixelfx's src/core/scene.js (MIT, Copyright (c) 2026
// Patch); the sheet's grid follows tools/lib/fxlib.mjs contactSheet.

#include "ContactSheet.h"

#include "pixelart/BitmapFont.h"
#include "pixelart/BuiltinPixelFont.h"

#include "pixelart/PixelFxKey.h"
#include "pose/PoseImage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace jasperengine::editor::pixelart::fx {

    namespace {

        using jasperengine::pixelart::Rgba8;

        PixelRgba hex(uint32_t rgb) {
            return PixelRgba{(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255};
        }

        PixelRgba mixRgb(PixelRgba a, PixelRgba b, double t) {
            PixelRgba out{0, 0, 0, 255};
            for (int k = 0; k < 3; k++) out[k] = (uint8_t)std::clamp((int)(a[k] + (b[k] - a[k]) * t + 0.5), 0, 255);
            return out;
        }

        // S51's built-in pixel font (builtin:pixel): its 6 x 9 cells, ink rows 0..8, so a strip's
        // text stays hard pixels at any sheet scale
        // the pen's step after a character, the font's proportional advance (a fixed cell leaves gaps after l, i, t)
        int pixelTextAdvance(char c) {
            const std::shared_ptr<jasperengine::pixelart::BitmapFont> font = jasperengine::pixelart::BitmapFont::builtinPixel();
            return font->advanceOf((unsigned char)c) + font->getSpacing();
        }

        int pixelTextWidth(const std::string& text) {
            int width = 0;
            for (char c : text) width += pixelTextAdvance(c);
            return width;
        }

        std::string fitPixelText(const std::string& text, int maxWidth) {
            if (pixelTextWidth(text) <= maxWidth) return text;
            const std::string ellipsis = "..";
            std::string fitted;
            for (char c : text) {
                if (pixelTextWidth(fitted + c + ellipsis) > maxWidth) break;
                fitted += c;
            }
            return fitted + ellipsis;
        }

        void drawPixelText(PoseImage& image, int x, int y, const std::string& text, PoseColor color) {
            for (char c : text) {
                const std::vector<std::string> rows = jasperengine::pixelart::builtinPixelGlyphRows((unsigned char)c);
                for (size_t row = 0; row < rows.size(); row++) {
                    for (size_t col = 0; col < rows[row].size(); col++) {
                        if (rows[row][col] == '#') image.setPixel(x + (int)col, y + (int)row, color);
                    }
                }
                x += pixelTextAdvance(c);
            }
        }

        // a strip's text (the strip already drawn blank), as PoseContactSheet places its own
        void drawStripText(PoseImage& image, int x, int y, int w, int h, const std::string& text) {
            const std::string fitted = fitPixelText(text, w - 2 * PoseContactSheet::LABEL_PADDING);
            const int textY = y + std::max((h - jasperengine::pixelart::kBuiltinCellHeight) / 2, 0);
            drawPixelText(image, x + PoseContactSheet::LABEL_PADDING, textY, fitted, PoseContactSheet::LABEL_TEXT);
        }

        PoseSheetLayout bandLayout(const SheetLayout& l, bool labels) {
            PoseSheetLayout band;
            band.columns = l.columns;
            band.rows = l.rows;
            band.cellWidth = l.cellWidth;
            band.cellHeight = l.frameHeight * l.scale;
            band.labelHeight = labels ? SheetLayout::kLabelHeight : 0;
            band.titleHeight = SheetLayout::kHeaderHeight;
            return band;
        }

    }

    const char* backdropName(Backdrop backdrop) {
        switch (backdrop) {
            case Backdrop::Checker: return "checker";
            case Backdrop::Dark: return "dark";
            case Backdrop::Light: return "light";
            case Backdrop::Sky: return "sky";
            case Backdrop::Ground: return "ground";
            case Backdrop::Black: return "black";
            case Backdrop::Scene: return "scene";
        }
        return "checker";
    }

    bool parseBackdrop(const std::string& name, Backdrop& out) {
        for (Backdrop b : {Backdrop::Checker, Backdrop::Dark, Backdrop::Light, Backdrop::Sky, Backdrop::Ground, Backdrop::Black, Backdrop::Scene}) {
            if (name == backdropName(b)) {
                out = b;
                return true;
            }
        }
        return false;
    }

    std::string backdropNames() {
        return "checker, dark, light, sky, ground, black, scene";
    }

    PixelImage backdropImage(Backdrop backdrop, int width, int height, int groundY, Rgba8 sceneColor) {
        PixelImage image(width, height);
        static const PixelRgba sky = hex(0x15131d), skyTop = hex(0x0d0b14), skyLow = hex(0x221d33), star = hex(0xcfc8ff);
        static const PixelRgba floorEdge = hex(0x4a4263), floorTop = hex(0x2b2639), floorDark = hex(0x211d2d), mortar = hex(0x1a1724);
        static const int bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        if (groundY < 0) groundY = height - std::max(1, height / 4);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                PixelRgba c = sky;
                switch (backdrop) {
                    case Backdrop::Light: c = hex(0xebe8f0); break;
                    case Backdrop::Black: c = hex(0x000000); break;
                    case Backdrop::Scene: c = PixelRgba{sceneColor.r, sceneColor.g, sceneColor.b, 255}; break;
                    case Backdrop::Checker: c = ((x / 4 + y / 4) & 1) ? hex(0x5a5a62) : hex(0x3c3c44); break;
                    case Backdrop::Sky: {
                        const double t = (double)y / std::max(1, height - 1);
                        const int step = (int)(t * 5.0 + bayer[((y & 3) << 2) | (x & 3)] / 16.0);
                        c = mixRgb(skyTop, skyLow, std::min(1.0, step / 4.0));
                        const float r = jasperengine::pixelart::pfxHash01((uint32_t)x, (uint32_t)y, 77u);
                        if (r > 0.994f) c = mixRgb(c, star, 0.35 + (r - 0.994) * 90.0);
                        break;
                    }
                    case Backdrop::Ground:
                        if (y >= groundY) {
                            const int gy = y - groundY;
                            if (gy == 0) {
                                c = floorEdge;
                            } else {
                                const int row = (gy - 1) / 5, off = row % 2 ? 6 : 0;
                                const bool isMortar = (gy - 1) % 5 == 4 || (x + off) % 12 == 0;
                                c = isMortar ? mortar : (gy < 3 ? floorTop : floorDark);
                            }
                        }
                        break;
                    case Backdrop::Dark: break;
                }
                image.set(x, y, c);
            }
        }
        return image;
    }

    IntPoint SheetLayout::frameOrigin(int band, int index) const {
        const int column = index % columns, row = index / columns;
        const int title = kHeaderHeight + 1;
        return {column * (cellWidth + 1), band * (bandHeight + 1) + title + row * (cellHeight + 1)};
    }

    SheetLayout layoutSheet(int frameWidth, int frameHeight, int frameCount, const SheetSettings& settings) {
        SheetLayout l;
        l.frameWidth = std::max(1, frameWidth);
        l.frameHeight = std::max(1, frameHeight);
        const int n = std::max(1, frameCount);
        const int maxWidth = std::max(1, settings.maxWidth);
        // the widest the sheet may be at scale k with c columns
        auto fits = [&](int k, int c) { return c * l.frameWidth * k + (c - 1) <= maxWidth; };
        int columns = settings.columns > 0 ? std::min(settings.columns, n) : std::min(n, 8);
        if (settings.columns <= 0) {
            while (columns > 1 && !fits(settings.scale > 0 ? settings.scale : 1, columns)) columns--;
        }
        int scale = settings.scale;
        if (scale <= 0) {
            scale = 1;
            while (scale < 16 && fits(scale + 1, columns)) scale++;
        }
        l.scale = std::clamp(scale, 1, 16);
        l.columns = std::max(1, columns);
        l.rows = (n + l.columns - 1) / l.columns;
        l.bands = settings.opaqueFrames ? 1 : std::max(1, (int)settings.backdrops.size());
        l.cellWidth = l.frameWidth * l.scale;
        l.cellHeight = l.frameHeight * l.scale + (settings.labels ? SheetLayout::kLabelHeight : 0);
        const PoseSheetLayout band = bandLayout(l, settings.labels);
        l.bandHeight = band.getHeight();
        l.width = band.getWidth();
        l.height = l.bands * l.bandHeight + (l.bands - 1);
        return l;
    }

    std::vector<double> sheetTimes(int frames, double duration, bool loop) {
        std::vector<double> times;
        for (int i = 0; i < frames; i++) {
            const double t = frames == 1 ? 0.0 : loop ? duration * i / frames : duration * i / (frames - 1);
            times.push_back(std::round(t * 1e6) / 1e6);
        }
        return times;
    }

    std::string timeLabel(double seconds) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f", seconds);
        std::string s = text;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        if (s == "-0") s = "0";
        return "t=" + s + "s";
    }

    PixelImage composeSheet(const std::vector<const PixelImage*>& frames, const std::vector<double>& times, const SheetSettings& settings,
                            SheetLayout* layoutOut) {
        if (frames.empty() || !frames.front()) return PixelImage();
        const int fw = frames.front()->width(), fh = frames.front()->height();
        const SheetLayout l = layoutSheet(fw, fh, (int)frames.size(), settings);
        if (layoutOut) *layoutOut = l;
        PoseImage stacked(l.width, l.height, PoseContactSheet::SEPARATOR_COLOR);
        const std::vector<Backdrop> bands = settings.opaqueFrames ? std::vector<Backdrop>{Backdrop::Scene} : settings.backdrops;
        for (int b = 0; b < l.bands; b++) {
            const Backdrop backdrop = b < (int)bands.size() ? bands[(size_t)b] : Backdrop::Checker;
            const PoseSheetLayout band = bandLayout(l, settings.labels);
            const std::string title = settings.opaqueFrames ? std::string("scene view") : std::string(backdropName(backdrop));
            // the strips blank here, their text in S51's pixel font once the band is in place
            PoseContactSheet sheet(band, std::string());
            sheet.setTitle(std::string());
            const PixelImage under = backdropImage(backdrop, fw, fh, settings.groundY, settings.sceneColor);
            for (size_t i = 0; i < frames.size(); i++) {
                const PixelImage* frame = frames[i];
                PoseImage cell(fw * l.scale, fh * l.scale);
                for (int y = 0; y < fh * l.scale; y++) {
                    for (int x = 0; x < fw * l.scale; x++) {
                        const int fx = x / l.scale, fy = y / l.scale;
                        PixelRgba out = under.at(fx, fy);
                        if (frame && fx < frame->width() && fy < frame->height()) {
                            const PixelRgba f = frame->at(fx, fy);
                            if (settings.opaqueFrames) {
                                out = PixelRgba{f[0], f[1], f[2], 255};
                            } else {
                                // the frame's premultiplied colour over the backdrop
                                const int keep = 255 - f[3];
                                for (int k = 0; k < 3; k++) out[k] = (uint8_t)std::min(255, f[k] + (out[k] * keep + 127) / 255);
                            }
                        }
                        cell.setPixel(x, y, PoseColor(out[0], out[1], out[2], 255));
                    }
                }
                const int column = (int)i % l.columns, row = (int)i / l.columns;
                sheet.setCell(column, row, cell);
                if (settings.labels) sheet.setLabel(column, row, std::string());
            }
            const int bandY = b * (l.bandHeight + 1);
            stacked.blit(sheet.getImage(), 0, bandY);
            drawStripText(stacked, 0, bandY, l.width, band.titleHeight, title);
            if (settings.labels) {
                for (size_t i = 0; i < frames.size(); i++) {
                    const int column = (int)i % l.columns, row = (int)i / l.columns;
                    drawStripText(stacked, band.cellX(column), bandY + band.labelY(row), band.cellWidth, band.labelHeight,
                                  "#" + std::to_string(i) + " " + timeLabel(i < times.size() ? times[i] : 0.0));
                }
            }
        }
        PixelImage out(l.width, l.height);
        std::span<uint8_t> pixels = out.editPixels();
        std::copy(stacked.getPixels().begin(), stacked.getPixels().end(), pixels.begin());
        return out;
    }

}
