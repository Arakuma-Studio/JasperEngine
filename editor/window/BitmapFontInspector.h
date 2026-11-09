// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Bitmap Font Inspector (docs/specs/S51-pixel-fonts.md R23, R24): a .bitmapfont's source, its
// glyph grid and character map, metrics, kerning, a preview and the import report; a .fnt or a
// TTF/OTF selected shows what Create › Bitmap Font would make of it. Every edit writes the file
// through one WriteFilesCmd (a drag merges into one undo step).
#pragma once

#include "json.hpp"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontParsers.h"
#include "texture/Texture.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class BitmapFontInspector {
    public:
        explicit BitmapFontInspector(Project* project);
        ~BitmapFontInspector();

        // a .bitmapfont, .fnt, .ttf or .otf; `header` false when the caller draws the file's own
        // header (a TrueType font's names and licence come from the Title Art font Inspector)
        void draw(const std::filesystem::path& asset, bool header = true);
        // the texture Inspector's banner for a sheet named <name>@WxH.png; true when it drew
        static bool drawFontSheetBanner(Project* project, const std::filesystem::path& image);

    private:
        void reload(const std::filesystem::path& asset);
        void rebuildFont();
        void write(const std::string& mergeKey = std::string());
        void drawBitmapFont();
        void drawSource();
        void drawGrid();
        void drawGlyphPanel();
        void drawMetrics();
        void drawKerning();
        void drawPreview();
        void drawReport();
        void drawFntOrTrueType();
        // the characters of the grid's cells (U+FFFF: none)
        std::vector<uint32_t> cellChars() const;
        void assignCell(int cell, uint32_t codepoint);
        std::string mergeKey(const char* property) const;
        int cellInkLeft(int cell) const;
        int cellInkRight(int cell) const;

        Project* project;
        std::filesystem::path asset;
        std::filesystem::file_time_type stamp{};
        std::string text;               // the file as last read or written
        nlohmann::json doc;
        bool docValid = false;
        ::jasperengine::pixelart::BitmapFontSettings settings;
        ::jasperengine::pixelart::BitmapFontReport report;
        std::shared_ptr<::jasperengine::pixelart::BitmapFont> font;
        std::vector<::jasperengine::pixelart::FontError> settingsErrors;

        // the grid sheet
        ::jasperengine::pixelart::DecodedPage sheet;
        int cellWidth = 0, cellHeight = 0, cols = 0, rows = 0;
        Texture sheetTexture;
        int zoom = 4;
        int selectedCell = -1;
        uint32_t selectedGlyph = 0;     // a BMFont/TrueType source: the selected codepoint
        bool draggingAdvance = false;
        bool draggingBaseline = false;
        unsigned int dragSerial = 0;

        // kerning
        std::string kernLeft, kernRight;
        int kernAmount = -1;
        int kernPreview = -1;
        Texture kernTexture;
        std::string kernKey;
        int kernWidth = 0, kernHeight = 0;

        // preview
        std::string previewText = "The quick brown fox 0123456789";
        int previewScale = 3;
        int previewBackdrop = 0;        // checker, dark, light
        int previewOutline = 0;
        int previewNeighbors = 8;
        bool previewShadow = false;
        int previewShadowX = 1, previewShadowY = 1;
        Texture previewTexture;
        std::string previewKey;
        int previewWidth = 0, previewHeight = 0;
        int textureSerial = 0;
    };

}
