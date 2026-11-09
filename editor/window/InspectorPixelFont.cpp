// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Text component's Inspector (S51 R22): its table's rows, and with a bitmap font a Pixel Font
// section: Font Size stepping in multiples of the native size, the pixel Outline Width and its
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

    void ipfLabel(const char* label){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
    }

}

void editor::InspectorWindow::drawTextComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    drawDefaultComponent(cpType, sceneProject, entities);
    TextComponent& text = sceneProject->scene->getComponent<TextComponent>(entities[0]);
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
