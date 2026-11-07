// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureSlicerToolDialog.h"
#include "window/EditorModal.h"
#include "Backend.h"
#include "Theme.h"
#include "window/Widgets.h"
#include "external/IconsFontAwesome6.h"
#include "sprite/SpriteSheetBuilder.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <climits>

namespace jasperengine {
namespace editor {

void TextureSlicerToolDialog::open(const Texture& previewTexture,
                                    int sheetWidth, int sheetHeight,
                                    std::function<void(const SliceResult&)> onApply,
                                    std::function<void()> onCancel) {
    m_isOpen = true;
    m_mode = Mode::SPRITE;
    m_previewTexture = previewTexture;
    m_sheetWidth = sheetWidth;
    m_sheetHeight = sheetHeight;
    m_onApply = onApply;
    m_onCancel = onCancel;
    m_previewDirty = true;

    if (!m_previewTexture.empty()) {
        int textureWidth = static_cast<int>(m_previewTexture.getWidth());
        int textureHeight = static_cast<int>(m_previewTexture.getHeight());

        // If texture data not loaded yet, force load to get dimensions
        if (textureWidth <= 0 || textureHeight <= 0) {
            m_previewTexture.load();
            textureWidth = static_cast<int>(m_previewTexture.getWidth());
            textureHeight = static_cast<int>(m_previewTexture.getHeight());
        }

        if (textureWidth > 0) {
            m_sheetWidth = textureWidth;
        }
        if (textureHeight > 0) {
            m_sheetHeight = textureHeight;
        }
    }

    // Smart defaults based on sheet size
    if (m_sheetWidth > 0 && m_sheetHeight > 0) {
        m_columns = std::max(1, m_sheetWidth / 32);
        m_rows = std::max(1, m_sheetHeight / 32);

        // Clamp to reasonable defaults
        if (m_columns > 16) m_columns = 4;
        if (m_rows > 16) m_rows = 4;
        m_cellWidth = m_sheetWidth / m_columns;
        m_cellHeight = m_sheetHeight / m_rows;
    }

    m_offsetX = 0;
    m_offsetY = 0;
    m_paddingX = 0;
    m_paddingY = 0;
    strncpy(m_prefixBuffer, "frame_", sizeof(m_prefixBuffer) - 1);
    m_prefixBuffer[sizeof(m_prefixBuffer) - 1] = '\0';
}

void TextureSlicerToolDialog::openTileset(const std::vector<SubmeshInfo>& submeshes,
                                           int sheetWidth, int sheetHeight,
                                           std::function<void(const SliceResult&)> onApply,
                                           std::function<void()> onCancel) {
    m_submeshes = submeshes;
    m_selectedSubmesh = 0;

    Texture previewTexture;
    if (!m_submeshes.empty()) {
        previewTexture = m_submeshes[0].texture;
    }

    // Initialize shared slicing state via the base open
    open(previewTexture, sheetWidth, sheetHeight, onApply, onCancel);

    // Override for tileset mode
    m_mode = Mode::TILESET;
    strncpy(m_prefixBuffer, "tile_", sizeof(m_prefixBuffer) - 1);
    m_prefixBuffer[sizeof(m_prefixBuffer) - 1] = '\0';
}

void TextureSlicerToolDialog::openTextureSheet(const Texture& previewTexture, const std::string& textureAbsolute, const TextureSheetSettings& current,
                                                std::function<void(const TextureSheetSettings&)> onApply) {
    open(previewTexture, 0, 0, nullptr, nullptr);
    m_mode = Mode::TEXTURE_SHEET;
    m_texturePath = textureAbsolute;
    // a Sprite without a texture slices nothing (the dialog still opens, its Slice greyed out)
    std::error_code ec;
    m_image.reset();
    if (!textureAbsolute.empty() && std::filesystem::is_regular_file(textureAbsolute, ec)) m_image = pixelart::PixelImage::loadPng(textureAbsolute, nullptr);
    if (m_image) {
        m_sheetWidth = m_image->width();
        m_sheetHeight = m_image->height();
    }
    m_onApplySheet = std::move(onApply);
    m_frameEdits.clear();
    m_jsonFrames.clear();
    m_jsonStatus.clear();
    m_selectedFrame = -1;
    m_skipEmpty = true;
    m_pivotPreset = 0;
    const std::string stem = std::filesystem::path(textureAbsolute).stem().string();
    snprintf(m_prefixBuffer, sizeof(m_prefixBuffer), "%s_", stem.c_str());
    // the stored grid, as it is
    if (current.mode == TextureSpriteMode::GRID && current.grid.cellWidth > 0) {
        m_sliceMode = SliceMode::CELL_SIZE;
        m_cellWidth = current.grid.cellWidth;
        m_cellHeight = current.grid.cellHeight;
        m_offsetX = current.grid.offsetX;
        m_offsetY = current.grid.offsetY;
        m_paddingX = current.grid.spacingX;
        m_paddingY = current.grid.spacingY;
        m_skipEmpty = current.grid.skipEmpty;
        if (!current.grid.prefix.empty()) snprintf(m_prefixBuffer, sizeof(m_prefixBuffer), "%s", current.grid.prefix.c_str());
    } else if (current.mode == TextureSpriteMode::MULTIPLE) {
        m_jsonFrames = current.frames;
        m_jsonStatus = std::to_string(current.frames.size()) + " frames of the stored Sprite Mode";
    }
    m_previewDirty = true;
}

TextureSheetSettings TextureSlicerToolDialog::sheetSettings() const {
    TextureSheetSettings settings;
    settings.pivotPreset = static_cast<PivotPreset>(m_pivotPreset);
    bool clipped = false;
    for (const SlicedRect& r : m_previewRects) {
        Rect rect = r.rect;
        clipped = clipped || rect.getWidth() != m_previewRects[0].rect.getWidth() || rect.getHeight() != m_previewRects[0].rect.getHeight();
    }
    if (m_jsonFrames.empty() && m_frameEdits.empty() && !clipped && !m_previewRects.empty()) {
        // a plain grid stays a grid (the pixels decide its empty cells again on every reimport)
        settings.mode = TextureSpriteMode::GRID;
        settings.grid.cellWidth = static_cast<int>(m_previewRects[0].rect.getWidth());
        settings.grid.cellHeight = static_cast<int>(m_previewRects[0].rect.getHeight());
        settings.grid.offsetX = m_offsetX;
        settings.grid.offsetY = m_offsetY;
        settings.grid.spacingX = m_paddingX;
        settings.grid.spacingY = m_paddingY;
        settings.grid.skipEmpty = m_skipEmpty;
        settings.grid.prefix = m_prefixBuffer;
        return settings;
    }
    settings.mode = TextureSpriteMode::MULTIPLE;
    for (size_t i = 0; i < m_previewRects.size(); i++) {
        auto edit = m_frameEdits.find(static_cast<int>(i));
        TextureSheetFrame f = edit != m_frameEdits.end() ? edit->second : TextureSheetFrame();
        f.name = m_previewRects[i].name;
        f.rect = m_previewRects[i].rect;
        if (edit == m_frameEdits.end() && i < m_jsonFrames.size() && m_previewRects.size() == m_jsonFrames.size()) {
            f.hasPivot = m_jsonFrames[i].hasPivot;
            f.pivot = m_jsonFrames[i].pivot;
            f.border = m_jsonFrames[i].border;
        }
        settings.frames.push_back(f);
    }
    return settings;
}

void TextureSlicerToolDialog::generatePreview() {
    m_previewRects.clear();

    if (m_sheetWidth <= 0 || m_sheetHeight <= 0) return;

    // frames from an Aseprite JSON (or the stored ones): as they are
    if (m_mode == Mode::TEXTURE_SHEET && !m_jsonFrames.empty()) {
        for (const TextureSheetFrame& f : m_jsonFrames) m_previewRects.push_back({f.name, f.rect});
        m_previewDirty = false;
        return;
    }

    int cols, rows, cw, ch;

    if (m_sliceMode == SliceMode::GRID) {
        cols = std::max(1, m_columns);
        rows = std::max(1, m_rows);
        int availW = m_sheetWidth - m_offsetX;
        int availH = m_sheetHeight - m_offsetY;
        cw = std::max(1, (availW - m_paddingX * (cols - 1)) / cols);
        ch = std::max(1, (availH - m_paddingY * (rows - 1)) / rows);
    } else {
        cw = std::max(1, m_cellWidth);
        ch = std::max(1, m_cellHeight);
        int availW = m_sheetWidth - m_offsetX;
        int availH = m_sheetHeight - m_offsetY;
        cols = std::max(1, (availW + m_paddingX) / (cw + m_paddingX));
        rows = std::max(1, (availH + m_paddingY) / (ch + m_paddingY));
    }

    std::string prefix(m_prefixBuffer);
    int rectIndex = 0;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            float fx = static_cast<float>(m_offsetX + c * (cw + m_paddingX));
            float fy = static_cast<float>(m_offsetY + r * (ch + m_paddingY));
            float fw = static_cast<float>(cw);
            float fh = static_cast<float>(ch);

            // Clip to sheet bounds
            if (fx + fw > m_sheetWidth) fw = m_sheetWidth - fx;
            if (fy + fh > m_sheetHeight) fh = m_sheetHeight - fy;
            if (fw <= 0 || fh <= 0) continue;

            // a texture sheet's empty cells (all alpha 0) are skipped
            if (m_mode == Mode::TEXTURE_SHEET && m_skipEmpty && m_image) {
                const pixelart::IntRect cell = pixelart::IntRect{(int)fx, (int)fy, (int)fw, (int)fh}.intersect(m_image->bounds());
                bool opaque = false;
                for (int yy = cell.y; yy < cell.y + cell.h && !opaque; yy++) {
                    const uint8_t* row = m_image->row(yy);
                    for (int xx = cell.x; xx < cell.x + cell.w && !opaque; xx++) opaque = row[xx * 4 + 3] != 0;
                }
                if (!opaque) continue;
            }
            SlicedRect slicedRect;
            slicedRect.name = prefix + std::to_string(rectIndex);
            slicedRect.rect = Rect(fx, fy, fw, fh);

            m_previewRects.push_back(slicedRect);
            rectIndex++;
        }
    }

    m_previewDirty = false;
}

void TextureSlicerToolDialog::show() {
    if (!m_isOpen) return;

    const bool isTileset = (m_mode == Mode::TILESET);
    const char* popupTitle = isTileset
        ? ICON_FA_TABLE_CELLS " Slice Tileset##TextureSlicerToolModal"
        : ICON_FA_GRIP " Slice Sprite Sheet##TextureSlicerToolModal";
    const char* rectLabel = isTileset ? "tile rects" : "frames";
    const char* namingLabel = isTileset ? "Rect Naming" : "Frame Naming";
    const char* detailsLabel = isTileset ? "Rect Details" : "Frame Details";

    ImGui::OpenPopup(popupTitle);

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(isTileset ? 540.0f : 520.0f, 0), ImGuiCond_Once);

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal;

    if (!EditorModal::begin(popupTitle, &m_isOpen, flags)) {
        if (m_isOpen) {
            m_isOpen = false;
            if (m_onCancel) m_onCancel();
        }
        return;
    }
    EditorModal::message(std::string("Slice the texture into ") + rectLabel + " (the preview shows them)");

    auto beginInputTable = [](const char* id, float firstColumnWidth = 130.0f) {
        if (ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, firstColumnWidth);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            return true;
        }
        return false;
    };

    auto beginInputRow = [](const char* label, float inputWidth = 0.0f) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(inputWidth > 0.0f ? inputWidth : 8.0f * ImGui::GetFontSize());
    };

    ImGui::Separator();

    if (m_sheetWidth > 0 && m_sheetHeight > 0) {
        ImGui::Text("%s size: %d x %d", isTileset ? "Texture" : "Sheet", m_sheetWidth, m_sheetHeight);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
            isTileset ? "Warning: Texture dimensions unknown. Assign a texture first."
                      : "Warning: Sheet dimensions unknown. Set Width/Height in sprite properties first.");
    }

    ImGui::Spacing();

    // --- Submesh selector (tileset only) ---
    if (isTileset && m_submeshes.size() > 1) {
        if (beginInputTable("SlicerSubmeshTable")) {
            beginInputRow("Submesh", 14.0f * ImGui::GetFontSize());
            const char* currentName = (m_selectedSubmesh >= 0 && m_selectedSubmesh < (int)m_submeshes.size())
                ? m_submeshes[m_selectedSubmesh].name.c_str() : "None";
            if (ImGui::BeginCombo("##Submesh", currentName)) {
                for (int i = 0; i < (int)m_submeshes.size(); i++) {
                    bool isSelected = (m_selectedSubmesh == i);
                    if (ImGui::Selectable(m_submeshes[i].name.c_str(), isSelected)) {
                        m_selectedSubmesh = i;
                        m_previewTexture = m_submeshes[i].texture;

                        if (!m_previewTexture.empty()) {
                            int textureWidth = static_cast<int>(m_previewTexture.getWidth());
                            int textureHeight = static_cast<int>(m_previewTexture.getHeight());
                            if (textureWidth <= 0 || textureHeight <= 0) {
                                m_previewTexture.load();
                                textureWidth = static_cast<int>(m_previewTexture.getWidth());
                                textureHeight = static_cast<int>(m_previewTexture.getHeight());
                            }
                            if (textureWidth > 0) m_sheetWidth = textureWidth;
                            if (textureHeight > 0) m_sheetHeight = textureHeight;
                        }
                        m_previewDirty = true;
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    // --- Slice Mode ---
    if (beginInputTable("SlicerSliceModeTable")) {
        beginInputRow("Type", 14.0f * ImGui::GetFontSize());
        {
            int mode = m_sliceMode == SliceMode::GRID ? 0 : 1;
            if (EditorModal::enumField("type", mode, {"Grid By Cell Count", "Grid By Cell Size"})) {
                m_sliceMode = mode == 0 ? SliceMode::GRID : SliceMode::CELL_SIZE;
                m_previewDirty = true;
            }
        }
        if (ImGui::RadioButton("Grid By Cell Count##SliceModeGrid", m_sliceMode == SliceMode::GRID)) {
            m_sliceMode = SliceMode::GRID;
            m_previewDirty = true;
        }
        if (ImGui::RadioButton("Grid By Cell Size##SliceModeCell", m_sliceMode == SliceMode::CELL_SIZE)) {
            m_sliceMode = SliceMode::CELL_SIZE;
            m_previewDirty = true;
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (m_sliceMode == SliceMode::GRID) {
        ImGui::SeparatorText("Grid");
        if (beginInputTable("SlicerGridTable")) {
            beginInputRow("Columns", 6.0f * ImGui::GetFontSize());
            if (EditorModal::dragInt("##Columns", "columns", &m_columns, 0.1f, 1, 64)) {
                m_previewDirty = true;
            }
            beginInputRow("Rows", 6.0f * ImGui::GetFontSize());
            if (EditorModal::dragInt("##Rows", "rows", &m_rows, 0.1f, 1, 64)) {
                m_previewDirty = true;
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::SeparatorText("Cell Size");
        if (beginInputTable("SlicerCellSizeTable")) {
            beginInputRow("Cell Width", 6.0f * ImGui::GetFontSize());
            if (EditorModal::dragInt("##CellWidth", "cell_width", &m_cellWidth, 1.0f, 1, m_sheetWidth > 0 ? m_sheetWidth : 4096)) {
                m_previewDirty = true;
            }
            beginInputRow("Cell Height", 6.0f * ImGui::GetFontSize());
            if (EditorModal::dragInt("##CellHeight", "cell_height", &m_cellHeight, 1.0f, 1, m_sheetHeight > 0 ? m_sheetHeight : 4096)) {
                m_previewDirty = true;
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Offset & Padding");
    if (beginInputTable("SlicerOffsetPaddingTable")) {
        beginInputRow("Offset X", 6.0f * ImGui::GetFontSize());
        if (EditorModal::dragInt("##OffsetX", "offset_x", &m_offsetX, 1.0f, 0, INT_MAX)) {
            m_previewDirty = true;
        }
        beginInputRow("Offset Y", 6.0f * ImGui::GetFontSize());
        if (EditorModal::dragInt("##OffsetY", "offset_y", &m_offsetY, 1.0f, 0, INT_MAX)) {
            m_previewDirty = true;
        }
        beginInputRow("Padding X", 6.0f * ImGui::GetFontSize());
        if (EditorModal::dragInt("##PaddingX", "padding_x", &m_paddingX, 1.0f, 0, INT_MAX)) {
            m_previewDirty = true;
        }
        beginInputRow("Padding Y", 6.0f * ImGui::GetFontSize());
        if (EditorModal::dragInt("##PaddingY", "padding_y", &m_paddingY, 1.0f, 0, INT_MAX)) {
            m_previewDirty = true;
        }
        ImGui::EndTable();
    }

    if (m_mode == Mode::TEXTURE_SHEET) {
        ImGui::Spacing();
        ImGui::SeparatorText("Sprite Mode");
        if (beginInputTable("SlicerSheetTable")) {
            beginInputRow("Skip Empty", 6.0f * ImGui::GetFontSize());
            if (EditorModal::checkbox("##SkipEmpty", "skip_empty", &m_skipEmpty)) m_previewDirty = true;
            beginInputRow("Pivot", 10.0f * ImGui::GetFontSize());
            EditorModal::combo("##Pivot", "pivot", &m_pivotPreset, {"Center", "Top Center", "Bottom Center", "Left Center", "Right Center",
                                                                    "Top Left", "Bottom Left", "Top Right", "Bottom Right"});
            ImGui::EndTable();
        }
        // Slice... > From JSON: the Aseprite --data file beside the texture
        if (EditorModal::button("From JSON", "from_json")) {
            std::filesystem::path json = std::filesystem::path(m_texturePath);
            json.replace_extension(".json");
            std::error_code jsonEc;
            std::ifstream in;
            if (std::filesystem::is_regular_file(json, jsonEc)) in.open(json, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            pixelart::PixelProblem problem;
            std::vector<TextureSheetFrame> frames;
            if (!in || !sprite::framesFromAsepriteJson(text, frames, problem)) {
                m_jsonStatus = in ? problem.message : "no " + json.filename().string() + " beside the texture";
            } else {
                m_jsonFrames = frames;
                m_frameEdits.clear();
                m_jsonStatus = std::to_string(frames.size()) + " frames from " + json.filename().string();
            }
            m_previewDirty = true;
        }
        if (!m_jsonFrames.empty()) {
            ImGui::SameLine();
            if (EditorModal::button("Back to Grid", "back_to_grid")) {
                m_jsonFrames.clear();
                m_jsonStatus.clear();
                m_previewDirty = true;
            }
        }
        if (!m_jsonStatus.empty()) ImGui::TextDisabled("%s", m_jsonStatus.c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText(namingLabel);
    if (beginInputTable("SlicerNamingTable")) {
        beginInputRow("Prefix", 10.0f * ImGui::GetFontSize());
        if (ImGui::InputText("##Prefix", m_prefixBuffer, sizeof(m_prefixBuffer))) {
            m_previewDirty = true;
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Generate preview
    if (m_previewDirty) {
        generatePreview();
    }

    // --- Visual preview ---
    ImGui::Text("Preview: %d %s", (int)m_previewRects.size(), rectLabel);

    if (m_sheetWidth > 0 && m_sheetHeight > 0) {
        auto drawPreview = [this, isTileset](const ImVec2& canvasPos, const ImVec2& canvasSize, ImDrawList* drawList) {
            const float scale = std::min(canvasSize.x / static_cast<float>(m_sheetWidth),
                                         canvasSize.y / static_cast<float>(m_sheetHeight));
            const float previewW = m_sheetWidth * scale;
            const float previewH = m_sheetHeight * scale;
            const float offsetX = (canvasSize.x - previewW) * 0.5f;
            const float offsetY = (canvasSize.y - previewH) * 0.5f;

            ImVec2 imageMin(canvasPos.x + offsetX, canvasPos.y + offsetY);
            ImVec2 imageMax(imageMin.x + previewW, imageMin.y + previewH);

            drawList->AddRectFilled(canvasPos,
                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                IM_COL32(40, 40, 40, 255));

            if (!m_previewTexture.empty() && m_previewTexture.getRender()) {
                Widgets::addImage(drawList, Backend::getImGuiTexture(m_previewTexture.getRender()), imageMin, imageMax);
            }

            drawList->AddRect(imageMin, imageMax, IM_COL32(100, 100, 100, 255));

            ImU32 borderColor = isTileset ? IM_COL32(80, 180, 220, 220) : IM_COL32(100, 200, 100, 220);
            for (size_t i = 0; i < m_previewRects.size(); i++) {
                const SlicedRect& slicedRect = m_previewRects[i];
                float rx = imageMin.x + slicedRect.rect.getX() * scale;
                float ry = imageMin.y + slicedRect.rect.getY() * scale;
                float rw = slicedRect.rect.getWidth() * scale;
                float rh = slicedRect.rect.getHeight() * scale;

                drawList->AddRect(ImVec2(rx, ry), ImVec2(rx + rw, ry + rh), borderColor);

                if (rw > Theme::dpi(16.0f) && rh > Theme::dpi(12.0f)) {
                    char indexStr[8];
                    snprintf(indexStr, sizeof(indexStr), "%d", (int)i);
                    ImVec2 textSize = ImGui::CalcTextSize(indexStr);
                    float textX = rx + (rw - textSize.x) * 0.5f;
                    float textY = ry + (rh - textSize.y) * 0.5f;
                    drawList->AddText(ImVec2(textX, textY), IM_COL32(255, 255, 255, 200), indexStr);
                }
            }

            drawList->AddRect(canvasPos,
                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                IM_COL32(80, 80, 80, 255));
        };

        const ImVec2 compactCanvasSize(Theme::dpi(100.0f), Theme::dpi(100.0f));
        ImVec2 compactCanvasPos = ImGui::GetCursorScreenPos();
        drawPreview(compactCanvasPos, compactCanvasSize, ImGui::GetWindowDrawList());
        ImGui::Dummy(compactCanvasSize);

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%d x %d", m_sheetWidth, m_sheetHeight);
            ImVec2 tooltipCanvasPos = ImGui::GetCursorScreenPos();
            ImVec2 tooltipCanvasSize(static_cast<float>(m_sheetWidth), static_cast<float>(m_sheetHeight));
            drawPreview(tooltipCanvasPos, tooltipCanvasSize, ImGui::GetWindowDrawList());
            ImGui::Dummy(tooltipCanvasSize);
            ImGui::EndTooltip();
        }
    }

    // Rect/frame list summary
    if (!m_previewRects.empty() && ImGui::TreeNode(detailsLabel)) {
        ImGui::BeginChild("RectList", ImVec2(0, Theme::dpi(isTileset ? 120.0f : 150.0f)), true);
        for (size_t i = 0; i < m_previewRects.size(); i++) {
            const SlicedRect& slicedRect = m_previewRects[i];
            char line[192];
            snprintf(line, sizeof(line), "[%d] %s: (%.0f, %.0f, %.0f, %.0f)%s", (int)i, slicedRect.name.c_str(), slicedRect.rect.getX(),
                     slicedRect.rect.getY(), slicedRect.rect.getWidth(), slicedRect.rect.getHeight(), m_frameEdits.count((int)i) ? " *" : "");
            if (m_mode == Mode::TEXTURE_SHEET) {
                if (ImGui::Selectable(line, m_selectedFrame == (int)i)) m_selectedFrame = (int)i;
            } else {
                ImGui::TextUnformatted(line);
            }
        }
        ImGui::EndChild();
        // the selected frame's own pivot and border (pixels; a border draws it Sliced)
        if (m_mode == Mode::TEXTURE_SHEET && m_selectedFrame >= 0 && m_selectedFrame < (int)m_previewRects.size()) {
            TextureSheetFrame& edit = m_frameEdits[m_selectedFrame];
            bool changed = false;
            int pivot[2] = {(int)edit.pivot.x, (int)edit.pivot.y};
            int border[4] = {(int)edit.border.x, (int)edit.border.y, (int)edit.border.z, (int)edit.border.w};
            changed |= EditorModal::checkbox("Own Pivot", "own_pivot", &edit.hasPivot);
            ImGui::BeginDisabled(!edit.hasPivot);
            changed |= ImGui::DragInt2("Pivot (px)", pivot, 0.2f, 0, 4096);
            ImGui::EndDisabled();
            changed |= ImGui::DragInt4("Border L T R B", border, 0.2f, 0, 4096);
            if (changed) {
                edit.pivot = Vector2((float)pivot[0], (float)pivot[1]);
                edit.border = Vector4((float)border[0], (float)border[1], (float)border[2], (float)border[3]);
            }
            if (!edit.hasPivot && edit.border.x == 0 && edit.border.y == 0 && edit.border.z == 0 && edit.border.w == 0 && !changed) {
                m_frameEdits.erase(m_selectedFrame);
            }
        }
        ImGui::TreePop();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Buttons
    // Unity-style button row: right-aligned, primary first
    float buttonsWidth = 240.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsWidth));

    ImGui::BeginDisabled(m_previewRects.empty());
    if (EditorModal::button("Slice", "yes", ImVec2(120, 0))) {
        if (m_mode == Mode::TEXTURE_SHEET) {
            if (m_onApplySheet) m_onApplySheet(sheetSettings());
        } else if (m_onApply) {
            m_onApply({m_previewRects, m_selectedSubmesh});
        }
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(120, 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        m_isOpen = false;
        if (m_onCancel) m_onCancel();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace editor
} // namespace jasperengine
