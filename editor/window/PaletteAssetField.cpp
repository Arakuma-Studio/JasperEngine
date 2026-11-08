// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteAssetField.h"

#include "Project.h"
#include "pixelart/BuiltinPalettes.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteAssets.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>

// file-local, in a namespace of its own: a unity build merges the editor's sources
namespace jasperengine::editor::paletteassetfield {
namespace {

    namespace pxc = jasperengine::pixelart;

    // the project's palettes as the popup lists them: scanned when it opens, not every frame
    std::vector<pixelart::ProjectPalette>& listedPalettes() {
        static std::vector<pixelart::ProjectPalette> palettes;
        return palettes;
    }

    ImU32 swatchColor(pxc::Rgba8 color) {
        return IM_COL32(color.r, color.g, color.b, color.a);
    }

    std::string hexOf(pxc::Rgba8 color) {
        char text[12];
        if (color.a == 255) std::snprintf(text, sizeof(text), "#%02X%02X%02X", color.r, color.g, color.b);
        else std::snprintf(text, sizeof(text), "#%02X%02X%02X%02X", color.r, color.g, color.b, color.a);
        return text;
    }

    bool isPaletteFile(const std::string& path) {
        std::string extension = FileUtils::pathFromUtf8(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return extension == ".palette";
    }

    std::string displayName(const pxc::Palette& palette, const std::string& path) {
        return palette.name.empty() ? FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).stem()) : palette.name;
    }

    // swatches across [min, max], `count` of `colors` (the rest is left out)
    void drawSwatches(ImDrawList* drawList, ImVec2 min, ImVec2 max, const std::vector<pxc::Rgba8>& colors, int count) {
        if (count <= 0 || max.x <= min.x) return;
        const float step = (max.x - min.x) / (float)count;
        for (int i = 0; i < count; i++) {
            const float x0 = std::floor(min.x + step * (float)i);
            const float x1 = std::max(x0 + 1.0f, std::floor(min.x + step * (float)(i + 1)));
            drawList->AddRectFilled(ImVec2(x0, min.y), ImVec2(x1, max.y), swatchColor(colors[(size_t)i]));
        }
    }

}
}

using namespace jasperengine::editor::paletteassetfield;

namespace jasperengine::editor {

bool paletteAssetField(const char* id, Project& project, const std::string& current, std::string& picked, bool allowNone, float width) {
    ImGui::PushID(id);
    bool changed = false;
    std::shared_ptr<const pxc::Palette> palette = current.empty() ? nullptr : pxc::PaletteLibrary::get(current);
    const std::string shown = current.empty() ? std::string("None")
                            : palette ? displayName(*palette, current) : "Missing (" + current + ")";

    const float fieldWidth = width > 0.0f ? width : std::max(1.0f, ImGui::GetContentRegionAvail().x + width);
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Text, current.empty() ? style.Colors[ImGuiCol_TextDisabled] : style.Colors[ImGuiCol_Text]);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    if (ImGui::Button((shown + "###palette_field").c_str(), ImVec2(fieldWidth, 0.0f))) {
        listedPalettes() = pixelart::listProjectPalettes(project);
        ImGui::OpenPopup("##palette_field_popup");
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    // a band of the palette's colours along the field's bottom edge
    if (palette && !palette->colors.empty()) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float band = std::max(2.0f, std::floor(ImGui::GetFontSize() * 0.22f));
        drawSwatches(ImGui::GetWindowDrawList(), ImVec2(min.x + style.FramePadding.x * 0.5f, max.y - band - 1.0f),
                     ImVec2(max.x - style.FramePadding.x * 0.5f, max.y - 1.0f), palette->colors,
                     std::min((int)palette->colors.size(), 256));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s%sDrop a .palette here, or click to pick a project palette or a built-in (copied into the project "
                          "on first use)", current.c_str(), current.empty() ? "" : "\n");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            for (const std::string& file : Util::getStringsFromPayload(payload)) {
                if (!isPaletteFile(file) || !project.isInsideAssetsPath(FileUtils::pathFromUtf8(file))) continue;
                picked = FileUtils::pathToGenericUtf8(project.normalizeToAssetsRelative(FileUtils::pathFromUtf8(file)));
                changed = picked != current;
                break;
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(fieldWidth, 0.0f), ImVec2(std::max(fieldWidth, ImGui::GetFontSize() * 24.0f),
                                                                         ImGui::GetFontSize() * 28.0f));
    if (ImGui::BeginPopup("##palette_field_popup")) {
        const float stripWidth = ImGui::GetFontSize() * 6.0f;
        // a strip of the entry's colours at the right end of its line
        auto entryStrip = [&](const std::vector<pxc::Rgba8>& colors) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const float inset = (max.y - min.y) * 0.25f;
            drawSwatches(ImGui::GetWindowDrawList(), ImVec2(std::max(min.x, max.x - stripWidth), min.y + inset), ImVec2(max.x, max.y - inset),
                         colors, std::min((int)colors.size(), 32));
        };
        if (allowNone && ImGui::Selectable("None##palette_none", current.empty())) {
            picked.clear();
            changed = !current.empty();
        }
        ImGui::SeparatorText("Project");
        if (listedPalettes().empty())
            ImGui::TextDisabled("No .palette in the project yet");
        for (const pixelart::ProjectPalette& entry : listedPalettes()) {
            const std::string label = displayName(entry.palette, entry.assetPath) + "##" + entry.assetPath;
            if (ImGui::Selectable(label.c_str(), entry.assetPath == current, 0, ImVec2(0.0f, 0.0f))) {
                picked = entry.assetPath;
                changed = picked != current;
            }
            entryStrip(entry.palette.colors);
            ImGui::SetItemTooltip("%s (%d colours)", entry.assetPath.c_str(), entry.palette.count());
        }
        ImGui::SeparatorText("Built-in");
        for (const pixelart::BuiltinPalette& builtin : pixelart::builtinPalettes()) {
            const std::string label = displayName(builtin.palette, builtin.id) + "##" + pixelart::kBuiltinPrefix + builtin.id;
            if (ImGui::Selectable(label.c_str(), false)) {
                picked = pixelart::kBuiltinPrefix + builtin.id;
                changed = true;
            }
            entryStrip(builtin.palette.colors);
            ImGui::SetItemTooltip("builtin:%s (%d colours): copied to the project's Palettes folder on first use",
                                  builtin.id.c_str(), builtin.palette.count());
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

void paletteSwatchStrip(const char* id, const std::vector<pxc::Rgba8>& colors, float width, float height, int maxColors) {
    ImGui::PushID(id);
    const int count = (int)colors.size();
    const int shown = std::min(count, std::max(1, maxColors));
    const std::string more = count > shown ? "+" + std::to_string(count - shown) : std::string();
    const float moreWidth = more.empty() ? 0.0f : ImGui::CalcTextSize(more.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
    const float avail = width > 0.0f ? width : std::max(1.0f, ImGui::GetContentRegionAvail().x + width);
    const float stripWidth = std::max(1.0f, avail - moreWidth);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##swatch_strip", ImVec2(stripWidth, std::max(1.0f, height)));
    drawSwatches(ImGui::GetWindowDrawList(), origin, ImVec2(origin.x + stripWidth, origin.y + height), colors, shown);
    if (shown > 0 && ImGui::IsItemHovered()) {
        const int index = std::clamp((int)((ImGui::GetIO().MousePos.x - origin.x) / stripWidth * (float)shown), 0, shown - 1);
        ImGui::SetTooltip("%d: %s", index, hexOf(colors[(size_t)index]).c_str());
    }
    if (!more.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", more.c_str());
    }
    ImGui::PopID();
}

}
