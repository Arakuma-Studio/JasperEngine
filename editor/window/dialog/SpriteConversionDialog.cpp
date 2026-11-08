// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteConversionDialog.h"

#include "sprite/SpriteMigration.h"

#include "Out.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "external/IconsFontAwesome6.h"
#include "window/EditorModal.h"

#include "imgui.h"

using namespace jasperengine;
using namespace jasperengine::editor;

struct SpriteConversionDialog::Scan {
    std::map<std::string, std::vector<sprite::LegacySprite>> byTexture;
};

void SpriteConversionDialog::open(Project* newProject) {
    project = newProject;
    openRequested = true;
    results.clear();
    scan();
}

void SpriteConversionDialog::scan() {
    found = std::make_shared<Scan>();
    auto& byTexture = found->byTexture;
    if (!project) return;
    for (sprite::LegacySprite& legacy : sprite::findLegacySprites(*project)) byTexture[legacy.texture].push_back(std::move(legacy));
}

void SpriteConversionDialog::show() {
    const char* title = ICON_FA_ARROW_RIGHT_ARROW_LEFT " Convert Inline Sprite Frames##SpriteConversion";
    if (openRequested) {
        ImGui::OpenPopup(title);
        openRequested = false;
        visible = true;
    }
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40.0f, 0), ImGuiCond_Appearing);
    bool keep = true;
    if (!EditorModal::begin(title, &keep, ImGuiWindowFlags_NoSavedSettings)) {
        visible = false;
        return;
    }
    if (!found) found = std::make_shared<Scan>();
    auto& byTexture = found->byTexture;
    size_t total = 0;
    for (const auto& [texture, sprites] : byTexture) total += sprites.size();
    EditorModal::message(std::to_string(total) + " legacy sprite(s) carry their own frames; converting moves them into their texture's Sprite Mode");
    ImGui::TextWrapped("%zu legacy sprite(s) on %zu texture(s). Converting a texture moves the frames into its Sprite Mode (Multiple, names and "
                       "order kept) and makes every sprite that uses it with the same frames show them by index: clip keys and Sprite "
                       "Animations keep working. Nothing changes until you convert (one undo step per texture).",
                       total, byTexture.size());
    ImGui::Separator();
    ImGui::BeginChild("legacy_list", ImVec2(0, ImGui::GetFontSize() * 18.0f), true);
    std::string convertTexture;
    for (const auto& [texture, sprites] : byTexture) {
        ImGui::PushID(texture.c_str());
        const bool open = ImGui::TreeNodeEx("##texture", ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu sprite(s), %zu frames)",
                                            texture.empty() ? "(no texture)" : texture.c_str(), sprites.size(), sprites[0].frames.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(texture.empty() || results.count(texture));
        if (ImGui::SmallButton("Convert")) convertTexture = texture;
        ImGui::EndDisabled();
        if (open) {
            for (const sprite::LegacySprite& s : sprites) {
                ImGui::BulletText("%s: %s (%zu frames)%s", s.owner.c_str(), s.entityPath.c_str(), s.frames.size(), s.open ? " (open)" : "");
            }
            ImGui::TreePop();
        }
        auto result = results.find(texture);
        if (result != results.end()) ImGui::TextDisabled("%s", result->second.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    bool convertAll = EditorModal::button("Convert All", "convert_all");
    ImGui::SameLine();
    if (EditorModal::button("Close", "cancel")) {
        keep = false;
    }
    auto convert = [&](const std::string& texture) {
        auto it = byTexture.find(texture);
        if (it == byTexture.end() || it->second.empty() || results.count(texture)) return;
        sprite::ConvertPlan plan;
        pixelart::PixelProblem problem;
        if (!sprite::planConversion(*project, it->second[0], true, plan, problem)) {
            results[texture] = problem.kind + ": " + problem.message;
            return;
        }
        auto report = std::make_shared<sprite::ConvertReport>();
        Command* command = new sprite::ConvertInlineFramesCmd(project, plan, report);
        command->setNoMerge();
        if (project->getProjectCommandHistory()->addCommand(command)) {
            results[texture] = "Converted " + std::to_string(report->converted) + " sprite(s)";
            Out::info("Converted %d sprite(s) of %s to its Sprite Mode", report->converted, texture.c_str());
        } else {
            results[texture] = "Failed: " + report->error;
        }
    };
    if (!convertTexture.empty()) convert(convertTexture);
    if (convertAll) {
        for (const auto& [texture, sprites] : byTexture) {
            if (!texture.empty()) convert(texture);
        }
    }
    if (!keep) {
        visible = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
