// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HeightImportFields.h"

#include "external/IconsFontAwesome6.h"
#include "util/FileUtils.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>

using namespace jasperengine;

bool editor::drawHeightImportFields(const std::string& path, const std::vector<uint8_t>& bytes, HeightImportOptions& options,
                                    HeightImport& preview, float maxHeight, bool* setHeightFromRange){
    bool changed = false;
    const HeightImport& p = preview;
    // a guessed value is marked; editing it makes it given
    auto guessed = [](bool value){
        if (!value) return;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.2f, 1.0f), ICON_FA_CIRCLE_QUESTION);
        ImGui::SetItemTooltip("Detected from the file");
    };
    ImGui::TextUnformatted(FileUtils::pathToUtf8(std::filesystem::u8path(path).filename()).c_str());

    int depth = static_cast<int>(options.format);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (EditorModal::combo("Depth", "depth", &depth, {"Detect", "Bit 8", "Bit 16", "Float 32", "PNG", "EXR"})){
        options.format = static_cast<HeightFileFormat>(depth);
        changed = true;
    }
    guessed(p.ok && p.formatGuessed);
    int width = options.width > 0 ? options.width : p.width;
    int height = options.height > 0 ? options.height : p.height;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (EditorModal::inputInt("Width", "width", &width, 0, 0)){
        options.width = std::max(0, width);
        changed = true;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (EditorModal::inputInt("Height", "height", &height, 0, 0)){
        options.height = std::max(0, height);
        changed = true;
    }
    guessed(p.ok && p.sizeGuessed);
    int order = static_cast<int>(options.byteOrder);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (EditorModal::combo("Byte Order", "byte_order", &order, {"Detect", "Windows", "Mac"})){
        options.byteOrder = static_cast<HeightByteOrder>(order);
        changed = true;
    }
    guessed(p.ok && p.byteOrderGuessed);
    changed = EditorModal::checkbox("Flip Vertically", "flip_vertically", &options.flipVertically) || changed;
    int range = static_cast<int>(options.range);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (EditorModal::combo("Value Range", "range", &range, {"Detected", "Full", "Source Min/Max"})){
        options.range = static_cast<HeightRange>(range);
        changed = true;
    }
    if (options.range == HeightRange::Source){
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        changed = EditorModal::inputFloat("Source Min", "source_min", &options.sourceMin) || changed;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        changed = EditorModal::inputFloat("Source Max", "source_max", &options.sourceMax) || changed;
        if (setHeightFromRange) EditorModal::checkbox("Set Terrain Height From Range", "set_height_from_range", setHeightFromRange);
    }
    if (changed){
        preview = TerrainRawIO::decode(path, bytes, options);
    }

    if (p.ok){
        double sum = 0.0;
        for (float v : p.normalized) sum += v;
        const double mean = p.normalized.empty() ? 0.0 : sum / static_cast<double>(p.normalized.size());
        const double clipped = p.normalized.empty() ? 0.0 : static_cast<double>(p.clippedLow + p.clippedHigh) / static_cast<double>(p.normalized.size());
        ImGui::Text("%s %d x %d. Data %.4g to %.4g; mapped %.4g to %.4g.", TerrainRawIO::formatName(p.format), p.width, p.height,
                    p.dataMin, p.dataMax, p.rangeLow, p.rangeHigh);
        ImGui::Text("Mean %.1f%% of Terrain Height (%.2f m); %.2f%% clipped.", mean * 100.0, mean * maxHeight, clipped * 100.0);
        // a 64-bin histogram of the mapped heights
        float bins[64] = {};
        for (float v : p.normalized) bins[std::min(63, static_cast<int>(v * 64.0f))] += 1.0f;
        ImGui::PlotHistogram("##import_histogram", bins, 64, 0, nullptr, 0.0f, FLT_MAX, ImVec2(ImGui::GetFontSize() * 20.0f, ImGui::GetFontSize() * 4.0f));
    }else{
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", p.error.c_str());
        ImGui::PopStyleColor();
    }
    return changed;
}
