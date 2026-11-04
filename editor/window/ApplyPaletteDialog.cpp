// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ApplyPaletteDialog.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/BuiltinPalettes.h"
#include "pixelart/PixelColor.h"
#include "shortcuts/CommandCatalog.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;
namespace pxc = jasperengine::pixelart;

// file-local, in a namespace of its own: unity builds merge the editor's sources
namespace jasperengine::editor::applypalettedialog {
namespace {

    const char* const kMetrics[] = {"OKLab", "CIEDE2000", "RGB"};
    const char* const kDithers[] = {"None", "Bayer 2x2", "Bayer 4x4", "Bayer 8x8", "Floyd-Steinberg"};

    pxc::ColorMetric metricOf(int index) {
        return index == 1 ? pxc::ColorMetric::Ciede2000 : index == 2 ? pxc::ColorMetric::Rgb : pxc::ColorMetric::OkLab;
    }

    pixelart::Dither ditherOf(int index) {
        switch (index) {
            case 1: return pixelart::Dither::Bayer2;
            case 2: return pixelart::Dither::Bayer4;
            case 3: return pixelart::Dither::Bayer8;
            case 4: return pixelart::Dither::FloydSteinberg;
            default: return pixelart::Dither::None;
        }
    }

    std::string lowerExtension(const fs::path& path) {
        std::string extension = path.extension().string();
        for (char& c : extension) c = (char)std::tolower((unsigned char)c);
        return extension;
    }

    // what the texture is now: a write (Apply, an undo) changes it
    std::string fileKey(const fs::path& path) {
        std::error_code ec;
        const auto time = fs::last_write_time(path, ec);
        if (ec) return FileUtils::pathToGenericUtf8(path) + "|missing";
        const uintmax_t size = fs::file_size(path, ec);
        return FileUtils::pathToGenericUtf8(path) + "|" + std::to_string((long long)time.time_since_epoch().count()) + "|" +
               std::to_string(ec ? 0 : size);
    }

    void drawChecker(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
        const float cell = std::max(4.0f, Theme::dpi(8.0f));
        drawList->AddRectFilled(min, max, IM_COL32(200, 200, 200, 255));
        drawList->PushClipRect(min, max, true);
        for (float y = min.y; y < max.y; y += cell) {
            for (float x = min.x; x < max.x; x += cell) {
                if (((int)((x - min.x) / cell) + (int)((y - min.y) / cell)) % 2 == 0) {
                    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + cell, y + cell), IM_COL32(150, 150, 150, 255));
                }
            }
        }
        drawList->PopClipRect();
    }

}
}

namespace apd = jasperengine::editor::applypalettedialog;

editor::ApplyPaletteDialog::ApplyPaletteDialog(Project* project): project(project) {
}

bool editor::ApplyPaletteDialog::isOpen() const {
    return windowOpen;
}

bool editor::ApplyPaletteDialog::isFocused() const {
    return windowOpen && focused;
}

bool editor::ApplyPaletteDialog::takesKeys() const {
    return isFocused() && !ImGui::GetIO().WantTextInput;
}

void editor::ApplyPaletteDialog::setNotice(const std::string& text, bool error) {
    notice = text;
    noticeIsError = error;
}

void editor::ApplyPaletteDialog::resetForProjectSwitch() {
    windowOpen = false;
    texture.clear();
    paletteName.clear();
    resolved = {};
    palettes.clear();
    palettesListed = false;
    sourceKey.clear();
    clearPictures();
    notice.clear();
    flicker = false;
    hoveredLost = -1;
}

void editor::ApplyPaletteDialog::bindCommands(CommandCatalog& catalog) {
    const CommandDef* existing = catalog.find("palette.flickerCompare");
    if (!existing) return;
    CommandDef def = *existing;
    def.run = [this](const CommandArgs&) { flicker = !flicker; };
    def.enabled = [this]() { return windowOpen && result != nullptr; };
    def.keyGate = [this]() { return takesKeys() && !ImGui::IsAnyItemActive(); };
    catalog.add(std::move(def));
}

void editor::ApplyPaletteDialog::resolvePalette(const std::string& name) {
    paletteName = name;
    resolved = {};
    if (!project) return;
    resolved = pixelart::resolvePaletteName(*project, name);
    if (!resolved.ok && name.empty()) {
        // no Default Palette: the project's first palette
        if (!palettesListed) {
            palettes = pixelart::listProjectPalettes(*project);
            palettesListed = true;
        }
        if (!palettes.empty()) {
            paletteName = palettes.front().path;
            resolved = pixelart::resolvePaletteName(*project, paletteName);
        }
    }
    if (!resolved.ok) {
        // Reduce Colours needs no palette: it only keeps a chosen palette's locked colours
        if (!reduce) setNotice(resolved.error.message + ": " + resolved.error.fix, true);
        paletteName.clear();
    } else if (noticeIsError) {
        notice.clear();
    }
}

void editor::ApplyPaletteDialog::open(const fs::path& requested, const std::string& palette, bool reduceColours) {
    if (!project) return;
    fs::path wanted = requested.is_relative() ? project->getProjectPath() / requested : requested;
    wanted = wanted.lexically_normal();
    if (wanted != texture) clearPictures();
    texture = wanted;
    reduce = reduceColours;
    notice.clear();
    flicker = false;
    hoveredLost = -1;
    palettesListed = false;
    resolvePalette(palette);
    windowOpen = true;
    focusRequested = true;
}

jasperengine::editor::pixelart::ApplyRequest editor::ApplyPaletteDialog::request() const {
    pixelart::ApplyRequest wanted;
    if (resolved.ok) {
        wanted.palette = resolved.palette.colors;
        wanted.locked = resolved.palette.locked;
    }
    wanted.metric = apd::metricOf(metric);
    wanted.dither = apd::ditherOf(dither);
    wanted.ditherStrength = std::clamp(ditherStrength, 0.0f, 1.0f);
    wanted.alphaThreshold = binaryAlpha ? std::clamp(alphaThreshold, 1, 255) : 0;
    wanted.reduce = reduce ? std::clamp(reduceColors, 2, pxc::Palette::kMaxColors) : 0;
    return wanted;
}

std::string editor::ApplyPaletteDialog::requestKey() const {
    const pixelart::ApplyRequest wanted = request();
    std::string key = sourceShownKey + "|" + std::to_string(metric) + "|" + std::to_string(dither) + "|" + std::to_string(wanted.ditherStrength) + "|" +
                      std::to_string(wanted.alphaThreshold) + "|" + std::to_string(wanted.reduce) + "|";
    for (const pxc::Rgba8& color : wanted.palette) key += pxc::toHex(color, true);
    key += "|";
    for (uint16_t index : wanted.locked) key += std::to_string(index) + ",";
    return key;
}

bool editor::ApplyPaletteDialog::resultCurrent() const {
    return result && shownKey == requestKey();
}

void editor::ApplyPaletteDialog::update() {
    // the texture's pixels: again whenever the file changes
    sourceKey = apd::fileKey(texture);
    if (sourceTask.isValid() && sourceTask.isReady()) {
        std::shared_ptr<Source> loaded;
        try {
            loaded = sourceTask.get();
        } catch (const std::exception& e) {
            loaded = std::make_shared<Source>();
            loaded->error = e.what();
        }
        sourceTask = {};
        source = loaded;
        sourceShownKey = sourceRunningKey;
        if (source && source->error.empty()) before.upload(source->image, retired);
        else before.retire(retired);
    }
    if (!sourceTask.isValid() && sourceKey != sourceShownKey) {
        const fs::path path = texture;
        sourceRunningKey = sourceKey;
        sourceTask = jobs::submit(
            [path]() {
                auto out = std::make_shared<Source>();
                std::string error;
                std::optional<pixelart::RgbaImage> image = pixelart::loadRgbaImage(path, &error);
                if (image) out->image = std::move(*image);
                else out->error = error.empty() ? "Not an image." : error;
                return out;
            },
            {jobs::Lane::Background, jobs::GateClass::Default, "apply-palette-source"});
    }

    // the mapping: a new request once the last one is done (a slider drag runs the latest)
    if (applyTask.isValid() && applyTask.isReady()) {
        std::shared_ptr<pixelart::ApplyResult> mapped;
        try {
            mapped = applyTask.get();
        } catch (const std::exception& e) {
            setNotice(std::string("Apply Palette failed: ") + e.what(), true);
        }
        applyTask = {};
        if (mapped) {
            result = mapped;
            shownKey = runningKey;
            hoveredLost = -1;
            after.upload(result->image, retired);
        }
    }
    if (applyTask.isValid() || !source || !source->error.empty() || sourceShownKey.empty()) return;
    const pixelart::ApplyRequest wanted = request();
    if (wanted.palette.empty() && wanted.reduce == 0) return;
    const std::string key = requestKey();
    if (key == shownKey) return;
    const std::shared_ptr<const Source> pixels = source;
    runningKey = key;
    applyTask = jobs::submit(
        [pixels, wanted]() { return std::make_shared<pixelart::ApplyResult>(pixelart::applyPalette(pixels->image, wanted)); },
        {jobs::Lane::Background, jobs::GateClass::Default, "apply-palette"});
}

// ---------------------------------------------------------------------------------- show

void editor::ApplyPaletteDialog::show() {
    for (Texture& old : retired) old.destroy();
    retired.clear();
    if (!windowOpen) {
        focused = false;
        return;
    }
    update();
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(760.0f), Theme::dpi(620.0f)), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    bool keepOpen = true;
    const std::string title = std::string(reduce ? "Reduce Colours" : WINDOW_NAME) + "###ApplyPaletteDialog";
    if (ImGui::Begin(title.c_str(), &keepOpen, ImGuiWindowFlags_NoCollapse)) {
        focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        ImGui::TextDisabled("Texture: %s", FileUtils::pathToGenericUtf8(project ? project->normalizeToProjectRelative(texture) : texture).c_str());
        drawSettings();
        if (!notice.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, noticeIsError ? Theme::Colors::ErrorText : Theme::Colors::WarningText);
            ImGui::TextWrapped("%s", notice.c_str());
            ImGui::PopStyleColor();
        }
        if (source && !source->error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
            ImGui::TextWrapped("%s", source->error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        drawButtons();
        drawStatistics();
        const float lostWidth = Theme::dpi(220.0f);
        if (ImGui::BeginTable("##apply_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Lost Colours", ImGuiTableColumnFlags_WidthFixed, lostWidth);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            drawPreview();
            ImGui::TableSetColumnIndex(1);
            drawLostColors();
            ImGui::EndTable();
        }
    } else {
        focused = false;
    }
    ImGui::End();
    if (!keepOpen || !windowOpen) {
        windowOpen = false;
        focused = false;
        // the pictures go with the window; a later open reads the texture again
        clearPictures();
    }
}

void editor::ApplyPaletteDialog::clearPictures() {
    // a running job still finishes: its handle goes, its result is never read
    applyTask = {};
    sourceTask = {};
    result.reset();
    source.reset();
    runningKey.clear();
    shownKey.clear();
    sourceRunningKey.clear();
    sourceShownKey.clear();
    hoveredLost = -1;
    before.retire(retired);
    after.retire(retired);
}

void editor::ApplyPaletteDialog::drawSettings() {
    if (ImGui::RadioButton("Apply Palette##apply_mode", !reduce)) {
        reduce = false;
        if (!resolved.ok) resolvePalette(paletteName);
    }
    ImGui::SetItemTooltip("Every pixel goes to its nearest palette colour");
    ImGui::SameLine();
    if (ImGui::RadioButton("Reduce Colours##reduce_mode", reduce)) reduce = true;
    ImGui::SetItemTooltip("Find the best N colours for this texture (median cut in OKLab, then k-means), then apply them");

    const float labelWidth = Theme::dpi(130.0f);
    auto label = [labelWidth](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    };

    label(reduce ? "Keep Locked Colours" : "Palette");
    std::string preview = resolved.ok ? (resolved.palette.name.empty() ? resolved.path : resolved.palette.name) : std::string(reduce ? "(None)" : "(Choose a Palette)");
    if (ImGui::BeginCombo("##apply_palette_choice", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing() && project) {
            palettes = pixelart::listProjectPalettes(*project);
            palettesListed = true;
        }
        if (reduce && ImGui::Selectable("(None)##apply_palette_none", !resolved.ok)) {
            resolved = {};
            paletteName.clear();
        }
        ImGui::TextDisabled("Project");
        for (size_t i = 0; i < palettes.size(); i++) {
            const std::string text = (palettes[i].palette.name.empty() ? palettes[i].path : palettes[i].palette.name) + "##apply_project_" + std::to_string(i);
            if (ImGui::Selectable(text.c_str(), resolved.ok && resolved.path == palettes[i].path)) resolvePalette(palettes[i].path);
            ImGui::SetItemTooltip("%s, %d colours", palettes[i].path.c_str(), palettes[i].palette.count());
        }
        ImGui::Separator();
        ImGui::TextDisabled("Built-in");
        for (const pixelart::BuiltinPalette& builtin : pixelart::builtinPalettes()) {
            const std::string text = builtin.palette.name + "##apply_builtin_" + builtin.id;
            if (ImGui::Selectable(text.c_str(), paletteName == std::string(pixelart::kBuiltinPrefix) + builtin.id)) {
                resolvePalette(std::string(pixelart::kBuiltinPrefix) + builtin.id);
            }
        }
        ImGui::EndCombo();
    }
    if (reduce) {
        label("Colours");
        ImGui::SliderInt("##reduce_colors", &reduceColors, 2, pxc::Palette::kMaxColors, "%d", ImGuiSliderFlags_AlwaysClamp);
    }
    label("Distance");
    ImGui::Combo("##apply_metric", &metric, apd::kMetrics, IM_ARRAYSIZE(apd::kMetrics));
    ImGui::SetItemTooltip("How colour difference is measured: OKLab (the default), CIEDE2000 (slower), or plain RGB");
    label("Dither");
    ImGui::Combo("##apply_dither", &dither, apd::kDithers, IM_ARRAYSIZE(apd::kDithers));
    if (dither != 0) {
        label("Strength");
        ImGui::SliderFloat("##apply_dither_strength", &ditherStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    }
    label("Alpha");
    int alphaMode = binaryAlpha ? 1 : 0;
    if (ImGui::Combo("##apply_alpha", &alphaMode, "Keep\0Binary\0")) binaryAlpha = alphaMode == 1;
    ImGui::SetItemTooltip("Keep: alpha as it is. Binary: under the threshold fully transparent, else opaque (the pixel-art default)");
    if (binaryAlpha) {
        label("Threshold");
        ImGui::SliderInt("##apply_alpha_threshold", &alphaThreshold, 1, 255, "%d", ImGuiSliderFlags_AlwaysClamp);
    }
}

void editor::ApplyPaletteDialog::drawButtons() {
    const bool ready = resultCurrent() && !applyTask.isValid();
    const bool png = apd::lowerExtension(texture) == ".png";
    ImGui::BeginDisabled(!ready || !png);
    if (ImGui::Button("Apply##apply_write")) {
        if (writeResult(texture, reduce ? "Reduce Colours" : "Apply Palette")) {
            Out::info("%s: %s.", reduce ? "Reduce Colours" : "Apply Palette", FileUtils::pathToUtf8(texture.filename()).c_str());
            windowOpen = false;
        }
    }
    ImGui::EndDisabled();
    if (!png) ImGui::SetItemTooltip("Only a PNG is rewritten in place: Save As writes the result as a new PNG");
    else ImGui::SetItemTooltip("Rewrite the texture (one undo step; its .meta and GUID stay)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Save As...##apply_save_as")) {
        const std::string chosen = FileDialogs::saveFileDialog(FileUtils::pathToUtf8(texture.parent_path()),
                                                               FileUtils::pathToUtf8(texture.stem()) + (reduce ? " Reduced.png" : " Palette.png"),
                                                               "PNG image", "png");
        if (!chosen.empty()) {
            fs::path target = FileUtils::pathFromUtf8(chosen);
            if (apd::lowerExtension(target) != ".png") target += ".png";
            if (writeResult(target, reduce ? "Reduce Colours" : "Apply Palette")) windowOpen = false;
        }
    }
    if (reduce) {
        ImGui::SameLine();
        if (ImGui::Button("Save Palette As...##apply_save_palette")) savePaletteAs();
        ImGui::SetItemTooltip("Write the colours Reduce Colours found as a .palette");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Flicker##apply_flicker", &flicker);
    ImGui::SetItemTooltip("The After half shows Before (%s toggles it)", CommandCatalog::editor().menuShortcutText("palette.flickerCompare").c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(120.0f));
    ImGui::SliderInt("Zoom##apply_zoom", &zoom, 0, 16, zoom == 0 ? "Fit" : "x%d", ImGuiSliderFlags_AlwaysClamp);
    if (applyTask.isValid() || (source && !resultCurrent() && source->error.empty())) {
        ImGui::SameLine();
        const char* spinner = "|/-\\";
        ImGui::Text("%c Mapping...", spinner[(int)(ImGui::GetTime() * 8.0) % 4]);
    }
}

void editor::ApplyPaletteDialog::drawStatistics() {
    if (!result) {
        if (source) ImGui::TextDisabled("Choose a palette (or Reduce Colours) to see the result.");
        else ImGui::TextDisabled("Reading the texture...");
        return;
    }
    const pixelart::ApplyStats& stats = result->stats;
    const int64_t pixels = source ? (int64_t)source->image.pixelCount() : 0;
    ImGui::Text("Colours: %d before, %d after   Pixels Changed: %lld (%.1f%%)   dE (OKLab x100): mean %.2f, max %.2f", stats.colorsBefore,
                stats.colorsAfter, (long long)stats.pixelsChanged, pixels > 0 ? 100.0 * (double)stats.pixelsChanged / (double)pixels : 0.0,
                stats.meanDeltaE, stats.maxDeltaE);
}

void editor::ApplyPaletteDialog::drawPreview() {
    if (!before.valid()) {
        ImGui::TextDisabled("No preview yet.");
        return;
    }
    if (!ImGui::BeginChild("##apply_preview", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }
    const float gap = Theme::dpi(8.0f);
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const float half = std::max(Theme::dpi(32.0f), (area.x - gap) * 0.5f);
    const float available = std::max(Theme::dpi(32.0f), area.y - ImGui::GetTextLineHeightWithSpacing());
    // integer zoom, or an integer reduction when the texture is larger than the half
    float scale = (float)zoom;
    if (zoom == 0) {
        const float fit = std::min(half / (float)before.width, available / (float)before.height);
        scale = fit >= 1.0f ? std::floor(fit) : 1.0f / std::ceil(1.0f / fit);
    }
    const ImVec2 size((float)before.width * scale, (float)before.height * scale);
    const bool showAfter = after.valid() && resultCurrent() && !flicker;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float captionHeight = ImGui::GetTextLineHeightWithSpacing();
    ImGui::Dummy(ImVec2(size.x * 2.0f + gap, size.y + captionHeight));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    drawList->AddText(top, text, "Before");
    drawList->AddText(ImVec2(top.x + size.x + gap, top.y), text, flicker ? "After (showing Before)" : "After");
    const ImVec2 origin(top.x, top.y + captionHeight);
    const ImVec2 leftMin = origin;
    const ImVec2 leftMax(origin.x + size.x, origin.y + size.y);
    const ImVec2 rightMin(origin.x + size.x + gap, origin.y);
    const ImVec2 rightMax(rightMin.x + size.x, rightMin.y + size.y);
    apd::drawChecker(drawList, leftMin, leftMax);
    apd::drawChecker(drawList, rightMin, rightMax);
    drawList->AddImage(before.imguiId(), leftMin, leftMax);
    drawList->AddImage(showAfter ? after.imguiId() : before.imguiId(), rightMin, rightMax);
    drawList->AddRect(leftMin, leftMax, ImGui::GetColorU32(ImGuiCol_Border));
    drawList->AddRect(rightMin, rightMax, ImGui::GetColorU32(ImGuiCol_Border));
    if (result && hoveredLost >= 0 && hoveredLost < (int)result->lost.size()) {
        const pixelart::LostGroup& group = result->lost[hoveredLost];
        for (const ImVec2& at : {leftMin, rightMin}) {
            const ImVec2 min(at.x + (float)group.x * scale - 1.0f, at.y + (float)group.y * scale - 1.0f);
            const ImVec2 max(at.x + (float)(group.x + group.w) * scale + 1.0f, at.y + (float)(group.y + group.h) * scale + 1.0f);
            drawList->AddRect(min, max, IM_COL32(255, 0, 255, 255), 0.0f, 0, Theme::dpi(2.0f));
        }
    }
    ImGui::EndChild();
}

void editor::ApplyPaletteDialog::drawLostColors() {
    ImGui::TextUnformatted("Lost Colours");
    ImGui::SetItemTooltip("Groups of pixels that ended far (over 0.06 OKLab) from their colour; hover one to outline it");
    if (!result) return;
    if (result->lost.empty()) {
        ImGui::TextDisabled("None: every pixel stayed close.");
        return;
    }
    if (!ImGui::BeginChild("##apply_lost", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
        ImGui::EndChild();
        return;
    }
    int hovered = -1;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float cell = ImGui::GetTextLineHeight();
    for (int i = 0; i < (int)result->lost.size(); i++) {
        const pixelart::LostGroup& group = result->lost[i];
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cell * 2.0f + 2.0f, cell));
        drawList->AddRectFilled(min, ImVec2(min.x + cell, min.y + cell), IM_COL32(group.original.r, group.original.g, group.original.b, 255));
        drawList->AddRectFilled(ImVec2(min.x + cell + 2.0f, min.y), ImVec2(min.x + cell * 2.0f + 2.0f, min.y + cell),
                                IM_COL32(group.mapped.r, group.mapped.g, group.mapped.b, 255));
        ImGui::SameLine();
        char row[128];
        std::snprintf(row, sizeof(row), "%d px at %d, %d (%d x %d)##lost_%d", group.count, group.x, group.y, group.w, group.h, i);
        ImGui::Selectable(row, hoveredLost == i);
        if (ImGui::IsItemHovered()) {
            hovered = i;
            ImGui::SetTooltip("%s became %s", pxc::toHex(group.original).c_str(), pxc::toHex(group.mapped).c_str());
        }
    }
    hoveredLost = hovered;
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------- writes

bool editor::ApplyPaletteDialog::writeResult(const fs::path& target, const std::string& what) {
    if (!project || !result) return false;
    const std::string png = pixelart::encodeRgbaPng(result->image);
    if (png.empty()) {
        setNotice("The result could not be encoded as a PNG.", true);
        return false;
    }
    std::vector<WriteFilesCmd::Item> items;
    // R7: a built-in used here is copied into the project in the same step
    if (!reduce && resolved.ok && resolved.needsCopy) items.push_back({project->getProjectPath() / FileUtils::pathFromUtf8(resolved.path), resolved.copyText});
    items.push_back({target, png});
    std::string error;
    if (!PaletteWindow::writeFiles(project, std::move(items), what + " (" + FileUtils::pathToUtf8(target.filename()) + ")", "", error)) {
        setNotice(error, true);
        Out::error("%s: %s", what.c_str(), error.c_str());
        return false;
    }
    if (!reduce && resolved.ok && resolved.needsCopy) resolvePalette(paletteName);
    return true;
}

void editor::ApplyPaletteDialog::savePaletteAs() {
    if (!project || !result || result->palette.empty()) return;
    const fs::path folder = texture.parent_path();
    const std::string chosen = FileDialogs::saveFileDialog(FileUtils::pathToUtf8(folder), FileUtils::pathToUtf8(texture.stem()) + PaletteWindow::EXTENSION,
                                                           "Palette", "palette");
    if (chosen.empty()) return;
    fs::path target = FileUtils::pathFromUtf8(chosen);
    if (apd::lowerExtension(target) != PaletteWindow::EXTENSION) target += PaletteWindow::EXTENSION;
    pxc::Palette found;
    found.name = FileUtils::pathToUtf8(texture.stem());
    found.source.kind = "texture";
    found.source.file = FileUtils::pathToUtf8(texture.filename());
    found.colors = result->palette;
    std::string error;
    if (!PaletteWindow::writeFiles(project, {{target, pxc::PaletteCodec::write(found)}}, "Save Palette " + FileUtils::pathToUtf8(target.filename()), "",
                                   error)) {
        setNotice(error, true);
        return;
    }
    setNotice("Saved " + FileUtils::pathToUtf8(target.filename()) + ".", false);
}
