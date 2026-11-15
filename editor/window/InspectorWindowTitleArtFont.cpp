// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Inspector's view of a font file (.ttf, .otf, .ttc): what Title Art reads of it at export
// (docs/specs/S81-title-art-runtime.md R56, R57): the family, glyphs, whether it is variable, its
// copyright and licence, the OS/2 embedding permission and what a build does with it, and the
// checkbox "I have a licence that permits embedding this font in a game"
// (import.font.embeddingAcknowledged in its .meta, one undo step per toggle).

#include "InspectorWindow.h"

#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "external/IconsFontAwesome6.h"
#include "titleart/runtime/TitleFontSubsetter.h"
#include "imgui.h"

#include <cstdio>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct FontInspectorState {
        fs::path asset;
        std::string stamp;              // size and time of the file the info was read from
        bool read = false;
        TitleFontInfo info;
        std::string error;
        std::string metaStamp;          // the .meta's, so the acknowledgement is read when it changes
        bool acknowledged = false;
    };

    FontInspectorState& fontState() {
        static FontInspectorState state;
        return state;
    }

    std::string stampOf(const fs::path& file) {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        const auto time = fs::last_write_time(file, ec);
        return ec ? std::string() : std::to_string(size) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
    }

    void row(const char* label, const std::string& value) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(value.empty() ? "-" : value.c_str());
        ImGui::PopTextWrapPos();
    }

    std::string permissionText(uint16_t fsType) {
        if (fsType & TITLE_FS_TYPE_RESTRICTED) return "Restricted License (no embedding)";
        if (fsType & TITLE_FS_TYPE_BITMAP_ONLY) return "Bitmap embedding only";
        std::string text = (fsType & 0x0004) ? "Preview & Print" : (fsType & 0x0008) ? "Editable" : "Installable";
        if (fsType & TITLE_FS_TYPE_NO_SUBSETTING) text += ", no subsetting";
        return text;
    }

}

void editor::InspectorWindow::drawFontAsset(const fs::path& asset) {
    FontInspectorState& state = fontState();
    drawAssetHeader(ICON_FA_FONT, asset, "Font");
    const std::string stamp = stampOf(asset);
    if (state.asset != asset || state.stamp != stamp) {
        state = FontInspectorState();
        state.asset = asset;
        state.stamp = stamp;
        std::ifstream file(asset, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        state.read = readTitleFontInfo(bytes, 0, state.info, state.error);
    }
    if (!state.read) {
        ImGui::TextColored(Theme::Colors::ErrorText, "%s", state.error.c_str());
        return;
    }
    const TitleFontInfo& info = state.info;
    if (ImGui::BeginTable("font_info", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        row("Family", info.family);
        row("Glyphs", std::to_string(info.glyphCount) + (info.variable ? " (variable)" : ""));
        row("Copyright", info.copyright);
        row("Licence", info.ofl ? "SIL Open Font License" + std::string(info.reservedFontName ? " (with a Reserved Font Name)" : "")
                                : info.license);
        row("Licence URL", info.licenseUrl);
        char fsType[16];
        std::snprintf(fsType, sizeof(fsType), "0x%04X", info.fsType);
        row("Embedding", permissionText(info.fsType) + " (fsType " + fsType + ")");
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Title Art (Realtime and Glyph Atlas titles ship their fonts)");
    const std::string metaStamp = stampOf(SetAssetMetaCmd::metaPathOf(asset));
    if (metaStamp != state.metaStamp || state.metaStamp.empty()) {
        state.metaStamp = metaStamp;
        state.acknowledged = titleFontEmbeddingAcknowledged(asset);
    }
    const bool acknowledged = state.acknowledged;
    bool value = acknowledged;
    if (ImGui::Checkbox(TITLE_FONT_ACK_LABEL, &value) && value != acknowledged) {
        // unticked removes the key: false is the default, and old metas have none
        auto* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(asset), TITLE_FONT_ACK_META_KEY,
                                            value ? YAML::Node(true) : YAML::Node());
        command->setAction(value ? "Acknowledge Font Embedding Licence" : "Clear Font Embedding Licence");
        command->setNoMerge();
        if (project->getProjectCommandHistory()->addCommand(command)) state.metaStamp.clear();
    }
    ImGui::SetItemTooltip("Only for a font whose OS/2 permission is Restricted License: exports ship it with Realtime and "
                          "Glyph Atlas titles when you have a licence that allows it (import.font.embeddingAcknowledged).");
    const TitleFontEmbeddingCheck check = checkTitleFontEmbedding(info, value, project->normalizeToProjectRelative(asset).generic_string());
    ImGui::PushTextWrapPos(0.0f);
    if (!check.ok) {
        ImGui::TextColored(Theme::Colors::ErrorText, "%s", check.message.c_str());
    } else if (!check.code.empty()) {
        ImGui::TextColored(Theme::Colors::WarningText, "%s", check.message.c_str());
    } else {
        ImGui::TextDisabled("%s", info.ofl && info.reservedFontName
            ? "Builds subset it to each title's Characters, renamed as its licence asks of a modified font, and credit it in licenses/THIRD-PARTY-FONTS.txt."
            : "Builds subset it to each title's Characters and credit it in licenses/THIRD-PARTY-FONTS.txt.");
    }
    ImGui::PopTextWrapPos();
}
