// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The font tools (S51 R26-R28): preview_font (group pixel_art, §6.1: the core listing had no room
// for view_asset's font parameters) draws a string or the glyph grid of a font, get_asset_info
// reports its metrics and pixel report, write_file returns the import report of a .bitmapfont.
#pragma once

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp {

    // a path preview_font draws: .bitmapfont, .fnt, .jfont, .ttf, .otf or builtin:pixel
    bool isFontViewPath(const std::string& path);
    // view_asset's answer for a font
    inline constexpr const char* kFontViewHint = "preview_font (tool group pixel_art) draws its glyphs or a string.";
    // preview_font; `enginePath` is relative to the assets folder (or builtin:pixel)
    void replyFontView(const CallPtr& call, const std::string& enginePath, int maxSize);
    // get_asset_info of a font file: R6's report, the source settings, kerning, overrides, the
    // pixel report S42 reads; typed errors when it does not load
    Json fontAssetInfo(Project* project, const std::filesystem::path& file, std::string& text);
    // write_file of a .bitmapfont: the import of `content` (the file is written either way)
    Json fontWriteReport(Project* project, const std::filesystem::path& file, const std::string& content, std::string& text);
    // the font path the engine opens: relative to the assets folder
    std::string fontEnginePath(Project* project, const std::filesystem::path& file);

    class McpRegistry;
    // preview_font (group pixel_art)
    void registerPixelFontTools(McpRegistry& registry);

}
