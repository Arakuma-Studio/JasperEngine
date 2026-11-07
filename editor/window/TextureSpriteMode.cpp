// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The texture importer's Sprite Mode (docs/specs/S45-sprite-atlas.md R10, R11, R31): Single /
// Grid / Multiple, Slice... (the Texture Slicer on the texture's `import.spriteSheet`), the frame
// table (name, rect, pivot, border), "Grid from file name (16x16)" for name@WxH files, and
// "Packed in: <atlas> (page n)" with a link. Each change is a project undo step of the meta.

#include "TextureImporterView.h"

#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "external/IconsFontAwesome6.h"
#include "sprite/SpriteAtlasBuilder.h"
#include "sprite/SpriteSheetBuilder.h"
#include "util/UIProbe.h"
#include "window/ProjectWindow.h"

#include "imgui.h"

namespace fs = std::filesystem;

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    struct SpriteModeCache {
        sprite::TextureSheetInfo info;
        sprite::TextureSheetBuild built;
    };

    std::string stampOf(const fs::path& path) {
        std::error_code ec;
        const auto time = fs::last_write_time(path, ec);
        return ec ? std::string("-") : std::to_string(time.time_since_epoch().count());
    }
}

void TextureImporterView::drawSpriteMode(const std::string& assetPath) {
    const fs::path absolute = project->getAssetsPath() / assetPath;
    // read again only when the texture or its meta changed (the Inspector draws every frame)
    const std::string key = assetPath + "|" + stampOf(absolute) + "|" + stampOf(SetAssetMetaCmd::metaPathOf(absolute));
    if (key != spriteModeKey || !spriteModeCache) {
        auto cache = std::make_shared<SpriteModeCache>();
        cache->info = sprite::textureSheetSettings(absolute);
        if (cache->info.isSheet()) cache->built = sprite::buildTextureSheet(absolute, assetPath, cache->info.settings);
        spriteModeCache = cache;
        spriteModeKey = key;
    }
    const SpriteModeCache& cached = *static_cast<const SpriteModeCache*>(spriteModeCache.get());
    const sprite::TextureSheetInfo& info = cached.info;
    auto write = [this, absolute, assetPath](const TextureSheetSettings* settings) {
        Command* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(absolute), sprite::kSpriteSheetKey,
                                               settings ? sprite::sheetSettingsToYaml(*settings) : YAML::Node());
        if (command->isNoOp()) {
            delete command;
            return;
        }
        command->setNoMerge();
        if (!project->getProjectCommandHistory()->addCommand(command)) Out::error("Couldn't write the Sprite Mode of %s", assetPath.c_str());
    };

    if (!ImGui::CollapsingHeader("Sprite Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
        slicer.show();
        return;
    }
    UIProbe::item("textureImporter.spriteMode");
    ImGui::BeginDisabled(info.readOnly);
    int mode = static_cast<int>(info.settings.mode);
    const char* modes[] = {"Single", "Grid", "Multiple"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::Combo("Sprite Mode", &mode, modes, 3) && mode != static_cast<int>(info.settings.mode)) {
        if (mode == 0) {
            // Single: the meta's sheet goes (a name@WxH file is a grid again by its name)
            write(nullptr);
        } else {
            TextureSheetSettings next = info.settings;
            next.mode = static_cast<TextureSpriteMode>(mode);
            if (next.mode == TextureSpriteMode::GRID && next.grid.cellWidth <= 0) {
                next.grid.cellWidth = info.cellWidth > 0 ? info.cellWidth : 16;
                next.grid.cellHeight = info.cellHeight > 0 ? info.cellHeight : 16;
            }
            write(&next);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_GRIP " Slice...")) {
        slicer.openTextureSheet(Texture(assetPath), absolute.string(), info.settings, [write](const TextureSheetSettings& settings) { write(&settings); });
    }
    UIProbe::item("textureImporter.slice");
    ImGui::EndDisabled();
    if (info.readOnly) ImGui::TextColored(Theme::Colors::WarningText, "Sprite Mode from a newer editor: read only here.");
    if (info.source == sprite::TextureSheetInfo::Source::FileName) {
        ImGui::TextDisabled("Grid from file name (%d\xC3\x97%d)", info.cellWidth, info.cellHeight);
        UIProbe::item("textureImporter.gridFromFileName");
        ImGui::SetItemTooltip("name@WxH: a grid of W x H cells, empty cells skipped; nothing is written until you change the Sprite Mode");
    }
    for (const pixelart::PixelProblem& p : info.problems) ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", p.message.c_str());

    if (info.isSheet()) {
        const sprite::TextureSheetBuild& built = cached.built;
        for (const std::string& w : built.warnings) ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", w.c_str());
        ImGui::TextDisabled("%zu frames", built.sheet.frames.size());
        if (ImGui::BeginTable("sprite_mode_frames", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, ImGui::GetFontSize() * 10.0f))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Rect");
            ImGui::TableSetupColumn("Pivot");
            ImGui::TableSetupColumn("Border");
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(built.sheet.frames.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    const SpriteFrame& f = built.sheet.frames[static_cast<size_t>(i)];
                    Rect r = f.rect;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(f.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.0f, %.0f, %.0f, %.0f", r.getX(), r.getY(), r.getWidth(), r.getHeight());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.0f, %.0f", f.pivot.x, f.pivot.y);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.0f, %.0f, %.0f, %.0f", f.border.x, f.border.y, f.border.z, f.border.w);
                }
            }
            ImGui::EndTable();
        }
    }

    // R31: the atlas that packs it, with a link
    int page = 0;
    const std::string atlas = sprite::SpriteAtlasBuilder::get().packedIn(assetPath, &page);
    if (!atlas.empty()) {
        const std::string line = "Packed in: " + atlas + " (page " + std::to_string(page) + ")";
        if (ImGui::SmallButton((std::string(ICON_FA_TABLE_CELLS " ") + line).c_str())) {
            if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) projectWindow->revealAsset(project->getAssetsPath() / atlas);
        }
        UIProbe::item("textureImporter.packedIn");
    }
    slicer.show();
}
