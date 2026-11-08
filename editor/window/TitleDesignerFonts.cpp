// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Font row and the font picker (docs/specs/S82-title-designer.md R24, R54-R60, D10). Names in
// their own face and the layer's text as the sample follow Pachislop's FontBrowser
// (src/ui/FontBrowser.tsx) by SafEight, used with the author's permission, adding WOFF/WOFF2,
// .ttc faces, variable axes, system fonts and Google Fonts.

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "profiler/Profile.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/designer/TitleRows.h"
#include "titleart/designer/TitleTextEdit.h"
#include "titleart/fonts/FontCatalog.h"
#include "titleart/fonts/FontImport.h"
#include "titleart/fonts/FontPreviewRaster.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"

#include "imgui.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
using namespace jasperengine::editor::titlefonts;
namespace ta = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::string utf8Of(uint32_t c) {
        std::string out;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
        return out;
    }

}

void TitleDesignerWindow::openFontPicker(const std::vector<std::string>& layers) {
    fontPickerLayers = layers;
    fontPickerOpen = true;
    fontQuery.clear();
    const std::vector<std::string> texts = fontTextOfSelection();
    fontCoversText = !texts.empty() && coversMyTextByDefault(texts[0]);
    if (!fontCatalog) {
        fontCatalog = std::make_unique<FontCatalog>(FontCatalog::editorServices(project));
        fontStrips = std::make_unique<FontPreviewRaster>();
    }
    // listed on background jobs when the picker first opens (R58), cached for the session
    fontCatalog->open();
}

void TitleDesignerWindow::drawFontRow(const std::vector<std::string>& layers, bool readOnly) {
    if (layers.empty()) return;
    const ta::TitleArtDoc& shown = doc.shown();
    const ta::Json* layer = layerById(shown, layers[0]);
    if (!layer) return;
    const ta::TitleAssetRef font = ta::decodeAssetRef(layer->value("font", ta::Json()));
    const ta::AssetResolver* resolver = &transient;
    const ta::ResolvedTitleAsset resolved = font.empty() ? ta::ResolvedTitleAsset{} : resolver->resolve(font, ta::TitleAssetKind::Font);
    const std::string name = font.empty() ? std::string("Built-in") : fs::path(font.path).stem().string();
    if (!fontStrips) fontStrips = std::make_unique<FontPreviewRaster>();
    if (!fontCatalog) fontCatalog = std::make_unique<FontCatalog>(FontCatalog::editorServices(project));
    fontStrips->beginFrame();
    // the current font's name drawn as a preview strip in that font, a missing-glyph badge
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted("Font");
    ImGui::SameLine(width * 0.4f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (readOnly) ImGui::BeginDisabled();
    const bool clicked = ImGui::Button(("##fontbutton" + layers[0]).c_str(), ImVec2(width * 0.6f - 4.0f, ImGui::GetFrameHeight() * 1.3f));
    if (readOnly) ImGui::EndDisabled();
    size_t missingCount = 0;
    std::vector<uint32_t> missing;
    if (resolved.found) {
        StripRequest request;
        request.font = {resolved.path, layer->value("face_index", 0)};
        request.text = name;
        request.pixelSize = ImGui::GetFontSize() * 1.1f;
        request.color = 0xEEEEEEFFu;
        if (const FontPreviewRaster::Strip* strip = fontStrips->request(request); strip && strip->texture) {
            ImGui::GetWindowDrawList()->AddImage(strip->texture, ImVec2(p.x + 6.0f, p.y + 2.0f), ImVec2(p.x + 6.0f + strip->width, p.y + 2.0f + strip->height));
        } else {
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 6.0f, p.y + 4.0f), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        }
        // the text's code points the font and the Fallback Fonts cannot map (R55)
        FontEntry entry;
        entry.path = resolved.path;
        entry.faceIndex = layer->value("face_index", 0);
        std::vector<FontFileRef> fallbacks;
        for (const ta::Json& ref : layer->value("fallback_fonts", ta::Json::array())) {
            const ta::ResolvedTitleAsset f = resolver->resolve(ta::decodeAssetRef(ref), ta::TitleAssetKind::Font);
            if (f.found) fallbacks.push_back({f.path, 0});
        }
        fontCatalog->missingCodepoints(entry, decodeUtf8(str(*layer, "text")), fallbacks, missing);
        missingCount = missing.size();
    } else {
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 6.0f, p.y + 4.0f), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
    }
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + width * 0.6f - 22.0f, p.y + 4.0f), ImGui::GetColorU32(ImGuiCol_Text), ICON_FA_CARET_DOWN);
    if (clicked) openFontPicker(layers);
    if (missingCount > 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %zu missing", missingCount);
        std::string chars;
        for (uint32_t c : missing) chars += utf8Of(c) + " ";
        ImGui::TextWrapped("Missing in %s: %s", name.c_str(), chars.c_str());
        if (!readOnly && ImGui::SmallButton("Add Fallback Font...")) ImGui::OpenPopup("##addfallback");
        if (ImGui::BeginPopup("##addfallback")) {
            // the project fonts that have all of them first
            fontCatalog->open();
            fontCatalog->poll();
            std::vector<std::pair<int, const FontEntry*>> ranked;
            for (const FontEntry& e : fontCatalog->entries()) {
                if (e.section != FontSection::Project || e.webFont) continue;
                std::vector<uint32_t> still;
                fontCatalog->missingCodepoints(e, missing, {}, still);
                ranked.push_back({(int)still.size(), &e});
            }
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            if (ranked.empty()) ImGui::TextDisabled("No project fonts: import one from the font picker");
            for (const auto& [left, e] : ranked) {
                const std::string label = e->family + " " + e->style + (left == 0 ? "  (covers all)" : "  (" + std::to_string(left) + " still missing)");
                if (ImGui::MenuItem(label.c_str())) {
                    ta::Json list = layer->value("fallback_fonts", ta::Json::array());
                    list.push_back(ta::Json{{"path", e->assetPath}, {"guid", e->guid}});
                    std::vector<FieldEdit> edits;
                    for (const std::string& id : layers) edits.push_back({id, "fallback_fonts", list});
                    commitNamed(editsToOps(edits), "Add Fallback Font " + e->family);
                }
            }
            ImGui::EndPopup();
        }
    }
    // R58: a system font copied into the project carries a note
    if (!font.empty() && font.path.rfind(SYSTEM_FONTS_FOLDER, 0) == 0) {
        ImGui::TextDisabled("System font: check its licence before you ship a game with it");
    }
    // R24: when the editor font lacks characters of the text, the text drawn in the layer's own font
    const std::string text = str(*layer, "text");
    bool editorHasAll = true;
    for (uint32_t c : decodeUtf8(text)) {
        if (c > 0x7F && c != '\n' && !ImGui::GetFont()->IsGlyphInFont((ImWchar)c)) editorHasAll = false;
    }
    if (!editorHasAll && resolved.found) {
        StripRequest request;
        request.font = {resolved.path, layer->value("face_index", 0)};
        request.text = text;
        request.pixelSize = ImGui::GetFontSize() * 1.4f;
        if (const FontPreviewRaster::Strip* strip = fontStrips->request(request); strip && strip->texture) {
            ImGui::Image(strip->texture, ImVec2((float)strip->width, (float)strip->height));
        }
    }
    // variable axes and named instances (R60)
    if (resolved.found) {
        if (const FontDetails* details = fontCatalog->details(FontFileRef{resolved.path, layer->value("face_index", 0)})) {
            if (!details->instances.empty()) {
                std::vector<std::string> names;
                for (const FontInstanceInfo& instance : details->instances) names.push_back(instance.name);
                ui::DocRowOptions options;
                options.label = "Style";
                options.canReset = false;
                options.readOnly = readOnly;
                const ui::DocRowResult r = ui::docRowEnum("##fontstyle", -1, names, options);
                if (r.kind == ui::DocRowResult::Kind::Set && r.value >= 0 && (size_t)r.value < details->instances.size()) {
                    ta::Json variations = ta::Json::object();
                    const FontInstanceInfo& instance = details->instances[(size_t)r.value];
                    for (size_t a = 0; a < details->axes.size() && a < instance.coordinates.size(); a++) variations[details->axes[a].tag] = instance.coordinates[a];
                    std::vector<FieldEdit> edits;
                    for (const std::string& id : layers) edits.push_back({id, "variations", variations});
                    commitNamed(editsToOps(edits), "Change Font Style to " + instance.name);
                }
            }
            const ta::Json variations = layer->value("variations", ta::Json::object());
            for (const FontAxisInfo& axis : details->axes) {
                if (axis.hidden) continue;
                ui::DocRowOptions options;
                options.label = axis.name.c_str();
                options.uiMin = options.hardMin = axis.minValue;
                options.uiMax = options.hardMax = axis.maxValue;
                options.defaultValue = axis.defaultValue;
                options.slider = ui::SliderMap::Linear;
                options.readOnly = readOnly;
                options.fieldPath = "variations." + axis.tag;
                const double value = variations.contains(axis.tag) ? variations[axis.tag].get<double>() : axis.defaultValue;
                options.isDefault = std::abs(value - axis.defaultValue) < 1e-6;
                const ui::DocRowResult r = ui::docRowNumber(("##axis" + axis.tag).c_str(), value, options);
                const std::string step = "Change Font " + axis.name;
                auto edits = [&](double v) {
                    std::vector<FieldEdit> out;
                    for (const std::string& id : layers) out.push_back({id, "variations." + axis.tag, v});
                    return out;
                };
                const double v = r.kind == ui::DocRowResult::Kind::Shift ? ui::shiftEach(value, r.delta, options) : r.value;
                std::string error;
                switch (r.phase) {
                    case ui::EditPhase::Begin: doc.beginGesture(step); break;
                    case ui::EditPhase::Changing: doc.stage(editsToOps(edits(v)), &error); break;
                    case ui::EditPhase::Committed:
                        doc.stage(editsToOps(edits(v)), &error);
                        doc.endGesture(&error);
                        break;
                    case ui::EditPhase::Cancelled: doc.cancelGesture(); break;
                    case ui::EditPhase::Applied: commitNamed(editsToOps(edits(v)), step); break;
                    default: break;
                }
                if (r.reset) commitNamed(editsToOps(edits(axis.defaultValue)), "Reset Font " + axis.name);
                if (!error.empty()) reportError(error);
            }
        }
    }
    fontStrips->endFrame();
}

void TitleDesignerWindow::drawFontPicker() {
    JE_PROFILE_SCOPE("TD drawFontPicker");
    if (fontPickerOpen) {
        ImGui::OpenPopup("Font###fontpicker");
        fontPickerOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("Font###fontpicker")) {
        if (!fontHovered.empty()) {
            fontHovered.clear();
            doc.clearPreview();
        }
        return;
    }
    if (!fontCatalog) {
        ImGui::EndPopup();
        return;
    }
    fontCatalog->poll();
    fontStrips->beginFrame();
    const ta::TitleArtDoc& shown = doc.shown();
    const ta::Json* layer = fontPickerLayers.empty() ? nullptr : layerById(shown, fontPickerLayers[0]);
    const std::string layerText = layer ? str(*layer, "text") : std::string();
    const std::string sample = previewTextFor(layerText);
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s", fontQuery.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##fontsearch", ICON_FA_MAGNIFYING_GLASS " Search family, style or file", buffer, sizeof(buffer))) fontQuery = buffer;
    ImGui::Checkbox("Covers My Text", &fontCoversText);
    ImGui::SameLine();
    static const FontCategory categories[] = {FontCategory::Any, FontCategory::Sans, FontCategory::Serif, FontCategory::Display, FontCategory::Handwriting,
                                              FontCategory::Monospace};
    for (FontCategory c : categories) {
        ImGui::SameLine();
        const bool on = fontCategory == fontCategoryLabel(c) || (fontCategory.empty() && c == FontCategory::Any);
        if (ImGui::RadioButton(fontCategoryLabel(c), on)) fontCategory = c == FontCategory::Any ? std::string() : fontCategoryLabel(c);
    }
    FontCatalog::Query query;
    query.search = fontQuery;
    query.coversText = fontCoversText;
    query.text = decodeUtf8(layerText);
    for (FontCategory c : categories) {
        if (fontCategory == fontCategoryLabel(c)) query.category = c;
    }
    if (layer) {
        for (const ta::Json& ref : layer->value("fallback_fonts", ta::Json::array())) {
            const ta::ResolvedTitleAsset f = transient.resolve(ta::decodeAssetRef(ref), ta::TitleAssetKind::Font);
            if (f.found) query.fallbacks.push_back({f.path, 0});
        }
    }
    const std::vector<const FontEntry*> entries = fontCatalog->filter(query);
    ImGui::BeginChild("##fontlist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), true);
    FontSection section = (FontSection)255;
    std::string hoveredNow;
    ImGuiListClipper clipper;
    const float rowHeight = ImGui::GetFontSize() * 2.6f;
    clipper.Begin((int)entries.size(), rowHeight);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const FontEntry& e = *entries[(size_t)i];
            if (e.section != section) {
                section = e.section;
                ImGui::SeparatorText(fontSectionLabel(section));
                if (section == FontSection::Google && fontCatalog->googleState() == FontCatalog::GoogleState::Offline) {
                    ImGui::TextDisabled("%s", GOOGLE_OFFLINE_MESSAGE);
                }
            }
            ImGui::PushID(e.key.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool chosen = ImGui::Selectable("##font", false, 0, ImVec2(0, rowHeight));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // family and style in its own face, then the user's text (R54)
            std::string label = e.family + " " + e.style + (e.collectionFace ? "  (face " + std::to_string(e.faceIndex) + ")" : std::string()) +
                                (e.webFont ? "  [Convert]" : std::string());
            dl->AddText(ImVec2(p.x + 4.0f, p.y + 2.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
            if (!e.path.empty()) {
                StripRequest request;
                request.font = e.file();
                request.text = sample;
                request.pixelSize = ImGui::GetFontSize() * 1.2f;
                request.fallbacks = query.fallbacks;
                if (const FontPreviewRaster::Strip* strip = fontStrips->request(request); strip && strip->texture) {
                    dl->AddImage(strip->texture, ImVec2(p.x + 4.0f, p.y + ImGui::GetFontSize() + 4.0f),
                                 ImVec2(p.x + 4.0f + strip->width, p.y + ImGui::GetFontSize() + 4.0f + strip->height));
                    if (!strip->missing.empty()) {
                        const std::string count = std::to_string(strip->missing.size()) + " missing";
                        const ImVec2 size = ImGui::CalcTextSize(count.c_str());
                        dl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - size.x - 4.0f, p.y + 2.0f), IM_COL32(240, 110, 90, 255), count.c_str());
                    }
                }
            } else if (e.section == FontSection::Google) {
                const auto state = fontCatalog->googleDownloadState(e.key);
                dl->AddText(ImVec2(p.x + 4.0f, p.y + ImGui::GetFontSize() + 4.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                            state == FontCatalog::DownloadState::Failed ? fontCatalog->googleDownloadError(e.key).c_str() : "Point at it to preview");
            }
            if (hovered) hoveredNow = e.key;
            if (chosen) {
                // set it, copying it into the project first when it is not there (D10)
                std::string error;
                std::string assetPath = e.assetPath;
                int faceIndex = e.faceIndex;
                std::string family = e.family;
                bool ok = true;
                if (e.section != FontSection::Project || e.webFont) {
                    FontImportPlan plan;
                    if (e.webFont) {
                        plan = planWebFontConversion({e.path, project->getAssetsPath(), FONTS_FOLDER});
                    } else if (e.section == FontSection::Google) {
                        GoogleFontImportRequest request;
                        request.family = e.family;
                        if (e.googleVariable) {
                            request.rangeMin = 100;
                            request.rangeMax = 900;
                        }
                        request.projectRoot = project->getProjectPath();
                        request.assetsDir = project->getAssetsPath();
                        plan = planGoogleFontImport(request);
                    } else {
                        FontCopyRequest request;
                        request.source = e.path;
                        request.faceIndex = e.faceIndex;
                        request.assetsDir = project->getAssetsPath();
                        request.folder = e.section == FontSection::Library ? LIBRARY_FONTS_FOLDER : SYSTEM_FONTS_FOLDER;
                        request.from = e.section == FontSection::Library ? FontImportSource::Library : FontImportSource::System;
                        request.licenceConfirmed = fontPending == e.key;
                        plan = planFontCopy(request);
                    }
                    if (!plan.ok) {
                        ok = false;
                        error = plan.error;
                    } else if (plan.needsLicenceConfirmation && fontPending != e.key) {
                        // Restricted License embedding asks first (R58)
                        fontPending = e.key;
                        fontPendingLayer = fontPickerLayers.empty() ? std::string() : fontPickerLayers[0];
                        ok = false;
                    } else if (!plan.alreadyPresent && !commitFontImport(project, plan, error)) {
                        ok = false;
                    } else {
                        assetPath = plan.assetPath;
                        faceIndex = plan.faceIndex;
                        if (!plan.family.empty()) family = plan.family;
                        if (!plan.licenceNote.empty()) {
                            toasts.push_back(plan.licenceNote);
                            toastUntil = now() + 6.0;
                        }
                    }
                }
                if (ok && !assetPath.empty()) {
                    doc.clearPreview();
                    // keep the axes the new font has, drop the others in the same step (R60)
                    std::vector<FontAxisInfo> axes;
                    if (const FontDetails* details = fontCatalog->details(e)) axes = details->axes;
                    std::vector<FieldEdit> edits;
                    bool reset = false;
                    for (const std::string& id : fontPickerLayers) {
                        const ta::Json* target = layerById(doc.committed(), id);
                        const AxesAfterFontChange kept = keepAxesOfNewFont(target ? target->value("variations", ta::Json::object()) : ta::Json::object(), axes);
                        reset = reset || kept.reset();
                        edits.push_back({id, "font", ta::Json{{"path", assetPath}, {"guid", ""}}});
                        edits.push_back({id, "face_index", faceIndex});
                        edits.push_back({id, "variations", kept.variations});
                    }
                    commitNamed(editsToOps(edits), changeFontUndoName(family, reset));
                    fontPending.clear();
                    ImGui::CloseCurrentPopup();
                }
                if (!error.empty()) reportError(error);
            }
            ImGui::PopID();
        }
    }
    clipper.End();
    ImGui::EndChild();
    // hover with intent previews the font on the canvas (transient, R54): system and Google fonts
    // from their file or download cache through the transient resolver
    if (hoveredNow != fontHovered) {
        fontHovered = hoveredNow;
        fontHoverSince = now();
        if (hoveredNow.empty()) doc.clearPreview();
    } else if (!fontHovered.empty() && now() - fontHoverSince >= 0.15 && !fontPickerLayers.empty()) {
        const FontEntry* e = fontCatalog->find(fontHovered);
        if (e && e->section == FontSection::Google && e->path.empty()) fontCatalog->requestGoogleDownload(e->key);
        if (e && !e->path.empty()) {
            std::string assetPath = e->assetPath;
            if (assetPath.empty() || e->webFont) {
                assetPath = std::string(e->section == FontSection::Google ? GOOGLE_FONTS_FOLDER : e->section == FontSection::Library ? LIBRARY_FONTS_FOLDER
                                                                                                                                       : SYSTEM_FONTS_FOLDER) +
                            "/" + fs::path(e->path).filename().generic_string();
                transient.add(assetPath, e->path);
            }
            PreviewItem item;
            item.kind = PreviewItem::Kind::Font;
            item.id = e->key;
            item.label = e->family;
            std::vector<FieldEdit> edits;
            for (const std::string& id : fontPickerLayers) {
                edits.push_back({id, "font", ta::Json{{"path", assetPath}, {"guid", ""}}});
                edits.push_back({id, "face_index", e->faceIndex});
            }
            item.ops = editsToOps(edits);
            item.stepName = "Change Font to " + e->family;
            std::string error;
            doc.setPreview(item, &error);
        }
    }
    if (!fontPending.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "This font's licence does not allow embedding it in a game. Exports of titles using it fail unless you confirm you hold a "
                           "licence. Click it again to copy it anyway.");
    }
    if (ImGui::Button("Import Font File...")) {
        const std::string file = FileDialogs::openFileDialogFiltered("", "Fonts", "ttf,otf,ttc,woff,woff2");
        if (!file.empty()) {
            const std::string ext = fs::path(file).extension().string();
            FontImportPlan plan = (ext == ".woff" || ext == ".woff2") ? planWebFontConversion({file, project->getAssetsPath(), FONTS_FOLDER})
                                                                     : planFontCopy({file, 0, project->getAssetsPath(), FONTS_FOLDER, FontImportSource::File, true, {}});
            std::string error;
            if (!plan.ok) reportError(plan.error);
            else if (!plan.alreadyPresent && !commitFontImport(project, plan, error)) reportError(error);
            else fontCatalog->refreshProject();
        }
    }
    fontStrips->endFrame();
    ImGui::EndPopup();
}
