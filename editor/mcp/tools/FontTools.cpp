// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "FontTools.h"

#include "Project.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontParsers.h"
#include "pixelart/BitmapText.h"
#include "pixelart/BitmapTextRaster.h"
#include "pixelart/FontPixelReport.h"
#include "util/Base64.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"
#include "util/StringUtils.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

// the engine's pixel-art runtime (jasperengine::editor::pixelart is the editor's)
namespace fontpx = ::jasperengine::pixelart;

namespace {

    constexpr int kFontTextLimit = 256;
    constexpr int kFontGridLimit = 512;
    constexpr int kFontPad = 4;
    const uint32_t kFontInk = fontpx::rgbaColor(255, 255, 255);
    const uint32_t kFontLabel = fontpx::rgbaColor(150, 150, 150);

    std::string fontLowerExtension(const std::string& path){
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of("/\\");
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return "";
        std::string ext = path.substr(dot);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        return ext;
    }

    Json fontErrorJson(const fontpx::FontError& e){
        Json j = {{"kind", e.kind}, {"message", e.message}, {"fix", e.fix.empty() ? std::string("see jasper://docs/pixel-fonts") : e.fix}};
        if (!e.field.empty()) j["field"] = e.field;
        return j;
    }

    Json fontErrorsJson(const std::vector<fontpx::FontError>& errors){
        Json list = Json::array();
        for (const fontpx::FontError& e : errors) list.push_back(fontErrorJson(e));
        return list;
    }

    std::string fontErrorsText(const std::vector<fontpx::FontError>& errors){
        std::string text;
        for (const fontpx::FontError& e : errors)
            text += (text.empty() ? "" : "\n") + e.kind + ": " + e.message + (e.fix.empty() ? "" : " Fix: " + e.fix);
        return text;
    }

    std::string fontUtf8Of(const std::vector<uint32_t>& codepoints){
        std::string out;
        for (uint32_t c : codepoints) out += fontpx::codepointToUtf8(c);
        return out;
    }

    Json fontCodepointList(const std::vector<uint32_t>& codepoints){
        Json list = Json::array();
        for (uint32_t c : codepoints) list.push_back(fontpx::codepointToUtf8(c));
        return list;
    }

    // R6's numbers
    Json fontReportJson(const fontpx::BitmapFontReport& r){
        Json j = {{"glyphs", r.glyphs}, {"cells_used", r.cellsUsed}, {"cells_empty", r.cellsEmpty}, {"native_size", r.nativeSize},
                  {"line_height", r.lineHeight}, {"baseline", r.baseline}, {"colored", r.colored},
                  {"partial_alpha_share", std::round(r.partialAlphaShare * 10000.0f) / 10000.0f}, {"missing_ascii", fontUtf8Of(r.missingAscii)},
                  {"empty_glyphs", fontCodepointList(r.emptyGlyphs)}, {"overrides_applied", r.overridesApplied}};
        if (r.cellWidth > 0) j["cell"] = Json::array({r.cellWidth, r.cellHeight});
        if (!r.detections.empty()) j["detected"] = r.detections;
        if (!r.warnings.empty()) j["warnings"] = fontErrorsJson(r.warnings);
        return j;
    }

    bool fontEncodePng(const fontpx::RasterImage& image, std::string& base64){
        std::vector<unsigned char> png;
        auto write = [](void* context, void* data, int size){
            auto* out = static_cast<std::vector<unsigned char>*>(context);
            out->insert(out->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
        };
        if (!stbi_write_png_to_func(write, &png, image.width, image.height, 4, image.rgba.data(), image.width * 4))
            return false;
        base64 = Base64::encode(png.data(), png.size());
        return true;
    }

    // the largest whole scale (1..16) at which `w` x `h` font pixels plus the padding fit
    int fontFittingScale(int w, int h, int maxSize){
        int k = 16;
        while (k > 1 && (w * k + 2 * kFontPad > maxSize || h * k + 2 * kFontPad > maxSize)) k--;
        return k;
    }

    // false (and the failure naming the fix) when an image of `width` x `height` at scale k is
    // larger than max_size: an explicit scale names the largest that fits
    bool fontFits(const CallPtr& call, int width, int height, int k, int largestFitting, bool explicitScale, int maxSize){
        if (width <= maxSize && height <= maxSize)
            return true;
        if (explicitScale && largestFitting > 0){
            call->fail("At scale " + std::to_string(k) + " the preview is " + std::to_string(width) + "x" + std::to_string(height) +
                       " px, over max_size " + std::to_string(maxSize) + ": the largest scale that fits is " + std::to_string(largestFitting) +
                       " (or raise max_size, up to 2048).");
        }else{
            call->fail("The preview is " + std::to_string(width) + "x" + std::to_string(height) + " px at scale 1, over max_size " +
                       std::to_string(maxSize) + ": raise max_size (up to 2048) or shorten the text.");
        }
        return false;
    }

    // a dark checker, so white and transparent texels both show
    void fontBackdrop(fontpx::RasterImage& image){
        image.checker(fontpx::rgbaColor(30, 30, 34), fontpx::rgbaColor(44, 44, 50), 8);
    }

    // smooth vector text: what an agent sees fail S42 (R26)
    void fontRasterVector(const FontFace& face, const std::string& text, float pixels, fontpx::RasterImage* image, int& width, int& height, int x0, int y0){
        const float scale = face.scaleForPixelHeight(pixels);
        const int ascent = (int)std::lround((float)face.getAscender() * scale);
        const int lineHeight = (int)std::lround((float)(face.getAscender() - face.getDescender() + face.getLineGap()) * scale);
        bool invalid = false;
        const std::vector<uint32_t> cp = StringUtils::decodeUtf8ToCodepoints(text, invalid);
        float pen = 0.0f;
        int line = 0;
        width = 0;
        std::vector<unsigned char> coverage;
        for (uint32_t c : cp){
            if (c == 10){
                line++;
                pen = 0.0f;
                continue;
            }
            const uint32_t glyph = face.getGlyphIndex(c);
            int w = 0, h = 0, xoff = 0, yoff = 0;
            if (image && face.rasterizeGlyph(glyph, scale, coverage, w, h, xoff, yoff)){
                for (int y = 0; y < h; y++){
                    for (int x = 0; x < w; x++){
                        const uint8_t px[4] = {255, 255, 255, coverage[(size_t)y * (size_t)w + (size_t)x]};
                        image->blend(x0 + (int)std::lround(pen) + xoff + x, y0 + line * lineHeight + ascent + yoff + y, px);
                    }
                }
            }
            pen += face.getGlyphAdvance(glyph, scale);
            width = std::max(width, (int)std::ceil(pen));
        }
        height = (line + 1) * lineHeight;
    }

    void fontReply(const CallPtr& call, const fontpx::RasterImage& image, const std::string& text, Json font){
        std::string base64;
        if (!fontEncodePng(image, base64)){
            call->fail("The font preview could not be encoded.");
            return;
        }
        font["width"] = image.width;
        font["height"] = image.height;
        ToolResult result = ToolResult::success(text, Json{{"font", font}});
        result.images.push_back({base64, "image/png"});
        call->reply(std::move(result));
    }

}

bool isFontViewPath(const std::string& path){
    if (path == fontpx::kBuiltinPixelFont)
        return true;
    const std::string ext = fontLowerExtension(path);
    return ext == ".bitmapfont" || ext == ".fnt" || ext == ".jfont" || ext == ".ttf" || ext == ".otf";
}

std::string fontEnginePath(Project* project, const fs::path& file){
    const fs::path relative = file.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal());
    if (relative.empty() || relative.generic_string().rfind("..", 0) == 0)
        return file.generic_string();
    return relative.generic_string();
}

void replyFontView(const CallPtr& call, const std::string& enginePath, int maxSize){
    const Json& args = call->args();
    const bool hasText = args.contains("text") && args["text"].is_string();
    const std::string text = hasText ? args["text"].get<std::string>() : std::string();
    bool invalid = false;
    const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(text, invalid);
    if (hasText && (codepoints.size() > (size_t)kFontTextLimit || codepoints.empty())){
        call->fail("text is 1 to 256 characters (\\n for new lines); split a longer string into several previews.");
        return;
    }
    int scale = 0;
    if (args.contains("scale")){
        if (!args["scale"].is_number_integer() || args["scale"].get<int>() < 1 || args["scale"].get<int>() > 16){
            call->fail("scale is a whole number from 1 to 16 (font pixels drawn scale x scale).");
            return;
        }
        scale = args["scale"].get<int>();
    }

    if (fontpx::fontKindOf(enginePath) == fontpx::FontKind::VECTOR){
        std::string error;
        std::shared_ptr<FontFace> face = FontFace::loadFile(enginePath, 0, &error);
        if (!face){
            call->fail(error + ". Check the path.");
            return;
        }
        const std::string sample = hasText ? text : std::string("The quick brown fox\n0123456789");
        int k = scale > 0 ? scale : 1;
        int w = 0, h = 0;
        fontRasterVector(*face, sample, 16.0f * (float)k, nullptr, w, h, 0, 0);
        if (w + 2 * kFontPad > maxSize || h + 2 * kFontPad > maxSize){
            int fitting = k - 1, fw = 0, fh = 0;
            for (; fitting >= 1; fitting--){
                fontRasterVector(*face, sample, 16.0f * (float)fitting, nullptr, fw, fh, 0, 0);
                if (fw + 2 * kFontPad <= maxSize && fh + 2 * kFontPad <= maxSize) break;
            }
            fontFits(call, w + 2 * kFontPad, h + 2 * kFontPad, k, fitting, scale > 0, maxSize);
            return;
        }
        fontpx::RasterImage image(w + 2 * kFontPad, h + 2 * kFontPad);
        fontBackdrop(image);
        fontRasterVector(*face, sample, 16.0f * (float)k, &image, w, h, kFontPad, kFontPad);
        const int detected = fontpx::detectPixelSize(*face);
        Json font = {{"kind", "vector"}, {"native_size", detected}, {"scale", k}, {"glyphs", nullptr}, {"colored", false},
                     {"missing", Json::array()}};
        std::string message = enginePath + ": a vector font, drawn smooth at " + std::to_string(16 * k) + " px (antialiased: S42's font_antialiasing warns).";
        if (detected > 0)
            message += " Its outlines sit on a " + std::to_string(detected) + " px grid: a .bitmapfont with source truetype draws it as pixels.";
        fontReply(call, image, message, font);
        return;
    }

    fontpx::BitmapFontReport report;
    std::shared_ptr<fontpx::BitmapFont> font = fontpx::BitmapFont::load(enginePath, &report);
    if (!font){
        call->fail(enginePath + " does not load:\n" + fontErrorsText(report.errors),
                   Json{{"errors", fontErrorsJson(report.errors)}, {"error", report.errors.empty() ? Json() : fontErrorJson(report.errors[0])}});
        return;
    }
    Json info = {{"kind", "bitmap"}, {"native_size", font->getNativeSize()}, {"line_height", font->getLineHeight()},
                 {"baseline", font->getBaseline()}, {"glyphs", font->getGlyphCount()}, {"colored", font->isColored()}};

    if (hasText){
        int w = 0, h = 0;
        fontpx::BitmapText::measure(*font, text, w, h);
        const int fitting = fontFittingScale(std::max(w, 1), std::max(h, 1), maxSize);
        const int k = scale > 0 ? scale : fitting;
        const bool oneFits = w + 2 * kFontPad <= maxSize && h + 2 * kFontPad <= maxSize;
        if (!fontFits(call, w * k + 2 * kFontPad, h * k + 2 * kFontPad, k, oneFits ? fitting : 0, scale > 0, maxSize))
            return;
        fontpx::RasterImage image(w * k + 2 * kFontPad, h * k + 2 * kFontPad);
        fontBackdrop(image);
        fontpx::rasterBitmapText(*font, text, k, kFontInk, image, kFontPad, kFontPad);
        std::set<uint32_t> missing;
        for (uint32_t c : codepoints)
            if (c != 10 && !font->hasGlyph(c)) missing.insert(c);
        info["scale"] = k;
        info["missing"] = fontCodepointList(std::vector<uint32_t>(missing.begin(), missing.end()));
        std::string message = enginePath + ": the text at x" + std::to_string(k) + " (" + std::to_string(w) + "x" + std::to_string(h) +
                              " font pixels), white on a dark checker.";
        if (!missing.empty())
            message += " Missing (drawn as the fallback glyph): " + fontUtf8Of(std::vector<uint32_t>(missing.begin(), missing.end())) + ".";
        fontReply(call, image, message, info);
        return;
    }

    // the glyph grid, each glyph labelled with its character in the built-in font
    std::shared_ptr<fontpx::BitmapFont> labels = fontpx::BitmapFont::builtinPixel();
    std::vector<uint32_t> shown = font->getCodepoints();
    const size_t total = shown.size();
    if (shown.size() > (size_t)kFontGridLimit) shown.resize((size_t)kFontGridLimit);
    int glyphW = 1;
    for (uint32_t c : shown){
        const fontpx::BitmapGlyph* g = font->glyph(c);
        glyphW = std::max(glyphW, std::max((int)g->advance, g->offsetX + (int)g->w));
    }
    const int labelW = 5 * (labels->getSpaceWidth() + 3), labelH = labels->getLineHeight() + 2;
    auto cellOf = [&](int k){ return std::make_pair(std::max(glyphW * k, labelW) + 4, font->getLineHeight() * k + labelH + 2); };
    // up to 16 columns, as many cells as max_size holds at scale k
    auto colsFor = [&](int k){ return std::min(16, (maxSize - 2 * kFontPad) / cellOf(k).first); };
    auto capacity = [&](int k){ return std::max(0, colsFor(k)) * std::max(0, (maxSize - 2 * kFontPad) / cellOf(k).second); };
    int largest = 16;
    while (largest > 0 && capacity(largest) < 1) largest--;
    int k = scale;
    if (k == 0){
        k = 16;
        while (k > 1 && capacity(k) < (int)shown.size()) k--;
    }
    if (capacity(k) < 1){
        const auto [cw, ch] = cellOf(k);
        fontFits(call, cw + 2 * kFontPad, ch + 2 * kFontPad, k, largest, scale > 0, maxSize);
        return;
    }
    // what does not fit is left out (the message says how many show)
    if ((int)shown.size() > capacity(k)) shown.resize((size_t)capacity(k));
    const int cols = std::min(colsFor(k), std::max((int)shown.size(), 1));
    const int rows = (int)((shown.size() + (size_t)cols - 1) / (size_t)cols);
    const auto [cellW, cellH] = cellOf(k);
    fontpx::RasterImage image(cellW * cols + 2 * kFontPad, cellH * std::max(rows, 1) + 2 * kFontPad);
    fontBackdrop(image);
    for (size_t i = 0; i < shown.size(); i++){
        const int x = kFontPad + (int)(i % cols) * cellW + 2;
        const int y = kFontPad + (int)(i / cols) * cellH + 1;
        fontpx::rasterBitmapText(*font, fontpx::codepointToUtf8(shown[i]), k, kFontInk, image, x, y);
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%04X", shown[i]);
        const std::string label = (shown[i] > 32 && labels->hasGlyph(shown[i])) ? fontpx::codepointToUtf8(shown[i]) : std::string(hex);
        fontpx::rasterBitmapText(*labels, label, 1, kFontLabel, image, x, y + font->getLineHeight() * k + 1);
    }
    info["scale"] = k;
    info["missing"] = fontCodepointList(report.missingAscii);
    std::string message = enginePath + ": " + std::to_string(total) + " glyphs at x" + std::to_string(k) + ", each labelled below";
    message += total > shown.size() ? " (the first " + std::to_string(shown.size()) + " shown)." : ".";
    if (!report.missingAscii.empty())
        message += " Missing ASCII: " + fontUtf8Of(report.missingAscii) + ".";
    fontReply(call, image, message, info);
}

Json fontAssetInfo(Project* project, const fs::path& file, std::string& text){
    const std::string enginePath = fontEnginePath(project, file);
    Json out;
    const fontpx::FontPixelReport pixel = fontpx::analyzeFontForPixelArt(enginePath);
    if (fontpx::fontKindOf(enginePath) == fontpx::FontKind::VECTOR){
        Json font = {{"kind", "vector"}, {"pixel_grid_detected", pixel.pixelGridDetected}};
        if (pixel.pixelGridDetected) font["native_size"] = pixel.nativeSize;
        out["font"] = font;
        out["pixel_report"] = {{"antialiased", true}, {"partial_alpha_share", nullptr}, {"native_size", pixel.pixelGridDetected ? Json(pixel.nativeSize) : Json()}};
        out["fix"] = "create a .bitmapfont with source truetype";
        text += pixel.pixelGridDetected ? ", a vector font on a " + std::to_string(pixel.nativeSize) + " px grid (a .bitmapfont with source truetype draws it as pixels)"
                                        : ", a vector font (antialiased)";
        return out;
    }
    fontpx::BitmapFontReport report;
    std::shared_ptr<fontpx::BitmapFont> font = fontpx::BitmapFont::load(enginePath, &report);
    if (!font){
        out["errors"] = fontErrorsJson(report.errors);
        out["font"] = {{"kind", "bitmap"}, {"loads", false}};
        text += ", a bitmap font that does not load: " + fontErrorsText(report.errors);
        return out;
    }
    Json info = fontReportJson(report);
    info["kind"] = "bitmap";
    info["spacing"] = font->getSpacing();
    info["space_width"] = font->getSpaceWidth();
    info["x_height"] = font->getXHeight();
    Json kerning = Json::array();
    std::vector<std::pair<uint64_t, int>> pairs(font->getKerningTable().begin(), font->getKerningTable().end());
    std::sort(pairs.begin(), pairs.end());
    for (const auto& [key, amount] : pairs){
        if (kerning.size() >= 256) break;
        kerning.push_back(Json::array({fontpx::codepointToUtf8((uint32_t)(key >> 32)), fontpx::codepointToUtf8((uint32_t)(key & 0xFFFFFFFFu)), amount}));
    }
    info["kerning_pairs"] = kerning;
    out["font"] = info;
    out["pixel_report"] = {{"antialiased", pixel.antialiased}, {"partial_alpha_share", info["partial_alpha_share"]}, {"native_size", font->getNativeSize()}};
    // the settings as the file holds them (a .fnt used directly has none: R3's defaults)
    if (fontLowerExtension(enginePath) == ".bitmapfont"){
        std::ifstream in(file, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Json doc = Json::parse(content, nullptr, false);
        if (!doc.is_discarded() && doc.is_object()){
            out["source"] = doc.value("source", Json::object());
            Json overrides = doc.value("glyphs", Json::object());
            out["overrides"] = overrides;
        }
    }else{
        out["source"] = {{"type", "bmfont"}, {"file", enginePath}};
        out["overrides"] = Json::object();
    }
    text += ", a bitmap font: " + std::to_string(report.glyphs) + " glyphs, " + std::to_string(font->getNativeSize()) + " px (line " +
            std::to_string(font->getLineHeight()) + ", baseline " + std::to_string(font->getBaseline()) + ")" +
            (font->isColored() ? ", coloured" : "") + (pixel.antialiased ? ", partial alpha (set alpha_cutoff)" : "");
    return out;
}

Json fontWriteReport(Project* project, const fs::path& file, const std::string& content, std::string& text){
    const std::string enginePath = fontEnginePath(project, file);
    fontpx::BitmapFontReport report;
    std::shared_ptr<fontpx::BitmapFont> font = fontpx::BitmapFont::loadSettings(enginePath, content, &report);
    Json out;
    if (!font){
        out["errors"] = fontErrorsJson(report.errors);
        text += " Written, but it does not load:\n" + fontErrorsText(report.errors);
        return out;
    }
    out["font"] = fontReportJson(report);
    text += " Bitmap font: " + std::to_string(report.glyphs) + " glyphs, " + std::to_string(report.nativeSize) + " px, line " +
            std::to_string(report.lineHeight) + ", baseline " + std::to_string(report.baseline) + ".";
    if (!report.missingAscii.empty())
        text += " Missing ASCII: " + fontUtf8Of(report.missingAscii) + ".";
    if (!report.warnings.empty())
        text += " " + fontErrorsText(report.warnings);
    return out;
}

namespace {

    void previewFontTool(const CallPtr& call){
        const Json& args = call->args();
        int maxSize = 512;
        if (args.contains("max_size")){
            if (!args["max_size"].is_number_integer() || args["max_size"].get<int>() < 16 || args["max_size"].get<int>() > 2048){
                call->fail("max_size is the longest side in pixels, 16 to 2048.");
                return;
            }
            maxSize = args["max_size"].get<int>();
        }
        const std::string path = args.value("path", std::string());
        if (path == fontpx::kBuiltinPixelFont){
            replyFontView(call, fontpx::kBuiltinPixelFont, maxSize);
            return;
        }
        if (path.empty()){
            call->fail("path is required: a project-relative font (.bitmapfont, .fnt, .ttf, .otf) or builtin:pixel.");
            return;
        }
        Project* project = call->ctx().project();
        fs::path file;
        std::string error;
        if (!McpRefs::resolveProjectPath(project, path, file, error, true)){
            call->fail(error);
            return;
        }
        file = file.lexically_normal();
        if (!isFontViewPath(file.generic_string())){
            call->fail(path + " is not a font (.bitmapfont, .fnt, .ttf, .otf, builtin:pixel); view_asset shows images.");
            return;
        }
        replyFontView(call, fontEnginePath(project, file), maxSize);
    }

}

void registerPixelFontTools(McpRegistry& registry){
    using namespace schema;
    ToolDef tool;
    tool.name = "preview_font";
    tool.title = "Preview Font";
    tool.description = "Draw a font as PNG: a bitmap font (.bitmapfont, .fnt, builtin:pixel) as its labelled glyph grid, or `text` at a "
                       "whole `scale` (default: the largest that fits max_size) with the missing characters listed; a TTF/OTF smooth. "
                       "Metrics, kerning, pixel report: get_asset_info.";
    tool.inputSchema = object({
        {"path", assetPath("")},
        {"text", string("At most 256 characters, \\n for lines.")},
        {"scale", integer("Font pixels per side.", 1, 16)},
        {"max_size", withDefault(integer("Longest side, px.", 16, 2048), 512)},
    }, {"path"});
    tool.annotations.readOnly = true;
    tool.annotations.idempotent = true;
    tool.group = "pixel_art";
    tool.handler = previewFontTool;
    registry.addTool(std::move(tool));
}

}
