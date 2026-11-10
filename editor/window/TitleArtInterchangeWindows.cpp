// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Title Art's import and export windows (docs/specs/S87-title-localisation-interchange.md R31,
// R56-R58): Assets > Import Title Art... (a Pachislop project, a PSD or an SVG into a new document:
// destination, kind, then one undo step and the Import Report, whose notes open the layer in the
// Title Designer), the Title Designer's Export... (PNG, PSD, SVG, Sprite Sheet, Atlas, Sequence,
// KTX2, Strings: the options of R36-R43 with Unity-like labels, languages, the output folder, a
// size readout, a background job with progress and Show in Folder; the last options kept per
// document in the project's editor-local settings), its Import Photoshop Style... and Import
// Photoshop Styles as Looks..., and the Title Art Inspector's languages line and import notes.

#include "TitleArtInterchangeWindows.h"

#include "Backend.h"
#include "App.h"
#include "Log.h"
#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleTranslations.h"
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitleDesignerState.h"
#include "titleart/interchange/TitleExport.h"
#include "titleart/interchange/TitleImport.h"
#include "titleart/localisation/TitleLanguageReport.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "window/InspectorWindowTitleArt.h"
#include "window/TitleDesignerWindow.h"
#include "window/dialog/EditorSettingsWindow.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    // ---- the import dialog and its report ----

    struct ImportState {
        bool dialog = false;
        char source[1024] = {0};
        char folder[512] = "titles";
        std::string error;
        bool report = false;
        fs::path document;
        ta::Json notes = ta::Json::array();
        std::string kind;
        size_t files = 0;
    };

    ImportState& importState() {
        static ImportState s;
        return s;
    }

    void runImport(Project* project) {
        ImportState& s = importState();
        const fs::path source = FileUtils::pathFromUtf8(s.source);
        const fs::path folder = project->getAssetsPath() / FileUtils::pathFromUtf8(s.folder);
        std::string stem = source.filename().string();
        for (const char* suffix : {".pslot.json", ".json", ".psd", ".psb", ".svg"}) {
            const std::string x = suffix;
            if (stem.size() > x.size() && stem.compare(stem.size() - x.size(), x.size(), x) == 0) {
                stem = stem.substr(0, stem.size() - x.size());
                break;
            }
        }
        const fs::path document = folder / (stem + ".titleart");
        std::error_code ec;
        if (fs::exists(document, ec)) {
            s.error = document.filename().string() + " exists in " + std::string(s.folder) + ": choose another folder or rename the source";
            return;
        }
        TitleCreateRequest request;
        request.sourcePath = source.string();
        request.documentPath = document.string();
        request.projectPath = project->getProjectPath().string();
        request.assetsPath = project->getAssetsPath().string();
        TitleCreateResult result;
        std::string code, error;
        if (!TitleArtOps::createFrom(request, result, code, error)) {
            s.error = code + ": " + error;
            return;
        }
        fs::create_directories(folder, ec);
        // one undo step removes every file it wrote (R57)
        if (!TitleArtDocument::create(project, document.string(), &result.doc, titleImportUndoName(result.kind, source.filename().string()), &error,
                                      result.files)) {
            s.error = error;
            return;
        }
        s.error.clear();
        s.dialog = false;
        s.report = true;
        s.document = document;
        s.notes = result.notes;
        s.kind = result.kind;
        s.files = result.files.size();
        InspectorWindowTitleArt::openInDesigner(document);
    }

    void drawImportDialog(Project* project) {
        ImportState& s = importState();
        if (s.dialog && !ImGui::IsPopupOpen("Import Title Art###titleImport")) ImGui::OpenPopup("Import Title Art###titleImport");
        ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Import Title Art###titleImport", &s.dialog)) {
            ImGui::TextWrapped("A Pachislop project (.pslot.json), a layered Photoshop file (.psd, .psb) or an SVG becomes a new Title Art: "
                               "its fonts and pictures go to <name>_assets beside it.");
            ImGui::SetNextItemWidth(-90.0f);
            ImGui::InputText("##titleImportSource", s.source, sizeof(s.source));
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                const std::string file = FileDialogs::openFileDialogFiltered("", "Title Art sources", "json,psd,psb,svg");
                if (!file.empty()) std::snprintf(s.source, sizeof(s.source), "%s", file.c_str());
            }
            ImGui::TextUnformatted("Destination folder (in the assets)");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##titleImportFolder", s.folder, sizeof(s.folder));
            if (!s.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", s.error.c_str());
            ImGui::BeginDisabled(!s.source[0] || !project);
            if (ImGui::Button("Import")) runImport(project);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                s.dialog = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void drawImportReport() {
        ImportState& s = importState();
        if (!s.report) return;
        ImGui::SetNextWindowSize(ImVec2(620.0f, 360.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Import Report###titleImportReport", &s.report)) {
            ImGui::Text("%s: a %s, %zu file(s), %zu note(s)", s.document.filename().string().c_str(), titleImportKindLabel(s.kind).c_str(), s.files,
                        s.notes.size());
            ImGui::Separator();
            if (ImGui::BeginTable("##importNotes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Layer");
                ImGui::TableSetupColumn("Field");
                ImGui::TableSetupColumn("What");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < s.notes.size(); i++) {
                    const ta::Json& n = s.notes[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const std::string layer = n.value("layer", std::string());
                    if (ImGui::Selectable((layer.empty() ? "(document)" : layer).c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && !layer.empty()) {
                        // a note opens its layer in the Title Designer
                        InspectorWindowTitleArt::openInDesigner(s.document);
                        if (TitleDesignerWindow* designer = Backend::getApp().getTitleDesignerWindow()) {
                            TitleDesignerState& state = designer->designer();
                            ta::TitleArtDoc copy = state.committed();
                            ta::TitleIdGenerator ids;
                            TitleOpsResult unused;
                            TitleOpContext context(copy, ids, unused);
                            if (const ta::Json* found = context.resolveLayer(layer, "layer")) state.selectOne(NodeRef{NodeKind::Layer, found->value("id", std::string()), "", ""});
                        }
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(n.value("field", std::string()).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", n.value("what", std::string()).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(n.value("action", std::string()).c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    // ---- the export dialog (R58) ----

    struct ExportState {
        bool open = false;
        fs::path document;
        int tab = 0;
        ta::Json options = ta::Json::object();
        std::vector<std::string> languages;
        std::vector<char> chosen;
        char out[1024] = {0};
        std::shared_ptr<TitleExportJob> job;
        std::string message;
        std::vector<std::string> files;
        fs::path folder;
    };

    ExportState& exportState() {
        static ExportState s;
        return s;
    }

    const char* const TABS[] = {"PNG", "PSD", "SVG", "Sprite Sheet", "Atlas", "Sequence", "KTX2", "Strings"};
    const char* const FORMATS[] = {"png", "psd", "svg", "sheet", "atlas", "sequence", "ktx2", "csv"};

    // the last options of each document, in the project's editor-local settings
    fs::path optionsFile(Project* project) {
        return project->getProjectInternalPath() / "titleart-export.json";
    }

    ta::Json loadOptions(Project* project, const std::string& key) {
        std::ifstream in(optionsFile(project));
        std::stringstream buffer;
        buffer << in.rdbuf();
        const ta::Json all = ta::Json::parse(buffer.str(), nullptr, false);
        return all.is_object() && all.contains(key) ? all[key] : ta::Json::object();
    }

    void saveOptions(Project* project, const std::string& key, const ta::Json& value) {
        std::ifstream in(optionsFile(project));
        std::stringstream buffer;
        buffer << in.rdbuf();
        ta::Json all = ta::Json::parse(buffer.str(), nullptr, false);
        if (!all.is_object()) all = ta::Json::object();
        all[key] = value;
        std::error_code ec;
        fs::create_directories(optionsFile(project).parent_path(), ec);
        if (!FileUtils::writeFileReplacing(optionsFile(project), all.dump(2)))
            Log::warn("Could not save the title interchange options to %s", optionsFile(project).string().c_str());
    }

    void option(ta::Json& o, const char* key, bool def, const char* label) {
        bool v = o.value(key, def);
        if (ImGui::Checkbox(label, &v)) o[key] = v;
    }

    void option(ta::Json& o, const char* key, float def, float lo, float hi, const char* label, const char* format = "%.2f") {
        float v = o.value(key, def);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat(label, &v, (hi - lo) / 400.0f, lo, hi, format)) o[key] = v;
    }

    void option(ta::Json& o, const char* key, int def, int lo, int hi, const char* label) {
        int v = o.value(key, def);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragInt(label, &v, 1.0f, lo, hi)) o[key] = std::clamp(v, lo, hi);
    }

    void choice(ta::Json& o, const char* key, const std::vector<std::pair<const char*, const char*>>& values, const char* label) {
        const std::string current = o.value(key, std::string(values.front().first));
        ImGui::SetNextItemWidth(160.0f);
        const char* shown = values.front().second;
        for (const auto& v : values) if (current == v.first) shown = v.second;
        if (ImGui::BeginCombo(label, shown)) {
            for (const auto& v : values) {
                if (ImGui::Selectable(v.second, current == v.first)) o[key] = v.first;
            }
            ImGui::EndCombo();
        }
    }

    void imageOptions(ta::Json& o) {
        option(o, "scale", 1.0f, 0.1f, 8.0f, "Scale");
        option(o, "trim", true, "Trim");
        option(o, "padding", 4.0f, 0.0f, 256.0f, "Padding", "%.0f px");
        int round = o.value("round_to", 4);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Round To", std::to_string(round).c_str())) {
            for (int r : {1, 2, 4, 8}) {
                if (ImGui::Selectable(std::to_string(r).c_str(), r == round)) o["round_to"] = r;
            }
            ImGui::EndCombo();
        }
        option(o, "alpha_threshold", 8, 1, 255, "Alpha Threshold");
        choice(o, "alpha", {{"straight", "Straight"}, {"premultiplied", "Premultiplied"}}, "Alpha");
        option(o, "alpha_bleed", true, "Bleed Color");
        int depth = o.value("bit_depth", 8);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Bit Depth", depth == 16 ? "16-bit" : "8-bit")) {
            if (ImGui::Selectable("8-bit", depth == 8)) o["bit_depth"] = 8;
            if (ImGui::Selectable("16-bit", depth == 16)) o["bit_depth"] = 16;
            ImGui::EndCombo();
        }
        option(o, "registered", false, "One Frame For Every Language");
    }

    void startExport(Project* project, ExportState& s) {
        const std::string format = s.tab == 7 ? s.options.value(TABS[7], ta::Json::object()).value("strings_format", std::string("csv")) : FORMATS[s.tab];
        TitleExportRequest request;
        request.project = project;
        request.document = s.document;
        std::error_code ec;
        request.relative = fs::relative(s.document, project->getProjectPath(), ec).generic_string();
        std::string code, error;
        if (!loadTitleArtForBake(project, s.document, request.doc, code, error)) {
            s.message = code + ": " + error;
            return;
        }
        request.format = format;
        // each tab keeps its own options
        ta::Json options = s.options.value(TABS[s.tab], ta::Json::object());
        options.erase("strings_format");
        request.options = options;
        for (size_t i = 0; i < s.languages.size(); i++) {
            if (s.chosen[i]) request.languages.push_back(s.languages[i]);
        }
        if (s.out[0]) request.out = (project->getProjectPath() / FileUtils::pathFromUtf8(s.out)).lexically_normal();
        request.insideProjectOnly = false;
        s.folder = request.out.empty() ? titleExportDefaultOut(project, s.document) : request.out;
        s.job = std::shared_ptr<TitleExportJob>(startTitleExport(std::move(request), code, error).release());
        s.files.clear();
        s.message = s.job ? "Exporting..." : code + ": " + error;
        saveOptions(project, fs::relative(s.document, project->getProjectPath(), ec).generic_string(), s.options);
    }

    void drawExportDialog(Project* project) {
        ExportState& s = exportState();
        if (!s.open || !project) return;
        if (s.job) {
            if (s.job->step(6.0)) {
                const TitleExportResult& r = s.job->result();
                s.files = r.files;
                s.message = r.ok ? "Exported " + std::to_string(r.files.size()) + " file(s)" + (r.warnings.empty() ? "" : ", " + std::to_string(r.warnings.size()) + " warning(s)")
                                 : r.code + ": " + r.error;
                s.job.reset();
            }
        }
        ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Export Title Art###titleExport", &s.open)) {
            ImGui::End();
            return;
        }
        ImGui::Text("%s", s.document.filename().string().c_str());
        if (ImGui::BeginTabBar("##exportTabs")) {
            for (int i = 0; i < 8; i++) {
                if (ImGui::BeginTabItem(TABS[i])) {
                    s.tab = i;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        if (!s.options.is_object()) s.options = ta::Json::object();
        if (!s.options.contains(TABS[s.tab]) || !s.options[TABS[s.tab]].is_object()) s.options[TABS[s.tab]] = ta::Json::object();
        ta::Json& o = s.options[TABS[s.tab]];
        switch (s.tab) {
            case 0:
                imageOptions(o);
                option(o, "parts", false, "Parts (each layer alone)");
                option(o, "split_titles", false, "Split Titles (rims, face, effects)");
                option(o, "zip", false, "Bundle as ZIP");
                break;
            case 1:
                option(o, "split_titles", false, "Split Titles (rims, face, effects)");
                ImGui::TextDisabled("Photoshop blends in sRGB: the merged image is the true render");
                break;
            case 2:
                option(o, "embed_raster", false, "Embed Pictures (effects, images) as PNG");
                option(o, "strict", false, "Fail on Anything Not Vector");
                break;
            case 3: case 4: case 5:
                imageOptions(o);
                option(o, "fps", 30.0f, 1.0f, 120.0f, "Frames Per Second", "%.0f");
                break;
            case 6:
                imageOptions(o);
                choice(o, "family", {{"desktop", "Desktop"}, {"mobile", "Mobile"}, {"web", "Web"}}, "Family");
                option(o, "mipmaps", false, "Mipmaps");
                break;
            default:
                choice(o, "strings_format", {{"csv", "CSV"}, {"xliff", "XLIFF 1.2"}}, "Format");
                break;
        }
        if (!s.languages.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("Languages");
            for (size_t i = 0; i < s.languages.size(); i++) {
                bool on = s.chosen[i] != 0;
                if (i) ImGui::SameLine();
                if (ImGui::Checkbox(s.languages[i].c_str(), &on)) s.chosen[i] = on ? 1 : 0;
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Output folder (project-relative; empty: Recordings/Title Art/<name>)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##exportOut", s.out, sizeof(s.out));
        // the readout: the canvas at the scale, trimmed when exported
        if (std::shared_ptr<TitleArtDocument> document = TitleArtDocument::find(s.document)) {
            const ta::TitleArtDoc& doc = document->doc();
            const double scale = o.value("scale", 1.0);
            size_t count = 0;
            for (char c : s.chosen) count += c ? 1 : 0;
            ImGui::TextDisabled("Up to %d x %d px%s, %zu language(s)", (int)std::lround(doc.canvasWidth() * scale), (int)std::lround(doc.canvasHeight() * scale),
                                o.value("trim", true) ? " (trimmed to the content)" : "", std::max<size_t>(count, 1));
        }
        ImGui::BeginDisabled(s.job != nullptr);
        if (ImGui::Button(ICON_FA_FILE_EXPORT " Export")) startExport(project, s);
        ImGui::EndDisabled();
        if (s.job) {
            ImGui::SameLine();
            ImGui::ProgressBar(s.job->progress(), ImVec2(-1.0f, 0.0f), s.job->progressText().c_str());
        }
        if (!s.message.empty()) ImGui::TextWrapped("%s", s.message.c_str());
        if (!s.files.empty()) {
            if (ImGui::Button("Show in Folder")) EditorSettingsWindow::openInFileManager(s.folder);
            for (const std::string& f : s.files) ImGui::BulletText("%s", f.c_str());
        }
        ImGui::End();
    }

    void openExport(TitleDesignerState& state) {
        ExportState& s = exportState();
        if (!state.document() || !state.project()) return;
        s.open = true;
        s.document = state.document()->absolutePath();
        std::error_code ec;
        s.options = loadOptions(state.project(), fs::relative(s.document, state.project()->getProjectPath(), ec).generic_string());
        s.languages = ta::titleLanguageCodes(state.committed().root);
        s.chosen.assign(s.languages.size(), 1);
        s.message.clear();
        s.files.clear();
    }

    // ---- Photoshop styles (R57) ----

    void importStyle(TitleDesignerState& state, bool asLooks) {
        const std::string file = FileDialogs::openFileDialogFiltered("", "Photoshop styles", "asl,psd");
        if (file.empty()) return;
        std::string error;
        const std::vector<std::string> layers = state.selectedLayers();
        ta::Json op = asLooks ? ta::Json{{"op", "import_ps_styles"}, {"file", file}}
                          : ta::Json{{"op", "apply_ps_style"}, {"layer", layers.empty() ? std::string() : "id:" + layers.front()}, {"file", file}};
        if (!asLooks && layers.empty()) return;
        if (!state.commit(op, asLooks ? "Import Photoshop Styles" : "Apply Photoshop Style", &error)) exportState().message = error;
    }

    void drawDesignerMenu(TitleDesignerState& state) {
        if (!state.isOpen()) return;
        if (ImGui::Button(ICON_FA_FILE_EXPORT " Export...")) openExport(state);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FILE_IMPORT "##psstyles")) ImGui::OpenPopup("##titlePsStyles");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import Photoshop styles (.asl, .psd)");
        if (ImGui::BeginPopup("##titlePsStyles")) {
            if (ImGui::MenuItem("Import Photoshop Style... (to the selected layer)", nullptr, false, !state.selectedLayers().empty() && state.editable())) importStyle(state, false);
            if (ImGui::MenuItem("Import Photoshop Styles as Looks...", nullptr, false, state.editable())) importStyle(state, true);
            ImGui::EndPopup();
        }
    }

    // ---- the Inspector (R31, R56) ----

    void drawInspector(Project* project, const fs::path& document, const ta::TitleArtDoc& doc) {
        if (ta::titleIsMultilingual(doc.root)) {
            const std::vector<TitleLanguageState> states = titleLanguageStates(doc, TitleArtRenderHost::editor().resolver(project));
            ImGui::TextUnformatted(titleLanguagesSummaryLine(states).c_str());
        }
        const ta::Json notes = doc.root.value("import_notes", ta::Json::array());
        if (!notes.empty()) {
            ImGui::Text("Import notes: %zu", notes.size());
            if (ImGui::IsItemHovered()) {
                std::string text;
                for (size_t i = 0; i < notes.size() && i < 12; i++) text += notes[i].get<std::string>() + "\n";
                ImGui::SetTooltip("%s", text.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                if (std::shared_ptr<TitleArtDocument> d = TitleArtDocument::open(project, document.string(), nullptr)) {
                    d->edit(ta::Json{{"op", "set"}, {"values", ta::Json{{"import_notes", ta::Json::array()}}}}, TitleArtEditOptions{"Clear Import Notes"});
                }
            }
        }
    }

}

void editor::openTitleArtImportDialog() {
    importState().dialog = true;
}

void editor::drawTitleArtInterchangeWindows(Project* project) {
    drawImportDialog(project);
    drawImportReport();
    drawExportDialog(project);
}

void editor::registerTitleArtInterchangeUi() {
    ToolbarItemDef menu;
    menu.id = "s87.interchange";
    menu.draw = drawDesignerMenu;
    addToolbarItem(std::move(menu));
    InspectorWindowTitleArt::addSummarySection("s87.languages", drawInspector);
}
