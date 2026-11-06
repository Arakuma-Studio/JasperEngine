// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Inspector's summary of one selected .titleart (S80 R51, §6.13): a 256 px preview on a
// backdrop (Checker, Black, White) rendered through TitlePreviewQueue, Open in Title Designer
// (S82), canvas, layers by type, fonts (missing ones red, with Locate), exposed parameters, the
// outputs with Fresh / Stale / Missing and Bake (Add Output / Remove Output undoable), problems
// and the Auto Bake setting. Read-only otherwise: the document is edited in the Title Designer
// or through edit_title.

#include "InspectorWindow.h"
#include "InspectorWindowTitleArt.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "render/preview/TitleArtPreviewRender.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "window/AssetPanels.h"
#include "imgui.h"

#include <algorithm>
#include <map>
#include <set>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct SectionEntry {
        std::string id;
        InspectorWindowTitleArt::SummarySection draw;
    };
    std::vector<SectionEntry>& sections() {
        static std::vector<SectionEntry> list;
        return list;
    }
    std::function<void(const fs::path&)>& designerOpener() {
        static std::function<void(const fs::path&)> open;
        return open;
    }

    constexpr TitlePreviewOwner INSPECTOR_OWNER = 0x494e5350;   // "INSP"

    struct TitleArtInspectorState {
        fs::path asset;
        std::shared_ptr<TitleArtDocument> document;
        std::string openError;
        uint64_t previewId = 0;
        std::string previewKey;             // rev + backdrop the texture shows
        int backdrop = 0;                   // Checker, Black, White
        Texture texture;
        texsynth::Image lastImage;          // the render the texture shows, for another backdrop
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        uint64_t uploads = 0;
        double statusTime = -1.0;
        TitleBakeStatus status;
        std::vector<titleart::TitleProblem> problems;
        uint64_t statusRev = 0;
    };

    // never destroyed: its texture must not outlive the GPU at exit
    TitleArtInspectorState& titleState() {
        static TitleArtInspectorState* state = new TitleArtInspectorState();
        return *state;
    }

    const char* const BACKDROPS[] = {"Checker", "Black", "White"};

    void upload(TitleArtInspectorState& state, const texsynth::Image& image) {
        state.width = image.width;
        state.height = image.height;
        state.pixels.assign(static_cast<size_t>(image.width) * image.height * 4, 0);
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                uint8_t bg;
                if (state.backdrop == 1) bg = 0;
                else if (state.backdrop == 2) bg = 255;
                else bg = ((x / 8 + y / 8) % 2) ? 204 : 255;
                const uint8_t* s = image.pixel(x, y);
                uint8_t* d = state.pixels.data() + (static_cast<size_t>(y) * image.width + x) * 4;
                for (int k = 0; k < 3; k++) d[k] = static_cast<uint8_t>((s[k] * s[3] + bg * (255 - s[3]) + 127) / 255);
                d[3] = 255;
            }
        }
        state.texture.destroy();
        state.texture.setData("editor:titleart-inspector:" + std::to_string(++state.uploads),
                              TextureData(image.width, image.height, static_cast<unsigned int>(state.pixels.size()), ColorFormat::RGBA, 4,
                                          state.pixels.data()));
        state.texture.load();
    }

    void sectionHeader(const char* label) {
        ImGui::Spacing();
        ImGui::SeparatorText(label);
    }

}

void InspectorWindowTitleArt::addSummarySection(const std::string& id, SummarySection section) {
    for (SectionEntry& e : sections()) {
        if (e.id == id) {
            e.draw = std::move(section);
            return;
        }
    }
    sections().push_back({id, std::move(section)});
}

void InspectorWindowTitleArt::removeSummarySection(const std::string& id) {
    auto& list = sections();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const SectionEntry& e) { return e.id == id; }), list.end());
}

void InspectorWindowTitleArt::drawSummarySections(Project* project, const fs::path& document, const titleart::TitleArtDoc& doc) {
    for (const SectionEntry& e : sections()) {
        if (e.draw) e.draw(project, document, doc);
    }
}

void InspectorWindowTitleArt::setOpenInDesigner(std::function<void(const fs::path&)> open) {
    designerOpener() = std::move(open);
}

bool InspectorWindowTitleArt::canOpenInDesigner() {
    return static_cast<bool>(designerOpener());
}

void InspectorWindowTitleArt::openInDesigner(const fs::path& document) {
    if (designerOpener()) designerOpener()(document);
}

void editor::InspectorWindow::drawTitleArtAsset(const fs::path& asset) {
    TitleArtInspectorState& state = titleState();
    drawAssetHeader(ICON_FA_FONT, asset, "Title Art");
    if (state.asset != asset) {
        TitlePreviewQueue::editor().cancelAll(INSPECTOR_OWNER);
        state.asset = asset;
        state.document = TitleArtDocument::open(project, asset.string(), &state.openError);
        state.previewId = 0;
        state.previewKey.clear();
        state.statusTime = -1.0;
        state.width = state.height = 0;
        state.lastImage = texsynth::Image();
    }
    if (!state.document) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", state.openError.c_str());
        return;
    }
    TitleArtDocument& document = *state.document;
    if (!document.editable()) {
        ImGui::TextWrapped("%s", document.versionError().c_str());
        ImGui::TextDisabled("Read-only: this editor neither renders nor bakes it.");
        return;
    }
    if (!document.loadError().empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", document.loadError().c_str());
    const titleart::TitleArtDoc& doc = document.doc();

    // the preview: requested for this rev, kept on screen until the next one is done
    const std::string revKey = std::to_string(document.revision());
    if (state.previewKey.rfind(revKey + ":", 0) != 0 && state.previewId == 0) {
        std::string error;
        state.previewId = requestTitleArtPreview(project, asset, 256, TitlePreviewPriority::Layers, INSPECTOR_OWNER, true, error);
    }
    if (state.previewId) {
        const TitlePreviewState s = TitlePreviewQueue::editor().state(state.previewId);
        if (s == TitlePreviewState::Done) {
            if (auto* job = dynamic_cast<TitleArtPreviewJob*>(TitlePreviewQueue::editor().result(state.previewId))) {
                state.lastImage = job->image().image;
                upload(state, state.lastImage);
            }
            state.previewKey = revKey + ":done";
        }
        if (s == TitlePreviewState::Done || s == TitlePreviewState::Failed || s == TitlePreviewState::Unknown) {
            TitlePreviewQueue::editor().cancel(state.previewId);
            state.previewId = 0;
            if (s != TitlePreviewState::Done) state.previewKey = revKey + ":failed";
        }
    }
    const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 18.0f);
    if (state.width > 0) {
        const float scale = side / static_cast<float>(std::max(state.width, state.height));
        ImGui::Image(Backend::getImGuiTexture(state.texture.getRender()), ImVec2(state.width * scale, state.height * scale));
    } else {
        ImGui::Dummy(ImVec2(side, side * 0.3f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%s", state.previewKey == revKey + ":failed" ? "The preview could not be rendered" : "Rendering the preview...");
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    if (ImGui::Combo("Backdrop", &state.backdrop, BACKDROPS, 3) && !state.lastImage.empty()) upload(state, state.lastImage);
    if (InspectorWindowTitleArt::canOpenInDesigner()) {
        if (ImGui::Button(ICON_FA_PEN_TO_SQUARE " Open in Title Designer", ImVec2(-1.0f, 0.0f))) InspectorWindowTitleArt::openInDesigner(asset);
    }

    // status and problems: read again twice a second, and at once after an edit
    const double now = ImGui::GetTime();
    if (state.statusTime < 0.0 || now - state.statusTime > 0.5 || state.statusRev != document.revision()) {
        state.statusTime = now;
        state.statusRev = document.revision();
        state.status = titleArtStatusOf(project, asset, doc);
        state.problems = titleart::titleArtDocumentProblems(doc);
        if (const EditorTitleAssetResolver* resolver = TitleArtRenderHost::editor().resolver(project)) {
            for (titleart::TitleProblem& p : titleArtMissingAssets(doc, *resolver)) state.problems.push_back(std::move(p));
            for (titleart::TitleProblem& p : titleArtMissingGlyphs(doc, *resolver)) state.problems.push_back(std::move(p));
        }
    }

    sectionHeader("Document");
    ImGui::Text("Canvas  %d x %d", doc.canvasWidth(), doc.canvasHeight());
    std::map<std::string, int> counts;
    titleart::forEachLayer(doc.root, [&](const titleart::Json& layer, const titleart::Json*, int) {
        counts[titleart::layerTypeLabel(titleart::layerTypeOf(layer))]++;
        return true;
    });
    std::string layers;
    for (const auto& [type, n] : counts) layers += (layers.empty() ? "" : ", ") + std::to_string(n) + " " + type;
    ImGui::Text("Layers  %s", layers.empty() ? "none" : layers.c_str());

    sectionHeader("Fonts");
    const EditorTitleAssetResolver* resolver = TitleArtRenderHost::editor().resolver(project);
    std::set<std::string> seenFonts;
    titleart::forEachLayer(doc.root, [&](const titleart::Json& layer, const titleart::Json*, int) {
        if (titleart::layerTypeOf(layer) != titleart::LayerType::Text) return true;
        std::vector<titleart::TitleAssetRef> refs = {titleart::decodeAssetRef(layer.contains("font") ? layer["font"] : titleart::Json())};
        if (layer.contains("fallback_fonts")) for (const auto& f : layer["fallback_fonts"]) refs.push_back(titleart::decodeAssetRef(f));
        for (const titleart::TitleAssetRef& ref : refs) {
            const std::string name = ref.empty() ? "(built-in)" : (ref.path.empty() ? ref.guid : ref.path);
            if (!seenFonts.insert(name).second) continue;
            const titleart::ResolvedTitleAsset resolved = ref.empty() || !resolver ? titleart::ResolvedTitleAsset() : resolver->resolve(ref, titleart::TitleAssetKind::Font);
            ImGui::PushID(name.c_str());
            if (!ref.empty() && !resolved.found) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s  Missing", name.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Locate")) {
                    if (MissingAssetsWindow* missing = Backend::getApp().getMissingAssetsWindow()) missing->setOpen(true);
                }
            } else if (ImGui::Selectable(name.c_str()) && resolved.found) {
                project->setInspectedAsset(fs::path(resolved.path));
            }
            ImGui::PopID();
        }
        return true;
    });

    const titleart::Json& exposed = doc.root["exposed"];
    if (!exposed.empty()) {
        sectionHeader("Exposed Parameters");
        for (const auto& e : exposed) {
            std::string targets;
            for (const auto& t : e["targets"]) targets += (targets.empty() ? "" : ", ") + t.value("field", std::string());
            ImGui::Text("%s  (%s)  %s", e.value("name", std::string()).c_str(), e.value("type", std::string()).c_str(), targets.c_str());
        }
    }

    sectionHeader("Outputs");
    const bool baking = TitleArtAutoBake::get().baking(asset);
    if (ImGui::BeginTable("##TitleArtOutputs", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("File");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("##bake", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < state.status.outputs.size(); i++) {
            const TitleOutputState& o = state.status.outputs[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(o.fileName.c_str());
            ImGui::TableNextColumn();
            if (o.width > 0) ImGui::Text("%d x %d", o.width, o.height);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            const ImVec4 color = o.status == TitleOutputStatus::Fresh ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
            ImGui::TextColored(color, "%s", titleOutputStatusLabel(o.status));
            if (!o.reason.empty()) ImGui::SetItemTooltip("%s", o.reason.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(baking);
            if (ImGui::SmallButton("Bake")) TitleArtAutoBake::get().requestBake(project, asset, {o.id}, true);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(state.status.outputs.size() <= 1);
            if (ImGui::SmallButton(ICON_FA_MINUS)) {
                titleart::TitleArtDoc copy = doc;
                copy.root["outputs"].erase(i);
                std::string error;
                document.commit(copy, "Remove Output " + o.id, {}, &error);
            }
            ImGui::SetItemTooltip("Remove Output (its PNG stays until you delete it)");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(baking);
    if (ImGui::Button(baking ? "Baking..." : ICON_FA_FIRE " Bake All")) TitleArtAutoBake::get().requestBake(project, asset, {}, true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.status.outputs.size() >= titleart::TITLE_MAX_OUTPUTS);
    if (ImGui::Button(ICON_FA_PLUS " Add Output")) {
        titleart::TitleArtDoc copy = doc;
        std::set<std::string> ids;
        for (const auto& o : copy.root["outputs"]) ids.insert(o.value("id", std::string()));
        std::string id = "glow";
        for (int n = 1; ids.count(id); n++) id = "output" + std::to_string(n);
        titleart::Json output = titleart::Json::object();
        output["id"] = id;
        if (id == "glow") output["content"] = "glow";
        copy.root["outputs"].push_back(output);
        std::string error;
        document.commit(copy, "Add Output " + id, {}, &error);
    }
    ImGui::EndDisabled();

    if (!state.problems.empty() || !document.warnings().empty()) {
        sectionHeader("Problems");
        for (const titleart::TitleProblem& p : state.problems) {
            const ImVec4 color = p.severity == titleart::TitleSeverity::Error ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", p.message.c_str());
            ImGui::PopStyleColor();
        }
        for (const titleart::TitleWarning& w : document.warnings()) ImGui::TextWrapped("%s", w.toString().c_str());
    }

    InspectorWindowTitleArt::drawSummarySections(project, asset, doc);

    sectionHeader("Auto Bake");
    ImGui::TextUnformatted(project->getTitleArtSettings().autoBake ? "On: outputs rebake after edits" : "Off: bake by hand");
    ImGui::SameLine();
    if (ImGui::SmallButton("Project Settings")) Backend::getApp().getProjectSettingsWindow().open(project, "Title Art");
}
