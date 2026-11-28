// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Text component's Inspector (S51 R22): its table's rows, the Alignment buttons (S93 R37), and
// with a bitmap font a Pixel Font section: Font Size stepping in multiples of the native size, the pixel Outline Width and its
// Neighbors, a whole-pixel Shadow Offset and a note of the scale. Each edit is one undo step.

#include "InspectorWindow.h"

#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/TextComponent.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontPool.h"
#include "pixelart/FontPixelReport.h"
#include "subsystem/UISystem.h"
#include "util/UIProbe.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    namespace ipf = ::jasperengine::pixelart;

    // the same value into every selected Text, one undo step
    template<typename T>
    void ipfCommit(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, const char* property, const T& value){
        auto multi = new MultiPropertyCmd();
        for (Entity entity : entities)
            multi->addPropertyCmd<T>(project, sceneProject->id, entity, ComponentType::TextComponent, property, value);
        CommandHandle::get(sceneProject->id)->addCommand(multi);
        multi->setNoMerge();
    }

    // one row of TMP's alignment buttons: the selected one drawn pressed; the clicked value or -1
    int ipfAlignmentRow(const char* id, const char* const* labels, const char* const* tips, int count, int selected){
        int clicked = -1;
        for (int i = 0; i < count; i++){
            if (i > 0)
                ImGui::SameLine(0.0f, 2.0f);
            const bool on = i == selected;
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            char button[64];
            std::snprintf(button, sizeof(button), "%s##%s%d", labels[i], id, i);
            if (ImGui::Button(button))
                clicked = i;
            if (on)
                ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", tips[i]);
        }
        return clicked;
    }

    void ipfLabel(const char* label){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
    }

}

// Unity TextMesh's Anchor (S94 F24): which point of the text box sits on the origin, a row of
// horizontal buttons and one of vertical ones; the selection's shared value is lit, a click sets
// every selected Text in one undo step
void editor::InspectorWindow::drawTextAnchor(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    struct AnchorButton {
        int value;
        const char* label;
        const char* tooltip;
    };
    static const AnchorButton horizontal[] = {
        {(int)TextAnchorHorizontal::LEFT, "Left", "Left: the box starts at the origin"},
        {(int)TextAnchorHorizontal::CENTER, "Center", "Center: the box centred on the origin"},
        {(int)TextAnchorHorizontal::RIGHT, "Right", "Right: the box ends at the origin"},
    };
    static const AnchorButton vertical[] = {
        {(int)TextAnchorVertical::TOP, "Top", "Top: the box's top edge on the origin"},
        {(int)TextAnchorVertical::MIDDLE, "Middle", "Middle: the box's centre on the origin"},
        {(int)TextAnchorVertical::BOTTOM, "Bottom", "Bottom: the box's bottom edge on the origin (the default)"},
        {(int)TextAnchorVertical::BASELINE, "Baseline", "Baseline: the first line's baseline on the origin"},
        {(int)TextAnchorVertical::CAPLINE, "Capline", "Capline: the middle of the first line's capitals on the origin"},
        {(int)TextAnchorVertical::MIDLINE, "Midline", "Midline: the middle of the first line's lowercase letters on the origin"},
    };

    // the value every selected Text shares, -1 when they differ
    auto shared = [&](auto member) -> int {
        int value = -1;
        for (size_t i = 0; i < entities.size(); i++){
            const int current = (int)(sceneProject->scene->getComponent<TextComponent>(entities[i]).*member);
            if (i == 0) value = current;
            else if (value != current) return -1;
        }
        return value;
    };
    auto buttons = [&](const char* property, const AnchorButton* items, size_t count, int current){
        for (size_t i = 0; i < count; i++){
            if (i > 0) ImGui::SameLine(0.0f, 2.0f);
            const bool lit = items[i].value == current;
            if (lit) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
            const std::string id = std::string(items[i].label) + "##" + property;
            if (ImGui::Button(id.c_str()) && !lit)
                ipfCommit<int>(project, sceneProject, entities, property, items[i].value);
            if (lit) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", items[i].tooltip);
            UIProbe::item(std::string("text.") + property + "." + std::to_string(items[i].value));
        }
    };

    separatorText("Anchor");
    beginTable(cpType, getLabelSize("Vertical"), "textalignment");
    ipfLabel("Horizontal");
    buttons("anchorHorizontal", horizontal, 3, shared(&TextComponent::anchorHorizontal));
    ipfLabel("Vertical");
    buttons("anchorVertical", vertical, 6, shared(&TextComponent::anchorVertical));
    endTable();
}

void editor::InspectorWindow::drawTextComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    drawDefaultComponent(cpType, sceneProject, entities);
    drawTextAnchor(cpType, sceneProject, entities);
    drawTextLocalizationRows(cpType, sceneProject, entities);
    TextComponent& text = sceneProject->scene->getComponent<TextComponent>(entities[0]);

    // S93 R37: TextMeshPro's Alignment, two rows of buttons (each click one undo step)
    beginTable(cpType, getLabelSize("Outline Width"), "textalignment");
    ipfLabel("Alignment");
    static const char* const horizontal[] = {ICON_FA_ALIGN_LEFT, ICON_FA_ALIGN_CENTER, ICON_FA_ALIGN_RIGHT, ICON_FA_ALIGN_JUSTIFY};
    static const char* const horizontalTips[] = {"Left", "Center", "Right", "Justified: wrapped lines fill the width (a paragraph's last line stays left)"};
    const int h = ipfAlignmentRow("textAlignH", horizontal, horizontalTips, 4, (int)text.horizontalAlignment);
    UIProbe::item("text.horizontalAlignment");
    if (h >= 0)
        ipfCommit<int>(project, sceneProject, entities, "horizontalAlignment", h);
    ipfLabel("");
    static const char* const vertical[] = {ICON_FA_ARROWS_UP_TO_LINE, ICON_FA_GRIP_LINES, ICON_FA_ARROWS_DOWN_TO_LINE, ICON_FA_UNDERLINE};
    static const char* const verticalTips[] = {"Top", "Middle", "Bottom", "Baseline: the first line's baseline on the middle"};
    const int v = ipfAlignmentRow("textAlignV", vertical, verticalTips, 4, (int)text.verticalAlignment);
    UIProbe::item("text.verticalAlignment");
    if (v >= 0)
        ipfCommit<int>(project, sceneProject, entities, "verticalAlignment", v);
    endTable();
    if (text.verticalAlignment != TextVerticalAlignment::TOP && !text.fixedHeight)
        ImGui::TextDisabled("Vertical alignment needs a Fixed size Height");

    if (!ipf::isBitmapFontPath(text.font[0]))
        return;

    separatorText("Pixel Font");
    ipf::BitmapFontReport report;
    std::shared_ptr<ipf::BitmapFont> font = ipf::BitmapFontPool::get(text.font[0], &report);
    if (!font){
        ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), ICON_FA_TRIANGLE_EXCLAMATION " %s does not load%s%s", text.font[0].c_str(),
                           report.errors.empty() ? "" : ": ", report.errors.empty() ? "" : report.errors[0].message.c_str());
        return;
    }
    const int native = font->getNativeSize();
    const int k = ipf::pixelScaleFor(text.fontSize, native);

    beginTable(cpType, getLabelSize("Outline Width"), "pixelfont");

    // Font Size: a stepper over the multiples of the native size, "18 (9 x 2)"
    ipfLabel("Font Size");
    char value[64];
    std::snprintf(value, sizeof(value), "%d (%d \xC3\x97 %d)", k * native, native, k);
    if (ImGui::Button("-##textPixelSizeDown") && k > 1)
        ipfCommit<unsigned int>(project, sceneProject, entities, "fontSize", (unsigned int)((k - 1) * native));
    ImGui::SameLine();
    ImGui::TextUnformatted(value);
    UIProbe::item("text.pixelFontSize");
    ImGui::SameLine();
    if (ImGui::Button("+##textPixelSizeUp") && k < 64)
        ipfCommit<unsigned int>(project, sceneProject, entities, "fontSize", (unsigned int)((k + 1) * native));
    if ((int)text.fontSize != k * native){
        ImGui::SameLine();
        if (ImGui::SmallButton("Snap##textPixelSizeSnap"))
            ipfCommit<unsigned int>(project, sceneProject, entities, "fontSize", (unsigned int)(k * native));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Font Size %u is not a multiple of %d: it draws at %d", text.fontSize, native, k * native);
    }

    // Outline Width: 0, 1 or 2 font pixels, and the Neighbors it grows through
    ipfLabel("Outline Width");
    static const char* const widths[] = {"0", "1", "2"};
    int width = std::clamp(std::isfinite(text.outlineWidth) ? (int)std::lround(text.outlineWidth) : 0, 0, 2);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##textPixelOutline", &width, widths, 3))
        ipfCommit<float>(project, sceneProject, entities, "outlineWidth", (float)width);
    ipfLabel("Neighbors");
    static const char* const neighbors[] = {"4 (plus)", "8 (square)"};
    int neighbor = text.outlineNeighbors == 4 ? 0 : 1;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##textPixelNeighbors", &neighbor, neighbors, 2))
        ipfCommit<int>(project, sceneProject, entities, "outlineNeighbors", neighbor == 0 ? 4 : 8);

    // Shadow Offset: whole font pixels (softness and dilate do not apply)
    ipfLabel("Shadow Offset");
    int offset[2] = {(int)std::lround(text.shadowOffset.x), (int)std::lround(text.shadowOffset.y)};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt2("##textPixelShadowOffset", offset))
        ipfCommit<Vector2>(project, sceneProject, entities, "shadowOffset", Vector2((float)std::clamp(offset[0], -64, 64), (float)std::clamp(offset[1], -64, 64)));

    endTable();

    // the note: native size, scale and the device pixels a font pixel takes
    const float unit = text.pixelUnit > 0.0f ? text.pixelUnit : (float)k;
    std::shared_ptr<UISystem> ui = sceneProject->scene->getSystem<UISystem>();
    const float device = unit * (ui ? ui->getTextRasterScale() : 1.0f);
    ImGui::TextDisabled("Native %d px, scale x%d, %g device px per font pixel%s", native, k, std::round(device * 100.0f) / 100.0f,
                        font->getReport().partialAlphaShare > 0.0f ? " (partial alpha: set Alpha Cutoff)" : "");
}
