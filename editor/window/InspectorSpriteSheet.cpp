// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sprite inspector's S45 parts (docs/specs/S45-sprite-atlas.md R12, R21, R30): the frame
// picker (a dropdown with thumbnails and search, plus the index), the Native size checkbox, the
// Sliced warning, the Slicer Tool writing the texture's Sprite Mode and setting the sprite's sheet
// and frame (one step: the scene's and the meta's linked), and the Inline Frames (legacy) header
// with Convert to Sprite Sheet.

#include "InspectorWindow.h"

#include "Backend.h"
#include "Out.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/SetAssetMetaCmd.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "external/IconsFontAwesome6.h"
#include "sprite/SpriteFrameGeometry.h"
#include "sprite/SpriteMigration.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetBuilder.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"
#include "subsystem/MeshSystem.h"
#include "util/UIProbe.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

using namespace jasperengine;

namespace {

    bool containsNoCase(const std::string& text, const char* needle) {
        if (!needle || !needle[0]) return true;
        std::string a = text, b = needle;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a.find(b) != std::string::npos;
    }

}

void editor::InspectorWindow::drawSheetFramePicker(ComponentType cpType, SceneProject* sceneProject, Entity entity, const SpriteSheetData& sheet) {
    const SpriteComponent& sprite = sceneProject->scene->getComponent<SpriteComponent>(entity);
    const int count = sheet.frameCount();
    const int current = std::clamp(sprite.frame, 0, count - 1);
    const float thumb = ImGui::GetFrameHeight() * 1.6f;
    // a frame's thumbnail: its rect of its page
    auto frameImage = [&](const SpriteFrame& f, float size) {
        if (f.page >= sheet.pages.size()) return;
        SheetPreviewTextures::Image* page = sheetThumbnails.get(project, sheet.pages[f.page].texture);
        if (!page || f.rect.getWidth() <= 0 || f.rect.getHeight() <= 0) {
            ImGui::Dummy(ImVec2(size, size));
            return;
        }
        const float pw = static_cast<float>(page->width), ph = static_cast<float>(page->height);
        const float k = size / std::max(f.rect.getWidth(), f.rect.getHeight());
        ImGui::Image(Backend::getImGuiTexture(page->texture.getRender()), ImVec2(f.rect.getWidth() * k, f.rect.getHeight() * k),
                     ImVec2(f.rect.getX() / pw, f.rect.getY() / ph), ImVec2((f.rect.getX() + f.rect.getWidth()) / pw, (f.rect.getY() + f.rect.getHeight()) / ph));
    };
    auto pick = [&](int i) {
        std::vector<Command*> commands;
        auto* multi = new MultiPropertyCmd();
        multi->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, "frame", i);
        multi->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "frameName", sheet.frames[static_cast<size_t>(i)].name);
        multi->setNoMerge();
        CommandHandle::get(sceneProject->id)->addCommand(multi);
    };

    beginTable(cpType, getLabelSize("Frame"), "sheet_sprite");
    propertyHeader("Frame");
    frameImage(sheet.frames[static_cast<size_t>(current)], ImGui::GetFrameHeight());
    ImGui::SameLine();
    const std::string label = std::to_string(current) + ": " + sheet.frames[static_cast<size_t>(current)].name;
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 3.0f);
    if (ImGui::BeginCombo("##sheet_frame", label.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##frame_search", "Search frames", sheetFrameSearch, sizeof(sheetFrameSearch));
        UIProbe::item("inspector.sprite.frameSearch");
        ImGuiListClipper clipper;
        std::vector<int> shown;
        for (int i = 0; i < count; i++) {
            if (containsNoCase(sheet.frames[static_cast<size_t>(i)].name, sheetFrameSearch) || containsNoCase(std::to_string(i), sheetFrameSearch)) shown.push_back(i);
        }
        clipper.Begin(static_cast<int>(shown.size()), thumb + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const int i = shown[static_cast<size_t>(row)];
                ImGui::PushID(i);
                const ImVec2 start = ImGui::GetCursorPos();
                if (ImGui::Selectable("##frame", i == current, 0, ImVec2(0, thumb)) && i != current) pick(i);
                ImGui::SetCursorPos(start);
                frameImage(sheet.frames[static_cast<size_t>(i)], thumb);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%d: %s", i, sheet.frames[static_cast<size_t>(i)].name.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    UIProbe::item("inspector.sprite.framePicker");
    ImGui::SameLine();
    int index = current;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputInt("##frame_index", &index, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && index >= 0 && index < count && index != current) pick(index);
    endTable();

    // Sliced needs a border
    if (sprite.drawMode == SpriteDrawMode::SLICED && !spriteFrameHasBorder(sheet.frames[static_cast<size_t>(current)])) {
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Frame \"%s\" has no border: it draws Simple",
                           sheet.frames[static_cast<size_t>(current)].name.c_str());
        ImGui::SetItemTooltip("Give the frame a border in the texture's Slicer (or a 9-patch slice in Aseprite)");
        UIProbe::item("inspector.sprite.slicedWarning");
    }
}

void editor::InspectorWindow::drawSpriteNativeSize(ComponentType cpType, SceneProject* sceneProject, Entity entity) {
    const SpriteComponent& sprite = sceneProject->scene->getComponent<SpriteComponent>(entity);
    bool native = sprite.width == 0 && sprite.height == 0;
    beginTable(cpType, getLabelSize("Texture Scale"), "sprite_native_size");
    propertyHeader("Native Size");
    if (ImGui::Checkbox("##native_size", &native)) {
        unsigned int w = 0, h = 0;
        if (!native && sprite.sheetData && !sprite.sheetData->frames.empty()) {
            const SpriteFrame& f = sprite.sheetData->frames[static_cast<size_t>(std::clamp(sprite.frame, 0, sprite.sheetData->frameCount() - 1))];
            w = static_cast<unsigned int>(std::max(1.0f, f.sourceSize.x));
            h = static_cast<unsigned int>(std::max(1.0f, f.sourceSize.y));
        } else if (!native) {
            w = h = 1;
        }
        auto* multi = new MultiPropertyCmd();
        multi->addPropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, "width", w);
        multi->addPropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, "height", h);
        multi->setNoMerge();
        CommandHandle::get(sceneProject->id)->addCommand(multi);
    }
    ImGui::SetItemTooltip("Width and Height 0: each frame at its own size (Sliced draws the sprite at Width x Height)");
    endTable();
}

void editor::InspectorWindow::openSpriteSlicer(ComponentType cpType, SceneProject* sceneProject, Entity entity, const std::string& texture) {
    const fs::path absolute = project->getAssetsPath() / texture;
    const sprite::TextureSheetInfo info = texture.empty() ? sprite::TextureSheetInfo() : sprite::textureSheetSettings(absolute);
    Texture preview(texture);
    const uint32_t sceneId = sceneProject->id;
    textureSlicerToolDialog.openTextureSheet(preview, absolute.string(), info.settings,
                                             [this, sceneId, entity, cpType, texture, absolute](const TextureSheetSettings& settings) {
        SceneProject* scene = project->getScene(sceneId);
        if (!scene || !scene->scene || !scene->scene->findComponent<SpriteComponent>(entity)) return;
        if (texture.empty()) {
            Out::warning("Slicer Tool: the Sprite has no texture to slice (set its texture first)");
            return;
        }
        // the texture's meta (the project's step) and the sprite (the scene's), linked as one undo step
        CommandHistory* projectHistory = project->getProjectCommandHistory();
        CommandHistory* sceneHistory = CommandHandle::get(sceneId);
        Command* meta = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(absolute), sprite::kSpriteSheetKey, sprite::sheetSettingsToYaml(settings));
        std::vector<CommandHistory*> histories;
        if (meta->isNoOp()) {
            delete meta;
        } else {
            meta->setNoMerge();
            if (!projectHistory->addCommand(meta)) {
                Out::error("Couldn't write the Sprite Mode of %s", texture.c_str());
                return;
            }
            histories.push_back(projectHistory);
        }
        sprite::SpriteSheetBuilder::get().invalidate(texture);
        const SpriteComponent& component = scene->scene->getComponent<SpriteComponent>(entity);
        auto* multi = new MultiPropertyCmd();
        const Guid guid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(absolute));
        std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(texture);
        const int frame = sheet && component.sheet.path == texture ? std::clamp(component.frame, 0, std::max(0, sheet->frameCount() - 1)) : 0;
        multi->addPropertyCmd<AssetRef>(project, sceneId, entity, cpType, "sheet", AssetRef(texture, guid));
        multi->addPropertyCmd<int>(project, sceneId, entity, cpType, "frame", frame);
        if (sheet && frame < sheet->frameCount()) {
            multi->addPropertyCmd<std::string>(project, sceneId, entity, cpType, "frameName", sheet->frames[static_cast<size_t>(frame)].name);
        }
        for (unsigned int i = 0; i < component.numFramesRect; i++) {
            multi->addPropertyCmd<std::string>(project, sceneId, entity, cpType, "framesRect[" + std::to_string(i) + "].name", std::string());
            multi->addPropertyCmd<Vector4>(project, sceneId, entity, cpType, "framesRect[" + std::to_string(i) + "].rect", Vector4(0, 0, 0, 0));
        }
        if (component.numFramesRect > 0) multi->addPropertyCmd<unsigned int>(project, sceneId, entity, cpType, "numFramesRect", 0u);
        if (component.sheet.empty()) {
            // a legacy sprite turning into a sheet sprite: each frame at its size, from its pivot
            multi->addPropertyCmd<unsigned int>(project, sceneId, entity, cpType, "width", 0u);
            multi->addPropertyCmd<unsigned int>(project, sceneId, entity, cpType, "height", 0u);
            multi->addPropertyCmd<PivotPreset>(project, sceneId, entity, cpType, "pivotPreset", PivotPreset::FRAME);
        }
        multi->setNoMerge();
        if (sceneHistory->addCommand(multi)) histories.push_back(sceneHistory);
        if (histories.size() > 1) UndoTimeline::link(histories);
        Out::info("Sliced %s into its Sprite Mode", texture.c_str());
    });
}

void editor::InspectorWindow::drawLegacyFramesHeader(ComponentType cpType, SceneProject* sceneProject, Entity entity) {
    (void)cpType;
    const SpriteComponent& sprite = sceneProject->scene->getComponent<SpriteComponent>(entity);
    separatorText("Inline Frames (legacy)");
    ImGui::TextDisabled("%u frame(s) stored in this Sprite", sprite.numFramesRect);
    ImGui::BeginDisabled(sprite.numFramesRect == 0);
    if (ImGui::Button(ICON_FA_ARROW_RIGHT_ARROW_LEFT " Convert to Sprite Sheet", ImVec2(-FLT_MIN, 0))) {
        sprite::LegacySprite legacy;
        sprite::ConvertPlan plan;
        pixelart::PixelProblem problem;
        if (!sprite::legacySpriteOf(*project, sceneProject->id, entity, legacy, problem) || !sprite::planConversion(*project, legacy, true, plan, problem)) {
            Out::warning("Convert to Sprite Sheet: %s (%s)", problem.message.c_str(), problem.fix.c_str());
            legacyConvertError = problem.message;
        } else {
            auto report = std::make_shared<sprite::ConvertReport>();
            Command* command = new sprite::ConvertInlineFramesCmd(project, plan, report);
            command->setNoMerge();
            if (project->getProjectCommandHistory()->addCommand(command)) {
                legacyConvertError.clear();
                Out::info("Converted %d sprite(s) to frames of %s's Sprite Mode", report->converted, plan.texture.c_str());
            } else {
                legacyConvertError = report->error;
            }
        }
    }
    ImGui::EndDisabled();
    UIProbe::item("inspector.sprite.convertToSheet");
    ImGui::SetItemTooltip("Moves these frames into the texture's Sprite Mode (Multiple, names and order kept); every sprite of the project "
                          "using the texture with the same frames converts too, one undo step");
    if (!legacyConvertError.empty()) ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", legacyConvertError.c_str());
}
