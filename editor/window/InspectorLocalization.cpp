// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's localisation rows (docs/specs/game-ui-localization.md R16, R32): Text's
// Localization Key, typed or picked from every String Table key (search over keys and source
// texts) with a "missing" badge when no table has it, one undo step per change; and a UI
// Document's read-only Localization foldout in Play: the language, its keyed elements, the literal
// text no key produced and this Play's misses.

#include "InspectorWindow.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/TextComponent.h"
#include "external/IconsFontAwesome6.h"
#include "htmlui/HtmlL10n.h"
#include "localization/Localization.h"
#include "localization/StringTableCook.h"
#include "util/UIProbe.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    bool ilContains(const std::string& haystack, const char* needle) {
        if (!needle[0]) return true;
        std::string a = haystack;
        std::string b = needle;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a.find(b) != std::string::npos;
    }

    void ilCommit(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& key) {
        auto multi = new MultiPropertyCmd();
        for (Entity entity : entities)
            multi->addPropertyCmd<std::string>(project, sceneProject->id, entity, ComponentType::TextComponent, "localizationKey", key);
        CommandHandle::get(sceneProject->id)->addCommand(multi);
        multi->setNoMerge();
    }

}

void editor::InspectorWindow::drawTextLocalizationRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities) {
    const TextComponent& text = sceneProject->scene->getComponent<TextComponent>(entities[0]);
    beginTable(cpType, getLabelSize("Localization Key"), "localization");
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Localization Key");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A String Table key (ns.key): in Play and games the text shows its message in the game language; "
                          "Text stays the fallback and what the scene saves.");
    }
    ImGui::TableSetColumnIndex(1);

    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s", text.localizationKey.c_str());
    const float pickWidth = ImGui::GetFrameHeight() * 1.6f;
    ImGui::SetNextItemWidth(-pickWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##textLocalizationKey", "ui.menu.play", buffer, sizeof(buffer));
    UIProbe::item("text.localizationKey");
    if (ImGui::IsItemDeactivatedAfterEdit() && text.localizationKey != buffer) ilCommit(project, sceneProject, entities, buffer);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LANGUAGE "##textLocalizationPick")) {
        // the keys as they are now
        localizationKeys.clear();
        for (const l10n::StringTableSet& set : l10n::loadProjectTables(project->getProjectPath()).sets) {
            for (const l10n::TableEntry& entry : set.table.entries) localizationKeys.emplace_back(set.table.ns + "." + entry.key, entry.text);
        }
        localizationKeySearch[0] = '\0';
        ImGui::OpenPopup("##textLocalizationKeys");
    }
    ImGui::SetItemTooltip("Pick a key of the project's String Tables");
    if (ImGui::BeginPopup("##textLocalizationKeys")) {
        ImGui::SetNextItemWidth(Theme::dpi(260.0f));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##textLocalizationSearch", "Search keys and texts", localizationKeySearch, sizeof(localizationKeySearch));
        if (localizationKeys.empty()) ImGui::TextDisabled("No String Tables: Create > Localization > String Table.");
        ImGui::BeginChild("##textLocalizationList", ImVec2(Theme::dpi(420.0f), Theme::dpi(220.0f)));
        int shown = 0;
        for (const auto& [key, source] : localizationKeys) {
            if (!ilContains(key, localizationKeySearch) && !ilContains(source, localizationKeySearch)) continue;
            if (++shown > 500) break;
            const std::string label = key + "  " + source + "##" + key;
            if (ImGui::Selectable(label.c_str(), key == text.localizationKey)) {
                ilCommit(project, sceneProject, entities, key);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();
        if (!text.localizationKey.empty() && ImGui::Button("Clear Key")) {
            ilCommit(project, sceneProject, entities, "");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    endTable();

    // the badge: a key no table has keeps showing Text as written (the keys read again every 2 s)
    if (!text.localizationKey.empty()) {
        static double readAt = -10.0;
        if (ImGui::GetTime() - readAt > 2.0) {
            readAt = ImGui::GetTime();
            localizationKeys.clear();
            for (const l10n::StringTableSet& set : l10n::loadProjectTables(project->getProjectPath()).sets) {
                for (const l10n::TableEntry& entry : set.table.entries) localizationKeys.emplace_back(set.table.ns + "." + entry.key, entry.text);
            }
        }
        const bool found = std::any_of(localizationKeys.begin(), localizationKeys.end(), [&](const auto& k) { return k.first == text.localizationKey; });
        if (!found) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " missing: no String Table has %s", text.localizationKey.c_str());
        }
    }
}

void editor::InspectorWindow::drawUIDocumentLocalization(SceneProject* sceneProject, Entity entity) {
    if (!project->isAnyScenePlaying() || !Localization::isActive()) return;
    if (!ImGui::CollapsingHeader("Localization##uidocument_l10n")) return;
    const jasperengine::htmlui::HtmlL10n::DocumentReport report = jasperengine::htmlui::HtmlL10n::reportOf(sceneProject->scene, entity);
    ImGui::Text("Language %s: %zu keyed elements, %zu literal texts, %zu misses", report.language.c_str(), report.keyed, report.literal.size(),
                report.missing.size());
    const ImVec4 warning(0.95f, 0.70f, 0.20f, 1.0f);
    size_t shown = 0;
    for (const auto& [where, value] : report.literal) {
        if (++shown > 20) break;
        ImGui::TextColored(warning, "literal");
        ImGui::SameLine();
        ImGui::TextWrapped("%s: \"%s\"", where.c_str(), value.c_str());
    }
    shown = 0;
    for (const Localization::Miss& miss : report.missing) {
        if (++shown > 20) break;
        ImGui::TextColored(warning, "%s", miss.kind.c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s: %s (%s, %u)", miss.where.c_str(), miss.key.c_str(), miss.language.c_str(), miss.count);
    }
}
