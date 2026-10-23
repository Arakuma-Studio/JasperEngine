// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CPU images for pose pictures (spec: pose animation phase 3, section 3): RGBA8 drawing
// (anti-aliased lines, filled circles and rects, a built-in 6x10 bitmap font), blits, the
// contact sheet layout and PNG encoding. Pure CPU with no engine state, so it is unit-tested
// on its own (engine/tests/pose/PoseImageTest.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Straight (not premultiplied) 8-bit RGBA
    struct PoseColor{
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 255;

        PoseColor() = default;
        constexpr PoseColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255): r(r), g(g), b(b), a(a) {}

        // 0xRRGGBB
        static constexpr PoseColor hex(uint32_t rgb, uint8_t alpha = 255){
            return PoseColor((uint8_t)((rgb >> 16) & 0xFF), (uint8_t)((rgb >> 8) & 0xFF), (uint8_t)(rgb & 0xFF), alpha);
        }
        // components 0..1, clamped
        static PoseColor fromFloat(float r, float g, float b, float a = 1.0f);

        bool operator==(const PoseColor& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
        bool operator!=(const PoseColor& o) const { return !(*this == o); }
    };

    // An RGBA8 image, rows top first, 4 bytes per pixel. Drawing clips to the image and
    // blends source-over with the colour's alpha (times the anti-aliasing coverage).
    class PoseImage{
    public:
        // the built-in font: printable ASCII 32..126 in 6x10 cells (5 px glyphs and a 1 px
        // gap, 7 px capitals from row 1, descenders in rows 8 and 9); other bytes draw as '?'
        static constexpr int GLYPH_WIDTH = 6;
        static constexpr int GLYPH_HEIGHT = 10;

        PoseImage() = default;
        PoseImage(int width, int height, PoseColor fill = PoseColor(0, 0, 0, 0));

        // a width x height image filled with fill (an empty image when either is <= 0)
        void create(int width, int height, PoseColor fill = PoseColor(0, 0, 0, 0));
        void clear();
        void fill(PoseColor color);

        int getWidth() const { return width; }
        int getHeight() const { return height; }
        bool isEmpty() const { return width <= 0 || height <= 0; }

        // width * height * 4 bytes, rows top first
        uint8_t* data() { return pixels.data(); }
        const uint8_t* data() const { return pixels.data(); }
        const std::vector<uint8_t>& getPixels() const { return pixels; }
        // takes ownership of width * height * 4 bytes (false, and nothing changes, when the size is wrong)
        bool assign(int width, int height, std::vector<uint8_t>&& rgba);

        // a pixel outside the image reads as transparent black and is not written
        PoseColor getPixel(int x, int y) const;
        void setPixel(int x, int y, PoseColor color);
        // source-over with color.a * coverage (coverage 0..1)
        void blendPixel(int x, int y, PoseColor color, float coverage = 1.0f);

        // copies src (no blending) with its top-left corner at (x, y), clipped
        void blit(const PoseImage& src, int x, int y);
        // copies the src rectangle (sx, sy, sw, sh) to (x, y), clipped
        void blit(const PoseImage& src, int sx, int sy, int sw, int sh, int x, int y);
        // src blended over this image at (x, y) with its own alpha
        void blendImage(const PoseImage& src, int x, int y);
        // a w x h copy of the rectangle at (x, y); the part outside the image is transparent
        PoseImage crop(int x, int y, int w, int h) const;

        // pixel-aligned rectangle, blended
        void fillRect(int x, int y, int w, int h, PoseColor color);
        // 1 px outline inside the rectangle
        void strokeRect(int x, int y, int w, int h, PoseColor color);
        // anti-aliased line with round caps; coordinates in pixels, pixel centres at +0.5
        void drawLine(float x0, float y0, float x1, float y1, float lineWidth, PoseColor color);
        // anti-aliased disc of that diameter centred on (cx, cy)
        void fillCircle(float cx, float cy, float diameter, PoseColor color);

        // text in the built-in font, its cell's top-left corner at (x, y), one line ('\n'
        // is drawn as '?'); returns the x after the last glyph
        int drawText(int x, int y, const std::string& text, PoseColor color);
        static int textWidth(const std::string& text);
        // text cut to fit maxWidth pixels, ending in ".." when it was cut
        static std::string fitText(const std::string& text, int maxWidth);
        // the font's 10 rows for a character, bit (0x20 >> column) set where it is drawn
        static const uint8_t* glyphRows(char c);

        // PNG of the RGBA pixels (alpha kept); false for an empty image
        bool encodePng(std::vector<uint8_t>& out) const;
        bool writePng(const std::string& path) const;
        // any PNG (or another format stb_image reads) decoded to RGBA
        static bool decodePng(const uint8_t* bytes, size_t size, PoseImage& out);
        static bool readPng(const std::string& path, PoseImage& out);

    private:
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };

    // Contact sheet geometry (spec: phase 3, section 3): an optional title strip on top, then
    // rows x columns equal cells, each with a label strip under it. 1 px separators lie between
    // neighbouring cells and under the title (none around the border), so
    //   width  = columns * cellWidth + (columns - 1)
    //   height = [titleHeight + 1] + rows * (cellHeight + labelHeight) + (rows - 1)
    struct PoseSheetLayout{
        static constexpr int SEPARATOR = 1;
        static constexpr int LABEL_HEIGHT = 14;

        int columns = 1;
        int rows = 1;
        int cellWidth = 160;
        int cellHeight = 200;
        int labelHeight = LABEL_HEIGHT;    // 0: no label strips
        int titleHeight = 0;               // 0: no title strip

        int getWidth() const;
        int getHeight() const;
        // top-left corner of a cell image and of the label strip under it
        int cellX(int column) const;
        int cellY(int row) const;
        int labelY(int row) const;
    };

    // Composes a contact sheet: cells are blitted into their slot (clipped to the cell size),
    // labels drawn white on #202022 and fitted with "..", the title the same way.
    class PoseContactSheet{
    public:
        static constexpr PoseColor SEPARATOR_COLOR = PoseColor(0, 0, 0, 255);
        static constexpr PoseColor LABEL_BACKGROUND = PoseColor(0x20, 0x20, 0x22, 255);
        static constexpr PoseColor LABEL_TEXT = PoseColor(255, 255, 255, 255);
        // the label text starts this far from the strip's left edge
        static constexpr int LABEL_PADDING = 3;

        explicit PoseContactSheet(const PoseSheetLayout& layout, const std::string& title = "");

        const PoseSheetLayout& getLayout() const { return layout; }

        void setCell(int column, int row, const PoseImage& cell);
        void setLabel(int column, int row, const std::string& text);
        void setTitle(const std::string& text);

        PoseImage& getImage() { return image; }
        const PoseImage& getImage() const { return image; }

        // label texts of the spec: "<index> <name>" and "t=<seconds>" with 2 decimals, plus
        // " C" and the feet in contact ("L", "R" or "LR") when any is
        static std::string poseLabel(int index, const std::string& name);
        static std::string timeLabel(float seconds, bool leftContact = false, bool rightContact = false);

    private:
        void drawStrip(int x, int y, int w, int h, const std::string& text);

        PoseSheetLayout layout;
        PoseImage image;
    };

}
