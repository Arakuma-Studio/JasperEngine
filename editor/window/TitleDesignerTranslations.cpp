// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Designer's Translations panel (docs/specs/S87-title-localisation-interchange.md R54):
// one row per text layer (Key, Layer, Source, Max, a column per language), cells red when missing
// and orange when stale with the fit scale and an overflow mark, filters All / Missing / Stale /
// Overflow, inline editing (one committed cell is one undo step), the cell menu (Copy Source, Mark
// Reviewed, Revert), Import... and Export... (CSV, XLIFF), and a double-click that selects the
// layer and shows that language on the canvas.

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleFit.h"
#include "titleart/TitleTranslations.h"
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitleDesignerState.h"
#include "titleart/localisation/TitleLanguageEditing.h"
#include "titleart/localisation/TitleLanguageOps.h"
#include "titleart/localisation/TitleStrings.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    enum class Filter : int { All, Missing, Stale, Overflow };

    struct PanelState {
        Filter filter = Filter::All;
        std::string message;
        // the cell being edited: its text until it is committed
        std::string editKey;
        char buffer[2048] = {0};
    };

    PanelState& panel() {
        static PanelState p;
        return p;
    }

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    struct Row {
        const ta::Json* layer = nullptr;
        std::string id, name, key;
    };

    void collect(const ta::Json& root, const ta::Json& list, std::vector<Row>& out) {
        for (const ta::Json& layer : list) {
            if (ta::layerTypeOf(layer) == ta::LayerType::Text && layer.value("translate", true) && !str(layer, "text").empty()) {
                out.push_back({&layer, str(layer, "id"), str(layer, "name"), titleStringKey(root, layer)});
            }
            if (layer.contains("children")) collect(root, layer["children"], out);
        }
    }

    bool commit(TitleDesignerState& state, const ta::Json& ops, const std::string& name) {
        std::string error;
        if (!state.commit(ops, name, &error)) {
            panel().message = error;
            return false;
        }
        panel().message.clear();
        return true;
    }

    void exportStrings(TitleDesignerState& state, bool xliff) {
        Project* project = state.project();
        if (!project || !state.document()) return;
        const fs::path document = state.document()->absolutePath();
        std::error_code ec;
        const std::string relative = fs::relative(document, project->getProjectPath(), ec).generic_string();
        const std::vector<TitleStringsDocument> docs = {{relative, &state.committed()}};
        if (!xliff) {
            const std::string file = FileDialogs::saveFileDialog(document.parent_path().string(), document.stem().string() + ".csv");
            if (file.empty()) return;
            const bool written = FileUtils::writeFileReplacing(FileUtils::pathFromUtf8(file), exportTitleStringsCsv(docs, {}));
            panel().message = (written ? "Exported " : "Couldn't write ") + fs::path(file).filename().string();
            return;
        }
        const std::string dir = FileDialogs::openFileDialog(document.parent_path().string(), FILE_DIALOG_ALL, true);
        if (dir.empty()) return;
        size_t count = 0;
        for (const auto& [code, text] : exportTitleStringsXliff(docs, {})) {
            std::string lower = code;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            const fs::path target = FileUtils::pathFromUtf8(dir) / (document.stem().string() + "." + lower + ".xlf");
            if (!FileUtils::writeFileReplacing(target, text)) {
                panel().message = "Couldn't write " + target.filename().string();
                return;
            }
            count++;
        }
        panel().message = "Exported " + std::to_string(count) + " XLIFF file(s)";
    }

    void importStrings(TitleDesignerState& state) {
        const std::string file = FileDialogs::openFileDialogFiltered("", "Strings", "csv,xlf,xliff");
        if (file.empty()) return;
        std::string error;
        if (!state.commit(ta::Json{{"op", "import_strings"}, {"file", file}}, "Import Strings (" + fs::path(file).filename().string() + ")", &error)) {
            panel().message = error;
            return;
        }
        const ta::Json report = state.lastResult().reports.value("0", ta::Json::object());
        panel().message = "Imported " + std::to_string(report.value("applied", 0)) + " translation(s), " +
                          std::to_string(report.value("notes", ta::Json::array()).size()) + " note(s)";
    }

    void drawPanel(TitleDesignerState& state) {
        if (!state.isOpen()) return;
        const ta::TitleArtDoc& doc = state.committed();
        PanelState& p = panel();
        if (!ta::titleIsMultilingual(doc.root)) {
            ImGui::TextDisabled("No languages yet: add one with the toolbar's Languages... button.");
            return;
        }
        const char* filters[] = {"All", "Missing", "Stale", "Overflow"};
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(filters[i], p.filter == (Filter)i)) p.filter = (Filter)i;
        }
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::BeginDisabled(!state.editable());
        if (ImGui::Button("Import...")) importStrings(state);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Export...")) ImGui::OpenPopup("##stringsExport");
        if (ImGui::BeginPopup("##stringsExport")) {
            if (ImGui::MenuItem("CSV...")) exportStrings(state, false);
            if (ImGui::MenuItem("XLIFF 1.2 (one per language)...")) exportStrings(state, true);
            ImGui::EndPopup();
        }
        if (!p.message.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", p.message.c_str());
        }
        std::vector<std::string> languages;
        for (const std::string& code : ta::titleLanguageCodes(doc.root)) {
            if (!ta::isSourceLanguage(doc.root, code)) languages.push_back(code);
        }
        std::vector<Row> rows;
        collect(doc.root, doc.root["layers"], rows);
        const ta::AssetResolver* assets = TitleArtRenderHost::editor().resolver(state.project());
        // the fit of each layer per language (cached by TitleFit)
        std::map<std::string, ta::TitleArtDoc> resolved;
        for (const std::string& code : languages) resolved[code] = ta::resolveTitleLanguage(doc, code);
        const int columns = 4 + (int)languages.size();
        if (!ImGui::BeginTable("##titleTranslations", columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 260.0f))) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Layer");
        ImGui::TableSetupColumn(("Source (" + ta::titleSourceLanguage(doc.root) + ")").c_str());
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 36.0f);
        for (const std::string& code : languages) ImGui::TableSetupColumn(code.c_str());
        ImGui::TableHeadersRow();
        for (const Row& row : rows) {
            const ta::Json& layer = *row.layer;
            const std::string source = str(layer, "text");
            const std::string hash = ta::translationSourceHash(source);
            // the filter: a row shows when one of its cells matches
            bool missingAny = false, staleAny = false, overAny = false;
            std::map<std::string, std::pair<double, bool>> fits;
            for (const std::string& code : languages) {
                const ta::Json* t = ta::findTranslation(layer, code);
                if (!ta::layerShownInLanguage(layer, code)) continue;
                if (!ta::translationHasText(t)) missingAny = true;
                else if (str(*t, "status") == "needs_review" || str(*t, "source_hash") != hash) staleAny = true;
                const ta::TitleArtDoc& r = resolved[code];
                if (const ta::Json* rl = ta::findLayer(r.root, row.id)) {
                    const ta::TitleFitResult fit = ta::computeTitleFit(*rl, r, assets);
                    if (fit.active) {
                        fits[code] = {fit.scale * fit.hScale, !fit.fits};
                        overAny = overAny || !fit.fits;
                    }
                }
            }
            if ((p.filter == Filter::Missing && !missingAny) || (p.filter == Filter::Stale && !staleAny) || (p.filter == Filter::Overflow && !overAny)) continue;
            ImGui::PushID(row.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(row.key.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                state.selectOne(NodeRef{NodeKind::Layer, row.id, "", ""});
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(source.c_str());
            ImGui::TableNextColumn();
            if (layer.value("max_chars", 0) > 0) ImGui::Text("%d", layer.value("max_chars", 0));
            for (const std::string& code : languages) {
                ImGui::TableNextColumn();
                ImGui::PushID(code.c_str());
                const ta::Json* t = ta::findTranslation(layer, code);
                const bool has = ta::translationHasText(t);
                const bool stale = has && (str(*t, "status") == "needs_review" || str(*t, "source_hash") != hash);
                if (!ta::layerShownInLanguage(layer, code)) {
                    ImGui::TextDisabled("(hidden)");
                    ImGui::PopID();
                    continue;
                }
                if (!has) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(150, 40, 40, 110));
                else if (stale) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(180, 110, 20, 110));
                const std::string cellKey = row.id + "|" + code;
                if (p.editKey != cellKey) std::snprintf(p.buffer, sizeof(p.buffer), "%s", has ? str(*t, "text").c_str() : "");
                ImGui::SetNextItemWidth(-48.0f);
                ImGui::BeginDisabled(!state.editable());
                char local[2048];
                std::snprintf(local, sizeof(local), "%s", p.editKey == cellKey ? p.buffer : (has ? str(*t, "text").c_str() : ""));
                if (ImGui::InputTextWithHint("##cell", source.c_str(), local, sizeof(local))) {
                    p.editKey = cellKey;
                    std::snprintf(p.buffer, sizeof(p.buffer), "%s", local);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    // one committed cell is one undo step
                    commit(state, ta::Json{{"op", "set_translation"}, {"layer", "id:" + row.id}, {"language", code}, {"text", std::string(p.buffer)}},
                           "Set Translation (" + row.name + ": " + code + ")");
                    p.editKey.clear();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    state.selectOne(NodeRef{NodeKind::Layer, row.id, "", ""});
                    state.setLanguage(code);
                }
                if (ImGui::BeginPopupContextItem("##cellmenu")) {
                    if (ImGui::MenuItem("Copy Source")) ImGui::SetClipboardText(source.c_str());
                    if (ImGui::MenuItem("Mark Reviewed", nullptr, false, stale)) {
                        commit(state, ta::Json{{"op", "mark_reviewed"}, {"language", code}, {"layers", ta::Json::array({"id:" + row.id})}},
                               "Mark Translations Reviewed (" + code + ")");
                    }
                    if (ImGui::MenuItem("Revert", nullptr, false, t != nullptr)) {
                        commit(state, ta::Json{{"op", "revert_translation"}, {"layer", "id:" + row.id}, {"language", code}, {"fields", ta::Json::array({"all"})}},
                               "Revert Translation (" + row.name + ": " + code + ")");
                    }
                    ImGui::EndPopup();
                }
                auto fit = fits.find(code);
                if (fit != fits.end()) {
                    ImGui::SameLine();
                    if (fit->second.second) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION);
                    else ImGui::TextDisabled("%.2f", fit->second.first);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(fit->second.second ? "Overflows its fit box" : "Fit scale");
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

}

void editor::registerTitleDesignerTranslations() {
    PanelDef def;
    def.id = "s87.translations";
    def.title = "Translations";
    def.region = PanelDef::Region::Bottom;
    def.draw = drawPanel;
    def.visibleByDefault = [](const TitleDesignerState& state) { return state.isOpen() && ta::titleIsMultilingual(state.committed().root); };
    addPanel(std::move(def));
}
