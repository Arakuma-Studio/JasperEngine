// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtRuntimeDesigner.h"

#include "Backend.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/TitleArtRuntime.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitlePages.h"
#include "titleart/designer/TitleRows.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleCharacterChecks.h"
#include "titleart/runtime/TitleCharacters.h"
#include "titleart/runtime/TitleFontSubsetter.h"

#include "imgui.h"

#include <algorithm>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.is_object() ? object.find(key) : object.end();
        return object.is_object() && it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    const ta::Json& objectAt(const ta::Json& object, const char* key) {
        static const ta::Json empty = ta::Json::object();
        if (!object.is_object()) return empty;
        auto it = object.find(key);
        return it != object.end() && it->is_object() ? *it : empty;
    }

    void commit(RowContext& ctx, const ta::Json& ops, const std::string& name) {
        std::string error;
        if (!ctx.state->commit(ops, name, &error) && ctx.host) ctx.host->reportError(error);
    }

    std::string kib(size_t bytes) {
        char text[32];
        std::snprintf(text, sizeof(text), bytes < 1024 * 1024 ? "%.0f KB" : "%.1f MB", bytes < 1024 * 1024 ? bytes / 1024.0 : bytes / (1024.0 * 1024.0));
        return text;
    }

    // what Check Characters found (R62), for the document revision it ran on
    struct CharacterCheck {
        uint64_t revision = ~0ull;
        const TitleDesignerState* state = nullptr;
        size_t count = 0;
        std::vector<std::string> fonts;             // "Dela Gothic One: 38 KB" per font
        std::string notInSet;                       // the document's characters its set leaves out
        std::vector<std::string> missingGlyphs;     // R55's sentences
        std::vector<std::string> warnings;
    };
    CharacterCheck& lastCheck() {
        static CharacterCheck check;
        return check;
    }

    // the count line, made again only when the committed document changes
    struct CharacterCount {
        const TitleDesignerState* state = nullptr;
        uint64_t revision = ~0ull;
        size_t count = 0;
        std::vector<std::string> warnings;
    };

    void runCheck(RowContext& ctx) {
        CharacterCheck check;
        check.state = ctx.state;
        check.revision = ctx.state->document() ? ctx.state->document()->revision() : 0;
        const ta::AssetResolver* assets = ctx.host ? ctx.host->assets() : nullptr;
        const ta::TitleArtDoc& doc = ctx.state->committed();
        TitleCharactersReport report;
        const TitleCharacterSet chars = titleDocumentCharacters(doc, assets ? titleCharacterFileReader(*assets) : TitleCharacterFileReader(), &report);
        check.count = chars.size();
        check.warnings = report.warnings;
        // the document's own characters its set leaves out (Characters Used by the Document off)
        TitleCharacterSet own;
        addTitleDocumentText(doc, own);
        for (uint32_t cp : own.list()) {
            if (!chars.contains(cp)) check.notInSet += titleUtf8Of(cp);
        }
        if (assets) {
            for (const TitleMissingCharacters& m : findTitleMissingCharacters(doc, chars, *assets, ctx.state->document() ? ctx.state->document()->projectPath() : std::string())) {
                check.missingGlyphs.push_back(m.message);
            }
            // the subset each font of the text layers ships (R50's estimate)
            TitleFontSubsetter subsetter{std::filesystem::path()};
            std::vector<std::string> seen;
            ta::forEachLayer(doc.root, [&](const ta::Json& layer, const ta::Json*, int) {
                if (ta::layerTypeOf(layer) != ta::LayerType::Text) return true;
                const ta::TitleAssetRef ref = ta::decodeAssetRef(layer.value("font", ta::Json()));
                if (ref.empty()) {
                    if (std::find(seen.begin(), seen.end(), std::string()) == seen.end()) {
                        seen.push_back(std::string());
                        check.fonts.push_back("the built-in face: ships with the engine");
                    }
                    return true;
                }
                const ta::ResolvedTitleAsset asset = assets->resolve(ref, ta::TitleAssetKind::Font);
                if (!asset.found || std::find(seen.begin(), seen.end(), asset.path) != seen.end()) return true;
                seen.push_back(asset.path);
                TitleSubsetRequest request;
                std::string error;
                if (!assets->readBytes(asset, request.bytes, &error)) {
                    check.fonts.push_back(asset.assetPath + ": " + error);
                    return true;
                }
                request.faceIndex = layer.value("face_index", 0);
                request.codepoints = chars.list();
                const TitleSubsetResult result = subsetter.subset(request);
                check.fonts.push_back(asset.assetPath + ": " + (result.ok ? kib(result.bytes.size()) + (result.whole ? " (ships whole)" : "") : result.error));
                return true;
            });
        }
        lastCheck() = std::move(check);
    }

    void runtimeSection(RowContext& ctx, const std::vector<std::string>&, const std::vector<std::string>&) {
        const std::vector<std::string> document = {std::string()};
        const ta::TitleArtDoc& doc = ctx.state->shown();
        if (beginSection(ctx, "characters", "Characters", std::string(), true)) {
            ctx.stepPrefix = "Characters ";
            if (ctx.collecting()) {
                fieldRow(ctx, {{std::string(), "runtime.characters.presets"}}, {.label = "Presets"});
            } else {
                // the presets as checkboxes, one step each
                const ta::Json* presets = fieldValue(doc, {std::string(), "runtime.characters.presets"});
                const ta::Json list = presets && presets->is_array() ? *presets : ta::Json::array();
                ImGui::TextUnformatted("Presets");
                if (ctx.readOnly) ImGui::BeginDisabled();
                for (const std::string& name : titleCharacterPresetNames()) {
                    bool on = std::find(list.begin(), list.end(), ta::Json(name)) != list.end();
                    ImGui::SameLine();
                    if (ImGui::Checkbox((name + "##preset").c_str(), &on)) {
                        ta::Json next = ta::Json::array();
                        for (const std::string& n : titleCharacterPresetNames()) {
                            const bool has = n == name ? on : std::find(list.begin(), list.end(), ta::Json(n)) != list.end();
                            if (has) next.push_back(n);
                        }
                        commit(ctx, editsToOps({{std::string(), "runtime.characters.presets", next}}), "Change Characters Presets");
                    }
                }
                if (ctx.readOnly) ImGui::EndDisabled();
            }
            for (const char* key : {"custom", "custom_file", "document_text"}) fieldRow(ctx, {{std::string(), std::string("runtime.characters.") + key}});
            ctx.stepPrefix.clear();
            if (!ctx.collecting()) {
                static CharacterCount count;
                const uint64_t revision = ctx.state->document() ? ctx.state->document()->revision() : 0;
                if (count.state != ctx.state || count.revision != revision) {
                    count.state = ctx.state;
                    count.revision = revision;
                    const ta::AssetResolver* assets = ctx.host ? ctx.host->assets() : nullptr;
                    TitleCharactersReport report;
                    count.count = titleDocumentCharacters(ctx.state->committed(), assets ? titleCharacterFileReader(*assets) : TitleCharacterFileReader(), &report).size();
                    count.warnings = report.warnings;
                }
                ImGui::TextDisabled("%zu characters ship in Realtime and Glyph Atlas builds", count.count);
                for (const std::string& w : count.warnings) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", w.c_str());
                if (ImGui::Button(ICON_FA_LIST_CHECK " Check Characters")) runCheck(ctx);
                CharacterCheck& check = lastCheck();
                if (check.state == ctx.state && check.revision == revision) {
                    for (const std::string& f : check.fonts) ImGui::BulletText("%s", f.c_str());
                    if (!check.notInSet.empty()) {
                        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Not in the set: \"%s\"", check.notInSet.c_str());
                        ImGui::SameLine();
                        if (!ctx.readOnly && ImGui::SmallButton("Add to Custom")) {
                            const ta::Json* custom = fieldValue(doc, {std::string(), "runtime.characters.custom"});
                            const std::string text = (custom && custom->is_string() ? custom->get<std::string>() : std::string()) + check.notInSet;
                            commit(ctx, editsToOps({{std::string(), "runtime.characters.custom", text}}), "Add Characters to Custom");
                        }
                    }
                    for (const std::string& m : check.missingGlyphs) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", m.c_str());
                    if (check.notInSet.empty() && check.missingGlyphs.empty()) ImGui::TextDisabled("No problems found by the character checks");
                }
            }
            endSection(ctx);
        }
        // the text layer a Glyph Atlas title lays out (R25)
        if (ctx.collecting()) {
            fieldRow(ctx, {{std::string(), "runtime.glyph_layer"}}, {.label = "Glyph Layer"});
            return;
        }
        const std::string current = str(objectAt(doc.root, "runtime"), "glyph_layer");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Glyph Layer");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
        ImGui::SetNextItemWidth(-1.0f);
        const ta::Json* chosen = current.empty() ? nullptr : layerById(doc, current);
        if (ctx.readOnly) ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##glyphlayer", chosen ? str(*chosen, "name").c_str() : "Automatic (the exposed text's layer)")) {
            if (ImGui::Selectable("Automatic (the exposed text's layer)", current.empty())) {
                commit(ctx, editsToOps({{std::string(), "runtime.glyph_layer", ""}}), "Change Glyph Layer");
            }
            ta::forEachLayer(doc.root, [&](const ta::Json& layer, const ta::Json*, int) {
                if (ta::layerTypeOf(layer) != ta::LayerType::Text) return true;
                const std::string id = str(layer, "id");
                if (ImGui::Selectable((str(layer, "name") + "##" + id).c_str(), id == current)) {
                    commit(ctx, editsToOps({{std::string(), "runtime.glyph_layer", id}}), "Change Glyph Layer");
                }
                return true;
            });
            ImGui::EndCombo();
        }
        if (ctx.readOnly) ImGui::EndDisabled();
    }

    // ---- Preview As Glyph Atlas (R28: a real Glyph Atlas title on the committed document) ----

    struct GlyphPreview {
        std::shared_ptr<ta::TitleArtLive> live;
        std::string path;
        uint64_t revision = ~0ull;
        std::string error;
    };
    GlyphPreview& glyphPreview() {
        static GlyphPreview preview;
        return preview;
    }

    bool drawGlyphAtlas(TitleDesignerState& state, ImDrawList* drawList, ImVec2 min, ImVec2 max, const CanvasXform& xform) {
        GlyphPreview& p = glyphPreview();
        if (!state.isOpen()) return false;
        const std::string path = state.document()->projectPath();
        const uint64_t revision = state.document()->revision();
        if (p.path != path || p.revision != revision) {
            // the file changed (a commit writes it): a new title of the committed document
            p.live.reset();
            p.error.clear();
            p.path = path;
            p.revision = revision;
            std::string error;
            p.live = ta::TitleArtRuntime::create(path, TitleArtRenderMode::GLYPH_ATLAS, &error);
            if (!p.live) {
                p.error = error;
            } else {
                ta::TitleArtLiveState live;
                live.label = "TitleArt/Title Designer";
                live.priority = 100;
                ta::TitleArtRuntime::setState(*p.live, live);
            }
        }
        if (!p.error.empty()) {
            drawList->AddText(ImVec2(min.x + 12.0f, min.y + 30.0f), IM_COL32(240, 170, 50, 255), p.error.c_str());
            return true;
        }
        if (!p.live) return false;
        const ta::TitleArtLiveStatus status = ta::TitleArtRuntime::status(*p.live);
        TextureRender* texture = ta::TitleArtRuntime::display(*p.live);
        const ta::TitleDisplayImage& front = p.live->front();
        if (status.failed) drawList->AddText(ImVec2(min.x + 12.0f, min.y + 30.0f), IM_COL32(240, 120, 90, 255), status.error.c_str());
        if (!texture || !front.valid()) return false;
        const ta::TitleArtDoc& doc = *p.live->document();
        const ta::TitleRectI frame = front.frame.empty() ? ta::TitleRectI{0, 0, doc.canvasWidth(), doc.canvasHeight()} : front.frame;
        const double left = frame.x - doc.canvasWidth() * 0.5, top = doc.canvasHeight() * 0.5 - frame.y;
        drawList->PushClipRect(min, max, true);
        drawList->AddImage(Backend::getImGuiTexture(texture), xform.toScreen({left, top}), xform.toScreen({left + frame.width, top - frame.height}));
        drawList->PopClipRect();
        if (!p.live->glyphAtlasPlan().fallback.empty()) {
            drawList->AddText(ImVec2(min.x + 12.0f, max.y - 44.0f), IM_COL32(240, 170, 50, 255),
                              ("Renders as Realtime: " + p.live->glyphAtlasPlan().fallback).c_str());
        }
        return true;
    }

}

void jasperengine::editor::registerTitleArtRuntimeDesigner() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    SectionDef runtime;
    runtime.page = SectionDef::Page::Document;
    runtime.id = "runtime";
    runtime.title = "Runtime";
    runtime.draw = runtimeSection;
    runtime.summary = [](const TitleDesignerState& state) {
        const ta::Json& characters = objectAt(objectAt(state.shown().root, "runtime"), "characters");
        std::string out;
        auto presets = characters.find("presets");
        if (presets != characters.end() && presets->is_array()) {
            for (const ta::Json& preset : *presets) out += (out.empty() ? "" : " + ") + (preset.is_string() ? preset.get<std::string>() : std::string());
        }
        auto own = characters.find("document_text");
        if (own == characters.end() || !own->is_boolean() || own->get<bool>()) out += out.empty() ? "document" : " + document";
        return out;
    };
    addSection(std::move(runtime));

    PreviewModeDef glyph;
    glyph.id = "glyph_atlas";
    glyph.label = "Glyph Atlas";
    glyph.note = "Cross-glyph effects are drawn per glyph";
    glyph.draw = drawGlyphAtlas;
    glyph.close = [] { glyphPreview() = GlyphPreview(); };
    addPreviewMode(std::move(glyph));
}
