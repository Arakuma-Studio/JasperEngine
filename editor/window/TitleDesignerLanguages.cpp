// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Designer's languages (docs/specs/S87-title-localisation-interchange.md R51-R53, R55):
// the toolbar's Language switcher with each language's status, Edit Languages (add from the 60
// most used game languages or a free BCP 47 tag, names, fonts, Bake, Copy From, remove with the
// count of translations lost, the project's languages, the source with its swap), Compare
// Languages (every language side by side through the preview queue, visible ones first), the
// Language section of a text layer (its overrides in the selected language, Revert to Source and
// Apply to Source), and the canvas overlay (the fit box dashed, an overflow in red, the language
// badge). Every change is one named undo step through the document's ops.

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleFit.h"
#include "titleart/TitleTranslations.h"
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitleDesignerState.h"
#include "titleart/designer/TitleLayerThumbnails.h"
#include "titleart/designer/TitlePages.h"
#include "titleart/localisation/TitleLanguageEditing.h"
#include "titleart/localisation/TitleLanguageNames.h"
#include "titleart/localisation/TitleLanguageReport.h"
#include "util/TitleArtLanguage.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    constexpr TitlePreviewOwner COMPARE_OWNER = 0x4c414e47ull;     // "LANG"

    // the last refused language edit (the dialogs show it)
    std::string& lastError() {
        static std::string error;
        return error;
    }

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // the states of the open document, measured again when it changes (overflow is geometry)
    struct StatesCache {
        const TitleDesignerState* state = nullptr;
        uint64_t change = ~uint64_t(0);
        std::vector<TitleLanguageState> states;
    };

    const std::vector<TitleLanguageState>& statesOf(TitleDesignerState& state) {
        static StatesCache cache;
        if (cache.state != &state || cache.change != state.changeCount()) {
            cache.state = &state;
            cache.change = state.changeCount();
            Project* project = state.project();
            cache.states = titleLanguageStates(state.committed(), TitleArtRenderHost::editor().resolver(project), project,
                                               state.document() ? state.document()->absolutePath() : std::filesystem::path());
        }
        return cache.states;
    }

    bool commit(TitleDesignerState& state, const ta::Json& ops, const std::string& name) {
        std::string error;
        if (!state.commit(ops, name, &error)) {
            lastError() = error;
            return false;
        }
        lastError().clear();
        return true;
    }

    std::string statusText(const TitleLanguageState& s) {
        if (s.source) return "source";
        std::string out;
        if (s.missing) out += "missing " + std::to_string(s.missing);
        if (s.stale) out += std::string(out.empty() ? "" : ", ") + "stale " + std::to_string(s.stale);
        if (s.overflow) out += std::string(out.empty() ? "" : ", ") + "overflow " + std::to_string(s.overflow);
        return out.empty() ? "complete" : out;
    }

    // ---- the dialogs' state ----

    struct Dialogs {
        bool editOpen = false;
        bool compareOpen = false;
        char search[64] = {0};
        char tag[64] = {0};
        std::string removeCode;
        bool swapSource = true;
        std::unique_ptr<TitleLayerThumbnails> thumbnails;
    };

    Dialogs& dialogs() {
        static Dialogs d;
        return d;
    }

    // ---- the switcher (R51) ----

    void drawSwitcher(TitleDesignerState& state) {
        if (!state.isOpen()) return;
        const ta::TitleArtDoc& doc = state.committed();
        if (!ta::titleIsMultilingual(doc.root)) {
            if (ImGui::Button(ICON_FA_LANGUAGE " Languages...")) dialogs().editOpen = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Translate this title: add languages (Edit Languages)");
            return;
        }
        const std::vector<TitleLanguageState>& states = statesOf(state);
        const std::string current = state.language().empty() ? ta::titleSourceLanguage(doc.root) : state.language();
        std::string label = "Language: " + titleLanguageLabel(current);
        for (const TitleLanguageState& s : states) {
            if (s.code == current) label += " \xc2\xb7 " + std::string(s.source ? "Source" : statusText(s));
        }
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("##titleLanguage", label.c_str())) {
            for (const TitleLanguageState& s : states) {
                const bool complete = !s.source && !s.missing && !s.stale && !s.overflow;
                const char* icon = s.source ? ICON_FA_STAR : complete ? ICON_FA_CHECK : ICON_FA_TRIANGLE_EXCLAMATION;
                const std::string item = std::string(icon) + " " + titleLanguageLabel(s.code, s.name) + "  (" + statusText(s) + ")###" + s.code;
                if (ImGui::Selectable(item.c_str(), s.code == current)) state.setLanguage(s.source ? std::string() : s.code);
            }
            ImGui::Separator();
            if (ImGui::Selectable("Edit Languages...")) dialogs().editOpen = true;
            if (ImGui::Selectable("Compare Languages")) dialogs().compareOpen = true;
            ImGui::EndCombo();
        }
    }

    // ---- Edit Languages (R53) ----

    void drawEditLanguages(TitleDesignerState& state) {
        Dialogs& d = dialogs();
        if (d.editOpen && !ImGui::IsPopupOpen("Edit Languages###titleEditLanguages")) ImGui::OpenPopup("Edit Languages###titleEditLanguages");
        ImGui::SetNextWindowSize(ImVec2(620.0f, 520.0f), ImGuiCond_Appearing);
        if (!EditorModal::begin("Edit Languages###titleEditLanguages", &d.editOpen)) return;
        EditorModal::message("The document's languages: its source, the languages it holds, and adding or removing one");
        const ta::TitleArtDoc& doc = state.committed();
        const std::string source = ta::titleSourceLanguage(doc.root);
        const bool editable = state.editable();
        ImGui::BeginDisabled(!editable);

        // the source, with the swap (D6)
        ImGui::Text("Source Language: %s", source.empty() ? "(none: the first language added sets it)" : titleLanguageLabel(source).c_str());
        if (!source.empty() && doc.root.contains("languages") && !doc.root["languages"].empty()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(170.0f);
            if (ImGui::BeginCombo("##titleSourceLanguage", "Make Source...")) {
                for (const ta::Json& l : doc.root["languages"]) {
                    const std::string code = str(l, "code");
                    if (ImGui::Selectable(titleLanguageLabel(code, str(l, "name")).c_str())) {
                        commit(state, ta::Json{{"op", "set_source_language"}, {"code", code}, {"swap", d.swapSource}}, "Set Source Language " + code);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Swap texts", &d.swapSource);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The new source's texts become the base; the old base becomes its language");
        }
        ImGui::Separator();

        // the languages
        if (ImGui::BeginTable("##titleLanguagesTable", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Font");
            ImGui::TableSetupColumn("Bake", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Copy From", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableHeadersRow();
            const ta::Json languages = doc.root.value("languages", ta::Json::array());
            for (size_t i = 0; i < languages.size(); i++) {
                const ta::Json& l = languages[i];
                const std::string code = str(l, "code");
                const std::string at = "languages[" + std::to_string(i) + "].";
                ImGui::PushID(code.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(code.c_str());
                ImGui::TableNextColumn();
                char name[96];
                std::snprintf(name, sizeof(name), "%s", str(l, "name").c_str());
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##name", titleLanguageEnglishName(code).c_str(), name, sizeof(name));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    commit(state, ta::Json{{"op", "set"}, {"values", ta::Json{{at + "name", std::string(name)}}}}, "Rename Language " + code);
                }
                ImGui::TableNextColumn();
                char font[256];
                std::snprintf(font, sizeof(font), "%s", ta::decodeAssetRef(l.value("font", ta::Json())).path.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##font", "(each layer's font)", font, sizeof(font));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    const ta::Json ref = std::string(font).empty() ? ta::Json() : ta::Json{{"path", std::string(font)}, {"guid", ""}};
                    commit(state, ta::Json{{"op", "set"}, {"values", ta::Json{{at + "font", ref}}}}, "Set Language Font " + code);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("An assets-relative font for every text layer of this language (a layer's translation may override it)");
                ImGui::TableNextColumn();
                bool bake = l.value("bake", true);
                if (ImGui::Checkbox("##bake", &bake)) commit(state, ta::Json{{"op", "set"}, {"values", ta::Json{{at + "bake", bake}}}}, "Set Language Bake " + code);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##copy", "Copy...")) {
                    for (const ta::Json& other : languages) {
                        const std::string from = str(other, "code");
                        if (from == code) continue;
                        if (ImGui::Selectable(from.c_str())) {
                            // the other language's translations, to review
                            ta::Json ops = ta::Json::array({ta::Json{{"op", "remove_language"}, {"code", code}},
                                                    ta::Json{{"op", "add_language"}, {"code", code}, {"name", str(l, "name")}, {"copy_from", from}}});
                            commit(state, ops, "Copy Language " + from + " to " + code);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_TRASH)) d.removeCode = code;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!d.removeCode.empty()) ImGui::OpenPopup("Remove Language?###titleRemoveLanguage");
        if (EditorModal::begin("Remove Language?###titleRemoveLanguage", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            int lost = 0;
            std::function<void(const ta::Json&)> count = [&](const ta::Json& list) {
                for (const ta::Json& layer : list) {
                    if (ta::findTranslation(layer, d.removeCode)) lost++;
                    if (layer.contains("children")) count(layer["children"]);
                }
            };
            count(doc.root["layers"]);
            const std::string question = "Remove " + titleLanguageLabel(d.removeCode) + " and its " + std::to_string(lost) +
                                         " translation(s)? (Undo brings them back.)";
            EditorModal::message(question);
            ImGui::TextUnformatted(question.c_str());
            if (EditorModal::button("Remove", "yes")) {
                commit(state, ta::Json{{"op", "remove_language"}, {"code", d.removeCode}}, "Remove Language " + d.removeCode);
                if (state.language() == d.removeCode) state.setLanguage({});
                d.removeCode.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (EditorModal::button("Cancel", "cancel")) {
                d.removeCode.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();

        // add: the common game languages, or any BCP 47 tag
        ImGui::TextUnformatted("Add Language");
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##titleLanguageSearch", "Search (German, de, \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e)", d.search, sizeof(d.search));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputTextWithHint("##titleLanguageTag", "or a tag: pt-BR", d.tag, sizeof(d.tag));
        ImGui::SameLine();
        std::string error;
        const bool tagValid = d.tag[0] && TitleArtLanguage::isValid(d.tag, &error);
        ImGui::BeginDisabled(!tagValid);
        if (ImGui::Button("Add")) {
            if (commit(state, ta::Json{{"op", "add_language"}, {"code", std::string(d.tag)}}, "Add Language " + std::string(d.tag))) d.tag[0] = '\0';
        }
        ImGui::EndDisabled();
        if (d.tag[0] && !tagValid) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", error.c_str());
        Project* project = state.project();
        if (project && !project->getLocalizationSettings().languages.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Add Project Languages")) {
                ta::Json ops = ta::Json::array();
                for (const std::string& code : project->getLocalizationSettings().languages) {
                    if (!ta::isSourceLanguage(doc.root, code) && !ta::findTitleLanguage(doc.root, code)) ops.push_back(ta::Json{{"op", "add_language"}, {"code", code}});
                }
                if (!ops.empty()) commit(state, ops, "Add Project Languages");
            }
        }
        ImGui::BeginChild("##titleLanguageList", ImVec2(0.0f, 150.0f), true);
        std::string needle = d.search;
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        for (const TitleLanguageName& n : titleLanguageNames()) {
            std::string hay = std::string(n.code) + " " + n.english + " " + n.native;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (!needle.empty() && hay.find(needle) == std::string::npos) continue;
            const bool have = ta::isSourceLanguage(doc.root, n.code) || ta::findTitleLanguage(doc.root, n.code);
            ImGui::BeginDisabled(have);
            const std::string row = std::string(n.english) + "  \xc2\xb7  " + n.native + "  (" + n.code + ")";
            if (ImGui::Selectable(row.c_str())) commit(state, ta::Json{{"op", "add_language"}, {"code", n.code}}, "Add Language " + std::string(n.code));
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        ImGui::EndDisabled();
        if (!lastError().empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", lastError().c_str());
        if (!lastError().empty()) EditorModal::error(lastError());
        if (EditorModal::button("Close", "ok")) {
            d.editOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Compare Languages (R55) ----

    void drawCompare(TitleDesignerState& state) {
        Dialogs& d = dialogs();
        if (!d.compareOpen) {
            if (d.thumbnails) {
                d.thumbnails->cancelAll();
                d.thumbnails.reset();
            }
            return;
        }
        if (!d.thumbnails) d.thumbnails = std::make_unique<TitleLayerThumbnails>(state.project(), COMPARE_OWNER);
        d.thumbnails->process();
        ImGui::SetNextWindowSize(ImVec2(760.0f, 480.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Compare Languages###titleCompareLanguages", &d.compareOpen)) {
            ImGui::End();
            return;
        }
        const ta::TitleArtDoc& doc = state.committed();
        const std::vector<TitleLanguageState>& states = statesOf(state);
        const float cell = 220.0f;
        const int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cell + 12.0f)));
        int column = 0;
        const ta::TitleRect canvas{-doc.canvasWidth() / 2.0, -doc.canvasHeight() / 2.0, (double)doc.canvasWidth(), (double)doc.canvasHeight()};
        const ta::AssetResolver* assets = TitleArtRenderHost::editor().resolver(state.project());
        for (const TitleLanguageState& s : states) {
            if (column++ % columns) ImGui::SameLine();
            ImGui::BeginGroup();
            auto resolved = std::make_shared<ta::TitleArtDoc>(ta::resolveTitleLanguage(doc, s.source ? std::string() : s.code));
            if (ta::titleHasFit(resolved->root)) ta::applyTitleFits(*resolved, assets);
            const uint64_t content = ta::hashTitleArt(*resolved).document;
            // the visible cells ask first (the queue's budget, contract decision 11)
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const bool visible = ImGui::IsRectVisible(at, ImVec2(at.x + cell, at.y + cell));
            ImTextureID picture = d.thumbnails->thumbnail("lang:" + s.code, resolved, "", canvas, content, (int)cell, visible,
                                                          visible ? TitlePreviewPriority::Thumbnails : TitlePreviewPriority::Prefetch);
            const float aspect = doc.canvasHeight() > 0 ? (float)doc.canvasHeight() / (float)doc.canvasWidth() : 1.0f;
            ImGui::PushID(s.code.c_str());
            if (picture) {
                if (ImGui::ImageButton("##cell", picture, ImVec2(cell, cell * aspect))) state.setLanguage(s.source ? std::string() : s.code);
            } else {
                if (ImGui::Button("rendering...##cell", ImVec2(cell, cell * aspect))) state.setLanguage(s.source ? std::string() : s.code);
            }
            ImGui::PopID();
            const std::string caption = ta::isSourceLanguage(doc.root, s.code) ? s.code + " (source)" : titleLanguageRowLabel(doc, s.code, assets);
            ImGui::TextColored(s.overflow ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", caption.c_str());
            ImGui::EndGroup();
        }
        ImGui::End();
    }

    // ---- the Language section of a text layer (R52) ----

    void drawLanguageSection(RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        if (ctx.collecting() || !ctx.state || layers.size() != 1) return;
        TitleDesignerState& state = *ctx.state;
        const ta::TitleArtDoc& doc = state.committed();
        const ta::Json* layer = ta::findLayer(doc.root, layers.front());
        if (!layer) return;
        const std::string code = state.language();
        if (code.empty()) {
            ImGui::TextDisabled("The source language (%s). Pick a language in the toolbar to edit its translation.",
                                ta::titleSourceLanguage(doc.root).c_str());
            return;
        }
        ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 1.0f), ICON_FA_LANGUAGE " Editing %s: text, font, size, layout and fit write its translation",
                           titleLanguageLabel(code).c_str());
        ImGui::TextDisabled("Shared property: colours, rims and effects change every language");
        const std::vector<std::string> overrides = titleLayerOverrides(*layer, code);
        const ta::Json* t = ta::findTranslation(*layer, code);
        if (ta::translationHasText(t)) {
            ImGui::Text("Text: %s", str(*t, "text").c_str());
            if (str(*t, "status") == "needs_review") ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "needs review");
        } else if (ta::layerTypeOf(*layer) == ta::LayerType::Text && layer->value("translate", true)) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "No %s text: the source text shows", code.c_str());
        }
        const std::string name = str(*layer, "name");
        for (const std::string& member : overrides) {
            ImGui::PushID(member.c_str());
            // Unity's override marker: a blue bar and a bold label
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + ImGui::GetTextLineHeight()), IM_COL32(60, 140, 255, 255));
            ImGui::Dummy(ImVec2(6.0f, 0.0f));
            ImGui::SameLine();
            ImGui::Text("%s = %s", member.c_str(), (*t)[member].dump().c_str());
            if (ImGui::BeginPopupContextItem("##override")) {
                if (ImGui::MenuItem("Revert to Source")) {
                    commit(state, ta::Json{{"op", "revert_translation"}, {"layer", "id:" + layers.front()}, {"language", code}, {"fields", ta::Json::array({member})}},
                           "Revert Translation (" + name + ": " + code + ")");
                }
                if (ImGui::MenuItem("Apply to Source")) {
                    const ta::Json ops = titleApplyToSourceOp(doc, layers.front(), code, member);
                    if (!ops.is_null()) commit(state, ops, "Apply Translation to Source (" + name + ": " + code + ")");
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Revert")) {
                commit(state, ta::Json{{"op", "revert_translation"}, {"layer", "id:" + layers.front()}, {"language", code}, {"fields", ta::Json::array({member})}},
                       "Revert Translation (" + name + ": " + code + ")");
            }
            ImGui::PopID();
        }
        if (overrides.empty() && t) ImGui::TextDisabled("No overrides: every member is the source's");
    }

    // ---- the canvas overlay (R51) ----

    // a point of layer space on screen, the layer's and its groups' transforms applied
    struct Placement {
        double x = 0, y = 0, rotation = 0, scale = 1;
    };

    bool placementOf(const ta::Json& list, const std::string& id, Placement parent, Placement& out) {
        for (const ta::Json& layer : list) {
            const ta::TitleTransform t = ta::decodeTransform(layer.value("transform", ta::Json::object()));
            const double r = parent.rotation * 3.14159265358979323846 / 180.0;
            // clockwise degrees in a y-up space
            const double px = parent.x + parent.scale * (t.position.x * std::cos(r) + t.position.y * std::sin(r));
            const double py = parent.y + parent.scale * (-t.position.x * std::sin(r) + t.position.y * std::cos(r));
            const Placement here{px, py, parent.rotation + t.rotation, parent.scale * t.scale};
            if (str(layer, "id") == id) {
                out = here;
                return true;
            }
            if (layer.contains("children") && placementOf(layer["children"], id, here, out)) return true;
        }
        return false;
    }

    void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color) {
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        const int dashes = std::max(1, (int)(len / 6.0f));
        for (int i = 0; i < dashes; i += 2) {
            const float t0 = (float)i / dashes, t1 = std::min(1.0f, (float)(i + 1) / dashes);
            dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0), ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1), color, 1.5f);
        }
    }

    void drawOverlay(TitleDesignerState& state, ImDrawList* dl, ImVec2 canvasMin, ImVec2 canvasMax, const CanvasXform& xform) {
        if (!state.isOpen() || !ta::titleIsMultilingual(state.committed().root)) return;
        const std::string code = state.language();
        const ta::TitleArtDoc resolved = ta::resolveTitleLanguage(state.shown(), code);
        const ta::AssetResolver* assets = TitleArtRenderHost::editor().resolver(state.project());
        std::function<void(const ta::Json&)> walk = [&](const ta::Json& list) {
            for (const ta::Json& layer : list) {
                if (layer.contains("children")) walk(layer["children"]);
                if (ta::layerTypeOf(layer) != ta::LayerType::Text || !layer.value("visible", true)) continue;
                const ta::Json fit = layer.value("fit", ta::Json::object());
                const ta::TitleVec2 box = ta::decodeVec2(fit.value("box", ta::Json::array({0, 0})));
                if (box.x <= 0 && box.y <= 0) continue;
                Placement p;
                if (!placementOf(resolved.root["layers"], str(layer, "id"), Placement{}, p)) continue;
                const double r = p.rotation * 3.14159265358979323846 / 180.0;
                const double hw = (box.x > 0 ? box.x : box.y) * 0.5 * p.scale, hh = (box.y > 0 ? box.y : box.x) * 0.5 * p.scale;
                ImVec2 corners[4];
                const double cx[4] = {-hw, hw, hw, -hw}, cy[4] = {hh, hh, -hh, -hh};
                for (int i = 0; i < 4; i++) {
                    corners[i] = xform.toScreen({p.x + cx[i] * std::cos(r) + cy[i] * std::sin(r), p.y - cx[i] * std::sin(r) + cy[i] * std::cos(r)});
                }
                const ta::TitleFitResult result = fit.value("mode", std::string("none")) == "none" ? ta::TitleFitResult() : ta::computeTitleFit(layer, resolved, assets);
                const ta::TitleVec2 extent = result.active ? result.extent : ta::measureTitleTextExtent(layer, resolved, assets);
                const bool over = (box.x > 0 && extent.x > box.x * (1 + ta::TITLE_FIT_TOLERANCE)) || (box.y > 0 && extent.y > box.y * (1 + ta::TITLE_FIT_TOLERANCE));
                for (int i = 0; i < 4; i++) dashedLine(dl, corners[i], corners[(i + 1) % 4], over ? IM_COL32(240, 70, 60, 255) : IM_COL32(120, 200, 255, 220));
                if (over) {
                    const double ex = extent.x * 0.5 * p.scale, ey = extent.y * 0.5 * p.scale;
                    const double ox[4] = {-ex, ex, ex, -ex}, oy[4] = {ey, ey, -ey, -ey};
                    ImVec2 outline[4];
                    for (int i = 0; i < 4; i++) {
                        outline[i] = xform.toScreen({p.x + ox[i] * std::cos(r) + oy[i] * std::sin(r), p.y - ox[i] * std::sin(r) + oy[i] * std::cos(r)});
                    }
                    dl->AddPolyline(outline, 4, IM_COL32(240, 70, 60, 255), ImDrawFlags_Closed, 2.0f);
                }
            }
        };
        walk(resolved.root["layers"]);
        // the language badge in the corner
        const std::string badge = code.empty() ? ta::titleSourceLanguage(state.committed().root) + " (source)" : code;
        const ImVec2 size = ImGui::CalcTextSize(badge.c_str());
        const ImVec2 at(canvasMax.x - size.x - 18.0f, canvasMin.y + 10.0f);
        dl->AddRectFilled(ImVec2(at.x - 6.0f, at.y - 3.0f), ImVec2(at.x + size.x + 6.0f, at.y + size.y + 3.0f),
                          code.empty() ? IM_COL32(60, 60, 60, 200) : IM_COL32(40, 110, 220, 220), 4.0f);
        dl->AddText(at, IM_COL32(255, 255, 255, 255), badge.c_str());
    }

}

void editor::registerTitleDesignerLanguages() {
    ToolbarItemDef switcher;
    switcher.id = "s87.language";
    switcher.draw = [](TitleDesignerState& state) {
        drawSwitcher(state);
        drawEditLanguages(state);
        drawCompare(state);
    };
    addToolbarItem(std::move(switcher));
    CanvasOverlayDef overlay;
    overlay.id = "s87.language";
    overlay.draw = drawOverlay;
    addCanvasOverlay(std::move(overlay));
    SectionDef section;
    section.page = SectionDef::Page::Layer;
    section.id = "s87.language";
    section.title = "Language";
    section.draw = drawLanguageSection;
    section.applies = [](const TitleDesignerState& state) { return state.isOpen() && ta::titleIsMultilingual(state.committed().root); };
    section.summary = [](const TitleDesignerState& state) {
        return state.language().empty() ? std::string("source") : state.language();
    };
    addSection(std::move(section));
    registerTitleDesignerTranslations();
}
