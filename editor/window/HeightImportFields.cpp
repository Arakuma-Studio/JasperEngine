// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HeightImportFields.h"

#include "Backend.h"
#include "external/IconsFontAwesome6.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"
#include "util/FileUtils.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

using namespace jasperengine;

namespace {

    // The preview's texture: one at a time, the one it replaces destroyed a frame later (ImGui may still draw it)
    struct HillshadeTexture {
        Texture texture;
        Texture retired;
        const float* source = nullptr;
        size_t count = 0;
        int serial = 0;
    };

    HillshadeTexture& hillshadeTexture() {
        static HillshadeTexture preview;
        return preview;
    }

}

std::vector<uint8_t> editor::heightHillshade(const std::vector<float>& normalized, int width, int height, int size) {
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4, 255);
    if (width < 2 || height < 2 || normalized.size() < static_cast<size_t>(width) * height) return rgba;
    auto at = [&](int x, int y) {
        return normalized[static_cast<size_t>(std::clamp(y, 0, height - 1)) * width + std::clamp(x, 0, width - 1)];
    };
    // relief as if the terrain were a quarter as high as it is wide; the light from the top left, 45 degrees up
    const float relief = 0.25f * static_cast<float>(std::max(width, height));
    const float lx = -0.5f, ly = -0.5f, lz = 0.70710678f;
    for (int v = 0; v < size; v++) {
        const int y = static_cast<int>((static_cast<float>(v) + 0.5f) * static_cast<float>(height) / static_cast<float>(size));
        for (int u = 0; u < size; u++) {
            const int x = static_cast<int>((static_cast<float>(u) + 0.5f) * static_cast<float>(width) / static_cast<float>(size));
            const float dx = (at(x + 1, y) - at(x - 1, y)) * 0.5f * relief;
            const float dy = (at(x, y + 1) - at(x, y - 1)) * 0.5f * relief;
            const float length = std::sqrt(dx * dx + dy * dy + 1.0f);
            const float shade = std::clamp((-dx * lx - dy * ly + lz) / length, 0.0f, 1.0f);
            const uint8_t value = static_cast<uint8_t>(std::lround(shade * 255.0f));
            const size_t index = (static_cast<size_t>(v) * size + u) * 4;
            rgba[index] = rgba[index + 1] = rgba[index + 2] = value;
        }
    }
    return rgba;
}

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
        // R61: a 256 x 256 hillshade of what the import would make, remade when the decode changes
        constexpr int SIDE = 256;
        HillshadeTexture& shade = hillshadeTexture();
        if (!shade.retired.empty()) {
            shade.retired.destroy();
            shade.retired = Texture();
        }
        if (shade.texture.empty() || shade.source != p.normalized.data() || shade.count != p.normalized.size()) {
            shade.retired = shade.texture;
            shade.texture = Texture();
            shade.source = p.normalized.data();
            shade.count = p.normalized.size();
            const std::vector<uint8_t> rgba = heightHillshade(p.normalized, p.width, p.height, SIDE);
            // TextureData frees what it is given
            if (void* pixels = std::malloc(rgba.size())) {
                std::memcpy(pixels, rgba.data(), rgba.size());
                TextureData data(SIDE, SIDE, static_cast<unsigned int>(rgba.size()), ColorFormat::RGBA, 4, pixels);
                shade.texture.setData("editor:height-import-hillshade:" + std::to_string(++shade.serial), data);
                shade.texture.setMinFilter(TextureFilter::LINEAR);
                shade.texture.setMagFilter(TextureFilter::LINEAR);
                shade.texture.getRender();
                shade.texture.releaseData();
            }
        }
        if (TextureRender* render = shade.texture.empty() ? nullptr : shade.texture.getRender(); render && render->isCreated()) {
            const float side = ImGui::GetFontSize() * 12.0f;
            ImGui::Image(ImTextureRef(Backend::getImGuiTexture(render)), ImVec2(side, side));
        }
    }else{
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", p.error.c_str());
        ImGui::PopStyleColor();
    }
    return changed;
}
