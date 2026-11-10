// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "BitmapFontInspector.h"

#include "Backend.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "external/IconsFontAwesome6.h"
#include "imgui.h"
#include "pixelart/BitmapFontCreate.h"
#include "pixelart/BitmapFontPool.h"
#include "pixelart/BitmapText.h"
#include "pixelart/BitmapTextRaster.h"
#include "pixelart/FontPixelReport.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/StringUtils.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    namespace bfi = ::jasperengine::pixelart;

    std::string bfiUtf8(uint32_t codepoint){
        return bfi::codepointToUtf8(codepoint);
    }

    uint32_t bfiFirst(const std::string& text){
        bool invalid = false;
        const std::vector<uint32_t> c = StringUtils::decodeUtf8ToCodepoints(text, invalid);
        return c.empty() ? 0 : c[0];
    }

    std::string bfiExtension(const fs::path& path){
        std::string ext = FileUtils::pathToUtf8(path.extension());
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        return ext;
    }

    std::string bfiEnginePath(Project* project, const fs::path& file){
        const fs::path rel = file.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal());
        return (rel.empty() || rel.generic_string().rfind("..", 0) == 0) ? file.generic_string() : rel.generic_string();
    }

    // an RGBA image as an ImGui texture (NEAREST: every texel a square)
    void bfiUpload(Texture& texture, const std::string& id, const bfi::RasterImage& image){
        texture.destroy();
        if (image.width <= 0 || image.height <= 0)
            return;
        const size_t bytes = image.rgba.size();
        unsigned char* pixels = static_cast<unsigned char*>(std::malloc(bytes));
        if (!pixels)
            return;
        std::memcpy(pixels, image.rgba.data(), bytes);
        TextureData data(image.width, image.height, (unsigned int)bytes, ColorFormat::RGBA, 4, pixels);
        data.setDataOwned(false);
        texture = Texture(id, data);
        texture.getData().setDataOwned(true);
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        texture.load();
    }

    void bfiImage(Texture& texture, float width, float height){
        if (TextureRender* render = texture.getRender())
            ImGui::Image(Backend::getImGuiTexture(render), ImVec2(width, height));
    }

    void bfiBackdrop(bfi::RasterImage& image, int backdrop){
        if (backdrop == 1) image.fill(bfi::rgbaColor(24, 24, 28));
        else if (backdrop == 2) image.fill(bfi::rgbaColor(232, 232, 232));
        else image.checker(bfi::rgbaColor(60, 60, 66), bfi::rgbaColor(84, 84, 92), 8);
    }

}

BitmapFontInspector::BitmapFontInspector(Project* project): project(project){
}

BitmapFontInspector::~BitmapFontInspector(){
    sheetTexture.destroy();
    previewTexture.destroy();
    kernTexture.destroy();
}

std::string BitmapFontInspector::mergeKey(const char* property) const{
    return "bitmapfont:" + asset.generic_string() + ":" + property + "#" + std::to_string(dragSerial);
}

void BitmapFontInspector::reload(const fs::path& file){
    const bool sameAsset = file == asset;
    asset = file;
    std::error_code ec;
    stamp = fs::last_write_time(file, ec);
    std::ifstream in(file, std::ios::binary);
    text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!sameAsset){
        selectedCell = -1;
        selectedGlyph = 0;
        previewKey.clear();
        kernKey.clear();
    }
    rebuildFont();
}

void BitmapFontInspector::rebuildFont(){
    const std::string enginePath = bfiEnginePath(project, asset);
    settingsErrors.clear();
    report = bfi::BitmapFontReport();
    font.reset();
    docValid = false;
    sheet = bfi::DecodedPage();
    cellWidth = cellHeight = cols = rows = 0;
    previewKey.clear();
    kernKey.clear();
    if (bfiExtension(asset) == ".bitmapfont"){
        doc = nlohmann::json::parse(text, nullptr, false);
        docValid = !doc.is_discarded() && doc.is_object();
        bfi::parseBitmapFontSettings(text, settings, settingsErrors);
        font = bfi::BitmapFont::loadSettings(enginePath, text, &report);
        // the grid's sheet, for the glyph grid
        if (docValid && settings.source == bfi::BitmapFontSettings::Source::GRID){
            const fs::path image = asset.parent_path() / FileUtils::pathFromUtf8(settings.file);
            std::ifstream png(image, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(png)), std::istreambuf_iterator<char>());
            if (bfi::decodeImage(bytes, sheet)){
                cellWidth = settings.cellWidth;
                cellHeight = settings.cellHeight;
                int gx = settings.gapX, gy = settings.gapY;
                if (cellWidth <= 0 || cellHeight <= 0){
                    if (!bfi::cellFromFileName(settings.file, cellWidth, cellHeight))
                        bfi::detectGridCell(sheet.rgba.data(), sheet.width, sheet.height, cellWidth, cellHeight, gx, gy);
                }
                if (cellWidth > 0 && cellHeight > 0){
                    const int strideX = cellWidth + gx, strideY = cellHeight + gy;
                    cols = std::max(0, (sheet.width - settings.marginX - cellWidth) / strideX + 1);
                    rows = std::max(0, (sheet.height - settings.marginY - cellHeight) / strideY + 1);
                    settings.gapX = gx;
                    settings.gapY = gy;
                }
                bfi::RasterImage image(sheet.width, sheet.height);
                image.checker(bfi::rgbaColor(60, 60, 66), bfi::rgbaColor(84, 84, 92), 4);
                for (int y = 0; y < sheet.height; y++)
                    for (int x = 0; x < sheet.width; x++)
                        image.blend(x, y, &sheet.rgba[((size_t)y * (size_t)sheet.width + (size_t)x) * 4]);
                bfiUpload(sheetTexture, "editor-bitmapfont-sheet-" + std::to_string(++textureSerial), image);
            }
        }
    }else{
        font = bfi::BitmapFont::load(enginePath, &report);
    }
}

void BitmapFontInspector::write(const std::string& key){
    if (!docValid)
        return;
    const std::string content = bfi::canonicalBitmapFontJson(doc.dump());
    if (content == text)
        return;
    std::vector<WriteFilesCmd::Item> items = {{asset, content}};
    // the fonts that read it load again, on undo and redo too
    auto afterChange = [project = project](const std::vector<fs::path>& changed){
        bool any = false;
        for (const fs::path& path : changed)
            any = !bfi::BitmapFontPool::invalidate(FileUtils::pathToUtf8(path)).empty() || any;
        if (any)
            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) sceneProject->needUpdateRender = true;
    };
    std::string error;
    if (key.empty()){
        if (!ProjectFileWrites::writeUndoable(project, std::move(items), error, afterChange, nullptr, {}, false, "Edit Bitmap Font"))
            return;
    }else{
        auto failure = std::make_shared<std::string>();
        WriteFilesCmd* cmd = new WriteFilesCmd(std::move(items), afterChange);
        cmd->setMergeKey(key);
        cmd->setName("Edit Bitmap Font");
        cmd->reportFailureTo(failure);
        if (!project->getProjectCommandHistory()->addCommand(cmd))
            return;
    }
    text = content;
    std::error_code ec;
    stamp = fs::last_write_time(asset, ec);
    rebuildFont();
}

std::vector<uint32_t> BitmapFontInspector::cellChars() const{
    std::vector<uint32_t> out((size_t)std::max(cols * rows, 0), 0xFFFF);
    if (!settings.rows.empty()){
        for (size_t r = 0; r < settings.rows.size(); r++){
            bool invalid = false;
            const std::vector<uint32_t> row = StringUtils::decodeUtf8ToCodepoints(settings.rows[r], invalid);
            for (size_t c = 0; c < row.size(); c++){
                const size_t cell = r * (size_t)cols + c;
                if (c < (size_t)cols && cell < out.size()) out[cell] = row[c];
            }
        }
    }else{
        const std::vector<uint32_t> chars = bfi::expandFontChars(settings.chars, "ascii");
        for (size_t i = 0; i < chars.size() && i < out.size(); i++) out[i] = chars[i];
    }
    return out;
}

void BitmapFontInspector::assignCell(int cell, uint32_t codepoint){
    if (cell < 0 || cell >= cols * rows || !docValid)
        return;
    std::vector<uint32_t> chars = cellChars();
    // the character leaves the cell it held
    for (uint32_t& c : chars)
        if (c == codepoint) c = 0xFFFF;
    chars[(size_t)cell] = codepoint;
    if (!settings.rows.empty()){
        nlohmann::json rowsJson = nlohmann::json::array();
        for (int r = 0; r < rows; r++){
            std::string row;
            int last = -1;
            for (int c = 0; c < cols; c++) if (chars[(size_t)(r * cols + c)] != 0xFFFF) last = c;
            for (int c = 0; c <= last; c++) row += bfiUtf8(chars[(size_t)(r * cols + c)]);
            rowsJson.push_back(row);
        }
        doc["source"]["rows"] = rowsJson;
    }else{
        size_t last = 0;
        for (size_t i = 0; i < chars.size(); i++) if (chars[i] != 0xFFFF) last = i + 1;
        std::string out;
        for (size_t i = 0; i < last; i++) out += bfiUtf8(chars[i]);
        doc["source"]["chars"] = out;
    }
    write();
}

int BitmapFontInspector::cellInkLeft(int cell) const{
    if (cell < 0 || cols <= 0) return 0;
    const int x0 = settings.marginX + (cell % cols) * (cellWidth + settings.gapX), y0 = settings.marginY + (cell / cols) * (cellHeight + settings.gapY);
    for (int x = 0; x < cellWidth; x++)
        for (int y = 0; y < cellHeight; y++)
            if (x0 + x < sheet.width && y0 + y < sheet.height && sheet.rgba[(((size_t)(y0 + y)) * (size_t)sheet.width + (size_t)(x0 + x)) * 4 + 3]) return x;
    return 0;
}

int BitmapFontInspector::cellInkRight(int cell) const{
    if (cell < 0 || cols <= 0) return -1;
    const int x0 = settings.marginX + (cell % cols) * (cellWidth + settings.gapX), y0 = settings.marginY + (cell / cols) * (cellHeight + settings.gapY);
    for (int x = cellWidth - 1; x >= 0; x--)
        for (int y = 0; y < cellHeight; y++)
            if (x0 + x < sheet.width && y0 + y < sheet.height && sheet.rgba[(((size_t)(y0 + y)) * (size_t)sheet.width + (size_t)(x0 + x)) * 4 + 3]) return x;
    return -1;
}

void BitmapFontInspector::draw(const fs::path& file, bool header){
    std::error_code ec;
    const fs::file_time_type now = fs::last_write_time(file, ec);
    if (file != asset || now != stamp)
        reload(file);

    const std::string ext = bfiExtension(file);
    if (header){
        ImGui::TextUnformatted(ICON_FA_FONT);
        ImGui::SameLine();
        ImGui::TextUnformatted(FileUtils::pathToUtf8(file.filename()).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ext == ".bitmapfont" ? "Bitmap Font" : ext == ".fnt" ? "BMFont" : "Font");
        ImGui::Separator();
    }

    if (ext == ".bitmapfont")
        drawBitmapFont();
    else
        drawFntOrTrueType();
}

void BitmapFontInspector::drawFntOrTrueType(){
    const std::string ext = bfiExtension(asset);
    if (ext == ".fnt"){
        if (font){
            ImGui::Text("%d glyphs, %d px, line %d, baseline %d", report.glyphs, report.nativeSize, report.lineHeight, report.baseline);
        }else{
            for (const bfi::FontError& e : report.errors) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s (%s)", e.message.c_str(), e.fix.c_str());
        }
        ImGui::TextWrapped("A .fnt is a font by itself, read-only here: a Bitmap Font over it adds overrides, kerning and metrics.");
        if (ImGui::Button(ICON_FA_FONT " Create Bitmap Font to edit")){
            std::string error;
            if (editor::pixelart::createBitmapFont(project, asset, error).empty())
                report.errors.push_back({"font_create_failed", error, "", ""});
        }
        drawPreview();
        return;
    }
    // a TrueType/OpenType font: a pixel font when its outlines sit on a grid (R24)
    const int size = editor::pixelart::detectedPixelFontSize(asset, project->getAssetsPath());
    if (size > 0){
        ImGui::TextWrapped("Pixel font detected (%d px): a Bitmap Font draws it as pixels at whole scales.", size);
        if (ImGui::Button(ICON_FA_FONT " Create Bitmap Font")){
            std::string error;
            if (editor::pixelart::createBitmapFont(project, asset, error).empty())
                ImGui::SetTooltip("%s", error.c_str());
        }
    }else{
        ImGui::TextWrapped("A vector font: Text draws it smooth (antialiased). For pixel art, use a Bitmap Font.");
    }
}

void BitmapFontInspector::drawBitmapFont(){
    if (!docValid){
        ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "Not a valid .bitmapfont (JSON): fix it in a text editor.");
        return;
    }
    const bool readOnly = settings.version > bfi::kBitmapFontVersion;
    if (readOnly)
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Version %d is newer than this editor's: read-only.", settings.version);
    for (const bfi::FontError& e : report.errors)
        ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), ICON_FA_TRIANGLE_EXCLAMATION " %s: %s (%s)", e.field.c_str(), e.message.c_str(), e.fix.c_str());
    if (readOnly) ImGui::BeginDisabled();
    if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) drawSource();
    if (ImGui::CollapsingHeader("Glyph Grid", ImGuiTreeNodeFlags_DefaultOpen)){
        drawGrid();
        drawGlyphPanel();
    }
    if (ImGui::CollapsingHeader("Metrics", ImGuiTreeNodeFlags_DefaultOpen)) drawMetrics();
    if (ImGui::CollapsingHeader("Kerning")) drawKerning();
    if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) drawPreview();
    if (ImGui::CollapsingHeader("Report", ImGuiTreeNodeFlags_DefaultOpen)) drawReport();
    if (readOnly) ImGui::EndDisabled();
}

void BitmapFontInspector::drawSource(){
    nlohmann::json& source = doc["source"];
    static const char* const types[] = {"grid", "bmfont", "truetype"};
    static const char* const typeLabels[] = {"Grid Sheet", "BMFont (.fnt)", "TrueType Pixel Font"};
    int type = (int)settings.source;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##bfSourceType", &type, typeLabels, 3) && type != (int)settings.source){
        const std::string file = settings.file;
        source = nlohmann::json{{"type", types[type]}};
        source[type == 0 ? "image" : "file"] = file;
        write();
        return;
    }
    const char* key = settings.source == bfi::BitmapFontSettings::Source::GRID ? "image" : "file";
    char path[512] = {};
    std::strncpy(path, settings.file.c_str(), sizeof(path) - 1);
    ImGui::TextUnformatted(settings.source == bfi::BitmapFontSettings::Source::GRID ? "Sheet" : "File");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##bfSourceFile", path, sizeof(path), ImGuiInputTextFlags_EnterReturnsTrue)){
        source[key] = std::string(path);
        write();
    }
    if (settings.source == bfi::BitmapFontSettings::Source::GRID){
        int cell[2] = {settings.cellWidth, settings.cellHeight};
        if (ImGui::InputInt2("Cell Size##bfCell", cell) && cell[0] > 0 && cell[1] > 0){
            source["cell"] = {cell[0], cell[1]};
            write();
        }
        int margin[2] = {settings.marginX, settings.marginY};
        if (ImGui::InputInt2("Margin##bfMargin", margin)){
            source["margin"] = {std::max(0, margin[0]), std::max(0, margin[1])};
            write();
        }
        int gap[2] = {settings.gapX, settings.gapY};
        if (ImGui::InputInt2("Gap##bfGap", gap)){
            source["gap"] = {std::max(0, gap[0]), std::max(0, gap[1])};
            write();
        }
        // D8: detection fills the file once
        if (ImGui::Button("Detect##bfDetectCell")){
            int w = 0, h = 0, gx = 0, gy = 0;
            if (bfi::cellFromFileName(settings.file, w, h) ||
                    (!sheet.rgba.empty() && bfi::detectGridCell(sheet.rgba.data(), sheet.width, sheet.height, w, h, gx, gy))){
                source["cell"] = {w, h};
                if (gx || gy) source["gap"] = {gx, gy};
                else source.erase("gap");
                write();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The cell from the file name (@WxH), else from transparent gutters");
        char chars[1024] = {};
        std::strncpy(chars, settings.chars.empty() ? "ascii" : settings.chars.c_str(), sizeof(chars) - 1);
        if (settings.rows.empty()){
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##bfChars", chars, sizeof(chars), ImGuiInputTextFlags_EnterReturnsTrue)){
                source["chars"] = std::string(chars);
                write();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Characters: ascii, latin1, or the cells' characters in order (Enter applies)");
        }else{
            ImGui::TextDisabled("Characters by row (%zu rows)", settings.rows.size());
        }
    }else if (settings.source == bfi::BitmapFontSettings::Source::TRUETYPE){
        float threshold = settings.threshold;
        if (ImGui::SliderFloat("Threshold##bfThreshold", &threshold, 0.05f, 0.95f, "%.2f")) source["threshold"] = std::round(threshold * 100.0f) / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) write();
        int size = settings.pixelSize;
        if (ImGui::InputInt("Pixel Size##bfPixelSize", &size)){
            source["pixel_size"] = std::clamp(size, 0, 64);
            write();
        }
        if (ImGui::Button("Detect##bfDetectSize")){
            const int detected = editor::pixelart::detectedPixelFontSize(asset.parent_path() / FileUtils::pathFromUtf8(settings.file), project->getAssetsPath());
            if (detected > 0){
                source["pixel_size"] = detected;
                write();
            }
        }
    }
}

void BitmapFontInspector::drawGrid(){
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::SliderInt("Zoom##bfZoom", &zoom, 1, 8, "%dx");
    if (cols <= 0 || rows <= 0){
        if (font && settings.source != bfi::BitmapFontSettings::Source::GRID){
            // a BMFont or TrueType source: its glyphs, select one to override it
            const std::vector<uint32_t>& codepoints = font->getCodepoints();
            const float cell = ImGui::GetFontSize() * 1.8f;
            const int perRow = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cell));
            for (size_t i = 0; i < codepoints.size(); i++){
                if (i % (size_t)perRow) ImGui::SameLine();
                const std::string label = (codepoints[i] == ' ' ? std::string("sp") : bfiUtf8(codepoints[i])) + "##bfg" + std::to_string(codepoints[i]);
                if (ImGui::Selectable(label.c_str(), selectedGlyph == codepoints[i], 0, ImVec2(cell, cell))) selectedGlyph = codepoints[i];
            }
        }else{
            ImGui::TextDisabled("No sheet or cell size: set the sheet and Cell Size, or Detect.");
        }
        return;
    }
    const std::vector<uint32_t> chars = cellChars();
    const float z = (float)zoom;
    const ImVec2 size(sheet.width * z, sheet.height * z);
    ImGui::BeginChild("##bfGridArea", ImVec2(0, std::min(size.y + ImGui::GetStyle().ScrollbarSize + 4, ImGui::GetFontSize() * 24)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    bfiImage(sheetTexture, size.x, size.y);
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##bfGrid", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)));
    UIProbe::item("bitmapFont.grid");
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int strideX = cellWidth + settings.gapX, strideY = cellHeight + settings.gapY;
    auto cellRect = [&](int cell){
        const float x = origin.x + (settings.marginX + (cell % cols) * strideX) * z;
        const float y = origin.y + (settings.marginY + (cell / cols) * strideY) * z;
        return ImVec4(x, y, x + cellWidth * z, y + cellHeight * z);
    };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hoveredCell = -1;
    for (int cell = 0; cell < cols * rows; cell++){
        const ImVec4 r = cellRect(cell);
        draw->AddRect(ImVec2(r.x, r.y), ImVec2(r.z, r.w), IM_COL32(120, 200, 255, 70));
        if (mouse.x >= r.x && mouse.x < r.z && mouse.y >= r.y && mouse.y < r.w) hoveredCell = cell;
        // the cell's character
        if (chars[(size_t)cell] != 0xFFFF && cellHeight * z >= 14.0f){
            const std::string label = chars[(size_t)cell] == ' ' ? std::string("sp") : bfiUtf8(chars[(size_t)cell]);
            draw->AddText(ImVec2(r.x + 1, r.y), IM_COL32(255, 220, 120, 200), label.c_str());
        }
        // the baseline across every cell
        const float baseY = r.y + (settings.baseline ? *settings.baseline : report.baseline) * z;
        draw->AddLine(ImVec2(r.x, baseY), ImVec2(r.z, baseY), IM_COL32(255, 90, 90, 140));
    }
    // the selected cell: its advance edge, draggable
    if (selectedCell >= 0 && selectedCell < cols * rows){
        const ImVec4 r = cellRect(selectedCell);
        draw->AddRect(ImVec2(r.x - 1, r.y - 1), ImVec2(r.z + 1, r.w + 1), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
        const uint32_t c = chars[(size_t)selectedCell];
        if (font && c != 0xFFFF && font->hasGlyph(c)){
            const int pen = settings.trimBoth ? cellInkLeft(selectedCell) : 0;
            const float edge = r.x + (pen + font->glyph(c)->advance) * z;
            draw->AddLine(ImVec2(edge, r.y), ImVec2(edge, r.w), IM_COL32(255, 160, 40, 255), 2.0f);
            const bool nearEdge = std::fabs(mouse.x - edge) <= 4.0f && mouse.y >= r.y && mouse.y < r.w;
            if (hovered && nearEdge && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) draggingAdvance = true;
            if (draggingAdvance && active){
                const int advance = std::clamp((int)std::lround((mouse.x - r.x) / z) - pen, 0, 255);
                if (advance != font->glyph(c)->advance){
                    doc["glyphs"][bfiUtf8(c)]["advance"] = advance;
                    write(mergeKey("advance"));
                }
            }
        }
    }
    // the baseline: a drag on the line of the hovered row
    if (hoveredCell >= 0 && !draggingAdvance){
        const ImVec4 r = cellRect(hoveredCell);
        const int baseline = settings.baseline ? *settings.baseline : report.baseline;
        if (hovered && std::fabs(mouse.y - (r.y + baseline * z)) <= 3.0f && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) draggingBaseline = true;
    }
    if (draggingBaseline && active && hoveredCell >= 0){
        const ImVec4 r = cellRect(hoveredCell);
        const int baseline = std::clamp((int)std::lround((mouse.y - r.y) / z), 0, cellHeight);
        if (baseline != (settings.baseline ? *settings.baseline : report.baseline)){
            doc["baseline"] = baseline;
            write(mergeKey("baseline"));
        }
    }
    if (!active && (draggingAdvance || draggingBaseline)){
        draggingAdvance = draggingBaseline = false;
        dragSerial++;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !draggingAdvance && !draggingBaseline && hoveredCell >= 0)
        selectedCell = hoveredCell;
    if (hovered && hoveredCell >= 0){
        const uint32_t c = chars[(size_t)hoveredCell];
        if (c == 0xFFFF){
            ImGui::SetTooltip("Cell %d: no character (select it and type one)", hoveredCell);
        }else if (font){
            const bfi::BitmapGlyph* g = font->glyph(c);
            std::string partners;
            for (const auto& [key, amount] : font->getKerningTable()){
                const uint32_t left = (uint32_t)(key >> 32), right = (uint32_t)(key & 0xFFFFFFFFu);
                if (left == c || right == c) partners += " " + bfiUtf8(left) + bfiUtf8(right) + " " + std::to_string(amount);
            }
            ImGui::SetTooltip("U+%04X '%s'\nAdvance %d, offset %d, %d%s%s", c, bfiUtf8(c).c_str(), g->advance, g->offsetX, g->offsetY,
                              partners.empty() ? "" : "\nKerning:", partners.c_str());
        }
    }
    // the keyboard: arrows move the selection, a typed character goes into the selected cell
    if (selectedCell >= 0 && (hovered || ImGui::IsWindowFocused()) && !ImGui::IsAnyItemActive()){
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) selectedCell = std::max(0, selectedCell - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) selectedCell = std::min(cols * rows - 1, selectedCell + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && selectedCell >= cols) selectedCell -= cols;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && selectedCell + cols < cols * rows) selectedCell += cols;
        ImGuiIO& io = ImGui::GetIO();
        for (int i = 0; i < io.InputQueueCharacters.Size; i++){
            const uint32_t typed = (uint32_t)io.InputQueueCharacters[i];
            if (typed >= 32 && typed != 127){
                assignCell(selectedCell, typed);
                break;
            }
        }
    }
    ImGui::EndChild();
}

void BitmapFontInspector::drawGlyphPanel(){
    uint32_t c = 0xFFFF;
    if (cols > 0 && selectedCell >= 0 && selectedCell < cols * rows) c = cellChars()[(size_t)selectedCell];
    else if (selectedGlyph) c = selectedGlyph;
    if (selectedCell < 0 && !selectedGlyph){
        ImGui::TextDisabled("Click a cell to edit its glyph.");
        return;
    }
    if (cols > 0){
        char character[16] = {};
        if (c != 0xFFFF) std::strncpy(character, bfiUtf8(c).c_str(), sizeof(character) - 1);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        if (ImGui::InputText("Character##bfChar", character, sizeof(character), ImGuiInputTextFlags_EnterReturnsTrue) && character[0])
            assignCell(selectedCell, bfiFirst(character));
    }
    if (c == 0xFFFF || !font || !font->hasGlyph(c))
        return;
    ImGui::SameLine();
    ImGui::TextDisabled("U+%04X", c);
    const std::string name = bfiUtf8(c);
    nlohmann::json* override = (doc.contains("glyphs") && doc["glyphs"].contains(name)) ? &doc["glyphs"][name] : nullptr;
    // "Auto" is what the source measures: the rightmost ink column + 1
    int autoAdvance = font->glyph(c)->advance;
    if (cols > 0 && selectedCell >= 0){
        const int pen = settings.trimBoth ? cellInkLeft(selectedCell) : 0;
        autoAdvance = std::max(0, cellInkRight(selectedCell) + 1 - pen);
    }
    bool overridden = override && override->contains("advance");
    if (ImGui::Checkbox("Advance##bfAdvanceOn", &overridden)){
        if (overridden) doc["glyphs"][name]["advance"] = font->glyph(c)->advance;
        else if (override) override->erase("advance");
        if (doc["glyphs"].contains(name) && doc["glyphs"][name].empty()) doc["glyphs"].erase(name);
        write();
        return;
    }
    ImGui::SameLine();
    int advance = font->glyph(c)->advance;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (!overridden) ImGui::BeginDisabled();
    if (ImGui::DragInt("##bfAdvance", &advance, 0.2f, 0, 255)){
        doc["glyphs"][name]["advance"] = advance;
        write(mergeKey("glyphAdvance"));
    }
    if (ImGui::IsItemDeactivated()) dragSerial++;
    if (!overridden) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Auto: %d", autoAdvance);
    int offset[2] = {0, 0};
    if (override && override->contains("offset") && (*override)["offset"].is_array() && (*override)["offset"].size() == 2){
        offset[0] = (*override)["offset"][0].get<int>();
        offset[1] = (*override)["offset"][1].get<int>();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::InputInt2("Offset##bfOffset", offset)){
        if (offset[0] == 0 && offset[1] == 0){
            if (override) override->erase("offset");
            if (doc["glyphs"].contains(name) && doc["glyphs"][name].empty()) doc["glyphs"].erase(name);
        }else{
            doc["glyphs"][name]["offset"] = {offset[0], offset[1]};
        }
        write();
    }
}

void BitmapFontInspector::drawMetrics(){
    auto intRow = [this](const char* label, const char* key, int value, int minimum, int maximum){
        int v = value;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        if (ImGui::DragInt(label, &v, 0.2f, minimum, maximum)){
            doc[key] = std::clamp(v, minimum, maximum);
            write(mergeKey(key));
        }
        if (ImGui::IsItemDeactivated()) dragSerial++;
    };
    intRow("Spacing##bfSpacing", "spacing", settings.spacing, -16, 16);
    intRow("Space Width##bfSpace", "space_width", font ? font->getSpaceWidth() : 3, 0, 255);
    intRow("Line Height##bfLine", "line_height", font ? font->getLineHeight() : 10, 1, 255);
    intRow("Baseline##bfBaseline", "baseline", font ? font->getBaseline() : 7, 0, 255);
    char fallback[16] = {};
    std::strncpy(fallback, settings.fallback.empty() ? "?" : settings.fallback.c_str(), sizeof(fallback) - 1);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
    if (ImGui::InputText("Fallback##bfFallback", fallback, sizeof(fallback), ImGuiInputTextFlags_EnterReturnsTrue) && fallback[0]){
        doc["fallback"] = bfiUtf8(bfiFirst(fallback));
        write();
    }
    static const char* const colored[] = {"Auto", "Monochrome", "Coloured"};
    int mode = settings.colored < 0 ? 0 : (settings.colored == 0 ? 1 : 2);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::Combo("Colored##bfColored", &mode, colored, 3)){
        if (mode == 0) doc["colored"] = "auto";
        else doc["colored"] = (mode == 2);
        write();
    }
    int cutoff = settings.alphaCutoff;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderInt("Alpha Cutoff##bfCutoff", &cutoff, 0, 255)){
        doc["alpha_cutoff"] = cutoff;
        write(mergeKey("alpha_cutoff"));
    }
    if (ImGui::IsItemDeactivated()) dragSerial++;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 keeps the alpha; else texels at or above it become opaque and the rest clear");
}

void BitmapFontInspector::drawKerning(){
    nlohmann::json pairs = doc.value("kerning", nlohmann::json::array());
    int remove = -1;
    if (ImGui::BeginTable("##bfKerning", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)){
        for (int i = 0; i < (int)pairs.size(); i++){
            const nlohmann::json& pair = pairs[(size_t)i];
            if (!pair.is_array() || pair.size() != 3) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string label = pair[0].get<std::string>() + " " + pair[1].get<std::string>() + "##bfKernRow" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), kernPreview == i)) kernPreview = i;
            ImGui::TableSetColumnIndex(1);
            int amount = pair[2].get<int>();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            if (ImGui::DragInt(("##bfKernAmount" + std::to_string(i)).c_str(), &amount, 0.1f, -32, 32)){
                doc["kerning"][(size_t)i][2] = amount;
                write(mergeKey("kerning"));
            }
            if (ImGui::IsItemDeactivated()) dragSerial++;
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton((ICON_FA_XMARK "##bfKernRemove" + std::to_string(i)).c_str())) remove = i;
        }
        ImGui::EndTable();
    }
    if (remove >= 0){
        doc["kerning"].erase((size_t)remove);
        if (doc["kerning"].empty()) doc.erase("kerning");
        kernPreview = -1;
        write();
        return;
    }
    char left[16] = {}, right[16] = {};
    std::strncpy(left, kernLeft.c_str(), sizeof(left) - 1);
    std::strncpy(right, kernRight.c_str(), sizeof(right) - 1);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 2.5f);
    if (ImGui::InputText("##bfKernLeft", left, sizeof(left))) kernLeft = left;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 2.5f);
    if (ImGui::InputText("##bfKernRight", right, sizeof(right))) kernRight = right;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
    ImGui::DragInt("##bfKernNew", &kernAmount, 0.1f, -32, 32);
    ImGui::SameLine();
    if (ImGui::Button("Add Pair##bfKernAdd") && bfiFirst(kernLeft) && bfiFirst(kernRight)){
        const std::string l = bfiUtf8(bfiFirst(kernLeft)), r = bfiUtf8(bfiFirst(kernRight));
        bool replaced = false;
        if (doc.contains("kerning"))
            for (nlohmann::json& pair : doc["kerning"])
                if (pair.is_array() && pair.size() == 3 && pair[0] == l && pair[1] == r){ pair[2] = kernAmount; replaced = true; }
        if (!replaced) doc["kerning"].push_back({l, r, kernAmount});
        kernLeft.clear();
        kernRight.clear();
        write();
        return;
    }
    // the pair preview
    if (font && kernPreview >= 0 && kernPreview < (int)pairs.size()){
        const std::string pair = pairs[(size_t)kernPreview][0].get<std::string>() + pairs[(size_t)kernPreview][1].get<std::string>();
        const std::string key = pair + std::to_string(font->getSerial());
        const int k = 6;
        if (key != kernKey){
            int w = 0, h = 0;
            bfi::BitmapText::measure(*font, pair, w, h);
            bfi::RasterImage image(w * k + 8, h * k + 8);
            bfiBackdrop(image, 1);
            bfi::rasterBitmapText(*font, pair, k, bfi::rgbaColor(255, 255, 255), image, 4, 4);
            bfiUpload(kernTexture, "editor-bitmapfont-kern-" + std::to_string(++textureSerial), image);
            kernKey = key;
            kernWidth = image.width;
            kernHeight = image.height;
        }
        bfiImage(kernTexture, (float)kernWidth, (float)kernHeight);
    }
}

void BitmapFontInspector::drawPreview(){
    char buffer[512] = {};
    std::strncpy(buffer, previewText.c_str(), sizeof(buffer) - 1);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##bfPreviewText", buffer, sizeof(buffer))) previewText = buffer;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::SliderInt("Scale##bfPreviewScale", &previewScale, 1, 8, "%dx");
    ImGui::SameLine();
    static const char* const backdrops[] = {"Checker", "Dark", "Light"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::Combo("Backdrop##bfBackdrop", &previewBackdrop, backdrops, 3);
    static const char* const outlines[] = {"0", "1", "2"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3);
    ImGui::Combo("Outline##bfPreviewOutline", &previewOutline, outlines, 3);
    ImGui::SameLine();
    static const char* const neighbors[] = {"4", "8"};
    int n = previewNeighbors == 4 ? 0 : 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3);
    if (ImGui::Combo("Neighbors##bfPreviewNeighbors", &n, neighbors, 2)) previewNeighbors = n == 0 ? 4 : 8;
    ImGui::Checkbox("Shadow##bfPreviewShadow", &previewShadow);
    if (previewShadow){
        ImGui::SameLine();
        int offset[2] = {previewShadowX, previewShadowY};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        if (ImGui::InputInt2("##bfPreviewShadowOffset", offset)){
            previewShadowX = std::clamp(offset[0], -8, 8);
            previewShadowY = std::clamp(offset[1], -8, 8);
        }
    }
    if (!font)
        return;
    const std::string key = previewText + "|" + std::to_string(previewScale) + "|" + std::to_string(previewBackdrop) + "|" +
                            std::to_string(previewOutline) + "|" + std::to_string(previewNeighbors) + "|" + std::to_string(previewShadow) + "|" +
                            std::to_string(previewShadowX) + "|" + std::to_string(previewShadowY) + "|" + std::to_string(font->getSerial());
    if (key != previewKey){
        bfi::BitmapTextStyle style;
        style.outlineWidth = previewOutline;
        style.outlineNeighbors = previewNeighbors;
        style.outlineColor = bfi::rgbaColor(0, 0, 0);
        style.shadow = previewShadow;
        style.shadowX = previewShadowX;
        style.shadowY = previewShadowY;
        style.shadowColor = bfi::rgbaColor(0, 0, 0, 160);
        int w = 0, h = 0;
        bfi::BitmapText::measure(*font, previewText, w, h);
        const int pad = (previewOutline + std::max(std::abs(previewShadowX), std::abs(previewShadowY)) + 1) * previewScale + 4;
        bfi::RasterImage image(std::max(w, 1) * previewScale + 2 * pad, std::max(h, 1) * previewScale + 2 * pad);
        bfiBackdrop(image, previewBackdrop);
        const uint32_t ink = previewBackdrop == 2 ? bfi::rgbaColor(20, 20, 20) : bfi::rgbaColor(255, 255, 255);
        bfi::rasterStyledBitmapText(*font, previewText, previewScale, ink, style, image, pad, pad);
        bfiUpload(previewTexture, "editor-bitmapfont-preview-" + std::to_string(++textureSerial), image);
        previewKey = key;
        previewWidth = image.width;
        previewHeight = image.height;
    }
    ImGui::BeginChild("##bfPreviewImage", ImVec2(0, std::min((float)previewHeight + ImGui::GetStyle().ScrollbarSize + 4, ImGui::GetFontSize() * 16)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    bfiImage(previewTexture, (float)previewWidth, (float)previewHeight);
    UIProbe::item("bitmapFont.preview");
    ImGui::EndChild();
}

void BitmapFontInspector::drawReport(){
    if (!font){
        ImGui::TextDisabled("The font does not load (see the errors above).");
        return;
    }
    ImGui::Text("%d glyphs, %d cells used, %d empty", report.glyphs, report.cellsUsed, report.cellsEmpty);
    ImGui::Text("Native size %d px, baseline %d, line height %d%s", report.nativeSize, report.baseline, report.lineHeight,
                report.colored ? ", coloured" : ", monochrome");
    ImGui::Text("Partial alpha: %.2f %%", report.partialAlphaShare * 100.0f);
    for (const std::string& detection : report.detections) ImGui::TextDisabled("Detected: %s", detection.c_str());
    if (report.partialAlphaShare > 0.0f)
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), ICON_FA_TRIANGLE_EXCLAMATION " Partial alpha: set Alpha Cutoff (pixel art has opaque texels)");
    if (!report.missingAscii.empty()){
        std::string missing;
        for (uint32_t c : report.missingAscii) missing += bfiUtf8(c);
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), ICON_FA_TRIANGLE_EXCLAMATION " Missing ASCII: %s", missing.c_str());
    }
    // empty glyphs: a click selects the cell
    const std::vector<uint32_t> chars = cellChars();
    for (uint32_t c : report.emptyGlyphs){
        const std::string label = ICON_FA_TRIANGLE_EXCLAMATION " Empty glyph '" + bfiUtf8(c) + "' (U+" + std::to_string(c) + ")##bfEmpty" + std::to_string(c);
        if (ImGui::Selectable(label.c_str())){
            for (size_t i = 0; i < chars.size(); i++) if (chars[i] == c) selectedCell = (int)i;
            selectedGlyph = c;
        }
    }
    for (const bfi::FontError& w : report.warnings){
        const std::string label = std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + w.message + "##bfWarn" + w.field;
        if (ImGui::Selectable(label.c_str()) && w.field.rfind("glyphs.", 0) == 0){
            const uint32_t c = bfiFirst(w.field.substr(7));
            for (size_t i = 0; i < chars.size(); i++) if (chars[i] == c) selectedCell = (int)i;
            selectedGlyph = c;
        }
    }
    if (report.overridesApplied > 0) ImGui::TextDisabled("%d overrides applied", report.overridesApplied);
}

bool BitmapFontInspector::drawFontSheetBanner(Project* project, const fs::path& image){
    int w = 0, h = 0;
    if (!editor::pixelart::looksLikeFontSheet(image.filename().string(), w, h))
        return false;
    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), ICON_FA_FONT " This looks like a font sheet (%dx%d cells):", w, h);
    ImGui::SameLine();
    if (ImGui::SmallButton("Create Bitmap Font##bfBanner")){
        std::string error;
        if (editor::pixelart::createBitmapFont(project, image, error).empty())
            ImGui::SetTooltip("%s", error.c_str());
    }
    UIProbe::item("bitmapFont.banner");
    return true;
}
